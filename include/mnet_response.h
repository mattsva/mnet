#ifndef MNET_RESPONSE_H
#define MNET_RESPONSE_H

#include <stddef.h>

typedef struct mnet_response {
    int status;

    const char *content_type;

    const void *body;
    size_t body_length;
} mnet_response_t;

void mnet_response_status(
    mnet_response_t *response,
    int status
);

void mnet_response_text(
    mnet_response_t *response,
    const char *text
);

void mnet_response_body(
    mnet_response_t *response,
    const void *data,
    size_t length
);

void mnet_response_content_type(
    mnet_response_t *response,
    const char *content_type
);

#endif
