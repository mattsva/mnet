#define _GNU_SOURCE
#include "mnet_app.h"
#include "mnet_socket.h"
#include "mnet_request.h"
#include "mnet_response.h"
#include "mnet_router.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int app_debug = 0;

#define MNET_INITIAL_ROUTE_CAPACITY 8
#define MNET_REQUEST_BUFFER_SIZE 8192
#define MNET_MAX_PARAMS 16
#define MNET_MAX_HEADERS 32
#define MNET_MAX_QUERY 16

struct mnet_app {
    mnet_route_t *routes;
    size_t route_count;
    size_t route_capacity;
    int running;
    int debug;
    mnet_response_t (*not_found_handler)(mnet_request_t *req);
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
    if (strcmp(method, "GET")    == 0) return MNET_HTTP_GET;
    if (strcmp(method, "POST")   == 0) return MNET_HTTP_POST;
    if (strcmp(method, "PUT")    == 0) return MNET_HTTP_PUT;
    if (strcmp(method, "PATCH")  == 0) return MNET_HTTP_PATCH;
    if (strcmp(method, "DELETE") == 0) return MNET_HTTP_DELETE;
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

typedef struct {
    char **param_names;
    char **param_values;
    size_t param_count;
    char **query_names;
    char **query_values;
    size_t query_count;
    char **header_names;
    char **header_values;
    size_t header_count;
} request_extras_t;

static request_extras_t extras = {0};

static void reset_extras(void)
{
    request_extras_t *e = &extras;
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
    memset(e, 0, sizeof(*e));
}

static void populate_extras(
    const char **param_values,
    int param_count,
    const char **param_names)
{
    request_extras_t *e = &extras;
    e->param_names = (char **)param_names;
    e->param_count = param_count;
    e->param_values = (char **)param_values;
}

static void send_response(mnet_socket_t client, const mnet_response_t *r)
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
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        r->status, status_text, r->body_length, ct);

    if (hlen < 0 || (size_t)hlen >= sizeof(header)) return;

    mnet_send(client, header, (size_t)hlen);

    if (r->body && r->body_length > 0) {
        mnet_send(client, r->body, r->body_length);
    }
}

static void send_not_found(mnet_socket_t client, const char *path)
{
    if (app_debug) {
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
        "Connection: close\r\n"
        "\r\n",
        sizeof(body) - 1);
    if (hlen > 0 && (size_t)hlen < sizeof(header)) {
        mnet_send(client, header, (size_t)hlen);
        mnet_send(client, body, sizeof(body) - 1);
    }
}

static void send_method_not_allowed(mnet_socket_t client, const char *method)
{
    if (app_debug) {
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
        "Connection: close\r\n"
        "\r\n",
        sizeof(body) - 1);
    if (hlen > 0 && (size_t)hlen < sizeof(header)) {
        mnet_send(client, header, (size_t)hlen);
        mnet_send(client, body, sizeof(body) - 1);
    }
}

static void mnet_handle_client(mnet_app_t *app, mnet_socket_t client)
{
    char buffer[MNET_REQUEST_BUFFER_SIZE];
    ssize_t received = mnet_recv(client, buffer, sizeof(buffer) - 1);
    if (received <= 0) return;
    buffer[received] = '\0';

    /* Parse request line */
    char method[16] = {0};
    char path[2048] = {0};
    char *body_ptr = NULL;
    size_t body_len = 0;

    if (parse_raw_request(buffer, method, path, &body_ptr, &body_len) != 0) {
        send_method_not_allowed(client, method);
        return;
    }

    /* Parse HTTP method */
    mnet_http_method_t hm = mnet_parse_method(method);
    if (hm == (mnet_http_method_t)-1) {
        send_method_not_allowed(client, method);
        return;
    }

    /* Parse headers */
    const char *line_end = memchr(buffer, '\n',
        strstr(buffer, "\r\n\r\n") ? strstr(buffer, "\r\n\r\n") - buffer : 0);
    if (line_end) {
        const char *hdr_section = line_end + 1;
        char **h_names = NULL;
        char **h_values = NULL;
        size_t h_count = 0;
        parse_headers(hdr_section, &h_names, &h_values, &h_count);
        extras.header_names = h_names;
        extras.header_values = h_values;
        extras.header_count = h_count;
    }

    /* Parse query string */
    char *qs = strchr(path, '?');
    char path_only[2048];
    if (qs) {
        size_t plen = (size_t)(qs - path);
        memcpy(path_only, path, plen);
        path_only[plen] = '\0';
        qs++;
        char **q_names = NULL;
        char **q_values = NULL;
        size_t q_count = 0;
        parse_query_string(qs, &q_names, &q_values, &q_count);
        extras.query_names = q_names;
        extras.query_values = q_values;
        extras.query_count = q_count;
    } else {
        strcpy(path_only, path);
    }

    /* Find route */
    const char *param_values[MNET_MAX_PARAMS] = {0};
    mnet_route_t *route = mnet_find_route(app, hm, path_only,
        param_values, MNET_MAX_PARAMS);

    mnet_response_t response = {0};

    if (route == NULL) {
        if (app->not_found_handler) {
            mnet_request_t nf_req = {
                .method = method,
                .path = path_only,
                .body = NULL,
                .body_length = 0,
            };
            response = app->not_found_handler(&nf_req);
            if (app_debug) {
                fprintf(stderr, "[mnet] %d %s %s\n",
                    response.status, method, path_only);
            }
            send_response(client, &response);
        } else {
            send_not_found(client, path_only);
        }
    } else {
        /* Build request struct */
        size_t pc = 0;
        if (route->param_names) {
            while (route->param_names[pc] != NULL) pc++;
        }

        mnet_request_t req = {
            .method = method,
            .path = path_only,
            .body = body_ptr,
            .body_length = body_len,
            .query_string = qs ? qs : "",
            .path_param_names = (const char **)route->param_names,
            .path_param_values = (const char **)param_values,
            .path_param_count = (int)pc,
            .query_names = (const char **)extras.query_names,
            .query_values = (const char **)extras.query_values,
            .query_count = (int)extras.query_count,
            .header_names = (const char **)extras.header_names,
            .header_values = (const char **)extras.header_values,
            .header_count = (int)extras.header_count,
        };

        /* Populate extras for the accessor macros */
        populate_extras(param_values, (int)pc, route->param_names);

        /* Call handler */
        response = route->handler(&req);

        /* Debug logging */
        if (app_debug) {
            fprintf(stderr, "[mnet] %d %s %s\n",
                response.status, method, path_only);
        }

        send_response(client, &response);
    }

    reset_extras();
}

static mnet_app_t *g_running_app = NULL;

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
    (void)app;
    app_debug = enabled;
}

void mnet_set_not_found_handler(mnet_app_t *app, mnet_response_t (*handler)(mnet_request_t *req))
{
    if (app != NULL) app->not_found_handler = handler;
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
        mnet_handle_client(app, client);
        mnet_close(client);
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
