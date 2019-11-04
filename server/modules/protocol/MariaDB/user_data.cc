/*
 * Copyright (c) 2019 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "user_data.hh"

#include <maxbase/host.hh>
#include <maxsql/mariadb_connector.hh>
#include <maxsql/mariadb.hh>
#include <maxscale/server.hh>
#include <maxscale/paths.h>
#include <maxscale/protocol/mariadb/module_names.hh>
#include "sqlite_strlike.hh"

using std::string;
using mxq::MariaDB;
using MutexLock = std::unique_lock<std::mutex>;
using Guard = std::lock_guard<std::mutex>;

namespace
{

constexpr auto acquire = std::memory_order_acquire;
constexpr auto release = std::memory_order_release;
constexpr auto npos = string::npos;
constexpr int ipv4min_len = 7;      // 1.1.1.1

namespace backend_queries
{
const string users_query = "SELECT * FROM mysql.user;";
const string db_grants_query =
    "SELECT DISTINCT * FROM ("
    // Select users/roles with general db-level privs ...
    "(SELECT a.user, a.host, a.db FROM mysql.db AS a) UNION "
    // and combine with table privs counting as db-level privs
    "(SELECT a.user, a.host, a.db FROM mysql.tables_priv AS a) UNION "
    // and finally combine with column-level privs as db-level privs
    "(SELECT a.user, a.host, a.db FROM mysql.columns_priv AS a) ) AS c;";
const string roles_query = "SELECT a.user, a.host, a.role FROM mysql.roles_mapping AS a;";
}
}

MariaDBUserManager::MariaDBUserManager(const string& service_name)
    : m_service_name(service_name)
{
}

void MariaDBUserManager::start()
{
    mxb_assert(!m_updater_thread.joinable());
    m_keep_running.store(true, release);
    update_user_accounts();
    m_updater_thread = std::thread([this] {
                                       updater_thread_function();
                                   });
}

void MariaDBUserManager::stop()
{
    mxb_assert(m_updater_thread.joinable());
    m_keep_running.store(false, release);
    m_notifier.notify_one();
    m_updater_thread.join();
}

bool MariaDBUserManager::find_user(const std::string& user, const std::string& host,
                                   const std::string& requested_db, UserEntry* entry_out) const
{
    auto userz = user.c_str();
    auto hostz = host.c_str();
    UserEntry entry;
    bool has_sufficient_privs = false;
    {
        Guard guard(m_userdb_lock);
        auto found = m_userdb.find_entry(user, host);
        if (found)
        {
            entry = *found;
            has_sufficient_privs = m_userdb.check_database_access(*found, requested_db);
        }
    }

    if (!entry.host_pattern.empty())
    {
        if (has_sufficient_privs)
        {
            MXB_INFO("Found matching user '%s'@'%s' for client '%s'@'%s' with sufficient privileges.",
                     entry.username.c_str(), entry.host_pattern.c_str(), userz, hostz);
            *entry_out = entry;
        }
        else
        {
            MXB_INFO("Found matching user '%s'@'%s' for client '%s'@'%s' but user does not have "
                     "sufficient privileges.",
                     entry.username.c_str(), entry.host_pattern.c_str(), userz, hostz);
        }
    }
    else
    {
        MXB_INFO("Found no matching user for client '%s'@'%s'.", userz, hostz);
        // TODO: anonymous users need to be handled specially, as not all authenticators support them.
    }

    return has_sufficient_privs;
}

void MariaDBUserManager::update_user_accounts()
{
    {
        Guard guard(m_notifier_lock);
        m_update_users_requested.store(true, release);
    }
    m_notifier.notify_one();
}

void MariaDBUserManager::set_credentials(const std::string& user, const std::string& pw)
{
    Guard guard(m_settings_lock);
    m_username = user;
    m_password = pw;
}

void MariaDBUserManager::set_backends(const std::vector<SERVER*>& backends)
{
    Guard guard(m_settings_lock);
    m_backends = backends;
}

std::string MariaDBUserManager::protocol_name() const
{
    return MXS_MARIADB_PROTOCOL_NAME;
}

void MariaDBUserManager::updater_thread_function()
{
    using mxb::TimePoint;
    using mxb::Duration;
    using mxb::Clock;

    // Minimum wait between update loops. User accounts should not be changing continuously.
    const std::chrono::seconds default_min_interval(1);
    // Default value for scheduled updates. Cannot set too far in the future, as the cv wait_until bugs and
    // doesn't wait.
    const std::chrono::hours default_max_interval(24);

    // In the beginning, don't update users right away as monitor may not have started yet.
    TimePoint last_update = Clock::now();
    int updates = 0;

    while (m_keep_running.load(acquire))
    {
        /**
         *  The user updating is controlled by several factors:
         *  1) In the beginning, a hardcoded interval is used to try to repeatedly update users as
         *  the monitor is performing its first loop.
         *  2) User refresh requests from the owning service. These can come at any time and rate.
         *  3) users_refresh_time, the minimum time which should pass between refreshes. This means that
         *  rapid update requests may be ignored.
         *  4) users_refresh_interval, the maximum time between refreshes. Users should be refreshed
         *  automatically if this time elapses.
         */

        // Calculate the time for the next scheduled update.
        TimePoint next_scheduled_update = last_update;
        if (updates == 0)
        {
            // If updating has not succeeded even once yet, keep trying again and again,
            // with just a minimal wait.
            next_scheduled_update += default_min_interval;
        }
        else if (m_max_refresh_interval > 0)
        {
            next_scheduled_update += Duration((double)m_max_refresh_interval);
        }
        else
        {
            next_scheduled_update += default_max_interval;
        }

        // Calculate the earliest allowed time for next update.
        TimePoint next_possible_update = last_update;
        if (m_min_refresh_interval > 0 && updates > 0)
        {
            next_possible_update += Duration((double)m_min_refresh_interval);
        }
        else
        {
            next_possible_update += default_min_interval;
        }

        MutexLock lock(m_notifier_lock);
        // Wait until "next_possible_update", or until the thread should stop.
        m_notifier.wait_until(lock, next_possible_update, [this]() {
                                  return !m_keep_running.load(acquire);
                              });

        // Wait until "next_scheduled_update", or until update requested or thread stop.
        m_notifier.wait_until(lock, next_scheduled_update, [this, &updates]() {
                                  return !m_keep_running.load(acquire)
                                  || m_update_users_requested.load(acquire) || updates == 0;
                              });
        lock.unlock();

        if (m_keep_running.load(acquire) && load_users())
        {
            updates++;
        }

        m_update_users_requested.store(false, release);
        last_update = Clock::now();
    }
}

