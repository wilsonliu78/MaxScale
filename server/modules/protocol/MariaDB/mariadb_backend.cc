/*
 * Copyright (c) 2016 MariaDB Corporation Ab
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

#include <maxscale/protocol/mariadb/backend_connection.hh>

#include <mysql.h>
#include <mysqld_error.h>
#include <maxbase/alloc.h>
#include <maxbase/format.hh>
#include <maxsql/mariadb.hh>
#include <maxscale/clock.h>
#include <maxscale/mainworker.hh>
#include <maxscale/modinfo.hh>
#include <maxscale/modutil.hh>
#include <maxscale/poll.hh>
#include <maxscale/router.hh>
#include <maxscale/server.hh>
#include <maxscale/utils.h>
#include <maxscale/protocol/mariadb/authenticator.hh>
#include <maxscale/protocol/mariadb/mysql.hh>
#include <maxscale/protocol/mariadb/client_connection.hh>

// For setting server status through monitor
#include "../../../core/internal/monitormanager.hh"

using mxs::ReplyState;
using std::string;

static bool get_ip_string_and_port(struct sockaddr_storage* sa,
                                   char* ip,
                                   int iplen,
                                   in_port_t* port_out);

namespace
{
using Iter = MariaDBBackendConnection::Iter;

void skip_encoded_int(Iter& it)
{
    switch (*it)
    {
    case 0xfc:
        it.advance(3);
        break;

    case 0xfd:
        it.advance(4);
        break;

    case 0xfe:
        it.advance(9);
        break;

    default:
        ++it;
        break;
    }
}

uint64_t get_encoded_int(Iter& it)
{
    uint64_t len = *it++;

    switch (len)
    {
    case 0xfc:
        len = *it++;
        len |= ((uint64_t)*it++) << 8;
        break;

    case 0xfd:
        len = *it++;
        len |= ((uint64_t)*it++) << 8;
        len |= ((uint64_t)*it++) << 16;
        break;

    case 0xfe:
        len = *it++;
        len |= ((uint64_t)*it++) << 8;
        len |= ((uint64_t)*it++) << 16;
        len |= ((uint64_t)*it++) << 24;
        len |= ((uint64_t)*it++) << 32;
        len |= ((uint64_t)*it++) << 40;
        len |= ((uint64_t)*it++) << 48;
        len |= ((uint64_t)*it++) << 56;
        break;

    default:
        break;
    }

    return len;
}

std::string get_encoded_str(Iter& it)
{
    uint64_t len = get_encoded_int(it);
    auto start = it;
    it.advance(len);
    return std::string(start, it);
}

void skip_encoded_str(Iter& it)
{
    auto len = get_encoded_int(it);
    it.advance(len);
}

bool is_last_eof(Iter it)
{
    std::advance(it, 3);    // Skip the command byte and warning count
    uint16_t status = *it++;
    status |= (*it++) << 8;
    return (status & SERVER_MORE_RESULTS_EXIST) == 0;
}
}

/**
 * Construct a detached backend connection. Session and authenticator attached separately.
 */
MariaDBBackendConnection::MariaDBBackendConnection(SERVER& server)
    : m_server(server)
    , m_auth_data(server.name())
{
}

/*******************************************************************************
 *******************************************************************************
 *
 * API Entry Point - Connect
 *
 * This is the first entry point that will be called in the life of a backend
 * (database) connection. It creates a protocol data structure and attempts
 * to open a non-blocking socket to the database. If it succeeds, the
 * protocol_auth_state will become MYSQL_CONNECTED.
 *
 *******************************************************************************
 ******************************************************************************/

std::unique_ptr<MariaDBBackendConnection>
MariaDBBackendConnection::create(MXS_SESSION* session, mxs::Component* component, SERVER& server)
{
    std::unique_ptr<MariaDBBackendConnection> backend_conn(new MariaDBBackendConnection(server));
    backend_conn->assign_session(session, component);
    return backend_conn;
}

bool MariaDBBackendConnection::init_connection()
{
    if (m_server.proxy_protocol)
    {
        // TODO: The following function needs a return value.
        send_proxy_protocol_header();
    }
    return true;
}

void MariaDBBackendConnection::finish_connection()
{
    mxb_assert(m_dcb->handler());

    if (m_state == State::HANDSHAKING)
    {
        memset(m_auth_data.scramble, 0, sizeof(m_auth_data.scramble));
        m_dcb->writeq_append(gw_generate_auth_response(false, false, 0));
    }

    /** Send COM_QUIT to the backend being closed */
    m_dcb->writeq_append(mysql_create_com_quit(nullptr, 0));
}

bool MariaDBBackendConnection::reuse_connection(BackendDCB* dcb, mxs::Component* upstream)
{
    bool rv = false;
    mxb_assert(dcb->session() && !dcb->readq() && !dcb->writeq());
    mxb_assert(m_ignore_replies >= 0);

    if (dcb->state() != DCB::State::POLLING || m_state != State::ROUTING || !m_delayed_packets.empty())
    {
        MXS_INFO("DCB and protocol state do not qualify for pooling: %s, %s, %s",
                 mxs::to_string(dcb->state()), to_string(m_state).c_str(),
                 m_delayed_packets.empty() ? "no packets" : "stored packets");
    }
    else
    {
        MXS_SESSION* orig_session = m_session;
        mxs::Component* orig_upstream = m_upstream;

        assign_session(dcb->session(), upstream);
        m_dcb = dcb;
        m_ignore_replies = 0;

        /**
         * This is a DCB that was just taken out of the persistent connection pool.
         * We need to sent a COM_CHANGE_USER query to the backend to reset the
         * session state.
         */
        if (m_stored_query)
        {
            /** It is possible that the client DCB is closed before the COM_CHANGE_USER
             * response is received. */
            gwbuf_free(m_stored_query);
            m_stored_query = nullptr;
        }

        GWBUF* buf = create_change_user_packet();
        if (dcb->writeq_append(buf))
        {
            MXS_INFO("Sent COM_CHANGE_USER");
            m_ignore_replies++;
            rv = true;
        }

        if (!rv)
        {
            // Restore situation
            assign_session(orig_session, orig_upstream);
        }
    }

    return rv;
}

/**
 * @brief Check if the response contain an error
 *
 * @param buffer Buffer with a complete response
 * @return True if the reponse contains an MySQL error packet
 */
bool is_error_response(GWBUF* buffer)
{
    uint8_t cmd;
    return gwbuf_copy_data(buffer, MYSQL_HEADER_LEN, 1, &cmd) && cmd == MYSQL_REPLY_ERR;
}

/**
 * @brief Log handshake failure
 *
 * @param dcb Backend DCB where authentication failed
 * @param buffer Buffer containing the response from the backend
 */
void MariaDBBackendConnection::handle_error_response(DCB* plain_dcb, GWBUF* buffer)
{
    mxb_assert(plain_dcb->role() == DCB::Role::BACKEND);
    BackendDCB* dcb = static_cast<BackendDCB*>(plain_dcb);
    uint16_t errcode = mxs_mysql_get_mysql_errno(buffer);

    if (m_session->service->config()->log_auth_warnings)
    {
        MXS_ERROR("Invalid authentication message from backend '%s'. Error code: %d, "
                  "Msg : %s", dcb->server()->name(), errcode, mxs::extract_error(buffer).c_str());
    }

    /** If the error is ER_HOST_IS_BLOCKED put the server into maintenance mode.
     * This will prevent repeated authentication failures. */
    if (errcode == ER_HOST_IS_BLOCKED)
    {
        auto main_worker = mxs::MainWorker::get();
        auto server = dcb->server();
        main_worker->execute([server]() {
                                 MonitorManager::set_server_status(server, SERVER_MAINT);
                             }, mxb::Worker::EXECUTE_AUTO);

        MXS_ERROR("Server %s has been put into maintenance mode due to the server blocking connections "
                  "from MaxScale. Run 'mysqladmin -h %s -P %d flush-hosts' on this server before taking "
                  "this server out of maintenance mode. To avoid this problem in the future, set "
                  "'max_connect_errors' to a larger value in the backend server.",
                  server->name(), server->address(), server->port());
    }
}

/**
 * @brief Prepare protocol for a write
 *
 * This prepares both the buffer and the protocol itself for writing a query
 * to the backend.
 *
 * @param buffer Buffer that will be written
 */
void MariaDBBackendConnection::prepare_for_write(GWBUF* buffer)
{
    if (!gwbuf_is_ignorable(buffer))
    {
        track_query(buffer);
    }

    if (gwbuf_should_collect_result(buffer))
    {
        m_collect_result = true;
    }
    m_track_state = gwbuf_should_track_state(buffer);
}

void MariaDBBackendConnection::ready_for_reading(DCB* event_dcb)
{
    mxb_assert(m_dcb == event_dcb);     // The protocol should only handle its own events.

    bool state_machine_continue = true;
    while (state_machine_continue)
    {
        switch (m_state)
        {
        case State::HANDSHAKING:
            {
                auto hs_res = handshake();
                switch (hs_res)
                {
                case StateMachineRes::IN_PROGRESS:
                    state_machine_continue = false;
                    break;

                case StateMachineRes::DONE:
                    m_state = State::AUTHENTICATING;
                    break;

                case StateMachineRes::ERROR:
                    m_state = State::FAILED;
                    break;
                }
            }
            break;

        case State::AUTHENTICATING:
            {
                auto auth_res = authenticate();
                switch (auth_res)
                {
                case StateMachineRes::IN_PROGRESS:
                    state_machine_continue = false;
                    break;

                case StateMachineRes::DONE:
                    m_state = State::CONNECTION_INIT;
                    break;

                case StateMachineRes::ERROR:
                    m_state = State::FAILED;
                    break;
                }
            }
            break;

        case State::CONNECTION_INIT:
            {
                auto init_res = send_connection_init_queries();
                switch (init_res)
                {
                case StateMachineRes::IN_PROGRESS:
                    state_machine_continue = false;
                    break;

                case StateMachineRes::DONE:
                    m_state = State::SEND_DELAYQ;
                    break;

                case StateMachineRes::ERROR:
                    m_state = State::FAILED;
                    break;
                }
            }
            break;

        case State::SEND_DELAYQ:
            send_delayed_packets();
            m_state = State::ROUTING;
            break;

        case State::ROUTING:
            normal_read();
            // Normal read always consumes all data.
            state_machine_continue = false;
            break;

        case State::FAILED:
            state_machine_continue = false;
            break;
        }
    }
}

