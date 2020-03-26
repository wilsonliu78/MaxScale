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

// To ensure that ss_info_assert asserts also when builing in non-debug mode.
#if !defined (SS_DEBUG)
#define SS_DEBUG
#endif
#if defined (NDEBUG)
#undef NDEBUG
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <maxbase/alloc.h>
#include <maxscale/server.hh>
#include <maxscale/paths.h>
#include <maxscale/config.hh>

// This is pretty ugly but it's required to test internal functions
#include "../server.cc"
#include "../internal/config.hh"
#include "../internal/server.hh"
#include "../internal/servermanager.hh"

static mxs::ConfigParameters* params = new mxs::ConfigParameters;

/**
 * test1    Allocate a server and do lots of other things
 *
 */
static int test1()
{
    std::string status;

    /* Server tests */
    fprintf(stderr, "testserver : creating server called MyServer");
    Server* server = ServerManager::create_server("uniquename", *params);
    mxb_assert_message(server, "Allocating the server should not fail");

    fprintf(stderr, "\t..done\nTesting Unique Name for Server.");
    mxb_assert_message(NULL == ServerManager::find_by_unique_name("non-existent"),
                       "Should not find non-existent unique name.");
    mxb_assert_message(server == ServerManager::find_by_unique_name("uniquename"),
                       "Should find by unique name.");
    fprintf(stderr, "\t..done\nTesting Status Setting for Server.");
    status = server->status_string();
    mxb_assert_message(status == "Down", "Status of Server should be Running by default.");
    server->set_status(SERVER_RUNNING | SERVER_MASTER);
    status = server->status_string();
    mxb_assert_message(status == "Master, Running", "Should find correct status.");
    server->clear_status(SERVER_MASTER);
    status = server->status_string();
    mxb_assert_message(status == "Running",
                       "Status of Server should be Running after master status cleared.");
    fprintf(stderr, "\t..done\nFreeing Server.");
    ServerManager::server_free(server);
    fprintf(stderr, "\t..done\n");
    return 0;
}

#define TEST(A, B) do {if (!(A)) {printf(B "\n"); return false;}} while (false)

bool test_load_config(const char* input, Server* server)
{
    DUPLICATE_CONTEXT dcontext;

    if (duplicate_context_init(&dcontext))
    {
        CONFIG_CONTEXT ccontext;

        if (config_load_single_file(input, &dcontext, &ccontext))
        {
            CONFIG_CONTEXT* obj = ccontext.m_next;
            mxs::ConfigParameters* param = &obj->m_parameters;
            config_add_defaults(&obj->m_parameters, common_server_params());

            TEST(strcmp(obj->name(), server->name()) == 0, "Server names differ");
            TEST(param->get_string("address") == server->address, "Server addresses differ");
            TEST(param->get_integer("port") == server->port, "Server ports differ");
            TEST(ServerManager::create_server(obj->name(), obj->m_parameters),
                 "Failed to create server from loaded config");
            duplicate_context_finish(&dcontext);
            config_context_free(obj);
        }
    }

    return true;
}

bool test_serialize()
{
    char name[] = "serialized-server";
    char config_name[] = "serialized-server.cnf";
    char old_config_name[] = "serialized-server.cnf.old";
    char* persist_dir = MXS_STRDUP_A("./");
    set_config_persistdir(persist_dir);
    Server* server = ServerManager::create_server(name, *params);
    TEST(server, "Server allocation failed");

    /** Make sure the files don't exist */
    unlink(config_name);
    unlink(old_config_name);

    /** Serialize server to disk */
    TEST(server->serialize(), "Failed to synchronize original server");

    /** Load it again */
    TEST(test_load_config(config_name, server), "Failed to load the serialized server");

    /** We should have two identical servers */
    Server* created = ServerManager::find_by_unique_name(name);

    rename(config_name, old_config_name);

    /** Serialize the loaded server to disk */
    TEST(created->serialize(), "Failed to synchronize the copied server");

    /** Check that they serialize to identical files */
    char cmd[1024];
    sprintf(cmd, "diff ./%s ./%s", config_name, old_config_name);
    TEST(system(cmd) == 0, "The files are not identical");

    return true;
}

int main(int argc, char** argv)
{
    /**
     * Prepare test environment by pre-loading modules. This prevents the server
     * allocation from failing if multiple modules from different directories are
     * loaded in one core function call.
     */
    mxs_log_init(NULL, NULL, MXS_LOG_TARGET_STDOUT);

    params->set_from_list({
        {"address", "127.0.0.1"},
        {"port", "9876"},
    }, common_server_params());
    int result = 0;

    result += test1();

    if (!test_serialize())
    {
        result++;
    }

    mxs_log_finish();
    exit(result);
}