bool MariaDBUserManager::load_users()
{
    MariaDB::ConnectionSettings sett;
    std::vector<SERVER*> backends;

    // Copy all settings under a lock.
    MutexLock lock(m_settings_lock);
    sett.user = m_username;
    sett.password = m_password;
    backends = m_backends;
    lock.unlock();

    mxq::MariaDB con;
    con.set_connection_settings(sett);

    bool found_valid_server = false;
    bool got_data = false;
    bool wrote_data = false;

    for (size_t i = 0; i < backends.size() && !got_data; i++)
    {
        SERVER* srv = backends[i];
        if (srv->is_active && srv->is_usable())
        {
            bool using_roles = false;
            auto version = srv->version();
            // Default roles are in server version 10.1.1.
            if (version.major > 10 || (version.major == 10
                                       && (version.minor > 1 || (version.minor == 1 && version.patch == 1))))
            {
                using_roles = true;
            }

            found_valid_server = true;
            if (con.open(srv->address, srv->port))
            {
                QResult users_res, dbs_res, roles_res;
                // Perform the queries. All must succeed on the same backend.
                if (((users_res = con.query(backend_queries::users_query)) != nullptr)
                    && ((dbs_res = con.query(backend_queries::db_grants_query)) != nullptr))
                {
                    if (using_roles)
                    {
                        if ((roles_res = con.query(backend_queries::roles_query)) != nullptr)
                        {
                            got_data = true;
                        }
                    }
                    else
                    {
                        got_data = true;
                    }
                }

                if (got_data)
                {
                    int rows = users_res->get_row_count();
                    if (write_users(std::move(users_res), using_roles))
                    {
                        write_dbs_and_roles(std::move(dbs_res), std::move(roles_res));
                        wrote_data = true;
                        // Add anonymous proxy user search.
                        MXB_NOTICE("Read %lu usernames with a total of %i user@host entries from '%s'.",
                                   m_userdb.size(), rows, srv->name());
                    }
                }
                else
                {
                    MXB_ERROR("Failed to query server '%s' for user account info. %s",
                              srv->name(), con.error());
                }
            }
        }
    }

    if (!found_valid_server)
    {
        MXB_ERROR("No valid servers from which to query MariaDB user accounts found.");
    }
    return wrote_data;
}

