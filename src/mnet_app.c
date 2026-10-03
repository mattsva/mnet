#define _GNU_SOURCE
#include "mnet_app.h"
#include "mnet_socket.h"
#include "mnet_request.h"
#include "mnet_response.h"
#include "mnet_router.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#define MNET_INITIAL_ROUTE_CAPACITY 8
#define MNET_REQUEST_BUFFER_SIZE 8192
#define MNET_MAX_BODY_SIZE (16 * 1024 * 1024)
#define MNET_MAX_PARAMS 16
#define MNET_MAX_HEADERS 32
#define MNET_MAX_QUERY 16

typedef struct {
    char *url_prefix;
    char *fs_path;
} static_config_t;

struct mnet_app {
    mnet_route_t *routes;
    size_t route_count;
    size_t route_capacity;
    int running;
    int debug;
    mnet_response_t (*not_found_handler)(mnet_request_t *req);
    mnet_middleware_t middleware;
    int timeout_seconds;
    int max_connections;
    int keep_alive_timeout;
    size_t max_body_size;
};
static int mnet_add_route(
    mnet_app_t *app,
    mnet_http_method_t method,
    const char *path,
    mnet_handler_t handler)
{
    if (app == NULL || path == NULL || handler == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (app->route_count == app->route_capacity) {
        size_t nc;
        if (app->route_capacity == 0) {
            nc = MNET_INITIAL_ROUTE_CAPACITY;
        } else {
            nc = app->route_capacity * 2;
        }
        mnet_route_t *nr = realloc(app->routes, nc * sizeof(*nr));
        if (nr == NULL) return -1;
        app->routes = nr;
        app->route_capacity = nc;
    }

    mnet_route_t *r = &app->routes[app->route_count];
    r->method = method;
    r->path = path;
    r->handler = handler;
    r->param_names = NULL;
    r->user_data = NULL;
    r->path_allocated = 0;

    /* Parse parameter names from the pattern */
    size_t param_count = 0;
    const char *p = path;
    while (*p) {
        if (*p == ':') {
            p++;
            const char *start = p;
            while (*p && *p != '/' && *p != '.') p++;
            size_t len = (size_t)(p - start);
            if (len == 0) continue;
            char *name = malloc(len + 1);
            if (name == NULL) return -1;
            memcpy(name, start, len);
            name[len] = '\0';

            const char **tmp = realloc(r->param_names,
                (param_count + 1) * sizeof(const char *));
            if (tmp == NULL) { free(name); return -1; }
            r->param_names = tmp;
            r->param_names[param_count] = name;
            param_count++;
        } else {
            p++;
        }
    }

    app->route_count++;
    return 0;
}

static void free_route(mnet_route_t *r)
{
    if (r->param_names) {
        for (size_t i = 0; r->param_names[i] != NULL; i++) {
            free((void *)r->param_names[i]);
        }
        free(r->param_names);
        r->param_names = NULL;
    }
    if (r->path_allocated) {
        free((void *)r->path);
        r->path = NULL;
        r->path_allocated = 0;
    }
    if (r->user_data) {
        static_config_t *cfg = (static_config_t *)r->user_data;
        free(cfg->url_prefix);
        free(cfg->fs_path);
        free(cfg);
        r->user_data = NULL;
    }
}

static mnet_route_t *mnet_find_route(
    mnet_app_t *app,
    mnet_http_method_t method,
    const char *path,
    const char **out_params,
    size_t max_params)
{
    for (size_t i = 0; i < app->route_count; i++) {
        mnet_route_t *r = &app->routes[i];
        if (r->method != method) continue;

        int n = mnet_route_match(r, path, out_params, max_params);
        if (n > 0) {
            return r;
        } else if (n == 0) {
            /* Try exact match next */
            if (strcmp(r->path, path) == 0) {
                return r;
            }
            continue;
        } else {
            /* n < 0: allocation error */
            return NULL;
        }
    }
    return NULL;
}

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

/* Parse headers from the header section */
static int parse_headers(const char *headers_raw,
    char ***out_names, char ***out_values, size_t *out_count)
{
    if (headers_raw == NULL || *headers_raw == '\0') {
        *out_names = NULL;
        *out_values = NULL;
        *out_count = 0;
        return 0;
    }

    char **names = NULL;
    char **values = NULL;
    size_t count = 0;
    size_t cap = MNET_MAX_HEADERS;

    names = malloc(cap * sizeof(char *));
    values = malloc(cap * sizeof(char *));
    if (names == NULL || values == NULL) goto fail;

    char *copy = strdup(headers_raw);
    if (copy == NULL) goto fail;

    char *save = NULL;
    char *line = strtok_r(copy, "\r\n", &save);
    while (line != NULL) {
        if (count >= cap) {
            cap *= 2;
            char **tmpn = realloc(names, cap * sizeof(char *));
            char **tmpv = realloc(values, cap * sizeof(char *));
            if (tmpn == NULL || tmpv == NULL) { free(tmpn); free(tmpv); goto fail; }
            names = tmpn; values = tmpv;
        }

        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            const char *v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;
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
    free(names); free(values);
    *out_names = NULL; *out_values = NULL; *out_count = 0;
    return -1;
}

/* Extract request line and body from raw buffer.
   Returns 0 on success. */
static int parse_raw_request(
    const char *buffer,
    char *method_out,
    char *path_out,
    char **body_out,
    size_t *body_len_out)
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

    /* Body starts after the separator */
    const char *body_start = end_of_headers + 4; /* skip \r\n\r\n */
    /* Also handle \n\n case */
    if (sep2 && !sep) {
        body_start = end_of_headers + 2; /* skip \n\n */
    }

    size_t blen = 0;
    const char *body_ptr = NULL;

    if (body_start < buffer + strlen(buffer)) {
        blen = strlen(body_start);
        body_ptr = body_start;
    }

    *body_out = (char *)body_ptr;
    *body_len_out = blen;
    return 0;
}



static int read_full_body(mnet_socket_t client, const char *buffer,
    ssize_t initial_received, char **body_out, size_t *body_len_out,
    size_t max_body_size)
{
    const char *sep = strstr(buffer, "\r\n\r\n");
    const char *sep2 = strstr(buffer, "\n\n");
    const char *end_of_headers = sep ? sep : (sep2 ? sep2 : NULL);
    if (end_of_headers == NULL) return -1;

    const char *body_start = end_of_headers + (sep ? 4 : 2);
    size_t initial_body_len = (size_t)(initial_received - (body_start - buffer));

    /* Find Content-Length header */
    const char *cl_header = NULL;
    const char *search = buffer;
    while ((search = strcasestr(search, "content-length:")) != NULL) {
        const char *line_start = search;
        while (line_start > buffer && *(line_start - 1) != '\n') line_start--;
        if (line_start == buffer || *(line_start - 1) == '\n') {
            cl_header = search;
            break;
        }
        search += 16;
    }

    size_t content_length = 0;
    if (cl_header) {
        const char *val = cl_header + 16;
        while (*val == ' ' || *val == '\t') val++;
        content_length = (size_t)strtoul(val, NULL, 10);
    }

    if (content_length == 0) {
        *body_out = (char *)body_start;
        *body_len_out = initial_body_len;
        return 0;
    }

    if (content_length > max_body_size) return -1;

    char *full_body = malloc(content_length + 1);
    if (full_body == NULL) return -1;

    memcpy(full_body, body_start, initial_body_len);
    size_t total_received = initial_body_len;

    while (total_received < content_length) {
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

static void free_extras(request_extras_t *e)
{
    if (e == NULL) return;
    for (size_t i = 0; i < e->param_count; i++) {
        free(e->param_values[i]);
    }
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

static void send_response(mnet_socket_t client, const mnet_response_t *r,
    int head_only, int keep_alive)
{
    const char *status_text = "OK";
    switch (r->status) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 204: status_text = "No Content"; break;
        case 301: status_text = "Moved Permanently"; break;
        case 400: status_text = "Bad Request"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 500: status_text = "Internal Server Error"; break;
        default: status_text = "Unknown"; break;
    }

    const char *ct = r->content_type ?
        r->content_type : "application/octet-stream";

    char header[1024];
    int hlen;

    if (r->chunked) {
        hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 %d %s\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Content-Type: %s\r\n"
            "Connection: %s\r\n"
            "\r\n",
            r->status, status_text, ct, keep_alive ? "keep-alive" : "close");
    } else {
        hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 %d %s\r\n"
            "Content-Length: %zu\r\n"
            "Content-Type: %s\r\n"
            "Connection: %s\r\n"
            "\r\n",
            r->status, status_text, r->body_length, ct,
            keep_alive ? "keep-alive" : "close");
    }

    if (hlen < 0 || (size_t)hlen >= sizeof(header)) return;

    mnet_send(client, header, (size_t)hlen);

    if (!head_only && r->body && r->body_length > 0) {
        if (r->chunked) {
            char chunk_header[32];
            int chlen = snprintf(chunk_header, sizeof(chunk_header),
                "%zx\r\n", r->body_length);
            mnet_send(client, chunk_header, (size_t)chlen);
            mnet_send(client, r->body, r->body_length);
            mnet_send(client, "\r\n", 2);
            mnet_send(client, "0\r\n\r\n", 5);
        } else {
            mnet_send(client, r->body, r->body_length);
        }
    }
}

static void send_not_found(mnet_socket_t client, const char *path, int debug,
    int keep_alive)
{
    if (debug) {
        fprintf(stderr, "[mnet] 404  %s %s\n", "GET", path);
    }
    const char body[] =
        "<!doctype html>"
        "<html><head><title>404 Not Found</title></head>"
        "<body>"
        "<h1>404 Not Found</h1>"
        "<p>The requested URL was not found on this server.</p>"
        "</body></html>";
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: %s\r\n"
        "\r\n",
        sizeof(body) - 1, keep_alive ? "keep-alive" : "close");
    if (hlen > 0 && (size_t)hlen < sizeof(header)) {
        mnet_send(client, header, (size_t)hlen);
        mnet_send(client, body, sizeof(body) - 1);
    }
}

