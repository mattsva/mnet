/* src/mnet_parse.c - HTTP request parsing: request line, headers, query
 * string, cookies and body. */

#define _GNU_SOURCE
#include "mnet_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mnet_http_method_t mnet_parse_method(const char *method)
{
    if (strcmp(method, "GET")     == 0) return MNET_HTTP_GET;
    if (strcmp(method, "POST")    == 0) return MNET_HTTP_POST;
    if (strcmp(method, "PUT")     == 0) return MNET_HTTP_PUT;
    if (strcmp(method, "PATCH")   == 0) return MNET_HTTP_PATCH;
    if (strcmp(method, "DELETE")  == 0) return MNET_HTTP_DELETE;
    if (strcmp(method, "HEAD")    == 0) return MNET_HTTP_HEAD;
    if (strcmp(method, "OPTIONS") == 0) return MNET_HTTP_OPTIONS;
    return (mnet_http_method_t)-1;
}

/* Split query string into name/value pairs */
static int parse_query_string(const char *qs,
    char ***out_names, char ***out_values, size_t *out_count)
{
    if (qs == NULL || *qs == '\0') {
        *out_names = NULL;
        *out_values = NULL;
        *out_count = 0;
        return 0;
    }

    /* Count pairs */
    size_t count = 1;
    for (const char *p = qs; *p; p++) {
        if (*p == '&') count++;
    }

    /* Cap the pair count so a request with a huge query string cannot force a
       correspondingly huge allocation. */
    if (count > MNET_MAX_QUERY) count = MNET_MAX_QUERY;

    char **names = malloc(count * sizeof(char *));
    char **values = malloc(count * sizeof(char *));
    if (names == NULL || values == NULL) {
        free(names); free(values);
        *out_names = NULL; *out_values = NULL; *out_count = 0;
        return -1;
    }

    size_t idx = 0;
    char *copy = strdup(qs);
    if (copy == NULL) {
        free(names); free(values);
        *out_names = NULL; *out_values = NULL; *out_count = 0;
        return -1;
    }

    char *save = NULL;
    char *token = strtok_r(copy, "&", &save);
    while (token != NULL && idx < count) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            names[idx] = strdup(token);
            values[idx] = strdup(eq + 1);
        } else {
            names[idx] = strdup(token);
            values[idx] = strdup("");
        }
        if (names[idx] == NULL || values[idx] == NULL) {
            free(names[idx]); free(values[idx]);
            for (size_t j = 0; j < idx; j++) {
                free(names[j]); free(values[j]);
            }
            free(names); free(values); free(copy);
            *out_names = NULL; *out_values = NULL; *out_count = 0;
            return -1;
        }

        /* A single parameter name or value is capped; a longer one is rejected
           rather than silently stored or truncated. */
        if (strlen(names[idx]) > MNET_MAX_QUERY_NAME ||
            strlen(values[idx]) > MNET_MAX_QUERY_VALUE) {
            free(names[idx]); free(values[idx]);
            for (size_t j = 0; j < idx; j++) {
                free(names[j]); free(values[j]);
            }
            free(names); free(values); free(copy);
            *out_names = NULL; *out_values = NULL; *out_count = 0;
            return -1;
        }

        /* Decode in place. The decoded form is never longer than the source,
           so the existing allocation is always large enough. */
        mnet_url_decode(names[idx], strlen(names[idx]) + 1, names[idx]);
        mnet_url_decode(values[idx], strlen(values[idx]) + 1, values[idx]);
        idx++;
        token = strtok_r(NULL, "&", &save);
    }
    free(copy);

    *out_names = names;
    *out_values = values;
    *out_count = idx;
    return 0;
}

/* Parse headers from the header section.
   Returns 0 on success, -1 on failure (too many headers or allocation). */
