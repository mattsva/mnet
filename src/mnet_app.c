#define _GNU_SOURCE
#include "mnet_app.h"
#include "mnet_socket.h"
#include "mnet_response.h"
#include "mnet_router.h"
#include "mnet_compat.h"
#include "mnet_internal.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* POSIX socket and network headers for client-side HTTP (mnet_call) */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#endif

/* Millisecond-resolution monotonic clock for deadlines. time(NULL) has
   1-second resolution, which makes sub-second timeouts unreliable. */
#ifdef _WIN32
static int64_t now_ms_mono(void)
{
    return (int64_t)GetTickCount64();
}
#else
static int64_t now_ms_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

/* poll() is not available on Windows. Use select() instead. */
#ifdef _WIN32
static int mnet_poll(mnet_socket_t fd, int timeout_ms)
{
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(fd + 1, &fds, NULL, NULL, &tv);
}
#else
static int mnet_poll(int fd, int timeout_ms)
{
    struct pollfd pfd = { fd, POLLIN, 0 };
    return poll(&pfd, 1, timeout_ms);
}
#endif

#define MNET_INITIAL_ROUTE_CAPACITY 8
#define MNET_REQUEST_BUFFER_SIZE 8192
#define MNET_MAX_HEADER_BYTES MNET_REQUEST_BUFFER_SIZE
#define MNET_MAX_HEADERS 100
#define MNET_MAX_HEADER_LINE 4096
#define MNET_MAX_QUERY_NAME 1024
#define MNET_MAX_QUERY_VALUE 1024
#define MNET_MAX_BODY_SIZE (16 * 1024 * 1024)
#define MNET_MAX_PARAMS 16
#define MNET_MAX_QUERY 16
#define MNET_MAX_WORKERS 64
#define MNET_DEFAULT_WORKERS 4
#define MNET_DEFAULT_TIMEOUT 30
#define MNET_LISTEN_BACKLOG 128

/* Parse/validation outcomes, surfaced to the client as an HTTP status. */
#define MNET_PARSE_OK 0
#define MNET_PARSE_BAD_REQUEST 1
#define MNET_PARSE_TOO_LARGE 2
#define MNET_PARSE_HEADERS_TOO_LARGE 3
#define MNET_PARSE_UNSUPPORTED_METHOD 4
#define MNET_PARSE_BAD_CONTENT_LENGTH 5
#define MNET_PARSE_DUPLICATE_CONTENT_LENGTH 6
#define MNET_PARSE_UNSUPPORTED_TRANSFER_ENCODING 7

typedef struct {
    char *url_prefix;
    char *fs_path;
    mnet_app_t *app; /* for logging; not owned */
} static_config_t;

struct mnet_app {
    mnet_route_t *routes;
    size_t route_count;
    size_t route_capacity;
    /*
     * Set from a signal handler, so it must be a volatile sig_atomic_t: the
     * C standard only guarantees that type is safe to write from a handler and
     * read asynchronously elsewhere.
     */
    volatile sig_atomic_t running;
    int debug;
    int dev;
    int https;
    int port; /* listening port, set by mnet_run() */
    mnet_response_t (*not_found_handler)(mnet_request_t *req);
    mnet_middleware_t middleware;
    int timeout_seconds;
    int max_connections;
    int keep_alive_timeout;
    size_t max_body_size;
    mnet_log_handler_t log_handler;
    int workers;
    int active_connections; /* guarded by the pool mutex */
};

/*
 * Process-wide log handler.
 *
 * The handler is set per app but messages can be emitted before an app exists
 * (listener setup) and from worker threads, so a single process-wide pointer
 * is kept as well. It is written before any worker starts and only read
 * afterwards, so no locking is needed.
 */
static mnet_log_handler_t g_log_handler = NULL;

void mnet_log_msg(int level, const char *fmt, ...)
{
    va_list args;

    if (g_log_handler != NULL) {
        char buf[1024];

        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        g_log_handler(level, "%s", buf);
        return;
    }

    const char *tag = "INFO";
    switch (level) {
        case MNET_LOG_ERROR: tag = "ERROR"; break;
        case MNET_LOG_WARN:  tag = "WARN";  break;
        case MNET_LOG_INFO:  tag = "INFO";  break;
        case MNET_LOG_DEBUG: tag = "DEBUG"; break;
        default: break;
    }

    va_start(args, fmt);
    fprintf(stderr, "[mnet] %s: ", tag);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

static void mnet_app_log(mnet_app_t *app, int level, const char *fmt, ...)
{
    va_list args;
    char buf[1024];

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (app != NULL && app->log_handler != NULL) {
        app->log_handler(level, "%s", buf);
        return;
    }
    mnet_log_msg(level, "%s", buf);
}

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
            char *name;
            if (len == 0) {
                /* Unnamed param: generate a name so the matcher's count
                   matches param_names. */
                name = malloc(16);
                if (name == NULL) goto fail;
                snprintf(name, 16, "_%zu", param_count);
            } else {
                name = malloc(len + 1);
                if (name == NULL) goto fail;
                memcpy(name, start, len);
                name[len] = '\0';
            }

            const char **tmp = realloc(r->param_names,
                (param_count + 2) * sizeof(const char *));
            if (tmp == NULL) { free(name); goto fail; }
            r->param_names = tmp;
            r->param_names[param_count] = name;
            param_count++;
            r->param_names[param_count] = NULL;
        } else if (*p == '*') {
            /*
             * A wildcard captures the rest of the path and is exposed under
             * the name "*", so MNET_PARAM(req, "*") returns it. It must be
             * registered here or the matcher's slot would have no name to
             * look up.
             */
            char *name = malloc(2);
            if (name == NULL) goto fail;
            name[0] = '*';
            name[1] = '\0';

            const char **tmp = realloc(r->param_names,
                (param_count + 2) * sizeof(const char *));
            if (tmp == NULL) { free(name); goto fail; }
            r->param_names = tmp;
            r->param_names[param_count] = name;
            param_count++;
            r->param_names[param_count] = NULL;

            /* Nothing after the wildcard is matched, so stop scanning. */
            break;
        } else {
            p++;
        }
    }

    app->route_count++;
    return 0;

