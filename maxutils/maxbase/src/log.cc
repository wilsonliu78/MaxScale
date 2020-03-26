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

#include <maxbase/log.h>
#include <maxbase/log.hh>

#include <sys/time.h>
#include <syslog.h>

#include <cinttypes>
#include <cmath>
#include <cstring>
#include <string>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include <maxbase/assert.h>
#include <maxbase/logger.hh>

/**
 * Variable holding the enabled priorities information.
 */
int mxb_log_enabled_priorities = (1 << LOG_ERR) | (1 << LOG_NOTICE) | (1 << LOG_WARNING);

// Number of chars needed to represent a number.
#define CALCLEN(i) ((size_t)(floor(log10(abs((int64_t)i))) + 1))
#define UINTLEN(i) (i < 10 ? 1 : (i < 100 ? 2 : (i < 1000 ? 3 : CALCLEN(i))))

namespace
{

int DEFAULT_LOG_AUGMENTATION = 0;

// A message that is logged 10 times in 1 second will be suppressed for 10 seconds.
static MXB_LOG_THROTTLING DEFAULT_LOG_THROTTLING = {10, 1000, 10000};

// BUFSIZ comes from the system. It equals with block size or its multiplication.
const int MAX_LOGSTRLEN = BUFSIZ;

// Current monotonic raw time in milliseconds.
uint64_t time_monotonic_ms()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// Regular timestamp
std::string get_timestamp(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    static const char timestamp_formatstr[] = "%04d-%02d-%02d %02d:%02d:%02d   ";
    static int required = snprintf(NULL,
                                   0,
                                   timestamp_formatstr,
                                   tm.tm_year + 1900,
                                   tm.tm_mon + 1,
                                   tm.tm_mday,
                                   tm.tm_hour,
                                   tm.tm_min,
                                   tm.tm_sec);
    char buf[required + 1];

    snprintf(buf,
             sizeof(buf),
             timestamp_formatstr,
             tm.tm_year + 1900,
             tm.tm_mon + 1,
             tm.tm_mday,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec);

    return buf;
}

// High-precision timestamp
std::string get_timestamp_hp(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    int usec = tv.tv_usec / 1000;

    static const char timestamp_formatstr_hp[] = "%04d-%02d-%02d %02d:%02d:%02d.%03d   ";
    static int required = snprintf(NULL,
                                   0,
                                   timestamp_formatstr_hp,
                                   tm.tm_year + 1900,
                                   tm.tm_mon + 1,
                                   tm.tm_mday,
                                   tm.tm_hour,
                                   tm.tm_min,
                                   tm.tm_sec,
                                   usec);

    char buf[required + 1];

    snprintf(buf,
             sizeof(buf),
             timestamp_formatstr_hp,
             tm.tm_year + 1900,
             tm.tm_mon + 1,
             tm.tm_mday,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             usec);

    return buf;
}

struct LOG_PREFIX
{
    const char* text;   // The prefix, e.g. "error: "
    int         len;    // The length of the prefix without the trailing NULL.
};

const char PREFIX_EMERG[] = "emerg  : ";
const char PREFIX_ALERT[] = "alert  : ";
const char PREFIX_CRIT[] = "crit   : ";
const char PREFIX_ERROR[] = "error  : ";
const char PREFIX_WARNING[] = "warning: ";
const char PREFIX_NOTICE[] = "notice : ";
const char PREFIX_INFO[] = "info   : ";
const char PREFIX_DEBUG[] = "debug  : ";

LOG_PREFIX level_to_prefix(int level)
{
    assert((level & ~LOG_PRIMASK) == 0);

    LOG_PREFIX prefix;

    switch (level)
    {
    case LOG_EMERG:
        prefix.text = PREFIX_EMERG;
        prefix.len = sizeof(PREFIX_EMERG);
        break;

    case LOG_ALERT:
        prefix.text = PREFIX_ALERT;
        prefix.len = sizeof(PREFIX_ALERT);
        break;

    case LOG_CRIT:
        prefix.text = PREFIX_CRIT;
        prefix.len = sizeof(PREFIX_CRIT);
        break;

    case LOG_ERR:
        prefix.text = PREFIX_ERROR;
        prefix.len = sizeof(PREFIX_ERROR);
        break;

    case LOG_WARNING:
        prefix.text = PREFIX_WARNING;
        prefix.len = sizeof(PREFIX_WARNING);
        break;

    case LOG_NOTICE:
        prefix.text = PREFIX_NOTICE;
        prefix.len = sizeof(PREFIX_NOTICE);
        break;

    case LOG_INFO:
        prefix.text = PREFIX_INFO;
        prefix.len = sizeof(PREFIX_INFO);
        break;

    case LOG_DEBUG:
        prefix.text = PREFIX_DEBUG;
        prefix.len = sizeof(PREFIX_DEBUG);
        break;

    default:
        assert(!true);
        prefix.text = PREFIX_ERROR;
        prefix.len = sizeof(PREFIX_ERROR);
        break;
    }

    --prefix.len;   // Remove trailing NULL.

    return prefix;
}

enum message_suppression_t
{
    MESSAGE_NOT_SUPPRESSED,     // Message is not suppressed.
    MESSAGE_SUPPRESSED,         // Message is suppressed for the first time (for this round)
    MESSAGE_STILL_SUPPRESSED    // Message is still suppressed (for this round)
};

class MessageRegistryKey
{
public:
    const char* filename;
    const int   linenumber;

