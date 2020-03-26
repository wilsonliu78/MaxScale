/*
 * Copyright (c) 2018 MariaDB Corporation Ab
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
#pragma once

/**
 * Internal header for the server type
 */

#include <maxbase/ccdefs.hh>

#include <map>
#include <mutex>
#include <maxscale/config.hh>
#include <maxscale/config2.hh>
#include <maxscale/server.hh>
#include <maxscale/resultset.hh>

// Private server implementation
class Server : public SERVER
{
public:
    static const int MAX_ADDRESS_LEN = 1024;
    static const int MAX_MONUSER_LEN = 512;
    static const int MAX_MONPW_LEN = 512;
    static const int MAX_VERSION_LEN = 256;

    Server(const std::string& name, std::unique_ptr<mxs::SSLContext> ssl = {})
        : m_name(name)
        , m_ssl_provider(std::move(ssl))
    {
    }

    ~Server() = default;

    const char* address() const override
    {
        return m_settings.address;
    }

    int port() const override
    {
        return m_settings.port;
    }

    int extra_port() const override
    {
        return m_settings.extra_port;
    }

    long persistpoolmax() const
    {
        return m_settings.persistpoolmax;
    }

    void set_persistpoolmax(long persistpoolmax)
    {
        m_settings.persistpoolmax = persistpoolmax;
    }

    long persistmaxtime() const
    {
        return m_settings.persistmaxtime.count();
    }

    void set_persistmaxtime(long persistmaxtime)
    {
        m_settings.persistmaxtime = std::chrono::seconds(persistmaxtime);
    }

    void set_rank(int64_t rank)
    {
        m_settings.rank = rank;
    }

    void set_priority(int64_t priority)
    {
        m_settings.priority = priority;
    }

    bool have_disk_space_limits() const override
    {
        std::lock_guard<std::mutex> guard(m_settings.lock);
        return !m_settings.disk_space_limits.empty();
    }

    DiskSpaceLimits get_disk_space_limits() const override
    {
        std::lock_guard<std::mutex> guard(m_settings.lock);
        return m_settings.disk_space_limits;
    }

    void set_disk_space_limits(const DiskSpaceLimits& new_limits)
    {
        std::lock_guard<std::mutex> guard(m_settings.lock);
        m_settings.disk_space_limits = new_limits;
    }

    bool persistent_conns_enabled() const override
    {
        return m_settings.persistpoolmax > 0;
    }

    void set_version(uint64_t version_num, const std::string& version_str) override;

    Version version() const override
    {
        return m_info.version_num();
    }

    Type type() const override
    {
        return m_info.type();
    }

    std::string version_string() const override
    {
        return m_info.version_string();
    }

    const char* name() const override
    {
        return m_name.c_str();
    }

    int64_t rank() const override
    {
        return m_settings.rank;
    }

    int64_t priority() const override
    {
        return m_settings.priority;
    }

    /**
     * Print server details to a dcb.
     *
     * @param dcb Dcb to print to
     */
    void print_to_dcb(DCB* dcb) const;

    /**
     * Allocates a new server. Should only be called from ServerManager::create_server().
     *
     * @param name Server name
     * @param params Configuration
     * @return The new server or NULL on error
     */
    static Server* server_alloc(const char* name, const mxs::ConfigParameters& params);

    /**
     * Creates a server without any configuration. This should be used in unit tests in place of
     * a default ctor.
     *
     * @return A new server
     */
    static Server* create_test_server();

    /**
     * Get server configuration specification
     */
    static mxs::config::Specification* specification();

    /**
     * Configure the server
     *
     * Must be done in the admin thread.
     *
     * @param params New parameters that have been validated
     */
    void configure(const mxs::ConfigParameters& params);

    /**
     * Print server details to a DCB
     *
     * Designed to be called within a debugger session in order
     * to display all active servers within the gateway
     */
    static void dprintServer(DCB*, const Server*);

    /**
     * Diagnostic to print number of DCBs in persistent pool for a server
     *
     * @param       pdcb    DCB to print results to
     * @param       server  SERVER for which DCBs are to be printed
     */
    static void dprintPersistentDCBs(DCB*, const Server*);