static int parse_headers(const char *headers_raw,
    char ***out_names, char ***out_values, size_t *out_count)
{
    char **names = NULL;
    char **values = NULL;
    char *copy = NULL;
    size_t count = 0;

    *out_names = NULL;
    *out_values = NULL;
    *out_count = 0;

    if (headers_raw == NULL || *headers_raw == '\0') {
        return 0;
    }

    names = malloc(MNET_MAX_HEADERS * sizeof(char *));
    values = malloc(MNET_MAX_HEADERS * sizeof(char *));
    if (names == NULL || values == NULL) goto fail;

    copy = strdup(headers_raw);
    if (copy == NULL) goto fail;

    char *save = NULL;
    char *line = strtok_r(copy, "\r\n", &save);
    while (line != NULL) {
        /* Hard cap on header count: refuse to grow beyond it. */
        if (count >= MNET_MAX_HEADERS) goto fail;

        /* Reject an individual header line that is unreasonably long rather
           than letting one line consume the whole budget. */
        if (strlen(line) >= MNET_MAX_HEADER_LINE) goto fail;

        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            const char *v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;

            /* The name must be a valid token; a line without a valid name is
               not a header and is skipped rather than stored. */
            if (!mnet_header_name_valid(line)) {
                line = strtok_r(NULL, "\r\n", &save);
                continue;
            }

            names[count] = strdup(line);
            values[count] = strdup(v);
            if (names[count] == NULL || values[count] == NULL) {
                free(names[count]); free(values[count]);
                goto fail;
            }
            count++;
        }
        line = strtok_r(NULL, "\r\n", &save);
    }
    free(copy);

    *out_names = names;
    *out_values = values;
    *out_count = count;
    return 0;

fail:
    for (size_t i = 0; i < count; i++) {
        free(names[i]); free(values[i]);
    }
    free(names); free(values); free(copy);
    *out_names = NULL; *out_values = NULL; *out_count = 0;
    return -1;
}

/* Extract the request line (method and target) from the raw buffer.
   Returns 0 on success, -1 if the request line is malformed. */
static int parse_raw_request(
    const char *buffer,
    char *method_out,
    char *path_out)
{
    /* Find header/body separator */
    const char *sep = strstr(buffer, "\r\n\r\n");
    const char *sep2 = strstr(buffer, "\n\n");
    const char *end_of_headers = sep ? sep : (sep2 ? sep2 : NULL);
    if (end_of_headers == NULL) return -1;

    /* Request line is first line */
    const char *line_end = memchr(buffer, '\n', (size_t)(end_of_headers - buffer));
    if (line_end == NULL) return -1;

    /* Parse method and path from request line */
    if (sscanf(buffer, "%15s %2047s", method_out, path_out) != 2)
        return -1;

    return 0;
}

/*
 * Locate the body according to the request headers.
 *
 * On success returns 0 and sets *body_out / *body_len_out. *body_heap is set
 * to 1 only when *body_out points at a fresh allocation the caller must free;
 * when the body already sits in the caller's buffer, *body_out points into
 * that buffer and *body_heap is 0. Callers must therefore never free the body
 * unless *body_heap is 1.
 *
 * Returns a negative value on failure.
 */