fail:
    /* Release whatever was allocated for this route; it is not registered. */
    if (r->param_names != NULL) {
        for (size_t i = 0; i < param_count; i++) {
            free((void *)r->param_names[i]);
        }
        free(r->param_names);
        r->param_names = NULL;
    }
    errno = ENOMEM;
    return -1;
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

/*
 * Find the route matching method + path.
 *
 * mnet_route_match() allocates a parameter value for each matched segment and
 * frees those it has allocated itself when the match later fails. It does not
 * know about values allocated by *previous* attempts, so any values left over
 * from a failed attempt must be released here before the next attempt. On
 * success *out_count reports how many values are live in out_params.
 */
static mnet_route_t *mnet_find_route(
    mnet_app_t *app,
    mnet_http_method_t method,
    const char *path,
    const char **out_params,
    size_t max_params,
    size_t *out_count)
{
    *out_count = 0;

    for (size_t i = 0; i < app->route_count; i++) {
        mnet_route_t *r = &app->routes[i];
        if (r->method != method) continue;

        int n = mnet_route_match(r, path, out_params, max_params);
        if (n >= 0) {
            *out_count = (size_t)n;
            return r;
        }
        /* n == -1: no match, already cleaned up. */
    }

    /*
     * HEAD is GET without the body (RFC 9110), so when no HEAD route is
     * registered it is served by the matching GET route; send_response() then
     * suppresses the body while keeping the Content-Length. Without this a
     * HEAD request would 404 on a path that GET serves.
     */
    if (method == MNET_HTTP_HEAD) {
        return mnet_find_route(app, MNET_HTTP_GET, path, out_params,
            max_params, out_count);
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
static void free_extras(request_extras_t *e)
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

static void send_response(mnet_socket_t client, const mnet_response_t *r,
    int head_only, int keep_alive)
{
    const char *status_text = "OK";
    switch (r->status) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 204: status_text = "No Content"; break;
        case 301: status_text = "Moved Permanently"; break;
        case 302: status_text = "Found"; break;
        case 400: status_text = "Bad Request"; break;
        case 401: status_text = "Unauthorized"; break;
        case 403: status_text = "Forbidden"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 413: status_text = "Payload Too Large"; break;
        case 429: status_text = "Too Many Requests"; break;
        case 431: status_text = "Request Header Fields Too Large"; break;
        case 500: status_text = "Internal Server Error"; break;
        case 503: status_text = "Service Unavailable"; break;
        default: status_text = "Unknown"; break;
    }

    const char *ct = r->content_type ?
        r->content_type : "application/octet-stream";

    /* A content type taken from untrusted input must not be able to inject
       CRLF and split the response. Reject rather than sanitize. */
    if (!mnet_header_value_valid(ct)) return;

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

    if (!head_only && !r->chunked && r->body && r->body_length > 0) {
        /* Combine header and body into a single send so the client
           receives the full response in one read. */
        size_t total = (size_t)hlen + r->body_length;
        char *combined = malloc(total);
        if (combined == NULL) return;
        memcpy(combined, header, (size_t)hlen);
        memcpy(combined + hlen, r->body, r->body_length);
        mnet_send(client, combined, total);
        free(combined);
    } else {
        mnet_send(client, header, (size_t)hlen);

        if (!head_only) {
            if (r->chunked) {
                if (r->body && r->body_length > 0) {
                    char chunk_header[32];
                    int chlen = snprintf(chunk_header, sizeof(chunk_header),
                        "%zx\r\n", r->body_length);
                    mnet_send(client, chunk_header, (size_t)chlen);
                    mnet_send(client, r->body, r->body_length);
                    mnet_send(client, "\r\n", 2);
                }
                /* Always send the terminating chunk, even for an empty body. */
                mnet_send(client, "0\r\n\r\n", 5);
            }
        }
    }
}

static void send_simple_error(mnet_socket_t client, int status,
    const char *status_text, const char *message, int debug)
{
    if (debug) {
        fprintf(stderr, "[mnet] %d %s\n", status, message);
    }

    char body[256];
    int blen = snprintf(body, sizeof(body),
        "<!doctype html><html><head><title>%d %s</title></head>"
        "<body><h1>%d %s</h1><p>%s</p></body></html>",
        status, status_text, status, status_text, message);
    if (blen < 0) return;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, blen);
    if (hlen < 0 || (size_t)hlen >= sizeof(header)) return;

    mnet_send(client, header, (size_t)hlen);
    mnet_send(client, body, (size_t)blen);
}

