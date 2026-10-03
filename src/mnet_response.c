#define _GNU_SOURCE
#include "mnet_response.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int mnet_url_decode_ex(const char *src, size_t src_len, char *dst,
    size_t dst_size)
{
    size_t i;
    size_t pos = 0;

    if (src == NULL || dst == NULL || dst_size == 0) return -1;

    for (i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        unsigned char out;

        if (c == '%') {
            int hi, lo;

            if (i + 2 >= src_len) return -1; /* truncated escape */
            hi = hex_value(src[i + 1]);
            lo = hex_value(src[i + 2]);
            if (hi < 0 || lo < 0) return -1; /* malformed escape */
            out = (unsigned char)((hi << 4) | lo);
            i += 2;
        } else if (c == '+') {
            out = ' ';
        } else {
            out = c;
        }

        /* A NUL escape would silently truncate the decoded value. */
        if (out == '\0') return -1;

        /* Refuse rather than truncate. */
        if (pos + 1 >= dst_size) return -1;

        dst[pos++] = (char)out;
    }

    dst[pos] = '\0';
    return (int)pos;
}

int mnet_url_decode_safe(const char *src, char *dst, size_t dst_size)
{
    if (src == NULL) return -1;
    return mnet_url_decode_ex(src, strlen(src), dst, dst_size);
}

size_t mnet_url_decode(char *out, size_t out_size, const char *s)
{
    int n;

    if (out == NULL || out_size == 0 || s == NULL) return 0;

    n = mnet_url_decode_ex(s, strlen(s), out, out_size);
    if (n < 0) {
        /* Malformed or NUL escape, or the result would not fit: yield an
           empty string rather than a partially decoded one, so callers never
           act on a truncated value. */
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

int mnet_header_value_valid(const char *value)
{
    if (value == NULL) return 1;

    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        /* Reject CR, LF and other control characters that could split a
           response or smuggle a header. */
        if (*p == '\r' || *p == '\n' || *p < 0x20 || *p == 0x7f) return 0;
    }
    return 1;
}

int mnet_header_name_valid(const char *name)
{
    if (name == NULL || *name == '\0') return 0;

    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        unsigned char c = *p;

        /* RFC 7230 token characters only. */
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') ||
                 strchr("!#$%&'*+-.^_`|~", c) != NULL;
        if (!ok) return 0;
    }
    return 1;
}

size_t mnet_json_escape(
    char *out,
    size_t out_size,
    const char *s)
{
    if (s == NULL) s = "";
    size_t len = 0;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  len += 2; break;
            case '\\': len += 2; break;
            case '\n': len += 2; break;
            case '\r': len += 2; break;
            case '\t': len += 2; break;
            default:
                if ((unsigned char)*p < 0x20) len += 6; /* \uXXXX */
                else len += 1;
                break;
        }
    }
    /* account for trailing NUL */
    if (out_size == 0) return len;

    size_t pos = 0;
    for (const char *p = s; *p && pos < out_size - 1; p++) {
        switch (*p) {
            case '"':
                if (pos + 2 <= out_size - 1) {
                    out[pos++] = '\\';
                    out[pos++] = '"';
                }
                break;
            case '\\':
                if (pos + 2 <= out_size - 1) {
                    out[pos++] = '\\';
                    out[pos++] = '\\';
                }
                break;
            case '\n':
                if (pos + 2 <= out_size - 1) {
                    out[pos++] = '\\';
                    out[pos++] = 'n';
                }
                break;
            case '\r':
                if (pos + 2 <= out_size - 1) {
                    out[pos++] = '\\';
                    out[pos++] = 'r';
                }
                break;
            case '\t':
                if (pos + 2 <= out_size - 1) {
                    out[pos++] = '\\';
                    out[pos++] = 't';
                }
                break;
            default:
                if ((unsigned char)*p < 0x20) {
                    if (pos + 6 <= out_size - 1) {
                        pos += snprintf(out + pos, 7, "\\u%04x", (unsigned char)*p);
                    }
                } else {
                    out[pos++] = *p;
                }
                break;
        }
    }
    out[pos] = '\0';
    return len;
}

mnet_response_t mnet_text(const char *text)
{
    if (text == NULL) {
        mnet_response_t r = {0};
        return r;
    }
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        mnet_response_t r = {0};
        return r;
    }
    memcpy(copy, text, len + 1);
    mnet_response_t r = {
        .status = 200,
        .content_type = "text/plain; charset=utf-8",
        .body = copy,
        .body_length = len
    };
    return r;
}

mnet_response_t mnet_html(const char *html)
{
    if (html == NULL) {
        mnet_response_t r = {0};
        return r;
    }
    size_t len = strlen(html);
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        mnet_response_t r = {0};
        return r;
    }
    memcpy(copy, html, len + 1);
    mnet_response_t r = {
        .status = 200,
        .content_type = "text/html; charset=utf-8",
        .body = copy,
        .body_length = len
    };
    return r;
}