    /**
     * @brief Constructs a message stats key
     *
     * @param filename    The filename where the message was reported. Must be a
     *                    statically allocated buffer, e.g. __FILE__.
     * @param linenumber  The linenumber where the message was reported.
     */
    MessageRegistryKey(const char* filename, int linenumber)
        : filename(filename)
        , linenumber(linenumber)
    {
    }

    bool eq(const MessageRegistryKey& other) const
    {
        return filename == other.filename   // Yes, we compare the pointer values and not the strings.
               && linenumber == other.linenumber;
    }

    size_t hash() const
    {
        /*
         * This is an implementation of the Jenkin's one-at-a-time hash function.
         * https://en.wikipedia.org/wiki/Jenkins_hash_function
         */
        uint64_t key1 = (uint64_t)filename;
        uint16_t key2 = (uint16_t)linenumber;   // The first 48 bits are likely to be 0.

        uint32_t hash_value = 0;
        size_t i;

        for (i = 0; i < sizeof(key1); ++i)
        {
            hash_value += (key1 >> i * 8) & 0xff;
            hash_value += (hash_value << 10);
            hash_value ^= (hash_value >> 6);
        }

        for (i = 0; i < sizeof(key2); ++i)
        {
            hash_value += (key1 >> i * 8) & 0xff;
            hash_value += (hash_value << 10);
            hash_value ^= (hash_value >> 6);
        }

        hash_value += (hash_value << 3);
        hash_value ^= (hash_value >> 11);
        hash_value += (hash_value << 15);
        return hash_value;
    }
};

class MessageRegistryStats
{
public:
    MessageRegistryStats()
        : m_first_ms(time_monotonic_ms())
        , m_last_ms(0)
        , m_count(0)
    {
    }

    message_suppression_t update_suppression(const MXB_LOG_THROTTLING& t)
    {
        message_suppression_t rv = MESSAGE_NOT_SUPPRESSED;

        uint64_t now_ms = time_monotonic_ms();

        std::lock_guard<std::mutex> guard(m_lock);

        ++m_count;

        // Less that t.window_ms milliseconds since the message was logged
        // the last time. May have to be throttled.
        if (m_count < t.count)
        {
            // t.count times has not been reached, still ok to log.
        }
        else if (m_count == t.count)
        {
            // t.count times has been reached. Was it within the window?
            if (now_ms - m_first_ms < t.window_ms)
            {
                // Within the window, suppress the message.
                rv = MESSAGE_SUPPRESSED;
            }
            else
            {
                // Not within the window, reset the situation.

                // The flooding situation is analyzed window by window.
                // That means that if there in each of two consequtive
                // windows are not enough messages for throttling to take
                // effect, but there would be if the window was placed at a
                // slightly different position (e.g. starting in the middle
                // of the first and ending in the middle of the second) it
                // will go undetected and no throttling will be made.
                // However, if that's the case, it was a spike so the
                // flooding will stop anyway.

                m_first_ms = now_ms;
                m_count = 1;
            }
        }
        else
        {
            // In suppression mode.
            if (now_ms - m_first_ms < (t.window_ms + t.suppress_ms))
            {
                // Still in the suppression window.
                rv = MESSAGE_STILL_SUPPRESSED;
            }
            else
            {
                // We have exited the suppression window, reset the situation.
                m_first_ms = now_ms;
                m_count = 1;
            }
        }

        m_last_ms = now_ms;

        return rv;
    }

private:
    std::mutex m_lock;
    uint64_t   m_first_ms;  /** The time when the error was logged the first time in this window. */
    uint64_t   m_last_ms;   /** The time when the error was logged the last time. */
    size_t     m_count;     /** How many times the error has been reported within this window. */
};
}

