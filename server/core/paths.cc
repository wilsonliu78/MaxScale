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

#include <maxscale/paths.h>

#include <string>

#include <maxbase/alloc.h>
#include <maxscale/utils.hh>


namespace
{
static std::string configdir = MXS_DEFAULT_CONFIGDIR;
static std::string config_persistdir = MXS_DEFAULT_CONFIG_PERSISTDIR;
static std::string module_configdir = MXS_DEFAULT_MODULE_CONFIGDIR;
static std::string logdir = MXS_DEFAULT_LOGDIR;
static std::string libdir = MXS_DEFAULT_LIBDIR;
static std::string cachedir = MXS_DEFAULT_CACHEDIR;
static std::string datadir = MXS_DEFAULT_DATADIR;
static std::string processdatadir = MXS_DEFAULT_DATADIR;
static std::string langdir = MXS_DEFAULT_LANGDIR;
static std::string piddir = MXS_DEFAULT_PIDDIR;
static std::string execdir = MXS_DEFAULT_EXECDIR;
static std::string connector_plugindir = MXS_DEFAULT_CONNECTOR_PLUGINDIR;
}

void set_configdir(const char* path)
{
    configdir = clean_up_pathname(path);
}

void set_module_configdir(const char* path)
{
    module_configdir = clean_up_pathname(path);
}

void set_config_persistdir(const char* path)
{
    config_persistdir = clean_up_pathname(path);
}

void set_logdir(const char* path)
{
    logdir = clean_up_pathname(path);
}

void set_langdir(const char* path)
{
    langdir = clean_up_pathname(path);
}

void set_piddir(const char* path)
{
    piddir = clean_up_pathname(path);
}

void set_cachedir(const char* path)
{
    cachedir = clean_up_pathname(path);
}

void set_datadir(const char* path)
{
    datadir = clean_up_pathname(path);
}

void set_process_datadir(const char* path)
{
    processdatadir = clean_up_pathname(path);
}

void set_libdir(const char* path)
{
    libdir = clean_up_pathname(path);
}

void set_execdir(const char* path)
{
    execdir = clean_up_pathname(path);
}

void set_connector_plugindir(const char* path)
{
    connector_plugindir = clean_up_pathname(path);
}

const char* get_libdir()
{
    return libdir.c_str();
}

const char* get_cachedir()
{
    return cachedir.c_str();
}

const char* get_datadir()
{
    return datadir.c_str();
}

const char* get_process_datadir()
{
    return processdatadir.c_str();
}

const char* get_configdir()
{
    return configdir.c_str();
}

const char* get_module_configdir()
{
    return module_configdir.c_str();
}

const char* get_config_persistdir()
{
    return config_persistdir.c_str();
}

const char* get_piddir()
{
    return piddir.c_str();
}

const char* get_logdir()
{
    return logdir.c_str();
}

const char* get_langdir()
{
    return langdir.c_str();
}

const char* get_execdir()
{
    return execdir.c_str();
}

const char* get_connector_plugindir()
{
    return connector_plugindir.c_str();
}