static void send_not_found(mnet_socket_t client, const char *path, int debug,
    int keep_alive)
{
    if (debug) {
        fprintf(stderr, "[mnet] 404  %s\n", path);
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

static mnet_response_t welcome_response(mnet_app_t *app)
{
    /* Welcome page shown when dev mode is enabled and no routes are defined. */
    static const char *welcome_html =
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>mnet Server</title></head>\n"
        "<body>\n"
        "<h1>Congratulations!</h1>\n"
        "<p>You successfully started the mnet server.</p>\n"
        "<p>Go to <a href=\"https://github.com/mattsva/mnet/blob/main/README.md\">\n"
        "README.md</a> for more information about MNET.</p>\n"
        "<p>Go to <a href=\"https://github.com/mattsva/mnet/blob/main/docs/clib.md\">\n"
        "clib.md</a> for further setup.</p>\n"
        "<p>Development state: %s</p>\n"
        "<p>Current https state: %s</p>\n"
        "<p>Listening port: %d</p>\n"
        "<p>Debug mode: %s</p>\n"
        "<p>Worker count: %d</p>\n"
        "<p>Max connections: %d</p>\n"
        "<p>Timeout: %d seconds</p>\n"
        "<p>Keep-alive timeout: %d seconds</p>\n"
        "<p>Max body size: %zu bytes</p>\n"
        "<p>Routes defined: %zu</p>\n"
        "<p>Build time: %s</p>\n"
        "<p>Git commit: <code>%12s</code></p>\n"
        "</body>\n"
        "</html>";

    char dev_state[8];
    snprintf(dev_state, sizeof(dev_state), "%s", app->dev ? "true" : "false");

    char https_state[8];
    snprintf(https_state, sizeof(https_state), "%s",
        app->https ? "https" : "http");

    /* Build a simple HTML page with all the info.
       The body is allocated with malloc() and must be freed by the caller. */
    size_t total_len = strlen(welcome_html) + strlen(dev_state) +
                       strlen(https_state) + 512;
    char *body = malloc(total_len);
    if (body == NULL) {
        return mnet_error(500, "internal server error");
    }

    /* Get current time */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", tm);

    /* Get git commit hash */
    char commit_hash[13] = "unknown";
    FILE *fp = fopen(".git/HEAD", "r");
    if (fp != NULL) {
        char line[256];
        if (fgets(line, sizeof(line), fp) != NULL) {
            /* Remove newline */
            line[strcspn(line, "\n")] = 0;
            /* Extract hash from ref: ref: refs/heads/main */
            if (strncmp(line, "ref:", 4) == 0) {
                char *hash = line + 4;
                while (*hash == ' ' || *hash == '\t') hash++;
                strncpy(commit_hash, hash, 12);
                commit_hash[12] = '\0';
            } else {
                /* Direct hash (unborn branch) */
                strncpy(commit_hash, line, 12);
                commit_hash[12] = '\0';
            }
        }
        fclose(fp);
    }

    int written = snprintf(body, total_len, welcome_html,
        dev_state, https_state, app->port,
        app->debug ? "true" : "false",
        app->workers, app->max_connections,
        app->timeout_seconds, app->keep_alive_timeout,
        app->max_body_size, app->route_count,
        time_str, commit_hash);

    mnet_response_t r;
    r.status = 200;
    r.content_type = "text/html";
    r.body = body;
    r.body_length = (size_t)written;
    r.chunked = 0;

    return r;
}

static mnet_response_t dispatch(mnet_app_t *app, parsed_request_t *parsed,
    const char **param_values, size_t max_params, size_t *out_param_count)
{
    size_t param_count = 0;
    mnet_route_t *route = mnet_find_route(app, parsed->method_enum,
        parsed->path_only, param_values, max_params, &param_count);

    *out_param_count = param_count;

    /* If dev mode is enabled and no routes are defined, serve the welcome page
       only for the root path ("/") and "/index.html". */
    if (app->route_count == 0 && app->dev) {
        if (strcmp(parsed->path_only, "/") == 0 ||
            strcmp(parsed->path_only, "/index.html") == 0) {
            return welcome_response(app);
        }
        return mnet_error(404, "not found");
    }

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
    int keep_alive = 0;
    char buffer[MNET_REQUEST_BUFFER_SIZE];
    size_t used = 0;
    do {
        /* Use an absolute deadline for the entire request, not a per-recv
           timeout. SO_RCVTIMEO applies per recv() call, so a client that
           sends one byte every timeout interval can hold the connection
           forever. */
        int timeout_ms = app->timeout_seconds > 0 ?
            app->timeout_seconds * 1000 : 30000;
        if (keep_alive && app->keep_alive_timeout > 0) {
            timeout_ms = app->keep_alive_timeout * 1000;
        }

        int have_request = 0;
        int too_large = 0;

        /* Check if there's already a complete request in the buffer from
           a previous read (keep-alive or pipelined). */
        if (used > 0 && (strstr(buffer, "\r\n\r\n") != NULL ||
                          strstr(buffer, "\n\n") != NULL)) {
            have_request = 1;
        }

        /* Read until the end of the header block arrives, the buffer is
           full, or the absolute deadline passes. */
        int64_t deadline_ms = now_ms_mono() + timeout_ms;
        while (!have_request && used < sizeof(buffer) - 1) {
            int64_t now_ms = now_ms_mono();
            int64_t left = deadline_ms - now_ms;
            if (left <= 0) break;

            int pr = mnet_poll(client, (int)left);
            if (pr <= 0) break;

            ssize_t n = mnet_recv(client, buffer + used,
                sizeof(buffer) - 1 - used);
            if (n <= 0) break;
            used += (size_t)n;
            buffer[used] = '\0';
            if (strstr(buffer, "\r\n\r\n") != NULL ||
                strstr(buffer, "\n\n") != NULL) {
                have_request = 1;
                break;
            }
        }

        if (!have_request) {
            if (used >= sizeof(buffer) - 1) too_large = 1;
            if (too_large) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: header block exceeds %d bytes",
                    (int)(sizeof(buffer) - 1));
                send_simple_error(client, 431, "Request Header Fields Too Large",
                    "Request headers exceed the maximum size.", app->debug);
            }
            break;
        }

        parsed_request_t parsed;
        size_t max_body = app->max_body_size > 0 ?
            app->max_body_size : MNET_MAX_BODY_SIZE;
        int pr = parse_request(client, buffer, (ssize_t)used, &parsed, max_body, timeout_ms);

        if (pr != MNET_PARSE_OK) {
            if (pr == MNET_PARSE_UNSUPPORTED_METHOD) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: unsupported method '%s'", parsed.method);
                send_method_not_allowed(client, parsed.method, app->debug, 0);
            } else if (pr == MNET_PARSE_TOO_LARGE) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: body exceeds the configured limit");
                send_simple_error(client, 413, "Payload Too Large",
                    "Request body exceeds the maximum size.", app->debug);
            } else if (pr == MNET_PARSE_BAD_CONTENT_LENGTH) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: malformed Content-Length");
                send_simple_error(client, 400, "Bad Request",
                    "The Content-Length header is malformed.", app->debug);
            } else if (pr == MNET_PARSE_DUPLICATE_CONTENT_LENGTH) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: duplicate Content-Length");
                send_simple_error(client, 400, "Bad Request",
                    "Duplicate Content-Length headers.", app->debug);
            } else if (pr == MNET_PARSE_HEADERS_TOO_LARGE) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: header count or line length exceeds the limit");
                send_simple_error(client, 431, "Request Header Fields Too Large",
                    "Request headers exceed the maximum size.", app->debug);
            } else if (pr == MNET_PARSE_UNSUPPORTED_TRANSFER_ENCODING) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: Transfer-Encoding not supported");
                send_simple_error(client, 501, "Not Implemented",
                    "Transfer-Encoding is not supported.", app->debug);
            } else {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: could not parse the request line");
                send_simple_error(client, 400, "Bad Request",
                    "The request could not be parsed.", app->debug);
            }
            free_extras(&parsed.extras);
            if (parsed.body_heap) free(parsed.body);
            break;
        }

        keep_alive = parsed.keep_alive;

        const char *param_values[MNET_MAX_PARAMS] = {0};
        size_t param_count = 0;

        mnet_response_t response = dispatch(app, &parsed, param_values,
            MNET_MAX_PARAMS, &param_count);

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

        /* Check if there's another complete request already in the buffer
           (pipelined). If so, don't read more — just loop and process it. */
        if (keep_alive) {
            const char *sep = strstr(buffer, "\r\n\r\n");
            const char *sep2 = strstr(buffer, "\n\n");
            const char *end_of_headers = sep ? sep : (sep2 ? sep2 : NULL);
            if (end_of_headers) {
                size_t header_len = (size_t)(end_of_headers - buffer) + (sep ? 4 : 2);
                if (header_len < used) {
                    /* There's more data after the headers — check if it
                       contains another complete request. */
                    size_t remaining = used - header_len;
                    if (remaining > 0 && strstr(buffer + header_len, "\r\n\r\n") != NULL) {
                        /* Move the pipelined request to the front of the buffer. */
                        memmove(buffer, buffer + header_len, remaining);
                        used = remaining;
                        buffer[used] = '\0';
                        continue;
                    }
                }
            }
            used = 0;
        }
    } while (keep_alive);
}

