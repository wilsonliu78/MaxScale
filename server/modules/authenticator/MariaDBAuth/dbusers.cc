/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-03-10
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * Loading MySQL users from a MySQL backend server
 */

#include "mysql_auth.hh"

#include <ctype.h>
#include <netdb.h>
#include <stdio.h>

#include <algorithm>
#include <vector>
#include <mysql.h>
#include <mysqld_error.h>
#include <maxbase/alloc.h>
#include <maxscale/dcb.hh>
#include <maxscale/maxscale.h>
#include <maxscale/mysql_utils.hh>
#include <maxscale/paths.h>
#include <maxscale/protocol/mariadb/mysql.hh>
#include <maxscale/pcre2.hh>
#include <maxscale/router.hh>
#include <maxscale/secrets.hh>
#include <maxscale/service.hh>
#include <maxscale/users.hh>
#include <maxscale/utils.h>
#include <maxscale/routingworker.hh>

namespace
{
using AuthRes = mariadb::ClientAuthenticator::AuthRes;
using mariadb::UserEntry;
}

/** Don't include the root user */
#define USERS_QUERY_NO_ROOT " AND user.user NOT IN ('root')"

// Query used with 10.0 or older
const char* mariadb_users_query_format =
    "SELECT u.user, u.host, d.db, u.select_priv, u.%s "
    "FROM mysql.user AS u LEFT JOIN mysql.db AS d "
    "ON (u.user = d.user AND u.host = d.host) WHERE u.plugin IN ('', 'mysql_native_password') %s "
    "UNION "
    "SELECT u.user, u.host, t.db, u.select_priv, u.%s "
    "FROM mysql.user AS u LEFT JOIN mysql.tables_priv AS t "
    "ON (u.user = t.user AND u.host = t.host) WHERE u.plugin IN ('', 'mysql_native_password') %s";

const char* clustrix_users_query_format =
    "SELECT u.username AS user, u.host, a.dbname AS db, "
    "       IF(a.privileges & 1048576, 'Y', 'N') AS select_priv, u.password "
    "FROM system.users AS u LEFT JOIN system.user_acl AS a ON (u.user = a.role) "
    "WHERE u.plugin IN ('', 'mysql_native_password') %s";

// Used with 10.2 or newer, supports composite roles
const char* mariadb_102_users_query =
    // `t` is users that are not roles
    "WITH RECURSIVE t AS ( "
    "  SELECT u.user, u.host, d.db, u.select_priv, "
    "         IF(u.password <> '', u.password, u.authentication_string) AS password, "
    "         u.is_role, u.default_role"
    "  FROM mysql.user AS u LEFT JOIN mysql.db AS d "
    "  ON (u.user = d.user AND u.host = d.host) "
    "  WHERE u.plugin IN ('', 'mysql_native_password') "
    "  UNION "
    "  SELECT u.user, u.host, t.db, u.select_priv, "
    "         IF(u.password <> '', u.password, u.authentication_string), "
    "         u.is_role, u.default_role "
    "  FROM mysql.user AS u LEFT JOIN mysql.tables_priv AS t "
    "  ON (u.user = t.user AND u.host = t.host)"
    "  WHERE u.plugin IN ('', 'mysql_native_password') "
    "), users AS ("
    // Select the root row, the actual user
    "  SELECT t.user, t.host, t.db, t.select_priv, t.password, t.default_role AS role FROM t"
    "  WHERE t.is_role = 'N'"
    "  UNION"
    // Recursively select all roles for the users
    "  SELECT u.user, u.host, t.db, t.select_priv, u.password, r.role FROM t"
    "  JOIN users AS u"
    "  ON (t.user = u.role)"
    "  LEFT JOIN mysql.roles_mapping AS r"
    "  ON (t.user = r.user)"
    "  WHERE t.is_role = 'Y'"
    ")"
    "SELECT DISTINCT t.user, t.host, t.db, t.select_priv, t.password FROM users AS t %s";