bool MariaDBUserManager::write_users(QResult users, bool using_roles)
{
    auto get_bool_enum = [&users](int64_t col_ind) {
            string val = users->get_string(col_ind);
            return val == "Y" || val == "y";
        };

    // Get column indexes for the interesting fields. Depending on backend version, they may not all
    // exist. Some of the field name start with a capital and some don't. Should the index search be
    // ignorecase?
    auto ind_user = users->get_col_index("User");
    auto ind_host = users->get_col_index("Host");
    auto ind_sel_priv = users->get_col_index("Select_priv");
    auto ind_ins_priv = users->get_col_index("Insert_priv");
    auto ind_upd_priv = users->get_col_index("Update_priv");
    auto ind_del_priv = users->get_col_index("Delete_priv");
    auto ind_ssl = users->get_col_index("ssl_type");
    auto ind_plugin = users->get_col_index("plugin");
    auto ind_pw = users->get_col_index("Password");
    auto ind_auth_str = users->get_col_index("authentication_string");
    auto ind_is_role = users->get_col_index("is_role");
    auto ind_def_role = users->get_col_index("default_role");

    bool has_required_fields = (ind_user >= 0) && (ind_host >= 0)
        && (ind_sel_priv >= 0) && (ind_ins_priv >= 0) && (ind_upd_priv >= 0) && (ind_del_priv >= 0)
        && (ind_ssl >= 0) && (ind_plugin >= 0) && (ind_pw >= 0) && (ind_auth_str >= 0)
        && (!using_roles || (ind_is_role >= 0 && ind_def_role >= 0));

    bool error = false;
    if (has_required_fields)
    {
        Guard guard(m_userdb_lock);

        // Delete any previous data.
        m_userdb.clear();

        while (users->next_row())
        {
            auto username = users->get_string(ind_user);

            UserEntry new_entry;
            new_entry.username = username;
            new_entry.host_pattern = users->get_string(ind_host);

            // Treat the user as having global privileges if any of the following global privileges
            // exists.
            new_entry.global_db_priv = get_bool_enum(ind_sel_priv) || get_bool_enum(ind_ins_priv)
                || get_bool_enum(ind_upd_priv) || get_bool_enum(ind_del_priv);

            // Require SSL if the entry is not empty.
            new_entry.ssl = !users->get_string(ind_ssl).empty();

            new_entry.plugin = users->get_string(ind_plugin);
            new_entry.password = users->get_string(ind_pw);
            new_entry.auth_string = users->get_string(ind_auth_str);

            if (using_roles)
            {
                new_entry.is_role = get_bool_enum(ind_is_role);
                new_entry.default_role = users->get_string(ind_def_role);
            }

            m_userdb.add_entry(username, new_entry);
        }
    }
    else
    {
        MXB_ERROR("Received invalid data when querying user accounts.");
        error = true;
    }
    return !error;
}

