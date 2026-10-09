/* src/mnet_log.c - Logging. */

#define _GNU_SOURCE
#include "mnet_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Process-wide log handler.
 *
 * The handler is set per app but messages can be emitted before an app exists
 * (listener setup) and from worker threads, so a single process-wide pointer
 * is kept as well. It is written before any worker starts and only read
 * afterwards, so no locking is needed.
 */
static mnet_log_handler_t g_log_handler = NULL;

void mnet_log_msg(int level, const char *fmt, ...)
{
    va_list args;

    if (g_log_handler != NULL) {
        char buf[1024];

        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        g_log_handler(level, "%s", buf);
        return;
    }

    const char *tag = "INFO";
    switch (level) {
        case MNET_LOG_ERROR: tag = "ERROR"; break;
        case MNET_LOG_WARN:  tag = "WARN";  break;
        case MNET_LOG_INFO:  tag = "INFO";  break;
        case MNET_LOG_DEBUG: tag = "DEBUG"; break;
        default: break;
    }

    va_start(args, fmt);
    fprintf(stderr, "[mnet] %s: ", tag);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

void mnet_app_log(mnet_app_t *app, int level, const char *fmt, ...)
{
    va_list args;
    char buf[1024];

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (app != NULL && app->log_handler != NULL) {
        app->log_handler(level, "%s", buf);
        return;
    }
    mnet_log_msg(level, "%s", buf);
}

/* Internal setter so mnet_set_log_handler() (mnet_app.c) can install the
 * process-wide handler that mnet_log_msg() consults. */
void mnet_log_set_handler(mnet_log_handler_t handler)
{
    g_log_handler = handler;
}