// Query used with MariaDB 10.1, supports basic roles
const char* mariadb_101_users_query
    =   // First, select all users
        "SELECT t.user, t.host, t.db, t.select_priv, t.password FROM "
        "( "
        "    SELECT u.user, u.host, d.db, u.select_priv, u.password AS password, u.is_role "
        "    FROM mysql.user AS u LEFT JOIN mysql.db AS d "
        "    ON (u.user = d.user AND u.host = d.host) "
        "    WHERE u.plugin IN ('', 'mysql_native_password') "
        "    UNION "
        "    SELECT u.user, u.host, t.db, u.select_priv, u.password AS password, u.is_role "
        "    FROM mysql.user AS u LEFT JOIN mysql.tables_priv AS t "
        "    ON (u.user = t.user AND u.host = t.host) "
        "    WHERE u.plugin IN ('', 'mysql_native_password') "
        ") AS t "
        // Discard any users that are roles
        "WHERE t.is_role <> 'Y' %s "
        "UNION "
        // Then select all users again
        "SELECT r.user, r.host, u.db, u.select_priv, t.password FROM "
        "( "
        "    SELECT u.user, u.host, d.db, u.select_priv, u.password AS password, u.default_role "
        "    FROM mysql.user AS u LEFT JOIN mysql.db AS d "
        "    ON (u.user = d.user AND u.host = d.host) "
        "    WHERE u.plugin IN ('', 'mysql_native_password') "
        "    UNION "
        "    SELECT u.user, u.host, t.db, u.select_priv, u.password AS password, u.default_role "
        "    FROM mysql.user AS u LEFT JOIN mysql.tables_priv AS t "
        "    ON (u.user = t.user AND u.host = t.host) "
        "    WHERE u.plugin IN ('', 'mysql_native_password') "
        ") AS t "
        // Join it to the roles_mapping table to only have users with roles
        "JOIN mysql.roles_mapping AS r "
        "ON (r.user = t.user AND r.host = t.host) "
        // Then join it into itself to get the privileges of the role with the name of the user
        "JOIN "
        "( "
        "    SELECT u.user, u.host, d.db, u.select_priv, u.password AS password, u.is_role "
        "    FROM mysql.user AS u LEFT JOIN mysql.db AS d "
        "    ON (u.user = d.user AND u.host = d.host) "
        "    WHERE u.plugin IN ('', 'mysql_native_password') "
        "    UNION "
        "    SELECT u.user, u.host, t.db, u.select_priv, u.password AS password, u.is_role "
        "    FROM mysql.user AS u LEFT JOIN mysql.tables_priv AS t "
        "    ON (u.user = t.user AND u.host = t.host) "
        "    WHERE u.plugin IN ('', 'mysql_native_password') "
        ") AS u "
        "ON (u.user = r.role AND u.is_role = 'Y') "
        // We only care about users that have a default role assigned
        "WHERE t.default_role = u.user %s;";

enum server_category_t
{
    SERVER_NO_ROLES,
    SERVER_ROLES,
    SERVER_CLUSTRIX
};

static char* get_mariadb_102_users_query(bool include_root)
{
    const char* with_root = include_root ? "" : " WHERE t.user <> 'root'";

    size_t n_bytes = snprintf(NULL, 0, mariadb_102_users_query, with_root);
    char* rval = static_cast<char*>(MXS_MALLOC(n_bytes + 1));
    MXS_ABORT_IF_NULL(rval);
    snprintf(rval, n_bytes + 1, mariadb_102_users_query, with_root);

    return rval;
}

static char* get_mariadb_101_users_query(bool include_root)
{
    const char* with_root = include_root ? "" : " AND t.user NOT IN ('root')";

    size_t n_bytes = snprintf(NULL, 0, mariadb_101_users_query, with_root, with_root);
    char* rval = static_cast<char*>(MXS_MALLOC(n_bytes + 1));
    MXS_ABORT_IF_NULL(rval);
    snprintf(rval, n_bytes + 1, mariadb_101_users_query, with_root, with_root);

    return rval;
}

/**
 * Return the column name of the password hash in the mysql.user table.
 *
 * @param version Server version
 * @return Column name
 */
static const char* get_password_column_name(const SERVER::Version& version)
{
    const char* rval = "password";      // Usual result, used in MariaDB.
    auto major = version.major;
    auto minor = version.minor;
    if ((major == 5 && minor == 7) || (major == 8 && minor == 0))
    {
        rval = "authentication_string";
    }
    return rval;
}

