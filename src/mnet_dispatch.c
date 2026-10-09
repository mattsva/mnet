/* src/mnet_dispatch.c - Route matching, response writing and dispatch to
 * handlers and middleware. */

#define _GNU_SOURCE
#include "mnet_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void send_response(mnet_socket_t client, const mnet_response_t *r,
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
                    if (chlen > 0 && (size_t)chlen < sizeof(chunk_header))
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

void send_simple_error(mnet_socket_t client, int status,
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
    /* snprintf returns the would-be length on truncation: clamp to the
       buffer so the send below cannot read past it. */
    if ((size_t)blen >= sizeof(body)) blen = (int)sizeof(body) - 1;

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

void send_not_found(mnet_socket_t client, const char *path, int debug,
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

void send_method_not_allowed(mnet_socket_t client, const char *method,
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

mnet_response_t dispatch(mnet_app_t *app, parsed_request_t *parsed,
    const char **param_values, size_t max_params, size_t *out_param_count)
{
    size_t param_count = 0;
    mnet_route_t *route = mnet_find_route(app, parsed->method_enum,
        parsed->path_only, param_values, max_params, &param_count);

    *out_param_count = param_count;

    /* If no routes are defined, serve the welcome page
       for the root path ("/") and "/index.html". */
    if (app->route_count == 0) {
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