namespace std
{

template<>
struct hash<MessageRegistryKey>
{
    typedef MessageRegistryKey Key;
    typedef size_t             result_type;

    size_t operator()(const MessageRegistryKey& key) const
    {
        return key.hash();
    }
};

template<>
struct equal_to<MessageRegistryKey>
{
    typedef bool               result_type;
    typedef MessageRegistryKey first_argument_type;
    typedef MessageRegistryKey second_argument_type;

    bool operator()(const MessageRegistryKey& lhs, const MessageRegistryKey& rhs) const
    {
        return lhs.eq(rhs);
    }
};
}

namespace
{

class MessageRegistry;

struct this_unit
{
    int                              augmentation;      // Can change during the lifetime of log_manager.
    bool                             do_highprecision;  // Can change during the lifetime of log_manager.
    bool                             do_syslog;         // Can change during the lifetime of log_manager.
    bool                             do_maxlog;         // Can change during the lifetime of log_manager.
    bool                             redirect_stdout;
    bool                             session_trace;
    MXB_LOG_THROTTLING               throttling;        // Can change during the lifetime of log_manager.
    std::unique_ptr<mxb::Logger>     sLogger;
    std::unique_ptr<MessageRegistry> sMessage_registry;
    size_t                           (* context_provider)(char* buffer, size_t len);
    void                             (* in_memory_log)(const char* buffer, size_t len);
} this_unit =
{
    DEFAULT_LOG_AUGMENTATION,   // augmentation
    false,                      // do_highprecision
    true,                       // do_syslog
    true,                       // do_maxlog
    false,                      // redirect_stdout
    false,                      // session_trace
    DEFAULT_LOG_THROTTLING,     // throttling
};

class MessageRegistry
{
public:
    typedef MessageRegistryKey   Key;
    typedef MessageRegistryStats Stats;

    MessageRegistry(const MessageRegistry&) = delete;
    MessageRegistry& operator=(const MessageRegistry&) = delete;

    MessageRegistry()
    {
    }

    Stats& get_stats(const Key& key)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        return m_registry[key];
    }

    message_suppression_t get_status(const char* file, int line)
    {
        message_suppression_t rv = MESSAGE_NOT_SUPPRESSED;

        // Copy the config to prevent the values from changing while we are using
        // them. It does not matter if they are changed just when we are copying
        // them, but we want to use one set of values throughout the function.
        MXB_LOG_THROTTLING t = this_unit.throttling;

        if ((t.count != 0) && (t.window_ms != 0) && (t.suppress_ms != 0))
        {
            MessageRegistry::Key key(file, line);
            MessageRegistry::Stats& stats = this_unit.sMessage_registry->get_stats(key);

            rv = stats.update_suppression(t);
        }

        return rv;
    }

private:
    std::mutex                     m_lock;
    std::unordered_map<Key, Stats> m_registry;
};
}