void MariaDBBackendConnection::do_handle_error(DCB* dcb, const std::string& errmsg, mxs::ErrorType type)
{
    std::ostringstream ss(errmsg);

    if (int err = gw_getsockerrno(dcb->fd()))
    {
        ss << " (" << err << ", " << mxs_strerror(err) << ")";
    }
    else if (dcb->is_fake_event())
    {
        // Fake events should not have TCP socket errors
        ss << " (Generated event)";
    }

    mxb_assert(!dcb->hanged_up());
    GWBUF* errbuf = mysql_create_custom_error(1, 0, 2003, ss.str().c_str());

    if (!m_upstream->handleError(type, errbuf, nullptr, m_reply))
    {
        mxb_assert(m_session->state() == MXS_SESSION::State::STOPPING);
    }

    gwbuf_free(errbuf);
}

/**
 * @brief Check if a reply can be routed to the client
 *
 * @param Backend DCB
 * @return True if session is ready for reply routing
 */
bool MariaDBBackendConnection::session_ok_to_route(DCB* dcb)
{
    bool rval = false;
    auto session = dcb->session();
    if (session->state() == MXS_SESSION::State::STARTED)
    {
        ClientDCB* client_dcb = session->client_connection()->dcb();
        if (client_dcb && client_dcb->state() == DCB::State::POLLING)
        {
            auto client_protocol = static_cast<MariaDBClientConnection*>(client_dcb->protocol());
            if (client_protocol)
            {
                if (client_protocol->in_routing_state())
                {
                    rval = true;
                }
            }
            else if (client_dcb->role() == DCB::Role::INTERNAL)
            {
                rval = true;
            }
        }
    }


    return rval;
}

bool MariaDBBackendConnection::expecting_text_result()
{
    /**
     * The addition of COM_STMT_FETCH to the list of commands that produce
     * result sets is slightly wrong. The command can generate complete
     * result sets but it can also generate incomplete ones if cursors
     * are used. The use of cursors most likely needs to be detected on
     * an upper level and the use of this function avoided in those cases.
     *
     * TODO: Revisit this to make sure it's needed.
     */

    uint8_t cmd = m_reply.command();
    return cmd == MXS_COM_QUERY || cmd == MXS_COM_STMT_EXECUTE || cmd == MXS_COM_STMT_FETCH;
}

bool MariaDBBackendConnection::expecting_ps_response()
{
    return m_reply.command() == MXS_COM_STMT_PREPARE;
}

bool MariaDBBackendConnection::complete_ps_response(GWBUF* buffer)
{
    MXS_PS_RESPONSE resp;
    bool rval = false;

    if (mxs_mysql_extract_ps_response(buffer, &resp))
    {
        int expected_packets = 1;

        if (resp.columns > 0)
        {
            // Column definition packets plus one for the EOF
            expected_packets += resp.columns + 1;
        }

        if (resp.parameters > 0)
        {
            // Parameter definition packets plus one for the EOF
            expected_packets += resp.parameters + 1;
        }

        int n_packets = modutil_count_packets(buffer);

        MXS_DEBUG("Expecting %u packets, have %u", n_packets, expected_packets);

        rval = n_packets == expected_packets;
    }

    return rval;
}

/**
 * Helpers for checking OK and ERR packets specific to COM_CHANGE_USER
 */
static inline bool not_ok_packet(const GWBUF* buffer)
{
    const uint8_t* data = GWBUF_DATA(buffer);

    return data[4] != MYSQL_REPLY_OK
           ||   // Should be more than 7 bytes of payload
           gw_mysql_get_byte3(data) < MYSQL_OK_PACKET_MIN_LEN - MYSQL_HEADER_LEN
           ||   // Should have no affected rows
           data[5] != 0
           ||   // Should not generate an insert ID
           data[6] != 0;
}

static inline bool not_err_packet(const GWBUF* buffer)
{
    return GWBUF_DATA(buffer)[4] != MYSQL_REPLY_ERR;
}

static inline bool auth_change_requested(GWBUF* buf)
{
    return mxs_mysql_get_command(buf) == MYSQL_REPLY_AUTHSWITCHREQUEST
           && gwbuf_length(buf) > MYSQL_EOF_PACKET_LEN;
}

bool MariaDBBackendConnection::handle_auth_change_response(GWBUF* reply, DCB* dcb)
{
    bool rval = false;

    if (strcmp((char*)GWBUF_DATA(reply) + 5, DEFAULT_MYSQL_AUTH_PLUGIN) == 0)
    {
        /**
         * The server requested a change of authentication methods.
         * If we're changing the authentication method to the same one we
         * are using now, it means that the server is simply generating
         * a new scramble for the re-authentication process.
         */
        rval = send_mysql_native_password_response(dcb, reply);
    }

    return rval;
}

/**
 * With authentication completed, read new data and write to backend
 *
 * @return 0 is fail, 1 is success
 */