static char* get_mariadb_users_query(bool include_root, const SERVER::Version& version)
{
    const char* password = get_password_column_name(version);
    const char* with_root = include_root ? "" : " AND u.user NOT IN ('root')";

    size_t n_bytes = snprintf(NULL, 0, mariadb_users_query_format, password, with_root, password, with_root);
    char* rval = static_cast<char*>(MXS_MALLOC(n_bytes + 1));
    MXS_ABORT_IF_NULL(rval);
    snprintf(rval, n_bytes + 1, mariadb_users_query_format, password, with_root, password, with_root);

    return rval;
}

static char* get_clustrix_users_query(bool include_root)
{
    const char* with_root;

    if (include_root)
    {
        with_root =
            "UNION ALL "
            "SELECT 'root' AS user, '127.0.0.1', '*' AS db, 'Y' AS select_priv, '' AS password";
    }
    else
    {
        with_root = "AND u.username <> 'root'";
    }

    size_t n_bytes = snprintf(NULL, 0, clustrix_users_query_format, with_root);
    char* rval = static_cast<char*>(MXS_MALLOC(n_bytes + 1));
    MXS_ABORT_IF_NULL(rval);
    snprintf(rval, n_bytes + 1, clustrix_users_query_format, with_root);

    return rval;
}

static char* get_users_query(const SERVER::Version& version, bool include_root, server_category_t category)
{
    char* rval = nullptr;

    switch (category)
    {
    case SERVER_ROLES:
        // Require 10.2.15 due to MDEV-15840 and MDEV-15556
        rval = version.total >= 100215 ? get_mariadb_102_users_query(include_root) :
            get_mariadb_101_users_query(include_root);
        break;

    case SERVER_CLUSTRIX:
        rval = get_clustrix_users_query(include_root);
        break;

    case SERVER_NO_ROLES:
        // Either an older MariaDB version or a MySQL variant, use the legacy query
        rval = get_mariadb_users_query(include_root, version);
        break;

    default:
        mxb_assert(!true);
    }

    return rval;
}

static bool
check_password(const char* password_entry, MYSQL_session* session)
{
    uint8_t stored_token[SHA_DIGEST_LENGTH] = {};
    size_t stored_token_len = sizeof(stored_token);

    if (*password_entry)
    {
        /** Convert the hexadecimal string to binary */
        mxs::hex2bin(password_entry, strlen(password_entry), stored_token);
    }

    /**
     * The client authentication token is made up of:
     *
     * XOR( SHA1(real_password), SHA1( CONCAT( scramble, <value of mysql.user.password> ) ) )
     *
     * Since we know the scramble and the value stored in mysql.user.password,
     * we can extract the SHA1 of the real password by doing a XOR of the client
     * authentication token with the SHA1 of the scramble concatenated with the
     * value of mysql.user.password.
     *
     * Once we have the SHA1 of the original password,  we can create the SHA1
     * of this hash and compare the value with the one stored in the backend
     * database. If the values match, the user has sent the right password.
     */

    /** First, calculate the SHA1 of the scramble and the hash stored in the database */
    uint8_t step1[SHA_DIGEST_LENGTH];
    gw_sha1_2_str(session->scramble, sizeof(session->scramble), stored_token, stored_token_len, step1);

    /** Next, extract the SHA1 of the real password by XOR'ing it with
     * the output of the previous calculation */
    uint8_t step2[SHA_DIGEST_LENGTH] = {};
    mxs::bin_bin_xor(session->auth_token.data(), step1, session->auth_token.size(), step2);

    /** The phase 2 scramble needs to be copied to the shared data structure as it
     * is required when the backend authentication is done. */
    session->auth_token_phase2.assign(step2, step2 + SHA_DIGEST_LENGTH);

    /** Finally, calculate the SHA1 of the hashed real password */
    uint8_t final_step[SHA_DIGEST_LENGTH];
    gw_sha1_str(step2, SHA_DIGEST_LENGTH, final_step);

    /** If the two values match, the client has sent the correct password */
    return memcmp(final_step, stored_token, stored_token_len) == 0;
}

/**
 * @brief Verify the user password
 *
 * @param entry        User account entry
 * @param session      Shared MySQL session
 * @return True if password was correct
 */
bool MariaDBClientAuthenticator::validate_mysql_user(const UserEntry* entry, MYSQL_session* session)
{
    // If the user entry has empty password and the client gave no password, accept.
    if (entry->password.empty() && session->auth_token.empty())
    {
        return true;
    }
    return check_password(entry->password.c_str(), session);
}

