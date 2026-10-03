#ifndef MNET_APP_H
#define MNET_APP_H

#include <stdint.h>
#include <stdarg.h>
#include "mnet_router.h"

typedef struct mnet_app mnet_app_t;

/*
 * Log levels passed to a log handler.
 *
 * MNET_LOG_ERROR   - a request was rejected or an operation failed
 * MNET_LOG_WARN    - something suspicious or recoverable happened
 * MNET_LOG_INFO    - lifecycle events (listening, stopped)
 * MNET_LOG_DEBUG   - per-request detail, only when debug is enabled
 */
#define MNET_LOG_ERROR 0
#define MNET_LOG_WARN  1
#define MNET_LOG_INFO  2
#define MNET_LOG_DEBUG 3

/*
 * Log callback.
 *
 * Called with a printf-style format string and its arguments. The handler is
 * responsible for formatting and for the trailing newline. It is invoked from
 * whichever thread is serving the request, so an implementation must be
 * thread-safe if the server runs with more than one worker.
 */
typedef void (*mnet_log_handler_t)(int level, const char *fmt, ...);

typedef mnet_response_t (*mnet_middleware_t)(
    mnet_request_t *req,
    mnet_response_t (*next)(mnet_request_t *));

mnet_app_t *mnet_create(void);

void mnet_destroy(mnet_app_t *app);
int mnet_run(mnet_app_t *app, uint16_t port);
int mnet_stop(mnet_app_t *app);

int mnet_route(
    mnet_app_t *app,
    mnet_http_method_t method,
    const char *path,
    mnet_handler_t handler);

// Those are Macros for easier route registration, e.g. MNET_GET(app, "/path", handler)
#define MNET_GET(app, path, handler)     mnet_route((app), MNET_HTTP_GET, (path), (handler))
#define MNET_POST(app, path, handler)    mnet_route((app), MNET_HTTP_POST, (path), (handler))
#define MNET_PUT(app, path, handler)     mnet_route((app), MNET_HTTP_PUT, (path), (handler))
#define MNET_PATCH(app, path, handler)   mnet_route((app), MNET_HTTP_PATCH, (path), (handler))
#define MNET_DELETE(app, path, handler)  mnet_route((app), MNET_HTTP_DELETE, (path), (handler))
#define MNET_HEAD(app, path, handler)    mnet_route((app), MNET_HTTP_HEAD, (path), (handler))
#define MNET_OPTIONS(app, path, handler) mnet_route((app), MNET_HTTP_OPTIONS, (path), (handler))

#define MNET_HANDLER(name) \
    static mnet_response_t name(mnet_request_t *req)

void mnet_set_debug(mnet_app_t *app, int enabled);

void mnet_set_not_found_handler(mnet_app_t *app, mnet_response_t (*handler)(mnet_request_t *req));

void mnet_use(mnet_app_t *app, mnet_middleware_t middleware);

/*
 * Serve static files from fs_path under url_prefix, e.g.
 *
 *     mnet_static(app, "/static", "/var/www");
 *
 * Path resolution:
 *   The configured root and the requested path are both resolved with
 *   realpath() (POSIX) / _fullpath() (Windows) before the file is opened, so
 *   "..", "." and symlinks are all collapsed to a canonical absolute path. The
 *   resolved target must equal the resolved root or lie beneath it, compared on
 *   a path-separator boundary so that a sibling such as /var/www2 is not
 *   accepted as being inside /var/www. Anything else is refused with 403 and a
 *   traversal attempt is logged. A path that cannot be resolved (including a
 *   missing file) yields 404.
 *
 *   Because the check happens on the resolved path and the file is opened by
 *   that resolved path, a symlink that leaves the root is rejected too.
 */
void mnet_static(mnet_app_t *app, const char *url_prefix,
    const char *fs_path);

/*
 * Set the log callback. Without one, messages go to stderr.
 */
void mnet_set_log_handler(mnet_app_t *app, mnet_log_handler_t handler);

/*
 * Socket read/write timeout in seconds for client connections.
 *
 * A bounded timeout is what stops a client from holding a connection open
 * forever without sending anything (Slowloris). The default is 30 seconds.
 * Passing 0 restores that default; passing a negative value disables the
 * timeout entirely, which is strongly discouraged in production because it
 * lets a single idle client pin a worker indefinitely. Prefer a positive
 * value.
 */
void mnet_set_timeout(mnet_app_t *app, int seconds);

/*
 * Number of worker threads.
 *
 * 0 or 1 runs the single-threaded blocking loop (the default, and the only
 * mode on a build without threads). A value above 1 serves that many
 * connections concurrently: the accept loop hands each connection to a worker
 * from a fixed-size pool. Handlers therefore run on several threads at once,
 * so they must not share mutable state without their own synchronisation.
 */
void mnet_set_workers(mnet_app_t *app, int workers);

/* Optional configuration. All values are optional; 0 means "use default". */

/* Maximum number of concurrent connections. 0 = unlimited (default). */
void mnet_set_max_connections(mnet_app_t *app, int max_connections);

/* Keep-alive idle timeout in seconds. 0 = 30 s default. */
void mnet_set_keep_alive_timeout(mnet_app_t *app, int seconds);

/* Maximum request body size in bytes. 0 = 16 MB (default). */
void mnet_set_max_body_size(mnet_app_t *app, size_t max_body_size);

#endif
