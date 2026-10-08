#define _GNU_SOURCE
#include "mnet_response.h"
#include "mnet_compat.h"

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
    if (out_size == 0) return 0;

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
    return pos;
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
            .status = 500,
            .content_type = "application/json",
            .body = strdup("null"),
            .body_length = 4,
        };
        return r;
    }

    if (format == NULL) {
        free(buf);
        mnet_response_t r = {
            .status = 500,
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
                    .status = 500,
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

        /* Reject trailing lone '%' */
        if (*f == '\0') {
            free(buf);
            mnet_response_t r = {
                .status = 500,
                .content_type = "application/json",
                .body = strdup("null"),
                .body_length = 4,
            };
            return r;
        }

        /* Reject '*' (dynamic width) */
        if (*f == '*') {
            free(buf);
            mnet_response_t r = {
                .status = 500,
                .content_type = "application/json",
                .body = strdup("null"),
                .body_length = 4,
            };
            return r;
        }

        /* Check for precision on %s: %.Ns */
        if (*f == '.') {
            const char *pf = f + 1;
            while (*pf >= '0' && *pf <= '9') pf++;
            if (*pf == 's') {
                /* Precision on %s: truncate then escape */
                int precision = 0;
                const char *pn = f + 1;
                while (*pn >= '0' && *pn <= '9') {
                    precision = precision * 10 + (*pn - '0');
                    pn++;
                }
                f = pf + 1; /* skip past 's' */
                const char *s = va_arg(args, const char *);
                if (s == NULL) s = "(null)";
                size_t slen = strlen(s);
                if ((size_t)precision < slen) {
                    /* Don't split UTF-8 sequences: if the byte at the
                       truncation point is a continuation byte (0x80-0xBF),
                       back up to the start of the sequence. */
                    size_t trunc = (size_t)precision;
                    while (trunc > 0 && ((unsigned char)s[trunc] & 0xC0) == 0x80)
                        trunc--;
                    if (trunc > 0 && ((unsigned char)s[trunc] & 0x80) != 0) {
                        /* The byte at trunc is a lead byte; check if the
                           sequence fits within the precision. */
                        int seq_len = 0;
                        unsigned char b = (unsigned char)s[trunc];
                        if ((b & 0xE0) == 0xC0) seq_len = 2;
                        else if ((b & 0xF0) == 0xE0) seq_len = 3;
                        else if ((b & 0xF8) == 0xF0) seq_len = 4;
                        if (seq_len > 0 && trunc + (size_t)seq_len > (size_t)precision)
                            trunc = 0; /* sequence doesn't fit, return empty */
                    }
                    slen = trunc;
                }
                size_t escaped_cap = slen * 6 + 1;
                char *escaped = malloc(escaped_cap);
                if (escaped == NULL) {
                    free(buf);
                    mnet_response_t r = {
                        .status = 500,
                        .content_type = "application/json",
                        .body = strdup("null"),
                        .body_length = 4,
                    };
                    return r;
                }
                /* Create a temporary buffer with just the truncated portion */
                char *truncated = malloc(slen + 1);
                if (truncated == NULL) {
                    free(buf);
                    mnet_response_t r = {
                        .status = 500,
                        .content_type = "application/json",
                        .body = strdup("null"),
                        .body_length = 4,
                    };
                    return r;
                }
                memcpy(truncated, s, slen);
                truncated[slen] = '\0';
                size_t elen = mnet_json_escape(escaped, escaped_cap, truncated);
                free(truncated);
                while (pos + elen + 1 >= cap) {
                    cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (nb == NULL) {
                        free(escaped);
                        free(buf);
                        mnet_response_t r = {
                            .status = 500,
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
            /* Precision on non-%s: fall through to general handling */
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
                    .status = 500,
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
                        .status = 500,
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

        /* %c: single character with JSON escaping */
        if (*f == 'c') {
            f++;
            int c = va_arg(args, int);
            char escaped[16];
            size_t elen;
            if (c == '"') {
                memcpy(escaped, "\\\"", 2);
                elen = 2;
            } else if (c == '\\') {
                memcpy(escaped, "\\\\", 2);
                elen = 2;
            } else if (c == '\n') {
                memcpy(escaped, "\\n", 2);
                elen = 2;
            } else if (c == '\r') {
                memcpy(escaped, "\\r", 2);
                elen = 2;
            } else if (c == '\t') {
                memcpy(escaped, "\\t", 2);
                elen = 2;
            } else if (c == 0) {
                memcpy(escaped, "\\u0000", 6);
                elen = 6;
            } else if (c < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)c);
                elen = 6;
            } else {
                escaped[0] = (char)c;
                elen = 1;
            }
            while (pos + elen + 1 >= cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (nb == NULL) {
                    free(buf);
                    mnet_response_t r = {
                        .status = 500,
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

        /* Reject truncated specifiers (no conversion char found) */
        if (*p == '\0') {
            free(buf);
            mnet_response_t r = {
                .status = 500,
                .content_type = "application/json",
                .body = strdup("null"),
                .body_length = 4,
            };
            return r;
        }

        size_t spec_len = (size_t)(p - spec_start) + 1;
        char spec[64];
        if (spec_len < sizeof(spec)) {
            memcpy(spec, spec_start, spec_len);
            spec[spec_len] = '\0';

            /* Reject dangerous specifiers: %n, %p, %a, '*' width, and any
               modifier on %s (flags, width, precision, length). */
            const char *scan = spec + 1; /* skip '%' */
            int reject = 0;
            while (*scan && *scan != '%') {
                if (strchr("0123456789+-# *.", *scan)) {
                    /* flags, width, precision are only safe on non-%s */
                    if (strchr("diouxXfFeEgGaA", *p)) {
                        /* allow flags/width/precision on numeric conversions */
                    } else {
                        reject = 1;
                        break;
                    }
                }
                if (*scan == 'n' || *scan == 'p' || *scan == 'a') {
                    reject = 1;
                    break;
                }
                if (*scan == 'l' || *scan == 'h' || *scan == 'z' ||
                    *scan == 'j' || *scan == 't' || *scan == 'L') {
                    /* length modifiers: allow on diouxX and fFeEgGaA */
                    if (!strchr("diouxXfFeEgGaA", *p)) {
                        reject = 1;
                        break;
                    }
                }
                scan++;
            }
            /* Also reject if the conversion char itself is n, p, or a */
            if (*p == 'n' || *p == 'p' || *p == 'a') reject = 1;

            if (reject) {
                free(buf);
                mnet_response_t r = {
                    .status = 500,
                    .content_type = "application/json",
                    .body = strdup("null"),
                    .body_length = 4,
                };
                return r;
            }

            /* Reject absurd widths that would produce huge outputs */
            {
                const char *w = spec + 1;
                long width = 0;
                while (*w >= '0' && *w <= '9') {
                    width = width * 10 + (*w - '0');
                    w++;
                }
                if (width > 4096) {
                    free(buf);
                    mnet_response_t r = {
                        .status = 500,
                        .content_type = "application/json",
                        .body = strdup("null"),
                        .body_length = 4,
                    };
                    return r;
                }
            }

            /*
             * Consume the argument with va_arg into a correctly-typed value,
             * then format that value. We must NOT pass the shared va_list to
             * vsnprintf here: mixing manual va_arg() (used by the %s/%c paths
             * above) with vsnprintf(va_list) on the same va_list is undefined
             * and corrupts the list on the macOS and Windows variadic ABIs,
             * which crashed the next conversion. Reading the value ourselves
             * keeps every argument consumed through va_arg exactly once.
             */
            char conv = *p;
            int is_float = strchr("fFeEgGaA", conv) != NULL;
            char stack_val[64];
            char *val = stack_val;
            size_t val_cap = sizeof(stack_val);
            int vlen = -1;
            /* spec is a local copy; p points into the original format, so use
               the conversion char's offset within spec, not p itself. */
            const char *conv_in_spec = spec + spec_len - 1;

            if (is_float) {
                /*
                 * Format the value with its own width: promote to long double
                 * only when the caller used %L (matching the type read). For a
                 * plain %f the argument is a double; formatting it directly
                 * avoids a double->long double promotion that misrenders
                 * infinity under some FP environments.
                 */
                char nspec[64];
                size_t si = 0;
                int has_L = strchr(spec, 'L') != NULL;
                if (has_L) {
                    long double dv = va_arg(args, long double);
                    nspec[si++] = '%';
                    for (const char *q = spec + 1; q < conv_in_spec; q++) {
                        if (*q == 'L') continue;
                        nspec[si++] = *q;
                    }
                    nspec[si++] = 'L';
                    nspec[si++] = conv;
                    nspec[si] = '\0';
                    int need = snprintf(NULL, 0, nspec, dv);
                    if (need < 0) need = 0;
                    if ((size_t)need + 1 > val_cap) {
                        val = malloc((size_t)need + 1);
                        val_cap = (size_t)need + 1;
                        if (val == NULL) {
                            free(buf);
                            mnet_response_t r = {
                                .status = 500,
                                .content_type = "application/json",
                                .body = strdup("null"),
                                .body_length = 4,
                            };
                            return r;
                        }
                    }
                    vlen = snprintf(val, val_cap, nspec, dv);
                } else {
                    double dv = va_arg(args, double);
                    /* spec already matches the double (e.g. %.2f, %f, %g). */
                    int need = snprintf(NULL, 0, spec, dv);
                    if (need < 0) need = 0;
                    if ((size_t)need + 1 > val_cap) {
                        val = malloc((size_t)need + 1);
                        val_cap = (size_t)need + 1;
                        if (val == NULL) {
                            free(buf);
                            mnet_response_t r = {
                                .status = 500,
                                .content_type = "application/json",
                                .body = strdup("null"),
                                .body_length = 4,
                            };
                            return r;
                        }
                    }
                    vlen = snprintf(val, val_cap, spec, dv);
                }
            } else {
                unsigned long long uv;
                int is_signed = strchr("di", conv) != NULL;
                /* Determine the length modifier present in the spec. */
                int mod_l = strstr(spec, "ll") != NULL;
                int mod_l1 = !mod_l && strchr(spec, 'l') != NULL;
                int mod_h = strstr(spec, "hh") != NULL;
                int mod_h1 = !mod_h && strchr(spec, 'h') != NULL;
                int mod_z = strchr(spec, 'z') != NULL;
                int mod_j = strchr(spec, 'j') != NULL;
                int mod_t = strchr(spec, 't') != NULL;

                /*
                 * Normalize the format to the (un)signed long long we read into:
                 * keep flags/width/precision, drop the original length modifier,
                 * force "ll". This avoids a type mismatch between the spec's
                 * modifier (e.g. %hhd, %zu) and the long long we pass.
                 */
                char nspec[64];
                size_t si = 0;
                nspec[si++] = '%';
                for (const char *q = spec + 1; q < conv_in_spec; q++) {
                    if (strchr("lhzjt", *q)) continue; /* drop length modifiers */
                    nspec[si++] = *q;
                }
                nspec[si++] = 'l';
                nspec[si++] = 'l';
                nspec[si++] = conv;
                nspec[si] = '\0';

                if (is_signed) {
                    long long sv;
                    if (mod_l) sv = va_arg(args, long long);
                    else if (mod_l1) sv = va_arg(args, long);
                    else if (mod_h) sv = (signed char)va_arg(args, int);
                    else if (mod_h1) sv = (short)va_arg(args, int);
                    else if (mod_z) sv = (long long)va_arg(args, ssize_t);
                    else if (mod_j) sv = va_arg(args, long long);
                    else if (mod_t) sv = (long long)va_arg(args, ptrdiff_t);
                    else sv = va_arg(args, int);
                    vlen = snprintf(val, val_cap, nspec, sv);
                    (void)uv;
                } else {
                    if (mod_l) uv = va_arg(args, unsigned long long);
                    else if (mod_l1) uv = va_arg(args, unsigned long);
                    else if (mod_h) uv = (unsigned char)va_arg(args, unsigned int);
                    else if (mod_h1) uv = (unsigned short)va_arg(args, unsigned int);
                    else if (mod_z) uv = va_arg(args, size_t);
                    else if (mod_j) uv = va_arg(args, unsigned long long);
                    else if (mod_t) uv = (unsigned long long)va_arg(args, ptrdiff_t);
                    else uv = va_arg(args, unsigned int);
                    vlen = snprintf(val, val_cap, nspec, uv);
                }
            }

            if (vlen > 0) {
                size_t vlen_sz = (size_t)vlen;
                /* NaN and Inf are not valid JSON: replace with null */
                if (is_float &&
                    (strstr(val, "nan") || strstr(val, "inf") ||
                     strstr(val, "NAN") || strstr(val, "INF"))) {
                    memcpy(val, "null", 5);
                    vlen_sz = 4;
                }

                while (pos + vlen_sz + 1 >= cap) {
                    cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (nb == NULL) {
                        if (val != stack_val) free(val);
                        free(buf);
                        mnet_response_t r = {
                            .status = 500,
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
            if (val != stack_val) free(val);
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