mnet_response_t mnet_json(const char *json)
{
    if (json == NULL) {
        mnet_response_t r = {0};
        return r;
    }
    size_t len = strlen(json);
    char *copy = strdup(json);
    if (copy == NULL) {
        mnet_response_t r = {0};
        return r;
    }
    mnet_response_t r = {
        .status = 200,
        .content_type = "application/json",
        .body = copy,
        .body_length = len
    };
    return r;
}


mnet_response_t mnet_jsonfv(const char *format, va_list args)
{
    size_t cap = 256;
    size_t pos = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        mnet_response_t r = {
            .status = 200,
            .content_type = "application/json",
            .body = strdup("null"),
            .body_length = 4,
        };
        return r;
    }

    const char *f = format;
    while (*f) {
        if (pos + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                mnet_response_t r = {
                    .status = 200,
                    .content_type = "application/json",
                    .body = strdup("null"),
                    .body_length = 4,
                };
                return r;
            }
            buf = nb;
        }

        if (*f != '%') {
            buf[pos++] = *f++;
            continue;
        }

        f++; /* skip '%' */

        if (*f == '%') {
            buf[pos++] = '%';
            f++;
            continue;
        }

        if (*f == 's') {
            f++; /* skip 's' */
            const char *s = va_arg(args, const char *);
            if (s == NULL) s = "(null)";
            size_t slen = strlen(s);
            size_t escaped_cap = slen * 6 + 1;
            char *escaped = malloc(escaped_cap);
            if (escaped == NULL) {
                free(buf);
                mnet_response_t r = {
                    .status = 200,
                    .content_type = "application/json",
                    .body = strdup("null"),
                    .body_length = 4,
                };
                return r;
            }
            size_t elen = mnet_json_escape(escaped, escaped_cap, s);
            while (pos + elen + 1 >= cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (nb == NULL) {
                    free(escaped);
                    free(buf);
                    mnet_response_t r = {
                        .status = 200,
                        .content_type = "application/json",
                        .body = strdup("null"),
                        .body_length = 4,
                    };
                    return r;
                }
                buf = nb;
            }
            memcpy(buf + pos, escaped, elen);
            pos += elen;
            free(escaped);
            continue;
        }

        /* Other conversion: collect the full specifier and use vsnprintf */
        const char *spec_start = f - 1; /* point to '%' */
        const char *p = f;
        while (*p && *p != '%') {
            if (strchr("diouxXfFeEgGaAcCpPn", *p)) break;
            p++;
        }

        size_t spec_len = (size_t)(p - spec_start) + 1;
        char spec[64];
        if (spec_len < sizeof(spec)) {
            memcpy(spec, spec_start, spec_len);
            spec[spec_len] = '\0';

            char val[256];
            int vlen = vsnprintf(val, sizeof(val), spec, args);
            if (vlen > 0) {
                size_t vlen_sz = (size_t)vlen;
                while (pos + vlen_sz + 1 >= cap) {
                    cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (nb == NULL) {
                        free(buf);
                        mnet_response_t r = {
                            .status = 200,
                            .content_type = "application/json",
                            .body = strdup("null"),
                            .body_length = 4,
                        };
                        return r;
                    }
                    buf = nb;
                }
                memcpy(buf + pos, val, vlen_sz);
                pos += vlen_sz;
            }
        }
        f = p + 1; /* advance past conversion character */
    }

    buf[pos] = '\0';

    mnet_response_t r = {
        .status = 200,
        .content_type = "application/json",
        .body = buf,
        .body_length = pos,
    };
    return r;
}

mnet_response_t mnet_jsonf(const char *format, ...)
{
    mnet_response_t r;
    va_list args;
    va_start(args, format);
    r = mnet_jsonfv(format, args);
    va_end(args);
    return r;
}

mnet_response_t mnet_error(int status, const char *message)
{
    mnet_response_t r = {0};

    if (message == NULL) message = "";
    r.status = status;
    r.content_type = "text/plain; charset=utf-8";

    size_t len = strlen(message);
    char *copy = malloc(len + 1);
    if (copy == NULL) return r; /* status 0 -> 500 by the server */
    memcpy(copy, message, len + 1);
    r.body = copy;
    r.body_length = len;
    return r;
}

mnet_response_t mnet_status(int status, const char *body)
{
    mnet_response_t r = {0};

    if (body == NULL) body = "";
    r.status = status;
    r.content_type = "text/plain; charset=utf-8";

    size_t len = strlen(body);
    char *copy = malloc(len + 1);
    if (copy == NULL) return r;
    memcpy(copy, body, len + 1);
    r.body = copy;
    r.body_length = len;
    return r;
}

mnet_response_t mnet_chunked(int status, const char *content_type,
    const void *body, size_t body_length)
{
    mnet_response_t r = {
        .status = status,
        .content_type = content_type ? content_type : "application/octet-stream",
        .body = body,
        .body_length = body_length,
        .chunked = 1,
    };
    return r;
}

void mnet_response_free(mnet_response_t *response)
{
    if (response == NULL) return;
    if (response->body != NULL && !response->chunked) {
        free((void *)response->body);
        response->body = NULL;
    }
    response->body_length = 0;
}
