#ifndef MNET_APP_H
#define MNET_APP_H

#include <stdint.h>
#include "mnet_router.h"

typedef struct mnet_app mnet_app_t;

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

void mnet_static(mnet_app_t *app, const char *url_prefix,
    const char *fs_path);

void mnet_set_timeout(mnet_app_t *app, int seconds);

/* Optional configuration. All values are optional; 0 means "use default". */

/* Maximum number of concurrent connections. 0 = unlimited (default). */
void mnet_set_max_connections(mnet_app_t *app, int max_connections);

/* Socket read/write timeout in seconds. 0 = 30 s default. */
void mnet_set_timeout(mnet_app_t *app, int seconds);

/* Keep-alive idle timeout in seconds. 0 = 30 s default. */
void mnet_set_keep_alive_timeout(mnet_app_t *app, int seconds);

/* Maximum request body size in bytes. 0 = 16 MB (default). */
void mnet_set_max_body_size(mnet_app_t *app, size_t max_body_size);

#endif
