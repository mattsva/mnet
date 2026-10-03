#ifndef MNET_RESPONSE_H
#define MNET_RESPONSE_H

#include <stddef.h>
#include <stdarg.h>

typedef struct mnet_response {
    int status;
    const char *content_type;
    const void *body;
    size_t body_length;
    int chunked;
} mnet_response_t;

mnet_response_t mnet_text(const char *text);
mnet_response_t mnet_html(const char *html);
mnet_response_t mnet_json(const char *json);
mnet_response_t mnet_jsonf(const char *format, ...);
mnet_response_t mnet_jsonfv(const char *format, va_list args);
mnet_response_t mnet_error(int status, const char *message);
mnet_response_t mnet_status(int status, const char *body);
mnet_response_t mnet_chunked(int status, const char *content_type,
    const void *body, size_t body_length);

void mnet_response_free(mnet_response_t *response);

size_t mnet_url_decode(char *out, size_t out_size, const char *s);

/*
 * Bounds-checked URL decoder.
 *
 * Decodes exactly src_len bytes from src into dst. Unlike mnet_url_decode(),
 * this takes an explicit source length (so it can be used on non-terminated
 * buffers) and reports failures instead of truncating.
 *
 * Returns the number of decoded bytes written (excluding the terminating NUL),
 * or -1 on failure: NULL arguments, zero dst_size, a malformed or truncated
 * percent escape, a %00 escape, or output that would not fit in dst_size.
 * On failure the contents of dst are unspecified. The output is always
 * NUL-terminated on success.
 */
int mnet_url_decode_ex(const char *src, size_t src_len, char *dst,
    size_t dst_size);

/*
 * Bounds-checked URL decoder with an explicit destination capacity.
 *
 * Decodes the NUL-terminated src into dst, never writing more than dst_size
 * bytes including the terminating NUL. It fails rather than truncating: if the
 * decoded result (or any percent escape) does not fit, nothing is written and
 * the return value is negative.
 *
 * Returns the decoded length (excluding the NUL) on success, or -1 on failure:
 * NULL arguments, zero dst_size, a malformed or truncated escape, a %00 escape,
 * or a result that does not fit.
 *
 * This is the same operation as mnet_url_decode_ex() with the source length
 * taken from strlen(); it exists because the (dst, dst_size, src) argument
 * order reads more naturally at call sites that already know the capacity.
 */
int mnet_url_decode_safe(const char *src, char *dst, size_t dst_size);

/*
 * Validate an HTTP header value taken from untrusted input.
 * Returns 1 if the value is safe to emit, 0 if it contains CR, LF or other
 * control characters (which could allow header injection / response splitting).
 * A NULL value is treated as valid (nothing to emit).
 */
int mnet_header_value_valid(const char *value);

/*
 * Validate an HTTP header name against the RFC 7230 token grammar.
 * Returns 1 if valid, 0 otherwise (including NULL or empty).
 */
int mnet_header_name_valid(const char *name);

size_t mnet_json_escape(
    char *out,
    size_t out_size,
    const char *s);

#endif
