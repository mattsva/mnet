#define _GNU_SOURCE
#include "mnet_response.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t mnet_url_decode(char *out, size_t out_size, const char *s)
{
    if (s == NULL) return 0;
    size_t pos = 0;
    for (const char *p = s; *p && pos < out_size - 1; p++) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = -1, lo = -1;
            char c1 = p[1], c2 = p[2];
            if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
            if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
            if (hi >= 0 && lo >= 0) {
                out[pos++] = (char)((hi << 4) | lo);
                p += 2;
            } else {
                out[pos++] = *p;
            }
        } else if (*p == '+') {
            out[pos++] = ' ';
        } else {
            out[pos++] = *p;
        }
    }
    out[pos] = '\0';
    return pos;
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
            char escaped[4096];
            size_t elen = mnet_json_escape(escaped, sizeof(escaped), s);
            while (pos + elen + 1 >= cap) {
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
            memcpy(buf + pos, escaped, elen);
            pos += elen;
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
