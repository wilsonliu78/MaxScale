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
#ifndef SS_DEBUG
#define SS_DEBUG
#endif

#include <iostream>
#include "../internal/config.hh"

using namespace std;

#define TEST(a) do {if (!(a)) {printf("Error: `" #a "` was not true\n"); return 1;}} while (false)

int test_validity()
{
    MXS_ENUM_VALUE enum_values[] =
    {
        {"a", (1 << 0)},
        {"b", (1 << 1)},
        {"c", (1 << 2)},
        {NULL}
    };

    MXS_MODULE_PARAM params[] =
    {
        {"p1",  MXS_MODULE_PARAM_INT,      "-123"                   },
        {"p2",  MXS_MODULE_PARAM_COUNT,    "123"                    },
        {"p3",  MXS_MODULE_PARAM_BOOL,     "true"                   },
        {"p4",  MXS_MODULE_PARAM_STRING,   "default"                },
        {"p5",  MXS_MODULE_PARAM_ENUM,     "a", MXS_MODULE_OPT_NONE, enum_values},
        {"p6",  MXS_MODULE_PARAM_PATH,     "/tmp", MXS_MODULE_OPT_PATH_F_OK},
        {"p7",  MXS_MODULE_PARAM_SERVICE,  "my-service"             },
        {"p8",  MXS_MODULE_PARAM_ENUM,     "a", MXS_MODULE_OPT_ENUM_UNIQUE, enum_values},
        {"p9",  MXS_MODULE_PARAM_DURATION, "4711s"                  },
        {"p10", MXS_MODULE_PARAM_DURATION, "4711s", MXS_MODULE_OPT_DURATION_S},
        {MXS_END_MODULE_PARAMS}
    };

    CONFIG_CONTEXT ctx;

    /** Int parameter */
    TEST(config_param_is_valid(params, "p1", "1", &ctx));
    TEST(config_param_is_valid(params, "p1", "-1", &ctx));
    TEST(config_param_is_valid(params, "p1", "0", &ctx));
    TEST(!config_param_is_valid(params, "p1", "should not be OK", &ctx));   // String value for int, should
                                                                            // fail

    /** Count parameter */
    TEST(config_param_is_valid(params, "p2", "1", &ctx));
    TEST(config_param_is_valid(params, "p2", "0", &ctx));
    TEST(!config_param_is_valid(params, "p2", "should not be OK", &ctx));   // String value for count, should
                                                                            // fail
    TEST(!config_param_is_valid(params, "p2", "-1", &ctx));                 // Negative values for count
                                                                            // should fail

    /** Boolean parameter */
    TEST(config_param_is_valid(params, "p3", "1", &ctx));
    TEST(config_param_is_valid(params, "p3", "0", &ctx));
    TEST(config_param_is_valid(params, "p3", "true", &ctx));
    TEST(config_param_is_valid(params, "p3", "false", &ctx));
    TEST(config_param_is_valid(params, "p3", "yes", &ctx));
    TEST(config_param_is_valid(params, "p3", "no", &ctx));
    TEST(!config_param_is_valid(params, "p3", "maybe", &ctx));
    TEST(!config_param_is_valid(params, "p3", "perhaps", &ctx));
    TEST(!config_param_is_valid(params, "p3", "42", &ctx));
    TEST(!config_param_is_valid(params, "p3", "0.50", &ctx));

    /** String parameter */
    TEST(config_param_is_valid(params, "p4", "should be OK", &ctx));
    TEST(!config_param_is_valid(params, "p4", "", &ctx));   // Empty string is not OK

    /** Enum parameter */
    TEST(config_param_is_valid(params, "p5", "a", &ctx));
    TEST(config_param_is_valid(params, "p5", "b", &ctx));
    TEST(config_param_is_valid(params, "p5", "c", &ctx));
    TEST(config_param_is_valid(params, "p5", "a,b", &ctx));
    TEST(config_param_is_valid(params, "p5", "b,a", &ctx));
    TEST(config_param_is_valid(params, "p5", "a, b, c", &ctx));
    TEST(config_param_is_valid(params, "p5", "c,a,b", &ctx));
    TEST(!config_param_is_valid(params, "p5", "d", &ctx));
    TEST(!config_param_is_valid(params, "p5", "a,d", &ctx));
    TEST(!config_param_is_valid(params, "p5", "a,b,c,d", &ctx));

    /** Path parameter */
    TEST(config_param_is_valid(params, "p6", "/tmp", &ctx));
    TEST(!config_param_is_valid(params, "p6", "This is not a valid path", &ctx));

    /** Duration parameter */
    TEST(config_param_is_valid(params, "p9", "4711", &ctx));
    TEST(config_param_is_valid(params, "p9", "4711h", &ctx));
    TEST(config_param_is_valid(params, "p9", "4711m", &ctx));
    TEST(config_param_is_valid(params, "p9", "4711s", &ctx));
    TEST(config_param_is_valid(params, "p9", "4711ms", &ctx));
    TEST(config_param_is_valid(params, "p9", "4711H", &ctx));
    TEST(config_param_is_valid(params, "p9", "4711M", &ctx));
    TEST(config_param_is_valid(params, "p9", "4711S", &ctx));
    TEST(config_param_is_valid(params, "p9", "4711MS", &ctx));
    TEST(!config_param_is_valid(params, "p9", "4711q", &ctx));
    TEST(!config_param_is_valid(params, "p10", "4711ms", &ctx));

    /** Service parameter */
    CONFIG_CONTEXT svc("test-service");
    ctx.m_next = &svc;
    config_add_param(&svc, "type", "service");
    TEST(config_param_is_valid(params, "p7", "test-service", &ctx));
    TEST(!config_param_is_valid(params, "p7", "test-service", NULL));
    TEST(!config_param_is_valid(params, "p7", "no-such-service", &ctx));

    /** Unique enum parameter */
    TEST(config_param_is_valid(params, "p8", "a", &ctx));
    TEST(config_param_is_valid(params, "p8", "b", &ctx));
    TEST(config_param_is_valid(params, "p8", "c", &ctx));
    TEST(!config_param_is_valid(params, "p8", "a,b", &ctx));
    TEST(!config_param_is_valid(params, "p8", "b,a", &ctx));
    TEST(!config_param_is_valid(params, "p8", "a, b, c", &ctx));
    TEST(!config_param_is_valid(params, "p8", "c,a,b", &ctx));
    TEST(!config_param_is_valid(params, "p8", "d", &ctx));
    TEST(!config_param_is_valid(params, "p8", "a,d", &ctx));
    TEST(!config_param_is_valid(params, "p8", "a,b,c,d", &ctx));
    return 0;
}