int MariaDBBackendConnection::normal_read()
{
    auto dcb = m_dcb;
    GWBUF* read_buffer = NULL;
    MXS_SESSION* session = dcb->session();
    int nbytes_read = 0;
    int return_code = 0;

    /* read available backend data */
    return_code = dcb->read(&read_buffer, 0);

    if (return_code < 0)
    {
        do_handle_error(dcb, "Read from backend failed");
        return 0;
    }

    if (read_buffer)
    {
        nbytes_read = gwbuf_length(read_buffer);
    }

    if (nbytes_read == 0)
    {
        mxb_assert(read_buffer == NULL);
        return return_code;
    }
    else
    {
        mxb_assert(read_buffer != NULL);
    }

    /** Ask what type of output the router/filter chain expects */
    uint64_t capabilities = service_get_capabilities(session->service);
    bool result_collected = false;
    auto proto = this;

    if (rcap_type_required(capabilities, RCAP_TYPE_PACKET_OUTPUT)
        || rcap_type_required(capabilities, RCAP_TYPE_STMT_OUTPUT)
        || rcap_type_required(capabilities, RCAP_TYPE_CONTIGUOUS_OUTPUT)
        || proto->m_collect_result
        || proto->m_ignore_replies != 0)
    {
        GWBUF* tmp;

        if (rcap_type_required(capabilities, RCAP_TYPE_REQUEST_TRACKING)
            && !rcap_type_required(capabilities, RCAP_TYPE_STMT_OUTPUT)
            && !proto->m_ignore_replies)
        {
            tmp = proto->track_response(&read_buffer);
        }
        else
        {
            tmp = modutil_get_complete_packets(&read_buffer);
        }

        // Store any partial packets in the DCB's read buffer
        if (read_buffer)
        {
            dcb->readq_set(read_buffer);

            if (m_reply.is_complete())
            {
                // There must be more than one response in the buffer which we need to process once we've
                // routed this response.
                dcb->trigger_read_event();
            }
        }

        if (tmp == NULL)
        {
            /** No complete packets */
            return 0;
        }

        read_buffer = tmp;

        if (rcap_type_required(capabilities, RCAP_TYPE_CONTIGUOUS_OUTPUT)
            || proto->m_collect_result
            || proto->m_ignore_replies != 0)
        {
            if ((tmp = gwbuf_make_contiguous(read_buffer)))
            {
                read_buffer = tmp;
            }
            else
            {
                /** Failed to make the buffer contiguous */
                gwbuf_free(read_buffer);
                dcb->trigger_hangup_event();
                return 0;
            }

            if (rcap_type_required(capabilities, RCAP_TYPE_RESULTSET_OUTPUT) || m_collect_result)
            {
                if (rcap_type_required(capabilities, RCAP_TYPE_REQUEST_TRACKING)
                    && !rcap_type_required(capabilities, RCAP_TYPE_STMT_OUTPUT))
                {
                    m_collectq.append(read_buffer);

                    if (!m_reply.is_complete())
                    {
                        return 0;
                    }

                    read_buffer = m_collectq.release();
                    proto->m_collect_result = false;
                    result_collected = true;
                }
                else if (expecting_text_result())
                {
                    if (mxs_mysql_is_result_set(read_buffer))
                    {
                        bool more = false;
                        int eof_cnt = modutil_count_signal_packets(read_buffer, 0, &more, NULL);
                        if (more || eof_cnt % 2 != 0)
                        {
                            dcb->readq_prepend(read_buffer);
                            return 0;
                        }
                    }

                    // Collected the complete result
                    proto->m_collect_result = false;
                    result_collected = true;
                }
                else if (expecting_ps_response()
                         && mxs_mysql_is_prep_stmt_ok(read_buffer)
                         && !complete_ps_response(read_buffer))
                {
                    dcb->readq_prepend(read_buffer);
                    return 0;
                }
                else
                {
                    // Collected the complete result
                    proto->m_collect_result = false;
                    result_collected = true;
                }
            }
        }
    }

    if (m_changing_user)
    {
        if (auth_change_requested(read_buffer)
            && handle_auth_change_response(read_buffer, dcb))
        {
            gwbuf_free(read_buffer);
            return 0;
        }
        else
        {
            /**
             * The client protocol always requests an authentication method
             * switch to the same plugin to be compatible with most connectors.
             *
             * To prevent packet sequence number mismatch, always return a sequence
             * of 3 for the final response to a COM_CHANGE_USER.
             */
            GWBUF_DATA(read_buffer)[3] = 0x3;
            m_changing_user = false;
        }
    }

    if (proto->m_ignore_replies > 0)
    {
        /** The reply to a COM_CHANGE_USER is in packet */
        GWBUF* query = proto->m_stored_query;
        proto->m_stored_query = NULL;
        proto->m_ignore_replies--;
        mxb_assert(proto->m_ignore_replies >= 0);
        GWBUF* reply = modutil_get_next_MySQL_packet(&read_buffer);

        while (read_buffer)
        {
            /** Skip to the last packet if we get more than one */
            gwbuf_free(reply);
            reply = modutil_get_next_MySQL_packet(&read_buffer);
        }

        mxb_assert(reply);
        mxb_assert(!read_buffer);
        uint8_t result = MYSQL_GET_COMMAND(GWBUF_DATA(reply));
        int rval = 0;

        if (result == MYSQL_REPLY_OK)
        {
            MXS_INFO("Response to COM_CHANGE_USER is OK, writing stored query");
            rval = query ? dcb->protocol_write(query) : 1;
        }
        else if (auth_change_requested(reply))
        {
            if (handle_auth_change_response(reply, dcb))
            {
                /** Store the query until we know the result of the authentication
                 * method switch. */
                proto->m_stored_query = query;
                proto->m_ignore_replies++;

                gwbuf_free(reply);
                return rval;
            }
            else
            {
                /** The server requested a change to something other than
                 * the default auth plugin */
                gwbuf_free(query);
                dcb->trigger_hangup_event();

                // TODO: Use the authenticators to handle COM_CHANGE_USER responses
                MXS_ERROR("Received AuthSwitchRequest to '%s' when '%s' was expected",
                          (char*)GWBUF_DATA(reply) + 5,
                          DEFAULT_MYSQL_AUTH_PLUGIN);
            }
        }
        else
        {
            /**
             * The ignorable command failed when we had a queued query from the
             * client. Generate a fake hangup event to close the DCB and send
             * an error to the client.
             */
            if (result == MYSQL_REPLY_ERR)
            {
                /** The COM_CHANGE USER failed, generate a fake hangup event to
                 * close the DCB and send an error to the client. */
                handle_error_response(dcb, reply);
            }
            else
            {
                /** This should never happen */
                MXS_ERROR("Unknown response to COM_CHANGE_USER (0x%02hhx), "
                          "closing connection",
                          result);
            }

            gwbuf_free(query);
            dcb->trigger_hangup_event();
        }

        gwbuf_free(reply);
        return rval;
    }

    do
    {
        GWBUF* stmt = NULL;

        if (result_collected)
        {
            /** The result set or PS response was collected, we know it's complete */
            stmt = read_buffer;
            read_buffer = NULL;
            gwbuf_set_type(stmt, GWBUF_TYPE_RESULT);

            // TODO: Remove this and use RCAP_TYPE_REQUEST_TRACKING in maxrows
            if (rcap_type_required(capabilities, RCAP_TYPE_STMT_OUTPUT)
                && rcap_type_required(capabilities, RCAP_TYPE_REQUEST_TRACKING))
            {
                GWBUF* tmp = proto->track_response(&stmt);
                mxb_assert(stmt == nullptr);
                stmt = tmp;
            }
        }
        else if (rcap_type_required(capabilities, RCAP_TYPE_STMT_OUTPUT)
                 && !rcap_type_required(capabilities, RCAP_TYPE_RESULTSET_OUTPUT))
        {
            // TODO: Get rid of RCAP_TYPE_STMT_OUTPUT and rely on RCAP_TYPE_REQUEST_TRACKING to provide all
            // the required information.
            stmt = modutil_get_next_MySQL_packet(&read_buffer);
            mxb_assert_message(stmt, "There should be only complete packets in read_buffer");

            if (!gwbuf_is_contiguous(stmt))
            {
                // Make sure the buffer is contiguous
                stmt = gwbuf_make_contiguous(stmt);
            }

            // TODO: Remove this and use RCAP_TYPE_REQUEST_TRACKING in maxrows
            if (rcap_type_required(capabilities, RCAP_TYPE_REQUEST_TRACKING))
            {
                GWBUF* tmp = proto->track_response(&stmt);
                mxb_assert(stmt == nullptr);
                stmt = tmp;
            }
        }
        else
        {
            stmt = read_buffer;
            read_buffer = NULL;
        }

        if (session_ok_to_route(dcb))
        {
            if (result_collected)
            {
                // Mark that this is a buffer containing a collected result
                gwbuf_set_type(stmt, GWBUF_TYPE_RESULT);
            }

            thread_local mxs::ReplyRoute route;
            route.clear();
            return_code = proto->m_upstream->clientReply(stmt, route, m_reply);
        }
        else    /*< session is closing; replying to client isn't possible */
        {
            gwbuf_free(stmt);
        }
    }
    while (read_buffer);

    return return_code;
}

void MariaDBBackendConnection::write_ready(DCB* event_dcb)
{
    mxb_assert(m_dcb == event_dcb);
    auto dcb = m_dcb;
    if (dcb->state() != DCB::State::POLLING)
    {
        /** Don't write to backend if backend_dcb is not in poll set anymore */
        uint8_t* data = NULL;
        bool com_quit = false;

        if (dcb->writeq())
        {
            data = (uint8_t*) GWBUF_DATA(dcb->writeq());
            com_quit = MYSQL_IS_COM_QUIT(data);
        }

        if (data)
        {
            if (!com_quit)
            {
                MXS_ERROR("Attempt to write buffered data to backend failed due internal inconsistent "
                          "state: %s", mxs::to_string(dcb->state()));
            }
        }
        else
        {
            MXS_DEBUG("Dcb %p in state %s but there's nothing to write either.",
                      dcb, mxs::to_string(dcb->state()));
        }
    }
    else
    {
        dcb->writeq_drain();
    }

    return;
}

int MariaDBBackendConnection::handle_persistent_connection(GWBUF* queue)
{
    int rc = 0;
    mxb_assert(m_ignore_replies > 0);

    if (MYSQL_IS_COM_QUIT((uint8_t*)GWBUF_DATA(queue)))
    {
        /** The COM_CHANGE_USER was already sent but the session is already
         * closing. */
        MXS_INFO("COM_QUIT received while COM_CHANGE_USER is in progress, closing pooled connection");
        gwbuf_free(queue);
        m_dcb->trigger_hangup_event();
    }
    else
    {
        /**
         * We're still waiting on the reply to the COM_CHANGE_USER, append the
         * buffer to the stored query. This is possible if the client sends
         * BLOB data on the first command or is sending multiple COM_QUERY
         * packets at one time.
         */
        MXS_INFO("COM_CHANGE_USER in progress, appending query to queue");
        m_stored_query = gwbuf_append(m_stored_query, queue);
        rc = 1;
    }

    return rc;
}

/*
 * Write function for backend DCB. Store command to protocol.
 *
 * @param queue Queue of buffers to write
 * @return      0 on failure, 1 on success
 */
int32_t MariaDBBackendConnection::write(GWBUF* queue)
{
    if (m_ignore_replies > 0)
    {
        return handle_persistent_connection(queue);
    }

    int rc = 0;
    switch (m_state)
    {
    case State::FAILED:
        if (m_session->state() != MXS_SESSION::State::STOPPING)
        {
            MXS_ERROR("Unable to write to backend '%s' because connection has failed. Server in state %s.",
                      m_server.name(), m_server.status_string().c_str());
        }

        gwbuf_free(queue);
        rc = 0;
        break;

    case State::ROUTING:
        {
            auto cmd = static_cast<mxs_mysql_cmd_t>(mxs_mysql_get_command(queue));

            MXS_DEBUG("write to dcb %p fd %d protocol state %s.",
                      m_dcb, m_dcb->fd(), to_string(m_state).c_str());

            queue = gwbuf_make_contiguous(queue);
            prepare_for_write(queue);

            if (m_reply.command() == MXS_COM_CHANGE_USER)
            {
                return change_user(queue);
            }
            else if (cmd == MXS_COM_QUIT && m_server.persistent_conns_enabled())
            {
                /** We need to keep the pooled connections alive so we just ignore the COM_QUIT packet */
                gwbuf_free(queue);
                rc = 1;
            }
            else
            {
                if (gwbuf_is_ignorable(queue))
                {
                    /** The response to this command should be ignored */
                    m_ignore_replies++;
                    mxb_assert(m_ignore_replies > 0);
                }

                /** Write to backend */
                rc = m_dcb->writeq_append(queue);
            }
        }
        break;

    default:
        {
            MXS_DEBUG("delayed write to dcb %p fd %d protocol state %s.",
                      m_dcb, m_dcb->fd(), to_string(m_state).c_str());

            /** Store data until authentication is complete */
            m_delayed_packets.append(queue);
            rc = 1;
        }
        break;
    }
    return rc;
}

/**
 * Error event handler.
 * Create error message, pass it to router's error handler and if error
 * handler fails in providing enough backend servers, mark session being
 * closed and call DCB close function which triggers closing router session
 * and related backends (if any exists.
 */
