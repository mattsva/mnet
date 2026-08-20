#include "mnet_request.h"

const char *mnet_request_method(
    const mnet_request_t *request
)
{
    if (request == NULL) {
        return NULL;
    }

    return request->method;
}

const char *mnet_request_path(
    const mnet_request_t *request
)
{
    if (request == NULL) {
        return NULL;
    }

    return request->path;
}

const char *mnet_request_body(
    const mnet_request_t *request
)
{
    if (request == NULL) {
        return NULL;
    }

    return request->body;
}

size_t mnet_request_body_length(
    const mnet_request_t *request
)
{
    if (request == NULL) {
        return 0;
    }

    return request->body_length;
}
