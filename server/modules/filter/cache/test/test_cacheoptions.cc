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

#include <iostream>
#include <maxscale/filtermodule.hh>
#include <maxscale/mock/backend.hh>
#include <maxscale/mock/client.hh>
#include <maxscale/mock/routersession.hh>
#include <maxscale/mock/session.hh>
#include "../cachefilter.hh"

#include "../../../../core/test/test_utils.hh"

using namespace std;
using maxscale::FilterModule;
namespace mock = maxscale::mock;

namespace
{

struct SETTINGS
{
    bool stop_at_first_error;
} settings =
{
    true,   // stop_at_first_error
};

// See
// https://github.com/mariadb-corporation/MaxScale/blob/2.2/Documentation/Filters/Cache.md#cache_inside_transactions
struct TEST_CASE
{
    cache_in_trxs_t cit;        /*< How to cache in transactions. */
    uint32_t        trx_state;  /*< The transaction state. */
    bool            should_use; /*< Whether the cache should be returned from the cache. */
} TEST_CASES[] =
{
    {
        CACHE_IN_TRXS_NEVER,
        SESSION_TRX_INACTIVE,
        true    // should_use
    },
    {
        CACHE_IN_TRXS_NEVER,
        SESSION_TRX_ACTIVE,
        false   // should_use
    },
    {
        CACHE_IN_TRXS_NEVER,
        SESSION_TRX_ACTIVE | SESSION_TRX_READ_ONLY,
        false   // should_use
    },
    {
        CACHE_IN_TRXS_READ_ONLY,
        SESSION_TRX_INACTIVE,
        true    // should_use
    },
    {
        CACHE_IN_TRXS_READ_ONLY,
        SESSION_TRX_ACTIVE,
        false   // should_use
    },
    {
        CACHE_IN_TRXS_READ_ONLY,
        SESSION_TRX_ACTIVE | SESSION_TRX_READ_ONLY,
        true    // should_use
    },
    {
        CACHE_IN_TRXS_ALL,
        SESSION_TRX_INACTIVE,
        true    // should_use
    },
    {
        CACHE_IN_TRXS_ALL,
        SESSION_TRX_ACTIVE,
        true    // should_use
    },
    {
        CACHE_IN_TRXS_ALL,
        SESSION_TRX_ACTIVE | SESSION_TRX_READ_ONLY,
        true    // should_use
    },
};

const size_t N_TEST_CASES = sizeof(TEST_CASES) / sizeof(TEST_CASES[0]);

const char* to_string(cache_in_trxs_t x)
{
    switch (x)
    {
    case CACHE_IN_TRXS_NEVER:
        return "never";

    case CACHE_IN_TRXS_READ_ONLY:
        return "read_only_transactions";

    case CACHE_IN_TRXS_ALL:
        return "all_transactions";

    default:
        mxb_assert(!true);
        return NULL;
    }
}

ostream& operator<<(ostream& out, cache_in_trxs_t x)
{
    out << to_string(x);
    return out;
}
}

