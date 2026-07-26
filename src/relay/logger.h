#ifndef RELAY_LOGGER_H
#define RELAY_LOGGER_H

#include <stdbool.h>
#include <stdio.h>

/** Severity assigned to a Relay log entry. */
typedef enum Relay_LogLevel {
    RELAY_LOG_LEVEL_DEBUG,
    RELAY_LOG_LEVEL_INFO,
    RELAY_LOG_LEVEL_WARNING,
    RELAY_LOG_LEVEL_ERROR
} Relay_LogLevel;

/** File-backed logger owned by the application. */
typedef struct Relay_Logger {
    FILE *stream;
} Relay_Logger;

/** Open the application's file-only log destination. */
bool relay_logger_init(Relay_Logger *logger);

/** Write one formatted entry to the application log. */
void relay_logger_log(Relay_Logger *logger, Relay_LogLevel level,
    const char *format, ...);

/** Flush and close the application's log destination. */
void relay_logger_shutdown(Relay_Logger *logger);

#endif