void MariaDBBackendConnection::error(DCB* event_dcb)
{
    mxb_assert(m_dcb == event_dcb);

    const auto dcb_state = m_dcb->state();
    if (dcb_state != DCB::State::POLLING || m_session->state() != MXS_SESSION::State::STARTED)
    {
        int error = 0;
        int len = sizeof(error);

        if (getsockopt(m_dcb->fd(), SOL_SOCKET, SO_ERROR, &error, (socklen_t*) &len) == 0 && error != 0)
        {
            const char* error_strz = mxs_strerror(errno);
            if (dcb_state != DCB::State::POLLING)
            {
                MXS_ERROR("DCB in state %s got error '%s'.", mxs::to_string(dcb_state), error_strz);
            }
            else
            {
                MXS_ERROR("Error '%s' in session that is not ready for routing.", error_strz);
            }
        }
    }
    else
    {
        do_handle_error(m_dcb, "Lost connection to backend server: network error");
    }
}

/**
 * Error event handler.
 * Create error message, pass it to router's error handler and if error
 * handler fails in providing enough backend servers, mark session being
 * closed and call DCB close function which triggers closing router session
 * and related backends (if any exists.
 *
 * @param event_dcb The current Backend DCB
 * @return 1 always
 */
void MariaDBBackendConnection::hangup(DCB* event_dcb)
{
    mxb_assert(m_dcb == event_dcb);
    mxb_assert(!m_dcb->is_closed());
    MXS_SESSION* session = m_dcb->session();
    mxb_assert(session);

    if (session->state() != MXS_SESSION::State::STARTED)
    {
        int error;
        int len = sizeof(error);
        if (getsockopt(m_dcb->fd(), SOL_SOCKET, SO_ERROR, &error, (socklen_t*) &len) == 0)
        {
            if (error != 0 && session->state() != MXS_SESSION::State::STOPPING)
            {
                MXS_ERROR("Hangup in session that is not ready for routing, "
                          "Error reported is '%s'.",
                          mxs_strerror(errno));
            }
        }
    }
    else
    {
        do_handle_error(m_dcb, "Lost connection to backend server: connection closed by peer");
    }
}

/**
 * This routine writes the delayq via dcb_write. The dcb->m_delayq contains data received
 * from the client before mysql backend authentication succeded
 *
 * @return The dcb_write status
 */
bool MariaDBBackendConnection::backend_write_delayqueue(GWBUF* buffer)
{
    bool rval = false;
    const uint8_t* data = GWBUF_DATA(buffer);
    if (MYSQL_IS_CHANGE_USER(data))
    {
        /** Recreate the COM_CHANGE_USER packet with the scramble the backend sent to us */
        rval = change_user(buffer);
    }
    else if (MYSQL_IS_COM_QUIT(data) && m_server.persistent_conns_enabled())
    {
        /** We need to keep the pooled connections alive so we just ignore the COM_QUIT packet */
        gwbuf_free(buffer);
        rval = true;
    }
    else
    {
        rval = m_dcb->writeq_append(buffer);
    }

    if (!rval)
    {
        do_handle_error(m_dcb, "Lost connection to backend server while writing delay queue.");
    }

    return rval;
}

/**
 * This routine handles the COM_CHANGE_USER command.
 *
 * @param queue         The GWBUF containing the COM_CHANGE_USER receveid
 * @return True on success
 */
bool MariaDBBackendConnection::change_user(GWBUF* queue)
{
    gwbuf_free(queue);
    return send_change_user_to_backend();
}

/**
 * Create COM_CHANGE_USER packet and store it to GWBUF.
 *
 * @return GWBUF buffer consisting of COM_CHANGE_USER packet
 * @note the function doesn't fail
 */
GWBUF* MariaDBBackendConnection::create_change_user_packet()
{
    auto make_auth_token = [this] {
            std::vector<uint8_t> rval;
            const string& hex_hash2 = m_auth_data.client_data->user_entry.entry.password;
            if (hex_hash2.empty())
            {
                return rval;    // Empty password -> empty token
            }

            // Need to compute the value of:
            // SHA1(scramble || SHA1(SHA1(password))) ⊕ SHA1(password)

            // SHA1(SHA1(password)) is in the user entry and needs to be converted to binary form.
            if (hex_hash2.length() == 2 * SHA_DIGEST_LENGTH)
            {
                uint8_t hash2[SHA_DIGEST_LENGTH];
                mxs::hex2bin(hex_hash2.c_str(), hex_hash2.length(), hash2);

                // Calculate SHA1(CONCAT(scramble, hash2) */
                uint8_t concat_hash[SHA_DIGEST_LENGTH];
                gw_sha1_2_str(m_auth_data.scramble, MYSQL_SCRAMBLE_LEN, hash2, SHA_DIGEST_LENGTH,
                              concat_hash);

                // SHA1(password) was sent by client and is in binary form.
                auto& hash1 = m_auth_data.client_data->auth_token_phase2;
                if (hash1.size() == SHA_DIGEST_LENGTH)
                {
                    // Compute the XOR */
                    uint8_t new_token[SHA_DIGEST_LENGTH];
                    mxs::bin_bin_xor(concat_hash, hash1.data(), SHA_DIGEST_LENGTH, new_token);
                    rval.assign(new_token, new_token + SHA_DIGEST_LENGTH);
                }
            }
            return rval;
        };

    auto mses = m_auth_data.client_data;
    std::vector<uint8_t> payload;
    payload.reserve(200);   // Enough for most cases.

    auto insert_stringz = [&payload](const std::string& str) {
            auto n = str.length() + 1;
            auto zstr = str.c_str();
            payload.insert(payload.end(), zstr, zstr + n);
        };

    // Command byte COM_CHANGE_USER 0x11 */
    payload.push_back(MXS_COM_CHANGE_USER);

    insert_stringz(mses->user);

    // Calculate the authentication token.
    auto token = make_auth_token();
    payload.push_back(token.size());
    payload.insert(payload.end(), token.begin(), token.end());

    insert_stringz(mses->db);

    uint8_t charset[2];
    mariadb::set_byte2(charset, mses->client_info.m_charset);
    payload.insert(payload.end(), charset, charset + sizeof(charset));

    insert_stringz(mses->plugin);
    auto& attr = mses->connect_attrs;
    payload.insert(payload.end(), attr.begin(), attr.end());

    GWBUF* buffer = gwbuf_alloc(payload.size() + MYSQL_HEADER_LEN);
    auto data = GWBUF_DATA(buffer);
    mariadb::set_byte3(data, payload.size());
    data += 3;
    *data++ = 0;    // Sequence.
    memcpy(data, payload.data(), payload.size());
    // COM_CHANGE_USER is a session command so the result must be collected.
    gwbuf_set_type(buffer, GWBUF_TYPE_COLLECT_RESULT);

    return buffer;
}

/**
 * Write a MySQL CHANGE_USER packet to backend server.
 *
 * @return True on success
 */
bool MariaDBBackendConnection::send_change_user_to_backend()
{
    GWBUF* buffer = create_change_user_packet();
    bool rval = false;
    if (m_dcb->writeq_append(buffer))
    {
        m_changing_user = true;
        rval = true;
    }
    return rval;
}

/* Send proxy protocol header. See
 * http://www.haproxy.org/download/1.8/doc/proxy-protocol.txt
 * for more information. Currently only supports the text version (v1) of
 * the protocol. Binary version may be added when the feature has been confirmed
 * to work.
 */
void MariaDBBackendConnection::send_proxy_protocol_header()
{
    // TODO: Add support for chained proxies. Requires reading the client header.

    const ClientDCB* client_dcb = m_session->client_connection()->dcb();
    const int client_fd = client_dcb->fd();
    const sa_family_t family = client_dcb->ip().ss_family;
    const char* family_str = nullptr;

    sockaddr_storage sa_peer {};
    sockaddr_storage sa_local {};
    socklen_t sa_peer_len = sizeof(sa_peer);
    socklen_t sa_local_len = sizeof(sa_local);

    /* Fill in peer's socket address.  */
    if (getpeername(client_fd, (struct sockaddr*)&sa_peer, &sa_peer_len) == -1)
    {
        MXS_ERROR("'%s' failed on file descriptor '%d'.", "getpeername()", client_fd);
        return;
    }

    /* Fill in this socket's local address. */
    if (getsockname(client_fd, (struct sockaddr*)&sa_local, &sa_local_len) == -1)
    {
        MXS_ERROR("'%s' failed on file descriptor '%d'.", "getsockname()", client_fd);
        return;
    }
    mxb_assert(sa_peer.ss_family == sa_local.ss_family);

    char peer_ip[INET6_ADDRSTRLEN];
    char maxscale_ip[INET6_ADDRSTRLEN];
    in_port_t peer_port;
    in_port_t maxscale_port;

    if (!get_ip_string_and_port(&sa_peer, peer_ip, sizeof(peer_ip), &peer_port)
        || !get_ip_string_and_port(&sa_local, maxscale_ip, sizeof(maxscale_ip), &maxscale_port))
    {
        MXS_ERROR("Could not convert network address to string form.");
        return;
    }

    switch (family)
    {
    case AF_INET:
        family_str = "TCP4";
        break;

    case AF_INET6:
        family_str = "TCP6";
        break;

    default:
        family_str = "UNKNOWN";
        break;
    }

    int rval;
    char proxy_header[108];     // 108 is the worst-case length
    if (family == AF_INET || family == AF_INET6)
    {
        rval = snprintf(proxy_header,
                        sizeof(proxy_header),
                        "PROXY %s %s %s %d %d\r\n",
                        family_str,
                        peer_ip,
                        maxscale_ip,
                        peer_port,
                        maxscale_port);
    }
    else
    {
        rval = snprintf(proxy_header, sizeof(proxy_header), "PROXY %s\r\n", family_str);
    }
    if (rval < 0 || rval >= (int)sizeof(proxy_header))
    {
        MXS_ERROR("Proxy header printing error, produced '%s'.", proxy_header);
        return;
    }

    GWBUF* headerbuf = gwbuf_alloc_and_load(strlen(proxy_header), proxy_header);
    if (headerbuf)
    {
        MXS_INFO("Sending proxy-protocol header '%s' to backend %s.", proxy_header, m_server.name());
        if (!m_dcb->writeq_append(headerbuf))
        {
            gwbuf_free(headerbuf);
        }
    }
    return;
}