static int read_full_body(mnet_socket_t client, const char *buffer,
    ssize_t initial_received, char **body_out, size_t *body_len_out,
    int *body_heap, size_t max_body_size, int timeout_ms)
{
    const char *sep = strstr(buffer, "\r\n\r\n");
    const char *sep2 = strstr(buffer, "\n\n");
    const char *end_of_headers = sep ? sep : (sep2 ? sep2 : NULL);
    size_t body_offset;
    size_t initial_body_len;

    *body_out = NULL;
    *body_len_out = 0;
    *body_heap = 0;

    if (end_of_headers == NULL) return -1;

    body_offset = (size_t)(end_of_headers - buffer) + (sep ? 4 : 2);
    initial_body_len = (size_t)initial_received - body_offset;

    /* Find the Content-Length header. Only a line-initial match counts, so a
       header such as "X-Content-Length:" is not mistaken for it.
       The search is confined to the header section only — a "Content-Length:"
       line in the body must not be honoured. */
    const char *cl_header = NULL;
    int cl_count = 0;
    const char *search = buffer;
    const char *header_end = end_of_headers ? end_of_headers : buffer + initial_received;
    while ((search = strcasestr(search, "content-length:")) != NULL) {
        if (search >= header_end) break;
        if (search == buffer || search[-1] == '\n') {
            cl_header = search;
            cl_count++;
        }
        search += 15;
    }
    /* Duplicate Content-Length is a request smuggling attempt. */
    if (cl_count > 1) return -3;

    /* Reject Transfer-Encoding: chunked — the server does not implement
       chunked request bodies. Confined to the header section only. */
    {
        const char *te_search = buffer;
        while ((te_search = strcasestr(te_search, "transfer-encoding:")) != NULL) {
            if (te_search >= header_end) break;
            if (te_search == buffer || te_search[-1] == '\n') {
                return -4;
            }
            te_search += 18;
        }
    }

    size_t content_length = 0;
    if (cl_header != NULL) {
        /* "content-length:" is 15 characters, not 16. */
        const char *val = cl_header + 15;
        const char *p;
        int digit_seen = 0;
        int overflow = 0;
        size_t parsed = 0;

        while (*val == ' ' || *val == '\t') val++;

        for (p = val; *p >= '0' && *p <= '9'; p++) {
            digit_seen = 1;
            if (parsed > (max_body_size / 10)) overflow = 1;
            parsed = parsed * 10 + (size_t)(*p - '0');
            if (parsed > max_body_size) overflow = 1;
        }
        /* The value must be a plain decimal number followed by end of line. */
        if (!digit_seen || (*p != '\r' && *p != '\n')) return -2;
        if (overflow) return -1;

        content_length = parsed;
    }

    if (content_length == 0) {
        /* No Content-Length means no body. Any bytes after the header
           block are not part of the request body. */
        *body_out = (char *)buffer + body_offset;
        *body_len_out = 0;
        return 0;
    }

    if (content_length > max_body_size) return -1;

    if (initial_body_len >= content_length) {
        /* Everything is already in the caller's buffer. Do not copy and do
           not allocate; just point at it. */
        *body_out = (char *)buffer + body_offset;
        *body_len_out = content_length;
        return 0;
    }

    char *full_body = malloc(content_length + 1);
    if (full_body == NULL) return -1;

    memcpy(full_body, buffer + body_offset, initial_body_len);

    size_t total_received = initial_body_len;
    int64_t body_deadline_ms = now_ms_mono() + timeout_ms;
    while (total_received < content_length) {
        int64_t now_ms = now_ms_mono();
        int64_t left = body_deadline_ms - now_ms;
        if (left <= 0) {
            free(full_body);
            return -1;
        }

        int pr = mnet_poll(client, (int)left);
        if (pr <= 0) {
            free(full_body);
            return -1;
        }

        ssize_t n = mnet_recv(client, full_body + total_received,
            content_length - total_received);
        if (n <= 0) {
            free(full_body);
            return -1;
        }
        total_received += (size_t)n;
    }

    full_body[content_length] = '\0';
    *body_out = full_body;
    *body_len_out = content_length;
    *body_heap = 1;
    return 0;
}

static void parse_cookies(const char *cookie_header,
    char ***out_names, char ***out_values, size_t *out_count)
{
    if (cookie_header == NULL || *cookie_header == '\0') {
        *out_names = NULL;
        *out_values = NULL;
        *out_count = 0;
        return;
    }

    size_t count = 1;
    for (const char *p = cookie_header; *p; p++) {
        if (*p == ';') count++;
    }

    char **names = malloc(count * sizeof(char *));
    char **values = malloc(count * sizeof(char *));
    if (names == NULL || values == NULL) {
        free(names); free(values);
        *out_names = NULL; *out_values = NULL; *out_count = 0;
        return;
    }

    char *copy = strdup(cookie_header);
    if (copy == NULL) {
        free(names); free(values);
        *out_names = NULL; *out_values = NULL; *out_count = 0;
        return;
    }

    size_t idx = 0;
    char *save = NULL;
    char *token = strtok_r(copy, ";", &save);
    while (token != NULL && idx < count) {
        while (*token == ' ') token++;
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            const char *v = eq + 1;
            while (*v == ' ') v++;
            names[idx] = strdup(token);
            values[idx] = strdup(v);
        } else {
            names[idx] = strdup(token);
            values[idx] = strdup("");
        }
        if (names[idx] == NULL || values[idx] == NULL) {
            free(names[idx]); free(values[idx]);
            for (size_t j = 0; j < idx; j++) {
                free(names[j]); free(values[j]);
            }
            free(names); free(values); free(copy);
            *out_names = NULL; *out_values = NULL; *out_count = 0;
            return;
        }
        idx++;
        token = strtok_r(NULL, ";", &save);
    }
    free(copy);

    *out_names = names;
    *out_values = values;
    *out_count = idx;
}

/*
 * Release the request extras.
 *
 * param_names and param_values are borrowed: param_names belongs to the route
 * and param_values to the router (freed by mnet_match_params_free()). Only the
 * query, header and cookie arrays are owned here.
 */
void free_extras(request_extras_t *e)
{
    if (e == NULL) return;
    for (size_t i = 0; i < e->query_count; i++) {
        free(e->query_names[i]);
        free(e->query_values[i]);
    }
    free(e->query_names);
    free(e->query_values);
    for (size_t i = 0; i < e->header_count; i++) {
        free(e->header_names[i]);
        free(e->header_values[i]);
    }
    free(e->header_names);
    free(e->header_values);
    for (size_t i = 0; i < e->cookie_count; i++) {
        free(e->cookie_names[i]);
        free(e->cookie_values[i]);
    }
    free(e->cookie_names);
    free(e->cookie_values);
    memset(e, 0, sizeof(*e));
}