void MariaDBUserManager::write_dbs_and_roles(QResult dbs, QResult roles)
{
    // Because the database grant and roles tables are quite simple and only require lookups, their contents
    // need not be saved in an sqlite database. This simplifies things quite a bit.
    using StringSetMap = UserDatabase::StringSetMap;

    auto map_builder = [](const string& grant_col_name, QResult source) {
            StringSetMap result;
            auto ind_user = source->get_col_index("user");
            auto ind_host = source->get_col_index("host");
            auto ind_grant = source->get_col_index(grant_col_name);
            bool valid_data = (ind_user >= 0 && ind_host >= 0 && ind_grant >= 0);
            if (valid_data)
            {
                while (source->next_row())
                {
                    string key = source->get_string(ind_user) + "@" + source->get_string(ind_host);
                    string grant = source->get_string(ind_grant);
                    result[key].insert(grant);
                }
            }
            return result;
        };

    // The maps are mutex-protected. Before locking, prepare the result maps entirely.
    StringSetMap new_db_grants = map_builder("db", std::move(dbs));
    StringSetMap new_roles_mapping;
    if (roles)
    {
        // Old backends may not have role data.
        new_roles_mapping = map_builder("role", std::move(roles));
    }

    Guard guard(m_userdb_lock);
    m_userdb.set_dbs_and_roles(std::move(new_db_grants), std::move(new_roles_mapping));
}

void UserDatabase::add_entry(const std::string& username, const UserEntry& entry)
{
    auto& entrylist = m_contents[username];
    // Find the correct spot to insert. Will insert duplicate hostname patterns, although these should
    // not exist in the source data.
    auto insert_iter = std::upper_bound(entrylist.begin(), entrylist.end(), entry,
                                        UserEntry::host_pattern_is_more_specific);
    entrylist.insert(insert_iter, entry);
}

void UserDatabase::clear()
{
    m_contents.clear();
}

const UserEntry* UserDatabase::find_entry(const std::string& username, const std::string& host) const
{
    const UserEntry* rval = nullptr;
    auto iter = m_contents.find(username);
    if (iter != m_contents.end())
    {
        auto& entrylist = iter->second;
        // List is already ordered, take the first matching entry.
        for (auto& entry : entrylist)
        {
            // The entry must not be a role (they should have empty hostnames in any case) and the hostname
            // pattern should match the host.
            if (!entry.is_role && address_matches_host_pattern(host, entry.host_pattern))
            {
                rval = &entry;
                break;
            }
        }
    }
    return rval;
}

size_t UserDatabase::size() const
{
    return m_contents.size();
}

void UserDatabase::set_dbs_and_roles(StringSetMap&& db_grants, StringSetMap&& roles_mapping)
{
    m_database_grants = std::move(db_grants);
    m_roles_mapping = std::move(roles_mapping);
}

bool UserDatabase::check_database_access(const UserEntry& entry, const std::string& db) const
{
    // Accept the user if the entry has a direct global privilege or if the user is not
    // connecting to a specific database,
    return entry.global_db_priv || db.empty()
            // or the user has a privilege to the database, or a table or column in the database,
           || (user_can_access_db(entry.username, entry.host_pattern, db))
            // or the user can access db through its default role.
           || (!entry.default_role.empty() && role_can_access_db(entry.default_role, db));
}

bool UserDatabase::user_can_access_db(const string& user, const string& host_pattern, const string& db) const
{
    string key = user + "@" + host_pattern;
    auto iter = m_database_grants.find(key);
    if (iter != m_database_grants.end())
    {
        return iter->second.count(db) > 0;
    }
    return false;
}

bool UserDatabase::role_can_access_db(const string& role, const string& db) const
{
    auto role_has_global_priv = [this](const string& role) {
            bool rval = false;
            auto iter = m_contents.find(role);
            if (iter != m_contents.end())
            {
                auto& entrylist = iter->second;
                // Because roles have an empty host-pattern, they must be first in the list.
                if (!entrylist.empty())
                {
                    auto& entry = entrylist.front();
                    if (entry.is_role && entry.global_db_priv)
                    {
                        rval = true;
                    }
                }
            }
            return rval;
        };

    auto find_linked_roles = [this](const string& role) {
            std::vector<string> rval;
            string key = role + "@";
            auto iter = m_roles_mapping.find(key);
            if (iter != m_roles_mapping.end())
            {
                auto& roles_set = iter->second;
                for (auto& linked_role : roles_set)
                {
                    rval.push_back(linked_role);
                }
            }
            return rval;
        };

    // Roles are tricky since one role may have access to other roles and so on.
    StringSet open_set;     // roles which still need to be expanded.
    StringSet closed_set;   // roles which have been checked already.

    open_set.insert(role);
    bool privilege_found = false;
    while (!open_set.empty() && !privilege_found)
    {
        string current_role = *open_set.begin();
        // First, check if role has global privilege.

        if (role_has_global_priv(current_role))
        {
            privilege_found = true;
        }
        // No global privilege, check db-level privilege.
        else if (user_can_access_db(current_role, "", db))
        {
            privilege_found = true;
        }
        else
        {
            // The current role does not have access to db. Add linked roles to the open set.
            auto linked_roles = find_linked_roles(current_role);
            for (const auto& linked_role : linked_roles)
            {
                if (open_set.count(linked_role) == 0 && closed_set.count(linked_role) == 0)
                {
                    open_set.insert(linked_role);
                }
            }
        }

        open_set.erase(current_role);
        closed_set.insert(current_role);
    }
    return privilege_found;
}