bool mxb_log_init(const char* ident,
                  const char* logdir,
                  const char* filename,
                  mxb_log_target_t target,
                  mxb_log_context_provider_t context_provider,
                  mxb_in_memory_log_t in_memory_log)
{
    assert(!this_unit.sLogger && !this_unit.sMessage_registry);

    // Trigger calculation of buffer lengths.
    get_timestamp();
    get_timestamp_hp();

    // Tests mainly pass a NULL logdir with MXB_LOG_TARGET_STDOUT but using
    // /dev/null as the default allows total suppression of logging
    std::string filepath = "/dev/null";

    if (logdir)
    {
        std::string suffix;

        if (!filename)
        {
#ifdef __GNUC__
            suffix = program_invocation_short_name;
#else
            suffix = "messages";
#endif
            suffix += ".log";
        }
        else
        {
            suffix = filename;
        }

        filepath = std::string(logdir) + "/" + suffix;
    }

    this_unit.sMessage_registry.reset(new(std::nothrow) MessageRegistry);

    switch (target)
    {
    case MXB_LOG_TARGET_FS:
    case MXB_LOG_TARGET_DEFAULT:
        this_unit.sLogger = mxb::FileLogger::create(filepath);

        if (this_unit.sLogger && this_unit.redirect_stdout)
        {
            // Redirect stdout and stderr to the log file
            FILE* unused __attribute__ ((unused));
            unused = freopen(this_unit.sLogger->filename(), "a", stdout);
            unused = freopen(this_unit.sLogger->filename(), "a", stderr);
        }
        break;

    case MXB_LOG_TARGET_STDOUT:
        this_unit.sLogger = mxb::StdoutLogger::create(filepath);
        break;

    default:
        assert(!true);
        break;
    }

    if (this_unit.sLogger && this_unit.sMessage_registry)
    {
        this_unit.context_provider = context_provider;
        this_unit.in_memory_log = in_memory_log;

        openlog(ident, LOG_PID | LOG_ODELAY, LOG_USER);
    }
    else
    {
        this_unit.sLogger.reset();
        this_unit.sMessage_registry.reset();
    }

    return this_unit.sLogger && this_unit.sMessage_registry;
}

void mxb_log_finish(void)
{
    assert(this_unit.sLogger && this_unit.sMessage_registry);

    closelog();
    this_unit.sLogger.reset();
    this_unit.sMessage_registry.reset();
    this_unit.context_provider = nullptr;
}

bool mxb_log_inited()
{
    return this_unit.sLogger && this_unit.sMessage_registry;
}

void mxb_log_set_augmentation(int bits)
{
    this_unit.augmentation = bits & MXB_LOG_AUGMENTATION_MASK;
}

void mxb_log_set_highprecision_enabled(bool enabled)
{
    this_unit.do_highprecision = enabled;

    MXB_NOTICE("highprecision logging is %s.", enabled ? "enabled" : "disabled");
}

bool mxb_log_is_highprecision_enabled()
{
    return this_unit.do_highprecision;
}

void mxb_log_set_syslog_enabled(bool enabled)
{
    this_unit.do_syslog = enabled;

    MXB_NOTICE("syslog logging is %s.", enabled ? "enabled" : "disabled");
}

bool mxb_log_is_syslog_enabled()
{
    return this_unit.do_syslog;
}

void mxb_log_set_maxlog_enabled(bool enabled)
{
    this_unit.do_maxlog = enabled;

    MXB_NOTICE("maxlog logging is %s.", enabled ? "enabled" : "disabled");
}

bool mxb_log_is_maxlog_enabled()
{
    return this_unit.do_maxlog;
}

void mxb_log_set_throttling(const MXB_LOG_THROTTLING* throttling)
{
    // No locking; it does not have any real impact, even if the struct
    // is used right when its values are modified.
    this_unit.throttling = *throttling;

    if ((this_unit.throttling.count == 0)
        || (this_unit.throttling.window_ms == 0)
        || (this_unit.throttling.suppress_ms == 0))
    {
        MXB_NOTICE("Log throttling has been disabled.");
    }
    else
    {
        MXB_NOTICE("A message that is logged %lu times in %lu milliseconds, "
                   "will be suppressed for %lu milliseconds.",
                   this_unit.throttling.count,
                   this_unit.throttling.window_ms,
                   this_unit.throttling.suppress_ms);
    }
}

void mxb_log_get_throttling(MXB_LOG_THROTTLING* throttling)
{
    // No locking; this is used only from maxadmin and an inconsistent set
    // may be returned only if mxb_log_set_throttling() is called via an
    // other instance of maxadmin at the very same moment.
    *throttling = this_unit.throttling;
}

void mxs_log_redirect_stdout(bool redirect)
{
    this_unit.redirect_stdout = redirect;
}

void mxb_log_set_session_trace(bool enabled)
{
    this_unit.session_trace = enabled;
}

bool mxb_log_get_session_trace()
{
    return this_unit.session_trace;
}

