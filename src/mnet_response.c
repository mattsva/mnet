#include "mnet_response.h"

#include <string.h>

void mnet_response_status(
    mnet_response_t *response,
    int status
)
{
    if (response == NULL) {
        return;
    }

    response->status = status;
}

void mnet_response_text(
    mnet_response_t *response,
    const char *text
)
{
    if (response == NULL) {
        return;
    }

    if (text == NULL) {
        response->body = NULL;
        response->body_length = 0;
        return;
    }

    response->body = text;
    response->body_length = strlen(text);
    response->content_type = "text/plain; charset=utf-8";
}

void mnet_response_body(
    mnet_response_t *response,
    const void *data,
    size_t length
)
{
    if (response == NULL) {
        return;
    }

    if (data == NULL && length != 0) {
        return;
    }

    response->body = data;
    response->body_length = length;
}

void mnet_response_content_type(
    mnet_response_t *response,
    const char *content_type
)
{
    if (response == NULL) {
        return;
    }

    response->content_type = content_type;
}