bool
UserDatabase::address_matches_host_pattern(const std::string& addr, const std::string& host_pattern) const
{
    // First, check the input address type. This affects how the comparison to the host pattern works.
    auto addrtype = parse_address_type(addr);
    // If host address form is unexpected, don't bother continuing.
    if (addrtype == AddrType::UNKNOWN)
    {
        MXB_ERROR("Address '%s' is not supported.", addr.c_str());      // TODO: print username as well.
        return false;
    }

    auto patterntype = parse_pattern_type(host_pattern);    // TODO: perform this step when loading users
    if (patterntype == PatternType::UNKNOWN)
    {
        MXB_ERROR("Host pattern '%s' is not supported.", addr.c_str());
        return false;
    }

    auto like = [](const string& pattern, const string& str) {
            return sql_strlike(pattern.c_str(), str.c_str(), '\\') == 0;
        };

    bool matched = false;
    if (patterntype == PatternType::ADDRESS)
    {
        if (like(host_pattern, addr))
        {
            matched = true;
        }
        else if (addrtype == AddrType::MAPPED)
        {
            // Try matching the ipv4-part of the address.
            auto ipv4_part = addr.find_last_of(':') + 1;
            if (like(host_pattern, addr.substr(ipv4_part)))
            {
                matched = true;
            }
        }
    }
    else if (patterntype == PatternType::MASK)
    {
        // Requires special handling, add later.
    }
    else if (patterntype == PatternType::HOSTNAME)
    {
        // Need a reverse lookup on the client address. This is slow. TODO: use a separate thread/cache
        string resolved_addr;
        if (mxb::reverse_name_lookup(addr, &resolved_addr))
        {
            if (like(host_pattern, resolved_addr))
            {
                matched = true;
            }
        }
    }

    return matched;
}

UserDatabase::AddrType UserDatabase::parse_address_type(const std::string& addr) const
{
    using mxb::Host;

    auto rval = AddrType::UNKNOWN;
    if (Host::is_valid_ipv4(addr))
    {
        rval = AddrType::IPV4;
    }
    else
    {
        // The address could be IPv4 mapped to IPv6.
        const string mapping_prefix = ":ffff:";
        auto prefix_loc = addr.find(mapping_prefix);
        if (prefix_loc != npos)
        {
            auto ipv4part_loc = prefix_loc + mapping_prefix.length();
            if (addr.length() >= (ipv4part_loc + ipv4min_len))
            {
                // The part after the prefix should be a normal ipv4-address.
                string ipv4part = addr.substr(ipv4part_loc);
                if (Host::is_valid_ipv4(ipv4part))
                {
                    rval = AddrType::MAPPED;
                }
            }
        }

        // Finally, the address could be ipv6.
        if (rval == AddrType::UNKNOWN && Host::is_valid_ipv6(addr))
        {
            rval = AddrType::IPV6;
        }
    }
    return rval;
}

