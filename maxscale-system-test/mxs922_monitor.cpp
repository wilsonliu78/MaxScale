/**
 * @file mxs922_monitor.cpp MXS-922: Monitor creation test
 *
 */

#include "config_operations.h"

int main(int argc, char* argv[])
{
    TestConnections* test = new TestConnections(argc, argv);
    Config config(test);

    test->tprintf("Creating monitor");

    config.create_all_listeners();
    config.create_monitor("mysql-monitor", "mysqlmon", 500);
    config.reset();

    sleep(1);

    test->check_maxscale_alive(0);

    config.destroy_monitor("mysql-monitor");

    test->check_maxscale_alive(0);

    test->maxscales->ssh_node(0, "for i in 0 1 2 3; do maxctrl clear server server$i running; done", true);

    test->add_result(test->maxscales->connect_maxscale(0) == 0, "Should not be able to connect");

    config.create_monitor("mysql-monitor2", "mysqlmon", 500);
    config.add_created_servers("mysql-monitor2");

    sleep(1);
    test->check_maxscale_alive(0);

    /** Try to alter the monitor user */
    test->maxscales->connect_maxscale(0);
    execute_query(test->maxscales->conn_rwsplit[0], "DROP USER 'test'@'%%'");
    execute_query(test->maxscales->conn_rwsplit[0], "CREATE USER 'test'@'%%' IDENTIFIED BY 'test'");
    execute_query(test->maxscales->conn_rwsplit[0], "GRANT ALL ON *.* TO 'test'@'%%'");
    test->maxscales->close_maxscale_connections(0);

    config.alter_monitor("mysql-monitor2", "user", "test");
    config.alter_monitor("mysql-monitor2", "password", "test");

    sleep(1);
    test->check_maxscale_alive(0);

    /** Remove the user */
    test->maxscales->connect_maxscale(0);
    execute_query(test->maxscales->conn_rwsplit[0], "DROP USER 'test'@'%%'");

    config.restart_monitors();

    /**
     * Make sure the server are in a bad state. This way we'll know that the
     * monitor is running if the states have changed and the query is
     * successful.
     */
    test->maxscales->ssh_node(0, "for i in 0 1 2 3; do maxctrl clear server server$i running; done", true);

    sleep(1);
    test->add_result(execute_query_silent(test->maxscales->conn_rwsplit[0], "SELECT 1") == 0,
                     "Query should fail when monitor has wrong credentials");
    test->maxscales->close_maxscale_connections(0);

    for (int i = 0; i < test->repl->N; i++)
    {
        config.alter_server(i, "monitoruser", "skysql", "monitorpw", "skysql");
    }

    config.restart_monitors();
    test->maxscales->wait_for_monitor();
    test->check_maxscale_alive(0);

    int rval = test->global_result;
    delete test;
    return rval;
}