static mnet_app_t *g_running_app = NULL;

static void sig_handler(int signum)
{
    (void)signum;
    if (g_running_app) {
        g_running_app->running = 0;
    }
}

/*
 * Worker pool.
 *
 * The accept loop pushes each accepted connection onto a bounded queue; the
 * workers pop one, serve it to completion (including keep-alive requests) and
 * loop. All shared state is guarded by the mutex: the queue itself, the
 * connection counter used to enforce max_connections, and the shutdown flag.
 *
 * app->routes and the handler table are only read once the pool is running, so
 * they need no locking: they are fully populated before mnet_run() is called
 * and the application is not expected to register routes afterwards.
 */
typedef struct {
    mnet_socket_t *slots;
    size_t capacity;
    size_t head;
    size_t count;
    int shutting_down;
    mnet_app_t *app;

    mnet_mutex_t mutex;
    mnet_cond_t not_empty;
    mnet_cond_t not_full;
    mnet_thread_t *threads;
    size_t thread_count;
} mnet_pool_t;

static int mnet_pool_push(mnet_pool_t *pool, mnet_socket_t client)
{
    mnet_mutex_lock(&pool->mutex);

    while (pool->count == pool->capacity && !pool->shutting_down) {
        /* The queue is full: wait for a worker to free a slot. If the server
           is stopping, stop accepting instead. */
        if (!pool->app->running) {
            pool->shutting_down = 1;
            break;
        }
        mnet_cond_wait(&pool->not_full, &pool->mutex);
    }

    if (pool->shutting_down) {
        mnet_mutex_unlock(&pool->mutex);
        return -1;
    }

    pool->slots[(pool->head + pool->count) % pool->capacity] = client;
    pool->count++;
    mnet_cond_signal(&pool->not_empty);
    mnet_mutex_unlock(&pool->mutex);
    return 0;
}

