#ifndef MNET_ROUTER_H
#define MNET_ROUTER_H

#include "mnet_request.h"
#include "mnet_response.h"
#include <stddef.h>

typedef enum {
    MNET_HTTP_GET,
    MNET_HTTP_POST,
    MNET_HTTP_PUT,
    MNET_HTTP_PATCH,
    MNET_HTTP_DELETE
} mnet_http_method_t;

typedef void (*mnet_handler_legacy_t)(
    mnet_request_t *request,
    mnet_response_t *response);

typedef mnet_response_t (*mnet_handler_t)(
    mnet_request_t *req);

typedef struct mnet_route {
    mnet_http_method_t method;
    const char *path;
    mnet_handler_t handler;
    mnet_handler_legacy_t legacy_handler; /* old-style, or NULL */
    /* Extracted parameter names for this route, NULL-terminated */
    const char **param_names;
} mnet_route_t;

int mnet_route_match(
    const mnet_route_t *route,
    const char *request_path,
    const char **out_params,
    size_t max_params);

void mnet_match_params_free(const char **values, int count);

#endif