/* Read IP and port from socket address structure, return IP as string and port
 * as host byte order integer.
 *
 * @param sa A sockaddr_storage containing either an IPv4 or v6 address
 * @param ip Pointer to output array
 * @param iplen Output array length
 * @param port_out Port number output
 */
static bool get_ip_string_and_port(struct sockaddr_storage* sa,
                                   char* ip,
                                   int iplen,
                                   in_port_t* port_out)
{
    bool success = false;
    in_port_t port;

    switch (sa->ss_family)
    {
    case AF_INET:
        {
            struct sockaddr_in* sock_info = (struct sockaddr_in*)sa;
            struct in_addr* addr = &(sock_info->sin_addr);
            success = (inet_ntop(AF_INET, addr, ip, iplen) != NULL);
            port = ntohs(sock_info->sin_port);
        }
        break;

    case AF_INET6:
        {
            struct sockaddr_in6* sock_info = (struct sockaddr_in6*)sa;
            struct in6_addr* addr = &(sock_info->sin6_addr);
            success = (inet_ntop(AF_INET6, addr, ip, iplen) != NULL);
            port = ntohs(sock_info->sin6_port);
        }
        break;
    }
    if (success)
    {
        *port_out = port;
    }
    return success;
}

bool MariaDBBackendConnection::established()
{
    return m_state == State::ROUTING && (m_ignore_replies == 0) && !m_stored_query;
}

void MariaDBBackendConnection::ping()
{
    if (m_reply.state() == ReplyState::DONE)
    {
        MXS_INFO("Pinging '%s', idle for %ld seconds", m_server.name(), seconds_idle());

        // TODO: Think of a better mechanism for the pings, the ignorable ping mechanism isn't pretty.
        write(modutil_create_ignorable_ping());
    }
}

int64_t MariaDBBackendConnection::seconds_idle() const
{
    return MXS_CLOCK_TO_SEC(mxs_clock() - std::max(m_dcb->last_read(), m_dcb->last_write()));
}

json_t* MariaDBBackendConnection::diagnostics() const
{
    return json_pack("{siss}", "connection_id", m_thread_id, "server", m_server.name());
}

int MariaDBBackendConnection::mysql_send_com_quit(DCB* dcb, int packet_number, GWBUF* bufparam)
{
    mxb_assert(packet_number <= 255);

    int nbytes = 0;
    GWBUF* buf = bufparam ? bufparam : mysql_create_com_quit(NULL, packet_number);
    if (buf)
    {
        nbytes = dcb->protocol_write(buf);
    }
    return nbytes;
}

/**
 * @brief Read a complete packet from a DCB
 *
 * Read a complete packet from a connected DCB. If data was read, @c readbuf
 * will point to the head of the read data. If no data was read, @c readbuf will
 * be set to NULL.
 *
 * @param dcb DCB to read from
 * @param readbuf Pointer to a buffer where the data is stored
 * @return True on success, false if an error occurred while data was being read
 */
bool MariaDBBackendConnection::read_complete_packet(DCB* dcb, GWBUF** readbuf)
{
    bool rval = false;
    GWBUF* localbuf = NULL;

    if (dcb->read(&localbuf, 0) >= 0)
    {
        rval = true;
        GWBUF* packets = modutil_get_complete_packets(&localbuf);

        if (packets)
        {
            /** A complete packet was read */
            *readbuf = packets;
        }

        if (localbuf)
        {
            /** Store any extra data in the DCB's readqueue */

            dcb->readq_append(localbuf);
        }
    }

    return rval;
}

/**
 * @brief Check if a buffer contains a result set
 *
 * @param buffer Buffer to check
 * @return True if the @c buffer contains the start of a result set
 */
bool MariaDBBackendConnection::mxs_mysql_is_result_set(GWBUF* buffer)
{
    bool rval = false;
    uint8_t cmd;

    if (gwbuf_copy_data(buffer, MYSQL_HEADER_LEN, 1, &cmd))
    {
        switch (cmd)
        {

        case MYSQL_REPLY_OK:
        case MYSQL_REPLY_ERR:
        case MYSQL_REPLY_LOCAL_INFILE:
        case MYSQL_REPLY_EOF:
            /** Not a result set */
            break;

        default:
            rval = true;
            break;
        }
    }

    return rval;
}

/**
 * Process a reply from a backend server. This method collects all complete packets and
 * updates the internal response state.
 *
 * @param buffer Pointer to buffer containing the raw response. Any partial packets will be left in this
 *               buffer.
 * @return All complete packets that were in `buffer`
 */
GWBUF* MariaDBBackendConnection::track_response(GWBUF** buffer)
{
    GWBUF* rval = process_packets(buffer);

    if (rval)
    {
        m_reply.add_bytes(gwbuf_length(rval));
    }

    return rval;
}

/**
 * Read the backend server MySQL handshake
 *
 * @return true on success, false on failure
 */
bool MariaDBBackendConnection::read_backend_handshake(mxs::Buffer&& buffer)
{
    bool rval = false;
    uint8_t* payload = GWBUF_DATA(buffer.get()) + 4;

    if (gw_decode_mysql_server_handshake(payload) >= 0)
    {
        rval = true;
    }

    return rval;
}

/**
 * Sends a response for an AuthSwitchRequest to the default auth plugin
 */
int MariaDBBackendConnection::send_mysql_native_password_response(DCB* dcb, GWBUF* reply)
{
    // Calculate the next sequence number
    uint8_t seqno = 0;
    gwbuf_copy_data(reply, 3, 1, &seqno);
    ++seqno;

    // Copy the new scramble. Skip packet header, command byte and null-terminated plugin name.
    const char default_plugin_name[] = DEFAULT_MYSQL_AUTH_PLUGIN;
    gwbuf_copy_data(reply, MYSQL_HEADER_LEN + 1 + sizeof(default_plugin_name),
                    sizeof(m_auth_data.scramble), m_auth_data.scramble);

    const auto& sha1_pw = m_auth_data.client_data->auth_token_phase2;
    const uint8_t* curr_passwd = sha1_pw.empty() ? null_client_sha1 : sha1_pw.data();

    GWBUF* buffer = gwbuf_alloc(MYSQL_HEADER_LEN + GW_MYSQL_SCRAMBLE_SIZE);
    uint8_t* data = GWBUF_DATA(buffer);
    gw_mysql_set_byte3(data, GW_MYSQL_SCRAMBLE_SIZE);
    data[3] = seqno;    // This is the third packet after the COM_CHANGE_USER
    mxs_mysql_calculate_hash(m_auth_data.scramble, curr_passwd, data + MYSQL_HEADER_LEN);

    return dcb->writeq_append(buffer);
}

/**
 * Decode mysql server handshake
 *
 * @param payload The bytes just read from the net
 * @return 0 on success, < 0 on failure
 *
 */
int MariaDBBackendConnection::gw_decode_mysql_server_handshake(uint8_t* payload)
{
    auto conn = this;
    uint8_t* server_version_end = NULL;
    uint16_t mysql_server_capabilities_one = 0;
    uint16_t mysql_server_capabilities_two = 0;
    uint8_t scramble_data_1[GW_SCRAMBLE_LENGTH_323] = "";
    uint8_t scramble_data_2[GW_MYSQL_SCRAMBLE_SIZE - GW_SCRAMBLE_LENGTH_323] = "";
    uint8_t capab_ptr[4] = "";
    uint8_t scramble_len = 0;
    uint8_t mxs_scramble[GW_MYSQL_SCRAMBLE_SIZE] = "";
    int protocol_version = 0;

    protocol_version = payload[0];

    if (protocol_version != GW_MYSQL_PROTOCOL_VERSION)
    {
        return -1;
    }

    payload++;

    // Get server version (string)
    server_version_end = (uint8_t*) gw_strend((char*) payload);

    payload = server_version_end + 1;

    // get ThreadID: 4 bytes
    uint32_t tid = gw_mysql_get_byte4(payload);

    MXS_INFO("Connected to '%s' with thread id %u", m_server.name(), tid);

    /* TODO: Correct value of thread id could be queried later from backend if
     * there is any worry it might be larger than 32bit allows. */
    conn->m_thread_id = tid;

    payload += 4;

    // scramble_part 1
    memcpy(scramble_data_1, payload, GW_SCRAMBLE_LENGTH_323);
    payload += GW_SCRAMBLE_LENGTH_323;

    // 1 filler
    payload++;

    mysql_server_capabilities_one = gw_mysql_get_byte2(payload);

    // Get capabilities_part 1 (2 bytes) + 1 language + 2 server_status
    payload += 5;

    mysql_server_capabilities_two = gw_mysql_get_byte2(payload);

    conn->server_capabilities = mysql_server_capabilities_one | mysql_server_capabilities_two << 16;

    // 2 bytes shift
    payload += 2;

    // get scramble len
    if (payload[0] > 0)
    {
        scramble_len = std::min(payload[0] - 1, GW_MYSQL_SCRAMBLE_SIZE);
    }
    else
    {
        scramble_len = GW_MYSQL_SCRAMBLE_SIZE;
    }

    mxb_assert(scramble_len > GW_SCRAMBLE_LENGTH_323);

    // skip 10 zero bytes
    payload += 11;

    // copy the second part of the scramble
    memcpy(scramble_data_2, payload, scramble_len - GW_SCRAMBLE_LENGTH_323);

    memcpy(mxs_scramble, scramble_data_1, GW_SCRAMBLE_LENGTH_323);
    memcpy(mxs_scramble + GW_SCRAMBLE_LENGTH_323, scramble_data_2, scramble_len - GW_SCRAMBLE_LENGTH_323);

    // full 20 bytes scramble is ready
    memcpy(m_auth_data.scramble, mxs_scramble, GW_MYSQL_SCRAMBLE_SIZE);
    return 0;
}