namespace
{

static int counter = 0;

string create_unique_select()
{
    stringstream ss;
    ss << "SELECT col" << ++counter << " FROM tbl";
    return ss.str();
}

int test(mock::Session& session,
         FilterModule::Session& filter_session,
         mock::RouterSession& router_session,
         const TEST_CASE& tc)
{
    int rv = 0;

    mock::Client& client = session.client();

    // Let's check that there's nothing pending.
    mxb_assert(client.n_responses() == 0);
    mxb_assert(router_session.idle());

    session.set_trx_state(tc.trx_state);
    session.set_autocommit(tc.trx_state == SESSION_TRX_INACTIVE);

    string select(create_unique_select());
    GWBUF* pStatement;
    pStatement = mock::create_com_query(select);

    cout << "Performing select: \"" << select << "\"" << flush;
    session.route_query(pStatement);

    if (!router_session.idle())
    {
        cout << ", reached backend." << endl;

        // Let's cause the backend to respond.
        router_session.respond();

        // And let's verify that the backend is now empty...
        mxb_assert(router_session.idle());
        // ...and that we have received a response.
        mxb_assert(client.n_responses() == 1);

        // Let's do the select again.
        pStatement = mock::create_com_query(select);

        cout << "Performing same select: \"" << select << "\"" << flush;
        session.route_query(pStatement);

        if (tc.should_use)
        {
            if (!router_session.idle())
            {
                cout << "\nERROR: Select reached backend and was not provided from cache." << endl;
                router_session.respond();
                ++rv;
            }
            else
            {
                cout << ", cache was used." << endl;

                // Let's check we did receive a response.
                mxb_assert(client.n_responses() == 2);
            }
        }
        else
        {
            if (router_session.idle())
            {
                cout << "\nERROR: Select was provided from cache and did not reach backend." << endl;
                ++rv;
            }
            else
            {
                cout << ", reached backend." << endl;
                router_session.respond();
            }
        }

        if (tc.trx_state != SESSION_TRX_INACTIVE
            && tc.trx_state != (SESSION_TRX_ACTIVE | SESSION_TRX_READ_ONLY))
        {
            // A transaction, but not a read-only one.

            string update("UPDATE tbl SET a=1;");
            pStatement = mock::create_com_query("UPDATE tbl SET a=1;");

            cout << "Performing update: \"" << update << "\"" << flush;
            session.route_query(pStatement);

            if (router_session.idle())
            {
                cout << "\n"
                     << "ERROR: Did not reach backend." << endl;
                ++rv;
            }
            else
            {
                cout << ", reached backend." << endl;
                router_session.respond();

                // Let's make the select again.
                pStatement = mock::create_com_query(select);

                cout << "Performing select: \"" << select << "\"" << flush;
                session.route_query(pStatement);

                if (router_session.idle())
                {
                    cout << "\nERROR: Did not reach backend." << endl;
                    ++rv;
                }
                else
                {
                    // The select reached the backend, i.e. the cache was not used after
                    // a non-SELECT.
                    cout << ", reached backend." << endl;
                    router_session.respond();
                }
            }
        }

        // Irrespective of what was going on above, the cache should now contain the
        // original select. So, let's do a select with no transaction.

        cout << "Setting transaction state to SESSION_TRX_INACTIVE" << endl;
        session.set_trx_state(SESSION_TRX_INACTIVE);
        session.set_autocommit(true);

        pStatement = mock::create_com_query(select);

        cout << "Performing select: \"" << select << "\"" << flush;
        session.route_query(pStatement);

        if (router_session.idle())
        {
            cout << ", cache was used." << endl;
        }
        else
        {
            cout << "\nERROR: cache was not used." << endl;
            router_session.respond();
            ++rv;
        }
    }
    else
    {
        cout << "\nERROR: Did not reach backend." << endl;
        ++rv;
    }

    return rv;
}

int test(FilterModule::Instance& filter_instance, const TEST_CASE& tc)
{
    int rv = 0;

    static int port = 3306;

    mxs::ConfigParameters parameters;
    parameters.set("connection_timeout", "10s");
    parameters.set(CN_NET_WRITE_TIMEOUT, "10s");
    parameters.set(CN_CONNECTION_KEEPALIVE, "100s");

    auto service = Service::create("service", "readconnroute", &parameters);

    mxs::ConfigParameters listener_params;
    listener_params.set(CN_ADDRESS, "0.0.0.0");
    listener_params.set(CN_PORT, std::to_string(port++).c_str());
    listener_params.set(CN_PROTOCOL, "mariadbclient");
    listener_params.set(CN_SERVICE, service->name());

    auto listener_data = mxs::ListenerSessionData::create_test_data(listener_params);
    mock::Client client("bob", "127.0.0.1");
    mock::Session session(&client, listener_data);
    mock::ResultSetBackend backend;
    mock::RouterSession router_session(&backend, &session);

    auto_ptr<FilterModule::Session> sFilter_session = filter_instance.newSession(&session,
                                                                                 service,
                                                                                 router_session.as_downstream(),
                                                                                 client.as_upstream());

    if (sFilter_session.get())
    {
        session.set_downstream(sFilter_session.get());
        router_session.set_upstream(sFilter_session.get());

        rv += test(session, *sFilter_session.get(), router_session, tc);
    }
    else
    {
        ++rv;
    }

    return rv;
}

int test(FilterModule& filter_module, const TEST_CASE& tc)
{
    int rv = 1;

    auto params = filter_module.create_default_parameters();
    params->set("cache_in_transactions", to_string(tc.cit));
    params->set("debug", "31");
    params->set("cached_data", "shared");
    params->set("selects", "verify_cacheable");

    auto_ptr<FilterModule::Instance> sInstance = filter_module.createInstance("test", params);

    if (sInstance.get())
    {
        rv = test(*sInstance, tc);
    }

    return rv;
}
}

namespace
{

int run()
{
    int rv = 1;

    auto_ptr<FilterModule> sModule = FilterModule::load("cache");

    if (sModule.get())
    {
        rv = 0;

        for (size_t i = 0; i < N_TEST_CASES; ++i)
        {
            const TEST_CASE& tc = TEST_CASES[i];

            cout << "CIT: " << tc.cit
                 << ", TRX_STATE: " << tc.trx_state
                 << ", should use: " << tc.should_use
                 << endl;

            rv += test(*sModule.get(), tc);

            cout << endl;

            if ((rv != 0) && settings.stop_at_first_error)
            {
                break;
            }
        }
    }
    else
    {
        cerr << "error: Could not load filter module." << endl;
    }

    return rv;
}
}

namespace
{

char USAGE[] =
    "usage: test_dbfwfilter [-d]\n"
    "\n"
    "-d    don't stop at first error\n";
}

int main(int argc, char* argv[])
{
    int rv = 0;

    int c;
    while ((c = getopt(argc, argv, "d")) != -1)
    {
        switch (c)
        {
        case 'd':
            settings.stop_at_first_error = false;
            break;

        default:
            rv = 1;
        }
    }

    if (rv == 0)
    {
        run_unit_test([&]() {
                          set_libdir(MXS_STRDUP(TEST_DIR
                                                "/server/modules/filter/cache/storage/storage_inmemory"));
                          preload_module("cache", "server/modules/filter/cache/", MODULE_FILTER);

                          rv = run();
                      });

        cout << rv << " failures." << endl;

        qc_process_end(QC_INIT_SELF);
    }
    else
    {
        cout << USAGE << endl;
    }

    return rv;
}