/**
 * If the hostname is of form a.b.c.d/e.f.g.h where e-h is 255 or 0, replace
 * the zeros in the first part with '%' and remove the second part. This does
 * not yet support netmasks completely, but should be sufficient for most
 * situations. In case of error, the hostname may end in an invalid state, which
 * will cause an error later on.
 *
 * @param host  The hostname, which is modified in-place. If merging is unsuccessful,
 *              it may end up garbled.
 */
static void merge_netmask(char* host)
{
    char* delimiter_loc = strchr(host, '/');
    if (delimiter_loc == NULL)
    {
        return;     // Nothing to do
    }
    /* If anything goes wrong, we put the '/' back in to ensure the hostname
     * cannot be used.
     */
    *delimiter_loc = '\0';

    char* ip_token_loc = host;
    char* mask_token_loc = delimiter_loc + 1;   // This is at minimum a \0

    while (ip_token_loc && mask_token_loc)
    {
        if (strncmp(mask_token_loc, "255", 3) == 0)
        {
            // Skip
        }
        else if (*mask_token_loc == '0' && *ip_token_loc == '0')
        {
            *ip_token_loc = '%';
        }
        else
        {
            /* Any other combination is considered invalid. This may leave the
             * hostname in a partially modified state.
             * TODO: handle more cases
             */
            *delimiter_loc = '/';
            MXS_ERROR("Unrecognized IP-bytes in host/mask-combination. "
                      "Merge incomplete: %s",
                      host);
            return;
        }

        ip_token_loc = strchr(ip_token_loc, '.');
        mask_token_loc = strchr(mask_token_loc, '.');
        if (ip_token_loc && mask_token_loc)
        {
            ip_token_loc++;
            mask_token_loc++;
        }
    }
    if (ip_token_loc || mask_token_loc)
    {
        *delimiter_loc = '/';
        MXS_ERROR("Unequal number of IP-bytes in host/mask-combination. "
                  "Merge incomplete: %s",
                  host);
    }
}

void add_mysql_user(sqlite3* handle,
                    const char* user,
                    const char* host,
                    const char* db,
                    bool anydb,
                    const char* pw)
{
    size_t dblen = db && *db ? strlen(db) + 2 : sizeof(null_token);     /** +2 for single quotes */
    char dbstr[dblen + 1];

    if (db && *db)
    {
        sprintf(dbstr, "'%s'", db);
    }
    else
    {
        strcpy(dbstr, null_token);
    }

    size_t pwlen = pw && *pw ? strlen(pw) + 2 : sizeof(null_token);     /** +2 for single quotes */
    char pwstr[pwlen + 1];

    if (pw && *pw)
    {
        if (strlen(pw) == 16)
        {
            MXS_ERROR("The user %s@%s has on old password in the "
                      "backend database. MaxScale does not support these "
                      "old passwords. This user will not be able to connect "
                      "via MaxScale. Update the users password to correct "
                      "this.",
                      user,
                      host);
            return;
        }
        else if (*pw == '*')
        {
            pw++;
        }
        sprintf(pwstr, "'%s'", pw);
    }
    else
    {
        strcpy(pwstr, null_token);
    }

    size_t len = sizeof(insert_user_query) + strlen(user) + strlen(host) + dblen + pwlen + 1;

    char insert_sql[len + 1];
    sprintf(insert_sql, insert_user_query, user, host, dbstr, anydb ? "1" : "0", pwstr);

    char* err;
    if (sqlite3_exec(handle, insert_sql, NULL, NULL, &err) != SQLITE_OK)
    {
        MXS_ERROR("Failed to insert user: %s", err);
        sqlite3_free(err);
    }

    MXS_INFO("Added user: %s@%s db: %s global: %s", user, host, dbstr, anydb ? "yes" : "no");
}

static void add_database(sqlite3* handle, const char* db)
{
    size_t len = sizeof(insert_database_query) + strlen(db) + 1;
    char insert_sql[len + 1];

    sprintf(insert_sql, insert_database_query, db);

    char* err;
    if (sqlite3_exec(handle, insert_sql, NULL, NULL, &err) != SQLITE_OK)
    {
        MXS_ERROR("Failed to insert database: %s", err);
        sqlite3_free(err);
    }
}