/**
 * Create a response to the server handshake
 *
 * @param with_ssl             Whether to create an SSL response or a normal response packet
 * @param ssl_established      Set to true if the SSL response has been sent
 * @param service_capabilities Capabilities of the connecting service
 *
 * @return Generated response packet
 */
GWBUF* MariaDBBackendConnection::gw_generate_auth_response(bool with_ssl, bool ssl_established,
                                                           uint64_t service_capabilities)
{
    auto client_data = m_auth_data.client_data;
    uint8_t client_capabilities[4] = {0, 0, 0, 0};
    const uint8_t* curr_passwd = NULL;

    if (!client_data->auth_token_phase2.empty())
    {
        curr_passwd = client_data->auth_token_phase2.data();
    }

    uint32_t capabilities = create_capabilities(with_ssl, client_data->db[0], service_capabilities);
    gw_mysql_set_byte4(client_capabilities, capabilities);

    /**
     * Use the default authentication plugin name. If the server is using a
     * different authentication mechanism, it will send an AuthSwitchRequest
     * packet.
     */
    const char* auth_plugin_name = DEFAULT_MYSQL_AUTH_PLUGIN;

    const std::string& username = client_data->user;
    long bytes = response_length(with_ssl,
                                 ssl_established,
                                 username.c_str(),
                                 curr_passwd,
                                 client_data->db.c_str(),
                                 auth_plugin_name);

    if (capabilities & this->server_capabilities & GW_MYSQL_CAPABILITIES_CONNECT_ATTRS)
    {
        bytes += client_data->connect_attrs.size();
    }

    // allocating the GWBUF
    GWBUF* buffer = gwbuf_alloc(bytes);
    uint8_t* payload = GWBUF_DATA(buffer);

    // clearing data
    memset(payload, '\0', bytes);

    // put here the paylod size: bytes to write - 4 bytes packet header
    gw_mysql_set_byte3(payload, (bytes - 4));

    // set packet # = 1
    payload[3] = ssl_established ? '\x02' : '\x01';
    payload += 4;

    // set client capabilities
    memcpy(payload, client_capabilities, 4);

    // set now the max-packet size
    payload += 4;
    gw_mysql_set_byte4(payload, 16777216);

    // set the charset
    payload += 4;
    *payload = client_data->client_info.m_charset;

    payload++;

    // 19 filler bytes of 0
    payload += 19;

    // Either MariaDB 10.2 extra capabilities or 4 bytes filler
    uint32_t extra_capabilities = client_data->extra_capabilitites();
    memcpy(payload, &extra_capabilities, sizeof(extra_capabilities));
    payload += 4;

    if (!with_ssl || ssl_established)
    {
        // 4 + 4 + 4 + 1 + 23 = 36, this includes the 4 bytes packet header
        memcpy(payload, username.c_str(), username.length());
        payload += username.length();
        payload++;

        if (curr_passwd)
        {
            payload = load_hashed_password(m_auth_data.scramble, payload, curr_passwd);
        }
        else
        {
            payload++;
        }

        // if the db is not NULL append it
        if (client_data->db[0])
        {
            memcpy(payload, client_data->db.c_str(), client_data->db.length());
            payload += client_data->db.length();
            payload++;
        }

        memcpy(payload, auth_plugin_name, strlen(auth_plugin_name));

        if ((capabilities & this->server_capabilities & GW_MYSQL_CAPABILITIES_CONNECT_ATTRS)
            && !client_data->connect_attrs.empty())
        {
            // Copy client attributes as-is. This allows us to pass them along without having to process them.
            payload += strlen(auth_plugin_name) + 1;
            memcpy(payload, client_data->connect_attrs.data(), client_data->connect_attrs.size());
        }
    }

    return buffer;
}

/**
 * @brief Computes the capabilities bit mask for connecting to backend DB
 *
 * We start by taking the default bitmask and removing any bits not set in
 * the bitmask contained in the connection structure. Then add SSL flag if
 * the connection requires SSL (set from the MaxScale configuration). The
 * compression flag may be set, although compression is NOT SUPPORTED. If a
 * database name has been specified in the function call, the relevant flag
 * is set.
 *
 * @param db_specified Whether the connection request specified a database
 * @param compress Whether compression is requested - NOT SUPPORTED
 * @return Bit mask (32 bits)
 * @note Capability bits are defined in maxscale/protocol/mysql.h
 */
uint32_t MariaDBBackendConnection::create_capabilities(bool with_ssl, bool db_specified,
                                                       uint64_t capabilities)
{
    uint32_t final_capabilities;

    /** Copy client's flags to backend but with the known capabilities mask */
    final_capabilities =
        (m_auth_data.client_data->client_capabilities() & (uint32_t)GW_MYSQL_CAPABILITIES_CLIENT);

    if (with_ssl)
    {
        final_capabilities |= (uint32_t)GW_MYSQL_CAPABILITIES_SSL;
        /*
         * Unclear whether we should include this
         * Maybe it should depend on whether CA certificate is provided
         * final_capabilities |= (uint32_t)GW_MYSQL_CAPABILITIES_SSL_VERIFY_SERVER_CERT;
         */
    }

    if (rcap_type_required(capabilities, RCAP_TYPE_SESSION_STATE_TRACKING))
    {
        /** add session track */
        final_capabilities |= (uint32_t)GW_MYSQL_CAPABILITIES_SESSION_TRACK;
    }

    /** support multi statments  */
    final_capabilities |= (uint32_t)GW_MYSQL_CAPABILITIES_MULTI_STATEMENTS;

    if (db_specified)
    {
        /* With database specified */
        final_capabilities |= (int)GW_MYSQL_CAPABILITIES_CONNECT_WITH_DB;
    }
    else
    {
        /* Without database specified */
        final_capabilities &= ~(int)GW_MYSQL_CAPABILITIES_CONNECT_WITH_DB;
    }

    final_capabilities |= (int)GW_MYSQL_CAPABILITIES_PLUGIN_AUTH;

    return final_capabilities;
}

GWBUF* MariaDBBackendConnection::process_packets(GWBUF** result)
{
    mxs::Buffer buffer(*result);
    auto it = buffer.begin();
    size_t total_bytes = buffer.length();
    size_t bytes_used = 0;

    while (it != buffer.end())
    {
        size_t bytes_left = total_bytes - bytes_used;

        if (bytes_left < MYSQL_HEADER_LEN)
        {
            // Partial header
            break;
        }

        // Extract packet length and command byte
        uint32_t len = *it++;
        len |= (*it++) << 8;
        len |= (*it++) << 16;
        ++it;   // Skip the sequence

        if (bytes_left < len + MYSQL_HEADER_LEN)
        {
            // Partial packet payload
            break;
        }

        bytes_used += len + MYSQL_HEADER_LEN;

        mxb_assert(it != buffer.end());
        auto end = it;
        end.advance(len);

        // Ignore the tail end of a large packet large packet. Only resultsets can generate packets this large
        // and we don't care what the contents are and thus it is safe to ignore it.
        bool skip_next = m_skip_next;
        m_skip_next = len == GW_MYSQL_MAX_PACKET_LEN;

        if (!skip_next)
        {
            process_one_packet(it, end, len);
        }

        it = end;
    }

    buffer.release();
    return gwbuf_split(result, bytes_used);
}

void MariaDBBackendConnection::process_one_packet(Iter it, Iter end, uint32_t len)
{
    uint8_t cmd = *it;
    switch (m_reply.state())
    {
    case ReplyState::START:
        process_reply_start(it, end);
        break;

    case ReplyState::DONE:
        if (cmd == MYSQL_REPLY_ERR)
        {
            update_error(++it, end);
        }
        else
        {
            // This should never happen
            MXS_ERROR("Unexpected result state. cmd: 0x%02hhx, len: %u server: %s",
                      cmd, len, m_server.name());
            session_dump_statements(m_session);
            session_dump_log(m_session);
            mxb_assert(!true);
        }
        break;

    case ReplyState::RSET_COLDEF:
        mxb_assert(m_num_coldefs > 0);
        --m_num_coldefs;

        if (m_num_coldefs == 0)
        {
            set_reply_state(ReplyState::RSET_COLDEF_EOF);
            // Skip this state when DEPRECATE_EOF capability is supported
        }
        break;

    case ReplyState::RSET_COLDEF_EOF:
        mxb_assert(cmd == MYSQL_REPLY_EOF && len == MYSQL_EOF_PACKET_LEN - MYSQL_HEADER_LEN);
        set_reply_state(ReplyState::RSET_ROWS);

        if (m_opening_cursor)
        {
            m_opening_cursor = false;
            MXS_INFO("Cursor successfully opened");
            set_reply_state(ReplyState::DONE);
        }
        break;

    case ReplyState::RSET_ROWS:
        if (cmd == MYSQL_REPLY_EOF && len == MYSQL_EOF_PACKET_LEN - MYSQL_HEADER_LEN)
        {
            set_reply_state(is_last_eof(it) ? ReplyState::DONE : ReplyState::START);

            ++it;
            uint16_t warnings = *it++;
            warnings |= *it << 8;

            m_reply.set_num_warnings(warnings);
        }
        else if (cmd == MYSQL_REPLY_ERR)
        {
            ++it;
            update_error(it, end);
            set_reply_state(ReplyState::DONE);
        }
        else
        {
            m_reply.add_rows(1);
        }
        break;

    case ReplyState::PREPARE:
        if (--m_ps_packets == 0)
        {
            set_reply_state(ReplyState::DONE);
        }
        break;
    }
}

