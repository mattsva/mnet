#ifndef MNET_RESPONSE_H
#define MNET_RESPONSE_H

#include <stddef.h>
#include <stdarg.h>

typedef struct mnet_response {
    int status;
    const char *content_type;
    const void *body;
    size_t body_length;
} mnet_response_t;

mnet_response_t mnet_text(const char *text);
mnet_response_t mnet_html(const char *html);
mnet_response_t mnet_json(const char *json);
mnet_response_t mnet_jsonf(const char *format, ...);
mnet_response_t mnet_jsonfv(const char *format, va_list args);
mnet_response_t mnet_error(int status, const char *message);
mnet_response_t mnet_status(int status, const char *body);

size_t mnet_json_escape(
    char *out,
    size_t out_size,
    const char *s);

#endif