/**
 * @brief Check permissions for a particular table.
 *
 * @param mysql         A valid MySQL connection.
 * @param service       The service in question.
 * @param user          The user in question.
 * @param table         The table whose permissions are checked.
 * @param query         The query using which the table permissions are checked.
 * @param log_priority  The priority using which a possible ER_TABLE_ACCESS_DENIED_ERROR
 *                      should be logged.
 * @param message       Additional log message.
 *
 * @return True if the table could accessed or if the priority is less than LOG_ERR,
 *         false otherwise.
 */
static bool check_table_permissions(MYSQL* mysql,
                                    SERVICE* service,
                                    const char* user,
                                    const char* table,
                                    const char* query,
                                    int log_priority,
                                    const char* message = nullptr)
{
    bool rval = true;

    if (mxs_mysql_query(mysql, query) != 0)
    {
        if (mysql_errno(mysql) == ER_TABLEACCESS_DENIED_ERROR)
        {
            if (log_priority >= LOG_ERR)
            {
                rval = false;
            }

            MXS_LOG_MESSAGE(log_priority,
                            "[%s] User '%s' is missing SELECT privileges "
                            "on %s table.%sMySQL error message: %s",
                            service->name(),
                            user,
                            table,
                            message ? message : " ",
                            mysql_error(mysql));
        }
        else
        {
            MXS_ERROR("[%s] Failed to query from %s table."
                      " MySQL error message: %s",
                      service->name(),
                      table,
                      mysql_error(mysql));
        }
    }
    else
    {

        MYSQL_RES* res = mysql_use_result(mysql);
        if (res == NULL)
        {
            MXS_ERROR("[%s] Result retrieval failed when checking for permissions to "
                      "the %s table: %s",
                      service->name(),
                      table,
                      mysql_error(mysql));
        }
        else
        {
            mysql_free_result(res);
        }
    }

    return rval;
}

/**
 * @brief Check table permissions on MySQL/MariaDB server
 *
 * @return True if the table permissions are OK, false otherwise.
 */