bool mxb_log_rotate()
{
    bool rval = this_unit.sLogger->rotate();

    if (this_unit.redirect_stdout && rval)
    {
        // Redirect stdout and stderr to the log file
        FILE* unused __attribute__ ((unused));
        unused = freopen(this_unit.sLogger->filename(), "a", stdout);
        unused = freopen(this_unit.sLogger->filename(), "a", stderr);
    }

    return rval;
}

const char* mxb_log_get_filename()
{
    return this_unit.sLogger->filename();
}

static const char* level_to_string(int level)
{
    switch (level)
    {
    case LOG_EMERG:
        return "emergency";

    case LOG_ALERT:
        return "alert";

    case LOG_CRIT:
        return "critical";

    case LOG_ERR:
        return "error";

    case LOG_WARNING:
        return "warning";

    case LOG_NOTICE:
        return "notice";

    case LOG_INFO:
        return "informational";

    case LOG_DEBUG:
        return "debug";

    default:
        assert(!true);
        return "unknown";
    }
}

bool mxb_log_set_priority_enabled(int level, bool enable)
{
    bool rv = false;
    const char* text = (enable ? "enable" : "disable");

    if ((level & ~LOG_PRIMASK) == 0)
    {
        int bit = (1 << level);

        if (enable)
        {
            mxb_log_enabled_priorities |= bit;
        }
        else
        {
            mxb_log_enabled_priorities &= ~bit;
        }

        MXB_NOTICE("The logging of %s messages has been %sd.", level_to_string(level), text);
        rv = true;
    }
    else
    {
        MXB_ERROR("Attempt to %s unknown syslog priority %d.", text, level);
    }

    return rv;
}