UserDatabase::PatternType UserDatabase::parse_pattern_type(const std::string& host_pattern) const
{
    using mxb::Host;
    // The pattern is more tricky, as it may have wildcards. Assume that if the pattern looks like
    // an address, it is an address and not a hostname. This is not strictly true, but is
    // a reasonable assumption. This parsing is useful, as if we can be reasonably sure the pattern
    // is not a hostname, we can skip the expensive reverse name lookup.

    auto is_wc = [](char c) {
            return c == '%' || c == '_';
        };

    auto patterntype = PatternType::UNKNOWN;
    // First, check some common special cases.
    if (Host::is_valid_ipv4(host_pattern) || Host::is_valid_ipv6(host_pattern))
    {
        // No wildcards, just an address.
        patterntype = PatternType::ADDRESS;
    }
    else if (std::all_of(host_pattern.begin(), host_pattern.end(), is_wc))
    {
        // Pattern is composed entirely of wildcards.
        patterntype = PatternType::ADDRESS;
        // Could be hostname as well, but this would only make a difference with a pattern
        // like "________" or "__%___" where the resolved hostname is of correct length
        // while the address is not.
    }
    else
    {
        auto div_loc = host_pattern.find('/');
        if (div_loc != npos && (div_loc >= ipv4min_len) && host_pattern.length() > (div_loc + ipv4min_len))
        {
            // May be a base_ip/netmask-combination.
            string base_ip = host_pattern.substr(0, div_loc);
            string netmask = host_pattern.substr(div_loc + 1);
            if (Host::is_valid_ipv4(base_ip) && Host::is_valid_ipv4(netmask))
            {
                patterntype = PatternType::MASK;
            }
        }
    }

    if (patterntype == PatternType::UNKNOWN)
    {
        // Pattern is a hostname, or an address with wildcards. Go through it and take an educated guess.
        bool maybe_address = true;
        bool maybe_hostname = true;
        const char esc = '\\';      // '\' is an escape char to allow e.g. my_host.com to match properly.
        bool escaped = false;

        auto classify_char = [is_wc, &maybe_address, &maybe_hostname](char c) {
                auto is_ipchar = [](char c) {
                        return std::isxdigit(c) || c == ':' || c == '.';
                    };

                auto is_hostnamechar = [](char c) {
                        return std::isalnum(c) || c == '_' || c == '.' || c == '-';
                    };

                if (is_wc(c))
                {
                    // Can be address or hostname.
                }
                else
                {
                    if (!is_ipchar(c))
                    {
                        maybe_address = false;
                    }
                    if (!is_hostnamechar(c))
                    {
                        maybe_hostname = false;
                    }
                }
            };

        for (auto c : host_pattern)
        {
            if (escaped)
            {
                // % is not a valid escaped character.
                if (c == '%')
                {
                    maybe_address = false;
                    maybe_hostname = false;
                }
                else
                {
                    classify_char(c);
                }
                escaped = false;
            }
            else if (c == esc)
            {
                escaped = true;
            }
            else
            {
                classify_char(c);
            }

            if (!maybe_address && !maybe_hostname)
            {
                // Unrecognized pattern type.
                break;
            }
        }

        if (maybe_address)
        {
            // Address takes priority.
            patterntype = PatternType::ADDRESS;
        }
        else if (maybe_hostname)
        {
            patterntype = PatternType::HOSTNAME;
        }
    }
    return patterntype;
}

bool UserEntry::host_pattern_is_more_specific(const UserEntry& lhs, const UserEntry& rhs)
{
    // Order entries according to https://mariadb.com/kb/en/library/create-user/
    const string& lhost = lhs.host_pattern;
    const string& rhost = rhs.host_pattern;
    const char wildcards[] = "%_";
    auto lwc_pos = lhost.find_first_of(wildcards);
    auto rwc_pos = rhost.find_first_of(wildcards);
    bool lwc = (lwc_pos != string::npos);
    bool rwc = (rwc_pos != string::npos);

    // The host without wc:s sorts earlier than the one with them,
    return (!lwc && rwc)
            // ... and if both have wc:s, the one with the later wc wins (ties broken by strcmp),
           || (lwc && rwc && ((lwc_pos > rwc_pos) || (lwc_pos == rwc_pos && lhost < rhost)))
            // ... and if neither have wildcards, use string order.
           || (!lwc && !rwc && lhost < rhost);
}