static bool check_default_table_permissions(MYSQL* mysql,
                                            SERVICE* service,
                                            SERVER* server,
                                            const char* user)
{
    bool rval = true;

    const char* format = "SELECT user, host, %s, Select_priv FROM mysql.user limit 1";
    const char* query_pw = get_password_column_name(server->version());

    char query[strlen(format) + strlen(query_pw) + 1];
    sprintf(query, format, query_pw);

    rval = check_table_permissions(mysql, service, user, "mysql.user", query, LOG_ERR);

    check_table_permissions(mysql, service, user,
                            "mysql.db",
                            "SELECT user, host, db FROM mysql.db limit 1",
                            LOG_WARNING,
                            "Database name will be ignored in authentication. ");

    check_table_permissions(mysql, service, user,
                            "mysql.tables_priv",
                            "SELECT user, host, db FROM mysql.tables_priv limit 1",
                            LOG_WARNING,
                            "Database name will be ignored in authentication. ");

    // Check whether the current user has the SHOW DATABASES privilege
    if (mxs_mysql_query(mysql, "SHOW GRANTS") == 0)
    {
        if (MYSQL_RES* res = mysql_use_result(mysql))
        {
            bool found = false;

            for (MYSQL_ROW row = mysql_fetch_row(res); row; row = mysql_fetch_row(res))
            {
                if (strcasestr(row[0], "SHOW DATABASES") || strcasestr(row[0], "ALL PRIVILEGES ON *.*"))
                {
                    // GRANT ALL PRIVILEGES ON *.* will overwrite SHOW DATABASES so it needs to be checked
                    // separately
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                MXS_WARNING("[%s] User '%s' is missing the SHOW DATABASES privilege. "
                            "This means that MaxScale cannot see all databases and authentication can fail.",
                            service->name(),
                            user);
            }
            mysql_free_result(res);
        }
    }

    return rval;
}

/**
 * @brief Check table permissions on a Clustrix server
 *
 * @return True if the table permissions are OK, false otherwise.
 */
static bool check_clustrix_table_permissions(MYSQL* mysql,
                                             SERVICE* service,
                                             SERVER* server,
                                             const char* user)
{
    bool rval = true;

    if (!check_table_permissions(mysql, service, user,
                                 "system.users",
                                 "SELECT username, host, password FROM system.users LIMIT 1",
                                 LOG_ERR))
    {
        rval = false;
    }

    if (!check_table_permissions(mysql, service, user,
                                 "system.user_acl",
                                 "SELECT privileges, role FROM system.user_acl LIMIT 1",
                                 LOG_ERR))
    {
        rval = false;
    }

    // TODO: SHOW DATABASES privilege is not checked.

    return rval;
}

/**
 * @brief Check service permissions on one server
 *
 * @param server Server to check
 * @param user Username
 * @param password Password
 * @return True if the service permissions are OK, false if one or more permissions
 * are missing.
 */
static bool check_server_permissions(SERVICE* service,
                                     SERVER* server,
                                     const char* user,
                                     const char* password)
{
    MYSQL* mysql = mysql_init(nullptr);
    const mxs::Config& cnf = mxs::Config::get();
    unsigned int timeout;

    timeout = cnf.auth_read_timeout.get().count();
    mysql_optionsv(mysql, MYSQL_OPT_READ_TIMEOUT, &timeout);
    timeout = cnf.auth_conn_timeout.get().count();
    mysql_optionsv(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    timeout = cnf.auth_write_timeout.get().count();
    mysql_optionsv(mysql, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
    mysql_optionsv(mysql, MYSQL_PLUGIN_DIR, get_connector_plugindir());

    if (mxs_mysql_real_connect(mysql, server, user, password) == NULL)
    {
        int my_errno = mysql_errno(mysql);

        MXS_ERROR("[%s] Failed to connect to server '%s' ([%s]:%d) when"
                  " checking authentication user credentials and permissions: %d %s",
                  service->name(),
                  server->name(),
                  server->address(),
                  server->port(),
                  my_errno,
                  mysql_error(mysql));

        mysql_close(mysql);
        return my_errno != ER_ACCESS_DENIED_ERROR;
    }

    if (server->version().total == 0)
    {
        mxs_mysql_update_server_version(server, mysql);
    }

    bool rval = true;
    if (server->type() == SERVER::Type::CLUSTRIX)
    {
        rval = check_clustrix_table_permissions(mysql, service, server, user);
    }
    else
    {
        rval = check_default_table_permissions(mysql, service, server, user);
    }

    mysql_close(mysql);

    return rval;
}

bool check_service_permissions(SERVICE* service)
{
    auto servers = service->reachable_servers();

    if (rcap_type_required(service_get_capabilities(service), RCAP_TYPE_NO_AUTH)
        || mxs::Config::get().skip_permission_checks.get()
        || servers.empty())     // No servers to check
    {
        return true;
    }

    const char* user;
    const char* password;

    serviceGetUser(service, &user, &password);

    auto dpasswd = decrypt_password(password);
    bool rval = false;

    for (auto server : servers)
    {
        if (server->is_mxs_service() || check_server_permissions(service, server, user, dpasswd.c_str()))
        {
            rval = true;
        }
    }

    return rval;
}

static bool roles_are_available(MYSQL* conn, SERVICE* service, SERVER* server)
{
    bool rval = false;
    if (server->version().total >= 100101)
    {
        static bool log_missing_privs = true;

        if (mxs_mysql_query(conn, "SET @roles_are_available=(SELECT 1 FROM mysql.roles_mapping LIMIT 1)") == 0
            && mxs_mysql_query(conn,
                               "SET @roles_are_available=(SELECT default_role FROM mysql.user LIMIT 1)") == 0)
        {
            rval = true;
        }
        else if (log_missing_privs)
        {
            log_missing_privs = false;
            MXS_WARNING("The user for service '%s' might be missing the SELECT grant on "
                        "`mysql.roles_mapping` or `mysql.user`. Use of default roles is disabled "
                        "until the missing privileges are added. Error was: %s",
                        service->name(),
                        mysql_error(conn));
        }
    }

    return rval;
}

static bool have_mdev13453_problem(MYSQL* con, SERVER* server)
{
    bool rval = false;

    if (mxs_pcre2_simple_match("SELECT command denied to user .* for table 'users'",
                               mysql_error(con), 0, NULL) == MXS_PCRE2_MATCH)
    {
        char user[256] = "<failed to query user>";      // Enough for all user-hostname combinations
        const char* quoted_user = "select concat(\"'\", user, \"'@'\", host, \"'\") as user "
                                  "from mysql.user "
                                  "where concat(user, \"@\", host) = current_user()";
        MYSQL_RES* res;

        if (mxs_mysql_query(con, quoted_user) == 0 && (res = mysql_store_result(con)))
        {
            MYSQL_ROW row = mysql_fetch_row(res);

            if (row && row[0])
            {
                snprintf(user, sizeof(user), "%s", row[0]);
            }

            mysql_free_result(res);
        }

        MXS_WARNING("Due to MDEV-13453, the service user requires extra grants on the `mysql` database in "
                    "order for roles to be used. To fix the problem, add the following grant: "
                    "GRANT SELECT ON `mysql`.* TO %s", user);
        rval = true;
    }

    return rval;
}

// Contains loaded user definitions, only used temporarily
struct User
{
    std::string user;
    std::string host;
    std::string db;
    bool        anydb;
    std::string pw;
};

bool query_and_process_users(const char* query, MYSQL* con, SERVICE* service, int* users,
                             std::vector<User>* userlist, server_category_t category)
{
    // Clustrix does not have a mysql database. If non-clustrix we set the
    // default database in case CTEs are used.
    bool rval = (category == SERVER_CLUSTRIX || mxs_mysql_query(con, "USE mysql") == 0);

    if (rval && mxs_mysql_query(con, query) == 0)
    {
        MYSQL_RES* result = mysql_store_result(con);

        if (result)
        {
            MYSQL_ROW row;

            while ((row = mysql_fetch_row(result)))
            {
                if (service->config()->strip_db_esc)
                {
                    strip_escape_chars(row[2]);
                }

                if (strchr(row[1], '/'))
                {
                    merge_netmask(row[1]);
                }

                userlist->push_back({row[0], row[1], row[2] ? row[2] : "",
                                     row[3] && strcmp(row[3], "Y") == 0,
                                     row[4] ? row[4] : ""});
                (*users)++;
            }

            mysql_free_result(result);
            rval = true;
        }
    }

    return rval;
}

int MariaDBAuthenticatorModule::get_users_from_server(MYSQL* con, SERVER* server, SERVICE* service)
{
    auto server_version = server->version();
    if (server_version.total == 0)      // No monitor or the monitor hasn't ran yet.
    {
        mxs_mysql_update_server_version(server, con);
        server_version = server->version();
    }

    server_category_t category;
    if (server->type() == SERVER::Type::CLUSTRIX)
    {
        category = SERVER_CLUSTRIX;
    }
    else if (roles_are_available(con, service, server))
    {
        category = SERVER_ROLES;
    }
    else
    {
        category = SERVER_NO_ROLES;
    }

    bool enable_root = service->config()->enable_root;
    char* query = get_users_query(server_version, enable_root, category);

    int users = 0;
    std::vector<User> userlist;
    std::vector<std::string> dblist;

    bool rv = query_and_process_users(query, con, service, &users, &userlist, category);

    if (!rv && have_mdev13453_problem(con, server))
    {
        /**
         * Try to work around MDEV-13453 by using a query without CTEs. Masquerading as
         * a 10.1.10 server makes sure CTEs aren't used.
         */
        MXS_FREE(query);
        query = get_users_query(server_version, enable_root, SERVER_ROLES);
        rv = query_and_process_users(query, con, service, &users, &userlist, SERVER_ROLES);
    }

    if (!rv)
    {
        MXS_ERROR("Failed to load users from server '%s': %s", server->name(), mysql_error(con));
    }

    MXS_FREE(query);

    /** Load the list of databases */
    if (mxs_mysql_query(con, "SHOW DATABASES") == 0)
    {
        MYSQL_RES* result = mysql_store_result(con);
        if (result)
        {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result)))
            {
                dblist.push_back(row[0]);
            }

            mysql_free_result(result);
        }
    }
    else
    {
        rv = false;
        MXS_ERROR("Failed to load list of databases: %s", mysql_error(con));
    }

    if (rv)
    {
        auto func = [this, userlist, dblist]() {
                sqlite3* handle = get_handle();

                for (const auto& user : userlist)
                {
                    add_mysql_user(handle, user.user.c_str(), user.host.c_str(),
                                   user.db.c_str(), user.anydb, user.pw.c_str());
                }

                for (const auto& db : dblist)
                {
                    add_database(handle, db.c_str());
                }
            };

        mxs::RoutingWorker::broadcast(func, mxs::RoutingWorker::EXECUTE_AUTO);
    }

    return users;
}