int mxb_log_message(int priority,
                    const char* modname,
                    const char* file,
                    int line,
                    const char* function,
                    const char* format,
                    ...)
{
    int err = 0;

    assert(this_unit.sLogger && this_unit.sMessage_registry);
    assert((priority & ~(LOG_PRIMASK | LOG_FACMASK)) == 0);

    int level = priority & LOG_PRIMASK;

    if ((priority & ~(LOG_PRIMASK | LOG_FACMASK)) == 0)     // Check that the priority is ok,
    {
        message_suppression_t status = MESSAGE_NOT_SUPPRESSED;

        // We only throttle errors and warnings. Info and debug messages
        // are never on during normal operation, so if they are enabled,
        // we are presumably debugging something. Notice messages are
        // assumed to be logged for a reason and always in a context where
        // flooding cannot be caused.
        if ((level == LOG_ERR) || (level == LOG_WARNING))
        {
            status = this_unit.sMessage_registry->get_status(file, line);
        }

        if (status != MESSAGE_STILL_SUPPRESSED)
        {
            va_list valist;

            char context[32];   // The documentation will guarantee a buffer of at least 32 bytes.
            int context_len = 0;

            if (this_unit.context_provider)
            {
                context_len = this_unit.context_provider(context, sizeof(context));

                if (context_len != 0)
                {
                    context_len += 3;   // The added "() "
                }
            }

            int modname_len = modname ? strlen(modname) + 3 : 0;    // +3 due to "[...] "

            // If we know the actual object name, add that also
            auto scope = mxb::LogScope::current_scope();
            int scope_len = scope ? strlen(scope) + 3 : 0;      // +3 due to "(...) "

            static const char SUPPRESSION[] =
                " (subsequent similar messages suppressed for %lu milliseconds)";
            int suppression_len = 0;
            size_t suppress_ms = this_unit.throttling.suppress_ms;

            if (status == MESSAGE_SUPPRESSED)
            {
                suppression_len += sizeof(SUPPRESSION) - 1; // Remove trailing NULL
                suppression_len -= 3;                       // Remove the %lu
                suppression_len += UINTLEN(suppress_ms);
            }

            /**
             * Find out the length of log string (to be formatted str).
             */
            va_start(valist, format);
            int message_len = vsnprintf(NULL, 0, format, valist);
            va_end(valist);

            if (message_len >= 0)
            {
                LOG_PREFIX prefix = level_to_prefix(level);

                static const char FORMAT_FUNCTION[] = "(%s): ";

                // Other thread might change this_unit.augmentation.
                int augmentation = this_unit.augmentation;
                int augmentation_len = 0;

                switch (augmentation)
                {
                case MXB_LOG_AUGMENT_WITH_FUNCTION:
                    augmentation_len = sizeof(FORMAT_FUNCTION) - 1; // Remove trailing 0
                    augmentation_len -= 2;                          // Remove the %s
                    augmentation_len += strlen(function);
                    break;

                default:
                    break;
                }

                int buffer_len = 0;
                buffer_len += prefix.len;
                buffer_len += context_len;
                buffer_len += modname_len;
                buffer_len += scope_len;
                buffer_len += augmentation_len;
                buffer_len += message_len;
                buffer_len += suppression_len;

                if (buffer_len > MAX_LOGSTRLEN)
                {
                    message_len -= (buffer_len - MAX_LOGSTRLEN);
                    buffer_len = MAX_LOGSTRLEN;

                    assert(prefix.len + context_len + modname_len + scope_len
                           + augmentation_len + message_len + suppression_len == buffer_len);
                }

                char buffer[buffer_len + 1];

                char* prefix_text = buffer;
                char* context_text = prefix_text + prefix.len;
                char* modname_text = context_text + context_len;
                char* scope_text = modname_text + modname_len;
                char* augmentation_text = scope_text + scope_len;
                char* message_text = augmentation_text + augmentation_len;
                char* suppression_text = message_text + message_len;

                strcpy(prefix_text, prefix.text);

                if (context_len)
                {
                    strcpy(context_text, "(");
                    strcat(context_text, context);
                    strcat(context_text, ") ");
                }

                if (modname_len)
                {
                    strcpy(modname_text, "[");
                    strcat(modname_text, modname);
                    strcat(modname_text, "] ");
                }

                if (scope_len)
                {
                    strcpy(scope_text, "(");
                    strcat(scope_text, scope);
                    strcat(scope_text, ") ");
                }

                if (augmentation_len)
                {
                    int len = 0;

                    switch (augmentation)
                    {
                    case MXB_LOG_AUGMENT_WITH_FUNCTION:
                        len = sprintf(augmentation_text, FORMAT_FUNCTION, function);
                        break;

                    default:
                        assert(!true);
                    }

                    (void)len;
                    assert(len == augmentation_len);
                }

                va_start(valist, format);
                vsnprintf(message_text, message_len + 1, format, valist);
                va_end(valist);

                if (suppression_len)
                {
                    sprintf(suppression_text, SUPPRESSION, suppress_ms);
                }

                if (this_unit.do_syslog && LOG_PRI(priority) != LOG_DEBUG)
                {
                    // Debug messages are never logged into syslog
                    syslog(priority, "%s", context_text);
                }

                std::string msg = this_unit.do_highprecision ? get_timestamp_hp() : get_timestamp();
                msg += buffer;

                // Remove any user-generated newlines.
                // This is safe to do as we know the message is not full of newlines
                while (msg.back() == '\n')
                {
                    msg.pop_back();
                }

                // Add a final newline into the message
                msg.push_back('\n');

                if (this_unit.session_trace)
                {
                    this_unit.in_memory_log(msg.c_str(), msg.length());
                }

                if (auto func = mxb::LogRedirect::current_redirect())
                {
                    // We only pass the message text to the handler, everything else is extra that's only
                    // needed by the default logging mechanism.
                    func(level, message_text);
                    err = 0;
                }
                else if (mxb_log_is_priority_enabled(level))
                {

                    err = this_unit.sLogger->write(msg.c_str(), msg.length()) ? 0 : -1;
                }
                else
                {
                    err = 0;
                }
            }
        }
    }
    else
    {
        MXB_WARNING("Invalid syslog priority: %d", priority);
    }

    return err;
}

int mxb_log_oom(const char* message)
{
    return this_unit.sLogger->write(message, strlen(message)) ? 0 : -1;
}

namespace maxbase
{
thread_local LogScope* LogScope::s_current_scope {nullptr};
thread_local LogRedirect::Func LogRedirect::s_redirect {nullptr};

LogRedirect::LogRedirect(Func func)
{
    mxb_assert(s_redirect == nullptr);
    s_redirect = func;
}

LogRedirect::~LogRedirect()
{
    s_redirect = nullptr;
}

// static
LogRedirect::Func LogRedirect::current_redirect()
{
    return s_redirect;
}
}