int test_add_parameter()
{
    MXS_ENUM_VALUE enum_values[] =
    {
        {"a", (1 << 0)},
        {"b", (1 << 1)},
        {"c", (1 << 2)},
        {NULL}
    };

    MXS_MODULE_PARAM params[] =
    {
        {"p1", MXS_MODULE_PARAM_INT,     "-123"                  },
        {"p2", MXS_MODULE_PARAM_COUNT,   "123"                   },
        {"p3", MXS_MODULE_PARAM_BOOL,    "true"                  },
        {"p4", MXS_MODULE_PARAM_STRING,  "default"               },
        {"p5", MXS_MODULE_PARAM_ENUM,    "a", MXS_MODULE_OPT_NONE, enum_values},
        {"p6", MXS_MODULE_PARAM_PATH,    "/tmp", MXS_MODULE_OPT_PATH_F_OK},
        {"p7", MXS_MODULE_PARAM_SERVICE, "my-service"            },
        {MXS_END_MODULE_PARAMS}
    };


    CONFIG_CONTEXT svc1("my-service");
    CONFIG_CONTEXT svc2("some-service");
    CONFIG_CONTEXT ctx;
    svc2.m_next = &svc1;
    ctx.m_next = &svc2;
    config_add_param(&svc1, "type", "service");
    config_add_param(&svc2, "type", "service");

    config_add_defaults(&ctx.m_parameters, params);

    /** Test default values */
    TEST(ctx.m_parameters.get_integer("p1") == -123);
    TEST(ctx.m_parameters.get_integer("p2") == 123);
    TEST(ctx.m_parameters.get_bool("p3") == true);
    TEST(ctx.m_parameters.get_string("p4") == "default");
    TEST(ctx.m_parameters.get_enum("p5", enum_values) == 1);
    TEST(ctx.m_parameters.get_string("p6") == "/tmp");
    TEST(ctx.m_parameters.get_string("p7") == "my-service");

    ctx.m_parameters.clear();

    /** Test custom parameters overriding default values */
    config_add_param(&ctx, "p1", "-321");
    config_add_param(&ctx, "p2", "321");
    config_add_param(&ctx, "p3", "false");
    config_add_param(&ctx, "p4", "strange");
    config_add_param(&ctx, "p5", "a,c");
    config_add_param(&ctx, "p6", "/dev/null");
    config_add_param(&ctx, "p7", "some-service");

    config_add_defaults(&ctx.m_parameters, params);

    TEST(ctx.m_parameters.get_integer("p1") == -321);
    TEST(ctx.m_parameters.get_integer("p2") == 321);
    TEST(ctx.m_parameters.contains("p3") && ctx.m_parameters.get_bool("p3") == false);
    TEST(ctx.m_parameters.get_string("p4") == "strange");
    int val = ctx.m_parameters.get_enum("p5", enum_values);
    TEST(val == 5);
    TEST(ctx.m_parameters.get_string("p6") == "/dev/null");
    TEST(ctx.m_parameters.get_string("p7") == "some-service");
    return 0;
}