void MariaDBBackendConnection::process_ok_packet(Iter it, Iter end)
{
    ++it;                   // Skip the command byte
    skip_encoded_int(it);   // Affected rows
    skip_encoded_int(it);   // Last insert ID
    uint16_t status = *it++;
    status |= (*it++) << 8;

    if ((status & SERVER_MORE_RESULTS_EXIST) == 0)
    {
        // No more results
        set_reply_state(ReplyState::DONE);
    }

    // Two bytes of warnings
    uint16_t warnings = *it++;
    warnings |= (*it++) << 8;
    m_reply.set_num_warnings(warnings);

    if (rcap_type_required(m_session->service->capabilities(), RCAP_TYPE_SESSION_STATE_TRACKING)
        && (status & SERVER_SESSION_STATE_CHANGED))
    {
        // TODO: Benchmark the extra cost of always processing the session tracking variables and see if it's
        // too much.
        mxb_assert(server_capabilities & GW_MYSQL_CAPABILITIES_SESSION_TRACK);

        skip_encoded_str(it);   // Skip human-readable info

        // Skip the total packet length, we don't need it since we know it implicitly via the end iterator
        MXB_AT_DEBUG(ptrdiff_t total_size = ) get_encoded_int(it);
        mxb_assert(total_size == std::distance(it, end));

        while (it != end)
        {
            uint64_t type = *it++;
            uint64_t total_size = get_encoded_int(it);

            switch (type)
            {
            case SESSION_TRACK_STATE_CHANGE:
                it.advance(total_size);
                break;

            case SESSION_TRACK_SCHEMA:
                skip_encoded_str(it);   // Schema name
                break;

            case SESSION_TRACK_GTIDS:
                skip_encoded_int(it);   // Encoding specification
                m_reply.set_variable(MXS_LAST_GTID, get_encoded_str(it));
                break;

            case SESSION_TRACK_TRANSACTION_CHARACTERISTICS:
                m_reply.set_variable("trx_characteristics", get_encoded_str(it));
                break;

            case SESSION_TRACK_SYSTEM_VARIABLES:
                {
                    auto name = get_encoded_str(it);
                    auto value = get_encoded_str(it);
                    m_reply.set_variable(name, value);
                }
                break;

            case SESSION_TRACK_TRANSACTION_TYPE:
                m_reply.set_variable("trx_state", get_encoded_str(it));
                break;

            default:
                mxb_assert(!true);
                it.advance(total_size);
                MXS_WARNING("Received unexpecting session track type: %lu", type);
                break;
            }
        }
    }
}

/**
 * Extract prepared statement response
 *
 *  Contents of a COM_STMT_PREPARE_OK packet:
 *
 * [0]     OK (1)            -- always 0x00
 * [1-4]   statement_id (4)  -- statement-id
 * [5-6]   num_columns (2)   -- number of columns
 * [7-8]   num_params (2)    -- number of parameters
 * [9]     filler (1)
 * [10-11] warning_count (2) -- number of warnings
 *
 * The OK packet is followed by the parameter definitions terminated by an EOF packet and the field
 * definitions terminated by an EOF packet. If the DEPRECATE_EOF capability is set, the EOF packets are not
 * sent (currently not supported).
 *
 * @param it  Start of the packet payload
 * @param end Past-the-end iterator of the payload
 */
void MariaDBBackendConnection::process_ps_response(Iter it, Iter end)
{
    mxb_assert(*it == MYSQL_REPLY_OK);
    ++it;

    // PS ID generated by the server
    uint32_t stmt_id = 0;
    stmt_id |= *it++;
    stmt_id |= *it++ << 8;
    stmt_id |= *it++ << 16;
    stmt_id |= *it++ << 24;

    // Columns
    uint16_t columns = *it++;
    columns += *it++ << 8;

    // Parameters
    uint16_t params = *it++;
    params += *it++ << 8;

    m_reply.set_generated_id(stmt_id);
    m_reply.set_param_count(params);

    m_ps_packets = 0;

    if (columns)
    {
        // Column definition packets plus one for the EOF
        m_ps_packets += columns + 1;
    }

    if (params)
    {
        // Parameter definition packets plus one for the EOF
        m_ps_packets += params + 1;
    }

    set_reply_state(m_ps_packets == 0 ? ReplyState::DONE : ReplyState::PREPARE);
}

void MariaDBBackendConnection::process_reply_start(Iter it, Iter end)
{
    if (m_reply.command() == MXS_COM_BINLOG_DUMP)
    {
        // Treat COM_BINLOG_DUMP like a response that never ends
    }
    else if (m_reply.command() == MXS_COM_STATISTICS)
    {
        // COM_STATISTICS returns a single string and thus requires special handling:
        // https://mariadb.com/kb/en/library/com_statistics/#response
        set_reply_state(ReplyState::DONE);
    }
    else if (m_reply.command() == MXS_COM_FIELD_LIST)
    {
        // COM_FIELD_LIST sends a strange kind of a result set that doesn't have field definitions
        set_reply_state(ReplyState::RSET_ROWS);
    }
    else
    {
        process_result_start(it, end);
    }
}

void MariaDBBackendConnection::process_result_start(Iter it, Iter end)
{
    uint8_t cmd = *it;

    switch (cmd)
    {
    case MYSQL_REPLY_OK:
        m_reply.set_is_ok(true);

        if (m_reply.command() == MXS_COM_STMT_PREPARE)
        {
            process_ps_response(it, end);
        }
        else
        {
            process_ok_packet(it, end);
        }
        break;

    case MYSQL_REPLY_LOCAL_INFILE:
        // The client will send a request after this with the contents of the file which the server will
        // respond to with either an OK or an ERR packet
        session_set_load_active(m_session, true);
        set_reply_state(ReplyState::DONE);
        break;

    case MYSQL_REPLY_ERR:
        // Nothing ever follows an error packet
        ++it;
        update_error(it, end);
        set_reply_state(ReplyState::DONE);
        break;

    case MYSQL_REPLY_EOF:
        // EOF packets are never expected as the first response unless changing user.
        mxb_assert(m_changing_user);
        break;

    default:
        // Start of a result set
        m_num_coldefs = get_encoded_int(it);
        m_reply.add_field_count(m_num_coldefs);
        set_reply_state(ReplyState::RSET_COLDEF);
        break;
    }
}

/**
 * Update @c m_error.
 *
 * @param it   Iterator that points to the first byte of the error code in an error packet.
 * @param end  Iterator pointing one past the end of the error packet.
 */
void MariaDBBackendConnection::update_error(Iter it, Iter end)
{
    uint16_t code = 0;
    code |= (*it++);
    code |= (*it++) << 8;
    ++it;
    auto sql_state_begin = it;
    it.advance(5);
    auto sql_state_end = it;
    auto message_begin = sql_state_end;
    auto message_end = end;

    m_reply.set_error(code, sql_state_begin, sql_state_end, message_begin, message_end);
}

uint64_t MariaDBBackendConnection::thread_id() const
{
    return m_thread_id;
}

void MariaDBBackendConnection::assign_session(MXS_SESSION* session, mxs::Component* upstream)
{
    m_session = session;
    m_upstream = upstream;
    MYSQL_session* client_data = static_cast<MYSQL_session*>(session->protocol_data());
    m_auth_data.client_data = client_data;
    m_authenticator = client_data->m_current_authenticator->create_backend_authenticator(m_auth_data);
}

/**
 * Track a client query
 *
 * Inspects the query and tracks the current command being executed. Also handles detection of
 * multi-packet requests and the special handling that various commands need.
 */
void MariaDBBackendConnection::track_query(GWBUF* buffer)
{
    mxb_assert(gwbuf_is_contiguous(buffer));
    uint8_t* data = GWBUF_DATA(buffer);

    if (m_changing_user)
    {
        // User reauthentication in progress, ignore the contents
        return;
    }

    if (session_is_load_active(m_session))
    {
        if (MYSQL_GET_PAYLOAD_LEN(data) == 0)
        {
            MXS_INFO("Load data ended");
            session_set_load_active(m_session, false);
            set_reply_state(ReplyState::START);
        }
    }
    else if (!m_large_query)
    {
        m_reply.clear();
        m_reply.set_command(MYSQL_GET_COMMAND(data));

        if (mxs_mysql_command_will_respond(m_reply.command()))
        {
            set_reply_state(ReplyState::START);
        }

        if (m_reply.command() == MXS_COM_STMT_EXECUTE)
        {
            // Extract the flag byte after the statement ID
            uint8_t flags = data[MYSQL_PS_ID_OFFSET + MYSQL_PS_ID_SIZE];

            // Any non-zero flag value means that we have an open cursor
            m_opening_cursor = flags != 0;
        }
        else if (m_reply.command() == MXS_COM_STMT_FETCH)
        {
            set_reply_state(ReplyState::RSET_ROWS);
        }
    }

    /**
     * If the buffer contains a large query, we have to skip the command
     * byte extraction for the next packet. This way current_command always
     * contains the latest command executed on this backend.
     */
    m_large_query = MYSQL_GET_PAYLOAD_LEN(data) == MYSQL_PACKET_LENGTH_MAX;
}

MariaDBBackendConnection::~MariaDBBackendConnection()
{
    gwbuf_free(m_stored_query);
}

void MariaDBBackendConnection::set_dcb(DCB* dcb)
{
    m_dcb = static_cast<BackendDCB*>(dcb);
}

const BackendDCB* MariaDBBackendConnection::dcb() const
{
    return m_dcb;
}

BackendDCB* MariaDBBackendConnection::dcb()
{
    return m_dcb;
}

void MariaDBBackendConnection::set_reply_state(mxs::ReplyState state)
{
    m_reply.set_reply_state(state);
}

