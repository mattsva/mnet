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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

static int app_debug = 0;

#define MNET_INITIAL_ROUTE_CAPACITY 8
#define MNET_REQUEST_BUFFER_SIZE 8192
#define MNET_MAX_BODY_SIZE (16 * 1024 * 1024)
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
    mnet_middleware_t middleware;
    int timeout_seconds;
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
    ssize_t initial_received, char **body_out, size_t *body_len_out)
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

    if (content_length > MNET_MAX_BODY_SIZE) return -1;

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
    memset(e, 0, sizeof(*e));
}

static void send_response(mnet_socket_t client, const mnet_response_t *r,
    int head_only)
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

    if (!head_only && r->body && r->body_length > 0) {
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
    if (app->timeout_seconds > 0) {
        struct timeval tv;
        tv.tv_sec = app->timeout_seconds;
        tv.tv_usec = 0;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    char buffer[MNET_REQUEST_BUFFER_SIZE];
    ssize_t received = mnet_recv(client, buffer, sizeof(buffer) - 1);
    if (received <= 0) return;
    buffer[received] = '\0';

    /* Parse request line */
    char method[16] = {0};
    char path[2048] = {0};
    char *body_ptr = NULL;
    size_t body_len = 0;
    int body_heap = 0;

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

    /* Read full body if Content-Length is present */
    char *full_body = NULL;
    size_t full_body_len = 0;
    if (read_full_body(client, buffer, received, &full_body, &full_body_len) != 0) {
        send_method_not_allowed(client, method);
        return;
    }
    if (full_body != NULL) {
        body_ptr = full_body;
        body_len = full_body_len;
        body_heap = 1;
    }

    request_extras_t extras = {0};

    /* Parse headers */
    const char *line_end = memchr(buffer, '\n',
        strstr(buffer, "\r\n\r\n") ? strstr(buffer, "\r\n\r\n") - buffer : 0);
    if (line_end) {
        const char *hdr_section = line_end + 1;
        parse_headers(hdr_section, &extras.header_names, &extras.header_values,
            &extras.header_count);
    }

    /* Parse query string */
    char *qs = strchr(path, '?');
    char path_only[2048];
    if (qs) {
        size_t plen = (size_t)(qs - path);
        memcpy(path_only, path, plen);
        path_only[plen] = '\0';
        qs++;
        parse_query_string(qs, &extras.query_names, &extras.query_values,
            &extras.query_count);
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
                .extras = &extras,
            };
            response = app->not_found_handler(&nf_req);
            if (app_debug) {
                fprintf(stderr, "[mnet] %d %s %s\n",
                    response.status, method, path_only);
            }
            send_response(client, &response, 0);
        } else {
            send_not_found(client, path_only);
        }
    } else {
        /* Build request struct */
        size_t pc = 0;
        if (route->param_names) {
            while (route->param_names[pc] != NULL) pc++;
        }

        extras.param_names = (char **)route->param_names;
        extras.param_count = (int)pc;
        extras.param_values = (char **)param_values;

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
            .extras = &extras,
            .user_data = route->user_data,
        };

        /* Call handler, optionally through middleware */
        if (app->middleware) {
            response = app->middleware(&req, route->handler);
        } else {
            response = route->handler(&req);
        }

        /* Debug logging */
        if (app_debug) {
            fprintf(stderr, "[mnet] %d %s %s\n",
                response.status, method, path_only);
        }

        send_response(client, &response, hm == MNET_HTTP_HEAD);

        mnet_match_params_free(param_values, (int)pc);
    }

    free_extras(&extras);
    if (body_heap) free(body_ptr);
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

void mnet_use(mnet_app_t *app, mnet_middleware_t middleware)
{
    if (app != NULL) app->middleware = middleware;
}

typedef struct {
    char *url_prefix;
    char *fs_path;
} static_config_t;

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

void mnet_static(mnet_app_t *app, const char *url_prefix,
    const char *fs_path)
{
    if (app == NULL || url_prefix == NULL || fs_path == NULL) return;

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