    /**
     * Get server parameters
     */
    mxs::ConfigParameters parameters() const
    {
        std::lock_guard<std::mutex> guard(m_settings.lock);
        return m_settings.all_parameters;
    }

    /**
     * @brief Serialize a server to a file
     *
     * This converts @c server into an INI format file. This allows created servers
     * to be persisted to disk. This will replace any existing files with the same
     * name.
     *
     * @return False if the serialization of the server fails, true if it was successful
     */
    bool serialize() const;

    /**
     * Update server-specific monitor username. Does not affect existing monitor connections,
     * only new connections will use the updated username.
     *
     * @param username New username. Must not be too long.
     * @return True, if value was updated
     */
    bool set_monitor_user(const std::string& user);

    /**
     * Update server-specific monitor password. Does not affect existing monitor connections,
     * only new connections will use the updated password.
     *
     * @param password New password. Must not be too long.
     * @return True, if value was updated
     */
    bool set_monitor_password(const std::string& password);

    std::string monitor_user() const;
    std::string monitor_password() const;

    /**
     * @brief Set the disk space threshold of the server
     *
     * @param server                The server.
     * @param disk_space_threshold  The disk space threshold as specified in the config file.
     *
     * @return True, if the provided string is valid and the threshold could be set.
     */
    bool set_disk_space_threshold(const std::string& disk_space_threshold);

    /**
     * Convert server to json. This does not add relations to other objects and should only be called from
     * ServerManager::server_to_json_data_relations().
     *
     * @param host Hostname of this server
     * @return Server as json
     */
    json_t* to_json_data(const char* host) const;

    /**
     * Convert a status string to a status bit. Only converts one status element.
     *
     * @param str   String representation
     * @return bit value or 0 on error
     */
    static uint64_t status_from_string(const char* str);

    json_t* json_attributes() const;

    std::unique_ptr<mxs::Endpoint> get_connection(mxs::Component* upstream, MXS_SESSION* session) override;

    const std::vector<mxs::Target*>& get_children() const override
    {
        static std::vector<mxs::Target*> no_children;
        return no_children;
    }

    uint64_t capabilities() const override
    {
        return 0;
    }

    bool active() const override
    {
        return m_active;
    }

    void deactivate() override
    {
        m_active = false;
    }

    int64_t replication_lag() const override
    {
        return m_rpl_lag;
    }

    void set_replication_lag(int64_t lag) override
    {
        m_rpl_lag = lag;
    }

    int64_t ping() const override
    {
        return m_ping;
    }

    void set_ping(int64_t ping) override
    {
        m_ping = ping;
    }

    bool set_address(const std::string& address) override;

    void set_port(int new_port) override;

    void set_extra_port(int new_port) override;

    uint64_t status() const override;
    void     set_status(uint64_t bit) override;
    void     clear_status(uint64_t bit) override;
    void     assign_status(uint64_t status) override;

    const mxs::SSLProvider& ssl() const override;
    mxs::SSLProvider&       ssl() override;

    void        set_variables(std::unordered_map<std::string, std::string>&& variables) override;
    std::string get_variable(const std::string& key) const override;

    uint64_t gtid_pos(uint32_t domain) const override;
    void     set_gtid_list(const std::vector<std::pair<uint32_t, uint64_t>>& positions) override;
    void     clear_gtid_list() override;

    uint8_t    charset() const override;
    void       set_charset(uint8_t charset) override;
    bool       proxy_protocol() const override;
    void       set_proxy_protocol(bool proxy_protocol) override;
    PoolStats& pool_stats();
    bool       is_mxs_service() const override;

private:
    bool create_server_config(const char* filename) const;

    struct Settings
    {
        mutable std::mutex lock;    /**< Protects array-like settings from concurrent access */

        /** All config settings in text form. This is only read and written from the admin thread
         *  so no need for locking. */
        mxs::ConfigParameters all_parameters;

        std::string protocol;       /**< Backend protocol module name. Does not change so needs no locking. */

