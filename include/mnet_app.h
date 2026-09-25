#ifndef MNET_APP_H
#define MNET_APP_H

#include <stdint.h>
#include "mnet_router.h"

typedef struct mnet_app mnet_app_t;

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
#define MNET_GET(app, path, handler)   mnet_route((app), MNET_HTTP_GET, (path), (handler))
#define MNET_POST(app, path, handler)  mnet_route((app), MNET_HTTP_POST, (path), (handler))
#define MNET_PUT(app, path, handler)   mnet_route((app), MNET_HTTP_PUT, (path), (handler))
#define MNET_PATCH(app, path, handler) mnet_route((app), MNET_HTTP_PATCH, (path), (handler))
#define MNET_DELETE(app, path, handler) mnet_route((app), MNET_HTTP_DELETE, (path), (handler))

#define MNET_HANDLER(name) \
    static mnet_response_t name(mnet_request_t *req)

void mnet_set_debug(mnet_app_t *app, int enabled);

void mnet_set_not_found_handler(mnet_app_t *app, mnet_response_t (*handler)(mnet_request_t *req));

#endif
