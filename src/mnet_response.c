#define _GNU_SOURCE
#include "mnet_response.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
                    /* \uXXXX - keep simple: just skip for now */
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
    char buf[65536];
    size_t pos = 0;
    const char *f = format;

    while (*f && pos < sizeof(buf) - 1) {
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
            /* JSON-escape the string into the buffer */
            char escaped[4096];
            size_t elen = mnet_json_escape(escaped, sizeof(escaped), s);
            if (pos + elen < sizeof(buf)) {
                memcpy(buf + pos, escaped, elen);
                pos += elen;
            } else {
                pos = sizeof(buf) - 1;
            }
            continue;
        }

        /* Other conversion: collect the full specifier and use vsnprintf */
        const char *spec_start = f - 1; /* point to '%' */
        const char *p = f;
        while (*p && *p != '%') {
            /* conversion characters (not 's', which we handled) */
            if (strchr("diouxXfFeEgGaAcCpPn", *p)) break;
            /* flag / width / precision / modifier chars keep going */
            p++;
        }

        /* Build the one-specifier format string */
        size_t spec_len = (size_t)(p - spec_start) + 1;
        char spec[64];
        if (spec_len < sizeof(spec)) {
            memcpy(spec, spec_start, spec_len);
            spec[spec_len] = '\0';

            char val[256];
            int vlen = vsnprintf(val, sizeof(val), spec, args);
            if (vlen > 0 && pos + (size_t)vlen < sizeof(buf)) {
                memcpy(buf + pos, val, (size_t)vlen);
                pos += (size_t)vlen;
            } else if (vlen > 0) {
                pos = sizeof(buf) - 1;
            }
        }
        f = p + 1; /* advance past conversion character */
    }

    buf[pos] = '\0';

    /* Heap-allocate so the response owns its body */
    char *owned = strdup(buf);
    if (owned == NULL) owned = strdup("null");

    mnet_response_t r = {
        .status = 200,
        .content_type = "application/json",
        .body = owned,
        .body_length = strlen(owned)
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
    if (message == NULL) message = "";
    size_t len = strlen(message);
    char *copy = strdup(message);
    if (copy == NULL) copy = strdup("");
    mnet_response_t r = {
        .status = status,
        .content_type = "text/plain; charset=utf-8",
        .body = copy,
        .body_length = len
    };
    return r;
}

mnet_response_t mnet_status(int status, const char *body)
{
    if (body == NULL) body = "";
    size_t len = strlen(body);
    char *copy = strdup(body);
    if (copy == NULL) copy = strdup("");
    mnet_response_t r = {
        .status = status,
        .content_type = "text/plain; charset=utf-8",
        .body = copy,
        .body_length = len
    };
    return r;
}