static void send_method_not_allowed(mnet_socket_t client, const char *method,
    int debug, int keep_alive)
{
    if (debug) {
        fprintf(stderr, "[mnet] 405  %s\n", method);
    }
    const char body[] =
        "<!doctype html>"
        "<html><head><title>405 Method Not Allowed</title></head>"
        "<body>"
        "<h1>405 Method Not Allowed</h1>"
        "<p>The method is not allowed for this URL.</p>"
        "</body></html>";
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 405 Method Not Allowed\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: %s\r\n"
        "\r\n",
        sizeof(body) - 1, keep_alive ? "keep-alive" : "close");
    if (hlen > 0 && (size_t)hlen < sizeof(header)) {
        mnet_send(client, header, (size_t)hlen);
        mnet_send(client, body, sizeof(body) - 1);
    }
}

typedef struct {
    char method[16];
    char path[2048];
    char path_only[2048];
    char *query_string;
    char *body;
    size_t body_length;
    int body_heap;
    mnet_http_method_t method_enum;
    request_extras_t extras;
    int keep_alive;
} parsed_request_t;

static int parse_request(mnet_socket_t client, const char *buffer,
    ssize_t received, parsed_request_t *out, size_t max_body_size)
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

    if (parse_raw_request(buffer, out->method, out->path, &body_ptr, &body_len) != 0) {
        return -1;
    }

    out->method_enum = mnet_parse_method(out->method);
    if (out->method_enum == (mnet_http_method_t)-1) {
        return -1;
    }

    char *full_body = NULL;
    size_t full_body_len = 0;
    if (read_full_body(client, buffer, received, &full_body, &full_body_len, max_body_size) != 0) {
        return -1;
    }
    if (full_body != NULL) {
        body_ptr = full_body;
        body_len = full_body_len;
        out->body_heap = 1;
    }

    out->body = body_ptr;
    out->body_length = body_len;

    const char *line_end = memchr(buffer, '\n',
        strstr(buffer, "\r\n\r\n") ? strstr(buffer, "\r\n\r\n") - buffer : 0);
    if (line_end) {
        const char *hdr_section = line_end + 1;
        parse_headers(hdr_section, &out->extras.header_names,
            &out->extras.header_values, &out->extras.header_count);

        for (size_t i = 0; i < out->extras.header_count; i++) {
            if (strcasecmp(out->extras.header_names[i], "Cookie") == 0) {
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

    return 0;
}

static mnet_response_t dispatch(mnet_app_t *app, parsed_request_t *parsed,
    const char **param_values, size_t param_count)
{
    mnet_route_t *route = mnet_find_route(app, parsed->method_enum,
        parsed->path_only, param_values, MNET_MAX_PARAMS);

    if (route == NULL) {
        if (app->not_found_handler) {
            mnet_request_t nf_req = {
                .method = parsed->method,
                .path = parsed->path_only,
                .body = NULL,
                .body_length = 0,
                .extras = &parsed->extras,
            };
            return app->not_found_handler(&nf_req);
        }
        return (mnet_response_t){0};
    }

    parsed->extras.param_names = (char **)route->param_names;
    parsed->extras.param_count = (int)param_count;
    parsed->extras.param_values = (char **)param_values;

    mnet_request_t req = {
        .method = parsed->method,
        .path = parsed->path_only,
        .body = parsed->body,
        .body_length = parsed->body_length,
        .query_string = parsed->query_string ? parsed->query_string : "",
        .path_param_names = (const char **)route->param_names,
        .path_param_values = (const char **)param_values,
        .path_param_count = (int)param_count,
        .query_names = (const char **)parsed->extras.query_names,
        .query_values = (const char **)parsed->extras.query_values,
        .query_count = (int)parsed->extras.query_count,
        .header_names = (const char **)parsed->extras.header_names,
        .header_values = (const char **)parsed->extras.header_values,
        .header_count = (int)parsed->extras.header_count,
        .cookie_names = (const char **)parsed->extras.cookie_names,
        .cookie_values = (const char **)parsed->extras.cookie_values,
        .cookie_count = (int)parsed->extras.cookie_count,
        .extras = &parsed->extras,
        .user_data = route->user_data,
    };

    if (app->middleware) {
        return app->middleware(&req, route->handler);
    }
    return route->handler(&req);
}

static void mnet_handle_client(mnet_app_t *app, mnet_socket_t client)
{
    if (app->timeout_seconds > 0) {
        struct timeval tv;
        tv.tv_sec = app->timeout_seconds;
        tv.tv_usec = 0;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    int keep_alive = 0;
    do {
        if (keep_alive && app->keep_alive_timeout > 0) {
            struct timeval tv;
            tv.tv_sec = app->keep_alive_timeout;
            tv.tv_usec = 0;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }

        char buffer[MNET_REQUEST_BUFFER_SIZE];
        ssize_t received = mnet_recv(client, buffer, sizeof(buffer) - 1);
        if (received <= 0) break;
        buffer[received] = '\0';

        parsed_request_t parsed;
        size_t max_body = app->max_body_size > 0 ? app->max_body_size : MNET_MAX_BODY_SIZE;
        if (parse_request(client, buffer, received, &parsed, max_body) != 0) {
            send_method_not_allowed(client, parsed.method, app->debug, keep_alive);
            break;
        }

        keep_alive = parsed.keep_alive;

        const char *param_values[MNET_MAX_PARAMS] = {0};
        size_t param_count = 0;
        if (parsed.extras.param_names) {
            while (parsed.extras.param_names[param_count] != NULL) param_count++;
        }

        mnet_response_t response = dispatch(app, &parsed, param_values, param_count);

        if (response.status == 0 && app->not_found_handler == NULL) {
            send_not_found(client, parsed.path_only, app->debug, keep_alive);
        } else {
            if (app->debug) {
                fprintf(stderr, "[mnet] %d %s %s\n",
                    response.status, parsed.method, parsed.path_only);
            }
            send_response(client, &response, parsed.method_enum == MNET_HTTP_HEAD,
                keep_alive);
            mnet_response_free(&response);
        }

        mnet_match_params_free(param_values, (int)param_count);
        free_extras(&parsed.extras);
        if (parsed.body_heap) free(parsed.body);
    } while (keep_alive);
}

static mnet_app_t *g_running_app = NULL;
static volatile int g_active_connections = 0;

static void sig_handler(int signum)
{
    (void)signum;
    if (g_running_app) {
        g_running_app->running = 0;
    }
}

mnet_app_t *mnet_create(void)
{
    mnet_app_t *app = calloc(1, sizeof(mnet_app_t));
    return app;
}

void mnet_destroy(mnet_app_t *app)
{
    if (app == NULL) return;
    for (size_t i = 0; i < app->route_count; i++) {
        free_route(&app->routes[i]);
    }
    free(app->routes);
    free(app);
}

void mnet_set_debug(mnet_app_t *app, int enabled)
{
    if (app != NULL) app->debug = enabled;
}

void mnet_set_not_found_handler(mnet_app_t *app, mnet_response_t (*handler)(mnet_request_t *req))
{
    if (app != NULL) app->not_found_handler = handler;
}

void mnet_use(mnet_app_t *app, mnet_middleware_t middleware)
{
    if (app != NULL) app->middleware = middleware;
}

static const char *mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";
    dot++;
    if (strcasecmp(dot, "html") == 0 || strcasecmp(dot, "htm") == 0) return "text/html";
    if (strcasecmp(dot, "css") == 0) return "text/css";
    if (strcasecmp(dot, "js") == 0) return "application/javascript";
    if (strcasecmp(dot, "json") == 0) return "application/json";
    if (strcasecmp(dot, "png") == 0) return "image/png";
    if (strcasecmp(dot, "jpg") == 0 || strcasecmp(dot, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, "gif") == 0) return "image/gif";
    if (strcasecmp(dot, "svg") == 0) return "image/svg+xml";
    if (strcasecmp(dot, "ico") == 0) return "image/x-icon";
    if (strcasecmp(dot, "txt") == 0) return "text/plain";
    if (strcasecmp(dot, "xml") == 0) return "application/xml";
    if (strcasecmp(dot, "pdf") == 0) return "application/pdf";
    return "application/octet-stream";
}

static mnet_response_t static_handler(mnet_request_t *req)
{
    static_config_t *cfg = (static_config_t *)req->user_data;
    if (cfg == NULL) return mnet_error(500, "internal server error");

    const char *url_path = req->path;
    const char *rel = url_path + strlen(cfg->url_prefix);
    if (*rel == '/') rel++;

    char fs_path[4096];
    snprintf(fs_path, sizeof(fs_path), "%s/%s", cfg->fs_path, rel);

    char *real = realpath(fs_path, NULL);
    if (real == NULL) {
        return mnet_error(404, "not found");
    }
    char *base_real = realpath(cfg->fs_path, NULL);
    if (base_real == NULL || strncmp(real, base_real, strlen(base_real)) != 0) {
        free(real);
        free(base_real);
        return mnet_error(403, "forbidden");
    }
    free(base_real);

    int fd = open(real, O_RDONLY);
    if (fd == -1) {
        free(real);
        return mnet_error(404, "not found");
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        free(real);
        return mnet_error(404, "not found");
    }

    char *data = malloc((size_t)st.st_size);
    if (data == NULL) {
        close(fd);
        free(real);
        return mnet_error(500, "internal server error");
    }

    ssize_t total = 0;
    while (total < (ssize_t)st.st_size) {
        ssize_t n = read(fd, data + total, (size_t)st.st_size - (size_t)total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    free(real);

    mnet_response_t r = {
        .status = 200,
        .content_type = mime_type(fs_path),
        .body = data,
        .body_length = (size_t)total,
    };
    return r;
}

void mnet_set_timeout(mnet_app_t *app, int seconds)
{
    if (app != NULL) app->timeout_seconds = seconds;
}

void mnet_set_max_connections(mnet_app_t *app, int max_connections)
{
    if (app != NULL) app->max_connections = max_connections;
}

void mnet_set_keep_alive_timeout(mnet_app_t *app, int seconds)
{
    if (app != NULL) app->keep_alive_timeout = seconds;
}

void mnet_set_max_body_size(mnet_app_t *app, size_t max_body_size)
{
    if (app != NULL) app->max_body_size = max_body_size;
}

void mnet_static(mnet_app_t *app, const char *url_prefix,
    const char *fs_path)
{
    if (app == NULL || url_prefix == NULL || fs_path == NULL) return;

    if (app->route_count == app->route_capacity) {
        size_t nc;
        if (app->route_capacity == 0) {
            nc = MNET_INITIAL_ROUTE_CAPACITY;
        } else {
            nc = app->route_capacity * 2;
        }
        mnet_route_t *nr = realloc(app->routes, nc * sizeof(*nr));
        if (nr == NULL) return;
        app->routes = nr;
        app->route_capacity = nc;
    }

    static_config_t *cfg = malloc(sizeof(static_config_t));
    if (cfg == NULL) return;
    cfg->url_prefix = strdup(url_prefix);
    cfg->fs_path = strdup(fs_path);
    if (cfg->url_prefix == NULL || cfg->fs_path == NULL) {
        free(cfg->url_prefix);
        free(cfg->fs_path);
        free(cfg);
        return;
    }

    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s/*", url_prefix);

    mnet_route_t *r = &app->routes[app->route_count];
    r->method = MNET_HTTP_GET;
    r->path = strdup(pattern);
    r->handler = static_handler;
    r->param_names = NULL;
    r->legacy_handler = NULL;
    r->user_data = cfg;
    r->path_allocated = 1;

    app->route_count++;
}

int mnet_run(mnet_app_t *app, uint16_t port)
{
    if (app == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Handle SIGINT/SIGTERM for graceful shutdown */
    struct sigaction sa = {0};
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    g_running_app = app;

    mnet_socket_t server = mnet_tcp_listen(port, 16);
    if (server == MNET_INVALID_SOCKET) return -1;

    app->running = 1;

    fprintf(stderr, "[mnet] listening on port %d\n", port);

    while (app->running) {
        mnet_socket_t client = mnet_tcp_accept(server);
        if (client == MNET_INVALID_SOCKET) {
            if (errno == EINTR) continue;
            break;
        }

        if (app->max_connections > 0) {
            while (g_active_connections >= app->max_connections && app->running) {
                struct timespec ts = {0, 100000000}; /* 100ms */
                nanosleep(&ts, NULL);
            }
            if (!app->running) {
                mnet_close(client);
                break;
            }
        }

        __sync_fetch_and_add(&g_active_connections, 1);
        mnet_handle_client(app, client);
        mnet_close(client);
        __sync_fetch_and_sub(&g_active_connections, 1);
    }

    mnet_close(server);
    fprintf(stderr, "[mnet] server stopped\n");
    return 0;
}

int mnet_stop(mnet_app_t *app)
{
    if (app == NULL) {
        errno = EINVAL;
        return -1;
    }
    app->running = 0;
    return 0;
}

int mnet_route(mnet_app_t *app, mnet_http_method_t method,
    const char *path, mnet_handler_t handler)
{
    return mnet_add_route(app, method, path, handler);
}