int parse_request(mnet_socket_t client, const char *buffer,
    ssize_t received, parsed_request_t *out, size_t max_body_size,
    int timeout_ms)
{
    out->method[0] = '\0';
    out->path[0] = '\0';
    out->path_only[0] = '\0';
    out->query_string = NULL;
    out->body = NULL;
    out->body_length = 0;
    out->body_heap = 0;
    out->keep_alive = 0;
    memset(&out->extras, 0, sizeof(out->extras));

    char *body_ptr = NULL;
    size_t body_len = 0;
    int body_heap = 0;

    if (parse_raw_request(buffer, out->method, out->path) != 0) {
        return MNET_PARSE_BAD_REQUEST;
    }

    out->method_enum = mnet_parse_method(out->method);
    if (out->method_enum == (mnet_http_method_t)-1) {
        return MNET_PARSE_UNSUPPORTED_METHOD;
    }

    int r = read_full_body(client, buffer, received, &body_ptr, &body_len,
        &body_heap, max_body_size, timeout_ms);
    if (r == -1) return MNET_PARSE_TOO_LARGE;
    if (r == -2) return MNET_PARSE_BAD_CONTENT_LENGTH;
    if (r == -3) return MNET_PARSE_DUPLICATE_CONTENT_LENGTH;
    if (r == -4) return MNET_PARSE_UNSUPPORTED_TRANSFER_ENCODING;

    out->body = body_ptr;
    out->body_length = body_len;
    out->body_heap = body_heap;

    /* Find the end of the header block to confine header parsing to headers
       only. Without this, a POST body containing lines like "X-Admin: true"
       would be parsed as a request header. */
    const char *body_sep = strstr(buffer, "\r\n\r\n");
    const char *body_sep2 = strstr(buffer, "\n\n");
    const char *end_of_headers = body_sep ? body_sep : (body_sep2 ? body_sep2 : NULL);

    if (end_of_headers) {
        /* We need to pass only the header section to parse_headers, not the
           body. Create a temporary buffer with just the headers. */
        size_t hdr_len = (size_t)(end_of_headers - buffer);
        char *hdr_copy = malloc(hdr_len + 1);
        if (hdr_copy == NULL) return MNET_PARSE_HEADERS_TOO_LARGE;
        memcpy(hdr_copy, buffer, hdr_len);
        hdr_copy[hdr_len] = '\0';

        int rc = parse_headers(hdr_copy, &out->extras.header_names,
            &out->extras.header_values, &out->extras.header_count);
        free(hdr_copy);
        if (rc != 0) {
            return MNET_PARSE_HEADERS_TOO_LARGE;
        }

        for (size_t i = 0; i < out->extras.header_count; i++) {
            if (strcasecmp(out->extras.header_names[i], "Cookie") == 0) {
                /* Free any previous cookie arrays before parsing another
                   Cookie header, so multiple Cookie headers don't leak. */
                if (out->extras.cookie_names != NULL) {
                    for (size_t j = 0; j < out->extras.cookie_count; j++) {
                        free(out->extras.cookie_names[j]);
                        free(out->extras.cookie_values[j]);
                    }
                    free(out->extras.cookie_names);
                    free(out->extras.cookie_values);
                    out->extras.cookie_names = NULL;
                    out->extras.cookie_values = NULL;
                    out->extras.cookie_count = 0;
                }
                parse_cookies(out->extras.header_values[i],
                    &out->extras.cookie_names, &out->extras.cookie_values,
                    &out->extras.cookie_count);
            }
            if (strcasecmp(out->extras.header_names[i], "Connection") == 0) {
                if (strcasecmp(out->extras.header_values[i], "keep-alive") == 0) {
                    out->keep_alive = 1;
                }
            }
        }
    }

    char *qs = strchr(out->path, '?');
    if (qs) {
        size_t plen = (size_t)(qs - out->path);
        memcpy(out->path_only, out->path, plen);
        out->path_only[plen] = '\0';
        qs++;
        out->query_string = qs;
        parse_query_string(qs, &out->extras.query_names,
            &out->extras.query_values, &out->extras.query_count);
    } else {
        strcpy(out->path_only, out->path);
    }

    return MNET_PARSE_OK;
}
