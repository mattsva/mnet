#ifndef MNET_REQUEST_H
#define MNET_REQUEST_H

#include <stddef.h>

typedef struct mnet_request {
    const char *method;
    const char *path;

    const char *body;
    size_t body_length;
} mnet_request_t;

const char *mnet_request_method(
    const mnet_request_t *request
);

const char *mnet_request_path(
    const mnet_request_t *request
);

const char *mnet_request_body(
    const mnet_request_t *request
);

size_t mnet_request_body_length(
    const mnet_request_t *request
);

#endif
