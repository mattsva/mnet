#ifndef MNET_ROUTER_H
#define MNET_ROUTER_H

#include "mnet_request.h"
#include "mnet_response.h"

typedef void (*mnet_handler_t)(
    mnet_request_t *request,
    mnet_response_t *response
);

typedef enum {
    MNET_METHOD_GET,
    MNET_METHOD_POST,
    MNET_METHOD_PUT,
    MNET_METHOD_DELETE
} mnet_method_t;

typedef struct mnet_route {
    mnet_method_t method;
    const char *path;
    mnet_handler_t handler;
} mnet_route_t;

#endif