int test_required_parameters()
{
    MXS_MODULE_PARAM params[] =
    {
        {"p1", MXS_MODULE_PARAM_INT,   "-123", MXS_MODULE_OPT_REQUIRED},
        {"p2", MXS_MODULE_PARAM_COUNT, "123",  MXS_MODULE_OPT_REQUIRED},
        {"p3", MXS_MODULE_PARAM_BOOL,  "true", MXS_MODULE_OPT_REQUIRED},
        {MXS_END_MODULE_PARAMS}
    };

    CONFIG_CONTEXT ctx;

    TEST(missing_required_parameters(params, ctx.m_parameters, "test"));
    config_add_defaults(&ctx.m_parameters, params);
    TEST(!missing_required_parameters(params, ctx.m_parameters, "test"));

    ctx.m_parameters.clear();

    config_add_param(&ctx, "p1", "1");
    config_add_param(&ctx, "p2", "1");
    config_add_param(&ctx, "p3", "1");
    TEST(!missing_required_parameters(params, ctx.m_parameters, "test"));
    return 0;
}

namespace
{

struct DISK_SPACE_THRESHOLD_RESULT
{
    const char* zPath;
    int32_t     size;
};

struct DISK_SPACE_THRESHOLD_TEST
{
    const char*                 zValue;
    bool                        valid;
    DISK_SPACE_THRESHOLD_RESULT results[5];
};

int dst_report(const DISK_SPACE_THRESHOLD_TEST& test,
               bool parsed,
               DiskSpaceLimits& result)
{
    int nErrors = 0;

    cout << test.zValue << endl;

    if (test.valid)
    {
        if (parsed)
        {
            const DISK_SPACE_THRESHOLD_RESULT* pResult = test.results;

            while (pResult->zPath)
            {
                auto i = result.find(pResult->zPath);

                if (i != result.end())
                {
                    result.erase(i);
                }
                else
                {
                    cout << "error: Expected " << pResult->zPath << " to be found, but it wasn't." << endl;
                    ++nErrors;
                }

                ++pResult;
            }

            if (result.size() != 0)
            {
                for (auto i = result.begin(); i != result.end(); ++i)
                {
                    cout << "error: " << i->first << " was found, although not expected." << endl;
                    ++nErrors;
                    ++i;
                }
            }
        }
        else
        {
            cout << "error: Expected value to be parsed, but it wasn't." << endl;
        }
    }
    else
    {
        if (parsed)
        {
            cout << "error: Expected value not to be parsed, but it was." << endl;
            ++nErrors;
        }
    }

    if (nErrors == 0)
    {
        cout << "OK, ";
        if (test.valid)
        {
            cout << "was valid and was parsed as such.";
        }
        else
        {
            cout << "was not valid, and was not parsed either.";
        }
        cout << endl;
    }

    return nErrors;
}
}

int test_disk_space_threshold()
{
    int nErrors = 0;

    static const DISK_SPACE_THRESHOLD_TEST tests[] =
    {
        {
            "/data:80", true,
            {
                {"/data",  80}
            }
        },
        {
            "/data1", false
        },
        {
            ":50", false
        },
        {
            "/data1:", false
        },
        {
            "/data1:abc", false
        },
        {
            "/data1:120", false
        },
        {
            "/data1:-50", false
        },
        {
            "/data1,/data2:50", false
        },
        {
            "/data1:50,/data2", false
        },
        {
            " /data1 : 40, /data2 :50, /data3: 70 ", true,
            {
                {"/data1", 40},
                {"/data2", 50},
                {"/data3", 70},
            }
        }
    };

    const int nTests = sizeof(tests) / sizeof(tests[0]);

    for (int i = 0; i < nTests; ++i)
    {
        const DISK_SPACE_THRESHOLD_TEST& test = tests[i];

        DiskSpaceLimits dst;

        bool parsed = config_parse_disk_space_threshold(&dst, test.zValue);

        nErrors += dst_report(test, parsed, dst);
    }

    return nErrors;
}

int main(int argc, char** argv)
{
    int result = 0;

    if (mxs_log_init(NULL, ".", MXS_LOG_TARGET_FS))
    {
        result += test_validity();
        result += test_add_parameter();
        result += test_required_parameters();
        result += test_disk_space_threshold();
        mxs_log_finish();
    }
    else
    {
        cerr << "Could not initialize log manager." << endl;
        ++result;
    }

    return result;
}