std::string MariaDBBackendConnection::to_string(State auth_state)
{
    std::string rval;
    switch (auth_state)
    {
    case State::HANDSHAKING:
        rval = "Handshaking";
        break;

    case State::AUTHENTICATING:
        rval = "Authenticating";
        break;

    case State::CONNECTION_INIT:
        rval = "Sending connection initialization queries";
        break;

    case State::SEND_DELAYQ:
        rval = "Sending delayed queries";
        break;

    case State::FAILED:
        rval = "Failed";
        break;

    case State::ROUTING:
        rval = "Routing";
        break;
    }
    return rval;
}

MariaDBBackendConnection::StateMachineRes MariaDBBackendConnection::handshake()
{
    auto rval = StateMachineRes::ERROR;
    bool state_machine_continue = true;

    while (state_machine_continue)
    {
        switch (m_hs_state)
        {
        case HandShakeState::EXPECT_HS:
            {
                // Read the server handshake.
                mxs::Buffer buffer;
                if (!read_protocol_packet(m_dcb, &buffer))
                {
                    // Socket error.
                    string errmsg = (string)"Handshake with '" + m_server.name() + "' failed.";
                    do_handle_error(m_dcb, errmsg, mxs::ErrorType::TRANSIENT);
                    m_hs_state = HandShakeState::FAIL;
                }
                else if (buffer.empty())
                {
                    // Only got a partial packet, wait for more.
                    state_machine_continue = false;
                    rval = StateMachineRes::IN_PROGRESS;
                }
                else if (mxs_mysql_get_command(buffer.get()) == MYSQL_REPLY_ERR)
                {
                    // Server responded with an error instead of a handshake, probably too many connections.
                    do_handle_error(m_dcb, "Connection rejected: " + mxs::extract_error(buffer.get()),
                                    mxs::ErrorType::TRANSIENT);
                    m_hs_state = HandShakeState::FAIL;
                }
                else
                {
                    // Have a complete response from the server.
                    buffer.make_contiguous();
                    if (read_backend_handshake(std::move(buffer)))
                    {
                        m_hs_state = m_server.ssl().context() ? HandShakeState::START_SSL :
                            HandShakeState::SEND_HS_RESP;
                    }
                    else
                    {
                        do_handle_error(m_dcb, "Bad handshake", mxs::ErrorType::TRANSIENT);
                        m_hs_state = HandShakeState::FAIL;
                    }
                }
            }
            break;

        case HandShakeState::START_SSL:
            {
                // SSL-connection starts by sending a cleartext SSLRequest-packet,
                // then initiating SSL-negotiation.
                GWBUF* ssl_req = gw_generate_auth_response(true, false, m_dcb->service()->capabilities());
                if (ssl_req && m_dcb->writeq_append(ssl_req) && m_dcb->ssl_handshake() >= 0)
                {
                    m_hs_state = HandShakeState::SSL_NEG;
                }
                else
                {
                    do_handle_error(m_dcb, "SSL failed", mxs::ErrorType::TRANSIENT);
                    m_hs_state = HandShakeState::FAIL;
                }
            }
            break;

        case HandShakeState::SSL_NEG:
            {
                // Check SSL-state.
                auto ssl_state = m_dcb->ssl_state();
                if (ssl_state == DCB::SSLState::ESTABLISHED)
                {
                    m_hs_state = HandShakeState::SEND_HS_RESP;      // SSL ready
                }
                else if (ssl_state == DCB::SSLState::HANDSHAKE_REQUIRED)
                {
                    state_machine_continue = false;     // in progress, wait for more data
                    rval = StateMachineRes::IN_PROGRESS;
                }
                else
                {
                    do_handle_error(m_dcb, "SSL failed", mxs::ErrorType::TRANSIENT);
                    m_hs_state = HandShakeState::FAIL;
                }
            }
            break;

        case HandShakeState::SEND_HS_RESP:
            {
                bool with_ssl = m_server.ssl().context();
                GWBUF* hs_resp = gw_generate_auth_response(with_ssl, with_ssl,
                                                           m_dcb->service()->capabilities());
                if (m_dcb->writeq_append(hs_resp))
                {
                    m_hs_state = HandShakeState::COMPLETE;
                }
                else
                {
                    m_hs_state = HandShakeState::FAIL;
                }
            }
            break;

        case HandShakeState::COMPLETE:
            state_machine_continue = false;
            rval = StateMachineRes::DONE;
            break;

        case HandShakeState::FAIL:
            state_machine_continue = false;
            rval = StateMachineRes::ERROR;
            break;
        }
    }
    return rval;
}

MariaDBBackendConnection::StateMachineRes MariaDBBackendConnection::authenticate()
{
    mxs::Buffer buffer;
    if (!read_protocol_packet(m_dcb, &buffer))
    {
        do_handle_error(m_dcb, "Socket error", mxs::ErrorType::TRANSIENT);
        return StateMachineRes::ERROR;
    }
    else if (buffer.empty())
    {
        // Didn't get enough data, read again later.
        return StateMachineRes::IN_PROGRESS;
    }
    else if (buffer.length() == MYSQL_HEADER_LEN)
    {
        // Effectively empty buffer. Should not happen during authentication. Error.
        do_handle_error(m_dcb, "Invalid packet", mxs::ErrorType::TRANSIENT);
        return StateMachineRes::ERROR;
    }

    // Have a complete response from the server.
    buffer.make_contiguous();
    uint8_t cmd = MYSQL_GET_COMMAND(GWBUF_DATA(buffer.get()));

    // Three options: OK, ERROR or AuthSwitch/other.
    auto rval = StateMachineRes::ERROR;
    if (cmd == MYSQL_REPLY_OK)
    {
        MXB_INFO("Authentication to '%s' succeeded.", m_server.name());
        rval = StateMachineRes::DONE;
    }
    else if (cmd == MYSQL_REPLY_ERR)
    {
        // Server responded with an error, authentication failed.
        handle_error_response(m_dcb, buffer.get());
        rval = StateMachineRes::ERROR;
    }
    else
    {
        // Something else, likely AuthSwitch or a message to the authentication plugin.
        using AuthRes = mariadb::BackendAuthenticator::AuthRes;
        mxs::Buffer output;
        auto res = m_authenticator->exchange(buffer, &output);
        if (!output.empty())
        {
            m_dcb->writeq_append(output.release());
        }

        rval = (res == AuthRes::SUCCESS) ? StateMachineRes::IN_PROGRESS : StateMachineRes::ERROR;
    }

    return rval;
}

bool MariaDBBackendConnection::send_delayed_packets()
{
    if (!m_delayed_packets.empty())
    {
        m_delayed_packets.make_contiguous();
        /** Send the queued commands to the backend */
        GWBUF* buffer = m_delayed_packets.release();
        prepare_for_write(buffer);
        backend_write_delayqueue(buffer);
    }
    return false;
}

MariaDBBackendConnection::StateMachineRes MariaDBBackendConnection::send_connection_init_queries()
{
    auto rval = StateMachineRes::ERROR;
    switch (m_init_query_status.state)
    {
    case InitQueryStatus::State::SENDING:
        {
            // First time in this function.
            const auto& init_query_data = m_session->service->connection_init_sql();
            const auto& query_contents = init_query_data.buffer_contents;
            if (query_contents.empty())
            {
                rval = StateMachineRes::DONE;   // no init queries configured, continue normally
            }
            else
            {
                // Send all the initialization queries in one packet. The server should respond with one
                // OK-packet per query.
                GWBUF* buffer = gwbuf_alloc_and_load(query_contents.size(), query_contents.data());
                m_dcb->writeq_append(buffer);
                m_init_query_status.ok_packets_expected = init_query_data.queries.size();
                m_init_query_status.ok_packets_received = 0;
                m_init_query_status.state = InitQueryStatus::State::RECEIVING;
                rval = StateMachineRes::IN_PROGRESS;
            }
        }
        break;

    case InitQueryStatus::State::RECEIVING:
        while (m_init_query_status.ok_packets_received < m_init_query_status.ok_packets_expected)
        {
            // Check result. If server returned anything else than OK, it's an error.
            mxs::Buffer buffer;
            if (!read_protocol_packet(m_dcb, &buffer))
            {
                do_handle_error(m_dcb, "Socket error", mxs::ErrorType::TRANSIENT);
            }
            else if (buffer.empty())
            {
                // Didn't get enough data, read again later.
                rval = StateMachineRes::IN_PROGRESS;
            }
            else
            {
                string wrong_packet_type;
                if (buffer.length() == MYSQL_HEADER_LEN)
                {
                    wrong_packet_type = "an empty packet";
                }
                else
                {
                    uint8_t cmd = MYSQL_GET_COMMAND(buffer.data());
                    if (cmd == MYSQL_REPLY_ERR)
                    {
                        wrong_packet_type = "an error packet";
                    }
                    else if (cmd != MYSQL_REPLY_OK)
                    {
                        wrong_packet_type = "a resultset packet";
                    }
                }

                if (wrong_packet_type.empty())
                {
                    // Got an ok packet.
                    m_init_query_status.ok_packets_received++;
                }
                else
                {
                    // Query failed or gave weird results.
                    const auto& init_queries = m_session->service->connection_init_sql().queries;
                    const string& errored_query = init_queries[m_init_query_status.ok_packets_received];
                    string errmsg = mxb::string_printf("Connection initialization query '%s' returned %s.",
                                                       errored_query.c_str(), wrong_packet_type.c_str());
                    do_handle_error(m_dcb, errmsg, mxs::ErrorType::PERMANENT);
                    break;
                }
            }
        }

        if (m_init_query_status.ok_packets_received == m_init_query_status.ok_packets_expected)
        {
            rval = StateMachineRes::DONE;
        }
        break;
    }
    return rval;
}