static int mnet_pool_pop(mnet_pool_t *pool, mnet_socket_t *out)
{
    mnet_mutex_lock(&pool->mutex);

    while (pool->count == 0 && !pool->shutting_down) {
        mnet_cond_wait(&pool->not_empty, &pool->mutex);
    }

    if (pool->count == 0) {
        mnet_mutex_unlock(&pool->mutex);
        return -1;
    }

    *out = pool->slots[pool->head];
    pool->head = (pool->head + 1) % pool->capacity;
    pool->count--;
    mnet_cond_signal(&pool->not_full);
    mnet_mutex_unlock(&pool->mutex);
    return 0;
}

MNET_THREAD_FN(mnet_worker_main)
{
    mnet_pool_t *pool = (mnet_pool_t *)arg;
    mnet_socket_t client;

    while (mnet_pool_pop(pool, &client) == 0) {
        mnet_handle_client(pool->app, client);
        mnet_close(client);

        mnet_mutex_lock(&pool->mutex);
        pool->app->active_connections--;
        mnet_mutex_unlock(&pool->mutex);
    }

    MNET_THREAD_RETURN;
}

static void mnet_pool_shutdown(mnet_pool_t *pool)
{
    mnet_mutex_lock(&pool->mutex);
    pool->shutting_down = 1;
    mnet_cond_broadcast(&pool->not_empty);
    mnet_cond_broadcast(&pool->not_full);
    mnet_mutex_unlock(&pool->mutex);

    for (size_t i = 0; i < pool->thread_count; i++) {
        mnet_thread_join(pool->threads[i]);
    }
    free(pool->threads);
    pool->threads = NULL;

    /* Drain anything still queued: no worker will pick it up now. */
    while (pool->count > 0) {
        mnet_socket_t c = pool->slots[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        mnet_close(c);
    }
    free(pool->slots);
    pool->slots = NULL;

    mnet_mutex_destroy(&pool->mutex);
    mnet_cond_destroy(&pool->not_empty);
    mnet_cond_destroy(&pool->not_full);
}

mnet_app_t *mnet_create(void)
{
    mnet_app_t *app = calloc(1, sizeof(mnet_app_t));

    if (app == NULL) return NULL;

    /*
     * Set the security-relevant defaults explicitly rather than relying on the
     * "0 means default" convention everywhere. A bounded timeout is what keeps
     * a client that connects and sends nothing from pinning the server, so it
     * is on from the start. mnet_set_timeout(app, 0) restores this value.
     */
    app->timeout_seconds = MNET_DEFAULT_TIMEOUT;
    app->keep_alive_timeout = MNET_DEFAULT_TIMEOUT;

    /*
     * Threaded by default. A single-threaded blocking server lets one client
     * that connects and then stalls occupy the whole server, which is the
     * thing this library most needs to avoid out of the box.
     * mnet_set_workers(app, 1) selects the single-threaded loop.
     */
    app->workers = MNET_DEFAULT_WORKERS;

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
    int written = snprintf(fs_path, sizeof(fs_path), "%s/%s", cfg->fs_path, rel);
    if (written < 0 || (size_t)written >= sizeof(fs_path)) {
        return mnet_error(404, "not found");
    }

    char *real = mnet_realpath(fs_path);
    if (real == NULL) {
        return mnet_error(404, "not found");
    }
    char *base_real = mnet_realpath(cfg->fs_path);
    if (base_real == NULL) {
        free(real);
        return mnet_error(404, "not found");
    }

    /* The resolved path must be the root itself or lie underneath it. A plain
       prefix comparison would also accept a sibling such as /var/www2 when the
       root is /var/www, so check the separator boundary too. */
    size_t base_len = strlen(base_real);
    int inside = (strncmp(real, base_real, base_len) == 0) &&
                 (real[base_len] == '\0' ||
                  real[base_len] == '/' ||
                  base_real[base_len - 1] == '/');

    if (!inside) {
        mnet_app_log(cfg->app, MNET_LOG_WARN,
            "blocked path traversal attempt: '%s' resolves outside the root",
            req->path);
        free(real);
        free(base_real);
        return mnet_error(403, "forbidden");
    }
    free(base_real);

    FILE *fp = fopen(real, "rb");
    if (fp == NULL) {
        free(real);
        return mnet_error(404, "not found");
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        free(real);
        return mnet_error(404, "not found");
    }

    long file_size = ftell(fp);
    if (file_size < 0) {
        fclose(fp);
        free(real);
        return mnet_error(404, "not found");
    }

    rewind(fp);

    char *data = malloc((size_t)file_size + 1);
    if (data == NULL) {
        fclose(fp);
        free(real);
        return mnet_error(500, "internal server error");
    }

    size_t total = fread(data, 1, (size_t)file_size, fp);
    fclose(fp);
    free(real);

    mnet_response_t r = {
        .status = 200,
        .content_type = mime_type(fs_path),
        .body = data,
        .body_length = total,
    };
    return r;
}

void mnet_set_timeout(mnet_app_t *app, int seconds)
{
    if (app == NULL) return;

    /* 0 restores the bounded default; a negative value disables the timeout
       entirely, which the documentation discourages for production. */
    app->timeout_seconds = (seconds == 0) ? MNET_DEFAULT_TIMEOUT : seconds;
}

void mnet_set_workers(mnet_app_t *app, int workers)
{
    if (app == NULL) return;

    /* 0 restores the threaded default; 1 selects the single-threaded loop. */
    if (workers == 0) workers = MNET_DEFAULT_WORKERS;
    if (workers < 0) workers = 1;
    if (workers > MNET_MAX_WORKERS) workers = MNET_MAX_WORKERS;
    app->workers = workers;
}

void mnet_set_log_handler(mnet_app_t *app, mnet_log_handler_t handler)
{
    g_log_handler = handler;
    if (app != NULL) app->log_handler = handler;
}

void mnet_set_https(mnet_app_t *app, int enabled)
{
    if (app != NULL) app->https = enabled;
}

void mnet_set_dev_mode(mnet_app_t *app, int enabled)
{
    if (app != NULL) app->dev = enabled;
}

void mnet_set_max_connections(mnet_app_t *app, int max_connections)
{
    if (app != NULL) app->max_connections = max_connections;
}

void mnet_set_keep_alive_timeout(mnet_app_t *app, int seconds)
{
    if (app == NULL) return;
    app->keep_alive_timeout = (seconds == 0) ? MNET_DEFAULT_TIMEOUT : seconds;
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

    char *path_copy = strdup(pattern);
    if (path_copy == NULL) {
        free(cfg->url_prefix);
        free(cfg->fs_path);
        free(cfg);
        return;
    }

    mnet_route_t *r = &app->routes[app->route_count];
    r->method = MNET_HTTP_GET;
    r->path = path_copy;
    r->handler = static_handler;
    r->param_names = NULL;
    r->user_data = cfg;
    r->path_allocated = 1;

    cfg->app = app;

    app->route_count++;
}

int mnet_run(mnet_app_t *app, uint16_t port)
{
    if (app == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Handle SIGINT/SIGTERM for graceful shutdown. */
#ifndef _WIN32
    struct sigaction sa = {0};
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#else
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#endif

    g_running_app = app;

    app->port = port;

    mnet_socket_t server = mnet_tcp_listen(port, MNET_LISTEN_BACKLOG);
    if (server == MNET_INVALID_SOCKET) {
        mnet_log_msg(MNET_LOG_ERROR, "failed to listen on port %d", port);
        return -1;
    }

    app->running = 1;
    app->active_connections = 0;

    mnet_app_log(app, MNET_LOG_INFO, "listening on port %d", port);

    /*
     * Single-threaded mode is kept for workers <= 1: it has no synchronisation
     * overhead and is what the library did before the pool existed.
     */
    if (app->workers <= 1) {
        while (app->running) {
            mnet_socket_t client = mnet_tcp_accept(server);
            if (client == MNET_INVALID_SOCKET) {
                if (mnet_socket_errno() == MNET_EINTR) continue;
                mnet_app_log(app, MNET_LOG_ERROR,
                    "accept failed, stopping the server");
                break;
            }

            if (app->max_connections > 0 &&
                app->active_connections >= app->max_connections) {
                /* Refuse rather than queue: a plain counter, not per-IP. */
                mnet_app_log(app, MNET_LOG_WARN,
                    "refused a connection: %d active (max_connections)",
                    app->active_connections);
                send_simple_error(client, 503, "Service Unavailable",
                    "The server is at its connection limit.", app->debug);
                mnet_close(client);
                continue;
            }

            app->active_connections++;
            mnet_handle_client(app, client);
            mnet_close(client);
            app->active_connections--;
        }

        mnet_close(server);
        mnet_app_log(app, MNET_LOG_INFO, "server stopped");
        return 0;
    }

    /* Multithreaded: a bounded queue feeding a fixed pool of workers. */
    size_t capacity = app->max_connections > 0 ?
        (size_t)app->max_connections : 128;

    mnet_pool_t pool;
    memset(&pool, 0, sizeof(pool));
    pool.app = app;
    pool.capacity = capacity;
    pool.slots = malloc(capacity * sizeof(mnet_socket_t));
    pool.threads = malloc((size_t)app->workers * sizeof(mnet_thread_t));

    if (pool.slots == NULL || pool.threads == NULL) {
        free(pool.slots);
        free(pool.threads);
        mnet_close(server);
        mnet_log_msg(MNET_LOG_ERROR, "failed to allocate the worker pool");
        return -1;
    }

    mnet_mutex_init(&pool.mutex);
    mnet_cond_init(&pool.not_empty);
    mnet_cond_init(&pool.not_full);

    for (size_t i = 0; i < (size_t)app->workers; i++) {
        if (mnet_thread_create(&pool.threads[i], mnet_worker_main, &pool) != 0) {
            mnet_app_log(app, MNET_LOG_ERROR,
                "failed to start worker %d of %d",
                (int)i, app->workers);
            /* Fall back to serving with the workers that did start. */
            pool.thread_count = i;
            app->workers = (int)i;
            if (i == 0) {
                mnet_pool_shutdown(&pool);
                mnet_close(server);
                return -1;
            }
            break;
        }
        pool.thread_count++;
    }

    mnet_app_log(app, MNET_LOG_INFO, "serving with %d worker threads",
        app->workers);

    while (app->running) {
        mnet_socket_t client = mnet_tcp_accept(server);
        if (client == MNET_INVALID_SOCKET) {
            if (mnet_socket_errno() == MNET_EINTR) continue;
            mnet_app_log(app, MNET_LOG_ERROR,
                "accept failed, stopping the server");
            break;
        }

        mnet_mutex_lock(&pool.mutex);
        int active = app->active_connections;
        mnet_mutex_unlock(&pool.mutex);

        if (app->max_connections > 0 && active >= app->max_connections) {
            mnet_app_log(app, MNET_LOG_WARN,
                "refused a connection: %d active (max_connections)", active);
            send_simple_error(client, 503, "Service Unavailable",
                "The server is at its connection limit.", app->debug);
            mnet_close(client);
            continue;
        }

        mnet_mutex_lock(&pool.mutex);
        app->active_connections++;
        mnet_mutex_unlock(&pool.mutex);

        if (mnet_pool_push(&pool, client) != 0) {
            mnet_mutex_lock(&pool.mutex);
            app->active_connections--;
            mnet_mutex_unlock(&pool.mutex);
            mnet_close(client);
            break;
        }
    }

    mnet_pool_shutdown(&pool);
    mnet_close(server);
    mnet_app_log(app, MNET_LOG_INFO, "server stopped");
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

/* ------------------------------------------------------------------ */
/* Client-side HTTP                                                    */
/* ------------------------------------------------------------------ */

/* Create a connected TCP socket to the given host:port.
 * Returns MNET_INVALID_SOCKET on failure. */
static mnet_socket_t mnet_socket_connect(const char *host, uint16_t port)
{
    mnet_socket_t sock;
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    char port_str[16];
    int error;

    if (host == NULL) {
        return MNET_INVALID_SOCKET;
    }

    snprintf(port_str, sizeof(port_str), "%u", (unsigned int)port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    error = getaddrinfo(host, port_str, &hints, &result);
    if (error != 0) {
        return MNET_INVALID_SOCKET;
    }

    sock = MNET_INVALID_SOCKET;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == MNET_INVALID_SOCKET) {
            continue;
        }

        if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
            break;
        }

        mnet_close(sock);
        sock = MNET_INVALID_SOCKET;
    }

    freeaddrinfo(result);
    return sock;
}

static int mnet_url_parse(const char *url,
    char *host_out, size_t host_size,
    uint16_t *port_out, char *path_out, size_t path_size)
{
    const char *p = url;
    const char *host_start = NULL;
    const char *port_start = NULL;
    const char *path_start = NULL;

    if (url == NULL || host_out == NULL || port_out == NULL || path_out == NULL) {
        return -1;
    }

    /* Skip scheme if present */
    if (strncmp(url, "https://", 8) == 0) {
        p += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        p += 7;
    } else {
        /* No scheme: assume http */
        p = url;
    }

    /* Find host */
    host_start = p;
    while (*p && *p != ':' && *p != '/' && *p != '?' && *p != '#') {
        p++;
    }

    if (host_start == p) {
        /* Empty host */
        return -1;
    }

    size_t host_len = (size_t)(p - host_start);
    if (host_len >= host_size) {
        host_len = host_size - 1;
    }
    memcpy(host_out, host_start, host_len);
    host_out[host_len] = '\0';

    /* Check for port */
    if (*p == ':') {
        port_start = p + 1;
        p++;
        while (*p && *p != '/' && *p != '?' && *p != '#') {
            p++;
        }
    }

    /* Check for path */
    if (*p == '/' || *p == '?' || *p == '#' || *p == '\0') {
        path_start = p;
    }

    if (path_start == NULL) {
        path_start = "/";
    }

    size_t path_len = strlen(path_start);
    if (path_len >= path_size) {
        path_len = path_size - 1;
    }
    memcpy(path_out, path_start, path_len);
    path_out[path_len] = '\0';

    /* Parse port */
    *port_out = 80;
    if (port_start != NULL && port_start != path_start) {
        char *endptr = NULL;
        long port = strtol(port_start, &endptr, 10);
        if (endptr != port_start && *endptr == '\0' && port > 0 && port < 65536) {
            *port_out = (uint16_t)port;
        }
    }

    return 0;
}

/* Read the HTTP response headers and body from a connected socket.
 * Returns 0 on success, -1 on failure. */
static int mnet_read_response(mnet_socket_t sock,
    char **out_body, size_t *out_body_len, size_t max_body)
{
    char buf[8192];
    size_t total_read = 0;
    int header_done = 0;
    int body_done = 0;
    int in_body = 0;
    ssize_t n;

    *out_body = NULL;
    *out_body_len = 0;

    /* Read until we have the full response */
    while (!body_done) {
        n = mnet_recv(sock, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && mnet_socket_errno() == MNET_EINTR) {
                continue;
            }
            break;
        }

        /* Check for header terminator */
        if (!header_done) {
            char *header_end = memchr(buf, '\r', (size_t)n);
            if (header_end != NULL) {
                header_done = 1;
                in_body = 1;
            }
        }

        if (header_done) {
            /* Already past headers, read body */
            if (!in_body) {
                /* Headers ended but we need to find the body start */
                char *header_end = memchr(buf, '\r', (size_t)n);
                if (header_end != NULL) {
                    size_t body_start = (size_t)(header_end - buf) + 2;
                    if ((size_t)n > body_start) {
                        memcpy(buf, buf + body_start, n - body_start);
                        total_read = n - body_start;
                        in_body = 1;
                    }
                }
            }

            if (in_body) {
                size_t bytes_to_copy = (size_t)n;
                if (total_read + bytes_to_copy > max_body) {
                    bytes_to_copy = max_body - total_read;
                }
                if (bytes_to_copy > 0) {
                    char *new_body = realloc(*out_body, total_read + bytes_to_copy + 1);
                    if (new_body == NULL) {
                        free(*out_body);
                        return -1;
                    }
                    *out_body = new_body;
                    memcpy(*out_body + total_read, buf, bytes_to_copy);
                    total_read += bytes_to_copy;
                    (*out_body)[total_read] = '\0';
                }

                if (total_read >= max_body) {
                    body_done = 1;
                }
            }
        }
    }

    if (n < 0) {
        free(*out_body);
        *out_body = NULL;
        *out_body_len = 0;
        return -1;
    }

    *out_body_len = total_read;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Client-side HTTP: perform a GET request and return the response body as a
 * malloc()-allocated string. Returns NULL on failure. The caller must free()
 * the returned string when done.
 * --------------------------------------------------------------------------- */
char *mnet_call(const char *url)
{
    char host[1024];
    char path[2048];
    uint16_t port;
    mnet_socket_t sock;
    char *body = NULL;
    size_t body_len = 0;
    int status = -1;

    if (url == NULL) {
        return NULL;
    }

    /* Parse the URL */
    if (mnet_url_parse(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        return NULL;
    }

    /* Connect to the server */
    sock = mnet_socket_connect(host, port);
    if (sock == MNET_INVALID_SOCKET) {
        return NULL;
    }

    /* Set a receive timeout */
    {
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* Build and send the HTTP request */
    {
        char req[4096];
        int req_len;

        if (strncmp(url, "https://", 8) == 0) {
            req_len = snprintf(req, sizeof(req),
                "GET %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: mnet/0.2.0\r\n"
                "Accept: */*\r\n"
                "Connection: close\r\n"
                "\r\n",
                path, host);
        } else {
            req_len = snprintf(req, sizeof(req),
                "GET %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: mnet/0.2.0\r\n"
                "Accept: */*\r\n"
                "Connection: close\r\n"
                "\r\n",
                path, host);
        }

        if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
            mnet_close(sock);
            return NULL;
        }

        if (mnet_send(sock, req, (size_t)req_len) < 0) {
            mnet_close(sock);
            return NULL;
        }
    }

    /* Read the response */
    if (mnet_read_response(sock, &body, &body_len, MNET_MAX_BODY_SIZE) != 0) {
        mnet_close(sock);
        free(body);
        body = NULL;
        return NULL;
    }

    mnet_close(sock);

    /* Parse the status code from the response */
    {
        const char *status_line = strstr(body, "\r\n");
        if (status_line != NULL) {
            const char *status_start = status_line + 2;
            const char *status_end = strchr(status_start, ' ');
            if (status_end != NULL) {
                status = atoi(status_start);
            }
        }
    }

    /* Check for HTTP error status (4xx, 5xx) */
    if (status >= 400) {
        free(body);
        body = NULL;
        return NULL;
    }

    return body;
}

/* ---------------------------------------------------------------------------
 * Client-side HTTP async: perform a GET request. The callback is invoked with
 * the response body (or NULL on failure) on the caller's thread. The response
 * is freed automatically after the callback returns.
 * --------------------------------------------------------------------------- */
void mnet_call_async(const char *url, void (*callback)(char *body))
{
    char *body;

    if (url == NULL || callback == NULL) {
        return;
    }

    body = mnet_call(url);
    callback(body);
    free(body);
}

/* ---------------------------------------------------------------------------
 * Configuration helpers
 * --------------------------------------------------------------------------- */
void mnet_set_req_body_limit(mnet_app_t *app, size_t limit)
{
    if (app != NULL) {
        app->max_body_size = limit;
    }
}

/* ---------------------------------------------------------------------------
 * MNET_CALL macro: a convenience wrapper for client-side HTTP requests.
 *
 * MNET_CALL(url) expands to mnet_call(url), returning a pointer to a
 * malloc()-allocated string containing the response body. Returns NULL on
 * failure. The caller must free() the returned string when done.
 *
 * Usage:
 *   MNET_HANDLER(fetch_google)
 *   {
 *       (void)req;
 *       return mnet_json(MNET_CALL("https://google.com"));
 *   }
 *
 * Supported URL forms:
 *   - https://example.com
 *   - http://example.com:8080/path
 *   - example.com
 *   - example.com:8080/path
 * --------------------------------------------------------------------------- */
#define MNET_CALL(url) mnet_call(url)