        char    address[MAX_ADDRESS_LEN + 1] = {'\0'};  /**< Server hostname/IP-address */
        int64_t port = -1;                              /**< Server port */
        int64_t extra_port = -1;                        /**< Alternative monitor port if normal port fails */

        char monuser[MAX_MONUSER_LEN + 1] = {'\0'}; /**< Monitor username, overrides monitor setting */
        char monpw[MAX_MONPW_LEN + 1] = {'\0'};     /**< Monitor password, overrides monitor setting */

        long                 persistpoolmax = 0;/**< Maximum size of persistent connections pool */
        std::chrono::seconds persistmaxtime {0};/**< Maximum number of seconds connection can live */

        int64_t rank;   /*< The ranking of this server, used to prioritize certain servers over others */

        int64_t priority;   /*< The priority of this server, Currently only used by galeramon to pick which
                             * server is the master. */

        bool proxy_protocol = false;    /**< Send proxy-protocol header to backends when connecting
                                         * routing sessions. */

        /** Disk space thresholds. Can be queried from modules at any time so access must be protected
         *  by mutex. */
        DiskSpaceLimits disk_space_limits;
    };

    /**
     * Stores server version info. Encodes/decodes to/from the version number received from the server.
     * Also stores the version string and parses information from it. */
    class VersionInfo
    {
    public:

        /**
         * Reads in version data. Deduces server type from version string.
         *
         * @param version_num Version number from server
         * @param version_string Version string from server
         */
        void set(uint64_t version_num, const std::string& version_string);

        Version     version_num() const;
        Type        type() const;
        std::string version_string() const;

    private:
        mutable std::mutex m_lock;      /**< Protects against concurrent writing */

        Version m_version_num;                              /**< Numeric version */
        Type    m_type = Type::MARIADB;                     /**< Server type */
        char    m_version_str[MAX_VERSION_LEN + 1] = {'\0'};/**< Server version string */
    };

    const std::string m_name;       /**< Server config name */
    Settings          m_settings;   /**< Server settings */
    VersionInfo       m_info;       /**< Server version and type information */
    uint64_t          m_status {0};
    bool              m_active {true};
    int64_t           m_rpl_lag {mxs::Target::RLAG_UNDEFINED};  /**< Replication lag in seconds */
    int64_t           m_ping {mxs::Target::PING_UNDEFINED};     /**< Ping in microseconds */
    mxs::SSLProvider  m_ssl_provider;
    PoolStats         m_pool_stats;

    // Character set. Read from backend and sent to client. As no character set has the numeric value of 0, it
    // can be used to detect servers we haven't connected to.
    uint8_t m_charset = 0;

    // Server side global variables
    std::unordered_map<std::string, std::string> m_variables;
    // Lock that protects m_variables
    mutable std::mutex m_var_lock;

    struct GTID
    {
        std::atomic<int64_t>  domain{-1};
        std::atomic<uint64_t> sequence{0};
    };

    mxs::WorkerGlobal<std::unordered_map<uint32_t, uint64_t>> m_gtids;

    /**
     * @brief Clean up any stale persistent connections
     *
     * This function purges any stale persistent connections from @c server.
     *
     * @param server Server to clean up
     */
    void cleanup_persistent_connections() const;
};

// A connection to a server
class ServerEndpoint final : public mxs::Endpoint
{
public:
    ServerEndpoint(mxs::Component* up, MXS_SESSION* session, Server* server);
    ~ServerEndpoint() override;

    mxs::Target* target() const override;

    bool connect() override;

    void close() override;

    bool is_open() const override;

    int32_t routeQuery(GWBUF* buffer) override;

    int32_t clientReply(GWBUF* buffer, mxs::ReplyRoute& down, const mxs::Reply& reply) override;

    bool handleError(mxs::ErrorType type, GWBUF* error, mxs::Endpoint* down,
                     const mxs::Reply& reply) override;

private:
    DCB*            m_dcb {nullptr};
    mxs::Component* m_up;
    MXS_SESSION*    m_session;
    Server*         m_server;
};

/**
 * Returns parameter definitions shared by all servers.
 *
 * @return Common server parameters.
 */
const MXS_MODULE_PARAM* common_server_params();
