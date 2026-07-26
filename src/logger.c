#include "relay/logger.h"

#include "relay/platform.h"

#include <stdarg.h>
#include <time.h>

#if RELAY_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#endif

/** Create the directory that contains Relay's runtime logs. */
static bool relay_logger_create_directory(void)
{
#if RELAY_PLATFORM_WINDOWS
    return CreateDirectoryA("logs", NULL) != 0 ||
        GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir("logs", 0755) == 0 || errno == EEXIST;
#endif
}

/** Return the stable display name for a log severity. */
static const char *relay_logger_level_name(Relay_LogLevel level)
{
    switch (level) {
    case RELAY_LOG_LEVEL_DEBUG:
        return "DEBUG";
    case RELAY_LOG_LEVEL_INFO:
        return "INFO";
    case RELAY_LOG_LEVEL_WARNING:
        return "WARN";
    case RELAY_LOG_LEVEL_ERROR:
        return "ERROR";
    }

    return "UNKNOWN";
}

/** Fill a local calendar value using the platform-safe local-time API. */
static bool relay_logger_local_time(time_t now, struct tm *calendar)
{
#if RELAY_PLATFORM_WINDOWS
    return localtime_s(calendar, &now) == 0;
#else
    return localtime_r(&now, calendar) != NULL;
#endif
}

bool relay_logger_init(Relay_Logger *logger)
{
    if (logger == NULL || !relay_logger_create_directory()) {
        return false;
    }

    logger->stream = fopen("logs/relay.log", "w");
    return logger->stream != NULL;
}

void relay_logger_log(Relay_Logger *logger, Relay_LogLevel level,
    const char *format, ...)
{
    time_t now;
    struct tm calendar;
    va_list arguments;

    if (logger == NULL || logger->stream == NULL || format == NULL) {
        return;
    }

    now = time(NULL);
    if (now != (time_t)-1 && relay_logger_local_time(now, &calendar)) {
        (void)fprintf(logger->stream, "%04d-%02d-%02d %02d:%02d:%02d ",
            calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday,
            calendar.tm_hour, calendar.tm_min, calendar.tm_sec);
    }
    (void)fprintf(logger->stream, "[%s] ", relay_logger_level_name(level));
    va_start(arguments, format);
    (void)vfprintf(logger->stream, format, arguments);
    va_end(arguments);
    (void)fputc('\n', logger->stream);
    (void)fflush(logger->stream);
}

void relay_logger_shutdown(Relay_Logger *logger)
{
    if (logger == NULL || logger->stream == NULL) {
        return;
    }

    (void)fclose(logger->stream);
    logger->stream = NULL;
}
