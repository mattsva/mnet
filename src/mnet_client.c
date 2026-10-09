/* src/mnet_client.c - Client-side HTTP: URL parsing and mnet_call(). */

#define _GNU_SOURCE
#include "mnet_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#ifdef __APPLE__
            /* See mnet_tcp_accept: suppress SIGPIPE on this socket. */
            int one = 1;
            setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
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
        /* The port digits must end exactly where the path begins: strtol
           stops at the first non-digit, so endptr must equal path_start.
           Requiring *endptr == '\0' instead would reject every URL that
           carries both a port and a path (":8080/path"). */
        if (endptr != port_start && (const char *)endptr == path_start &&
            port > 0 && port < 65536) {
            *port_out = (uint16_t)port;
        }
    }

    return 0;
}

/* Read the HTTP response from a connected socket.
 *
 * Strips the response headers and returns only the body, NUL-terminated, in
 * *out_body (the caller frees it). *out_status receives the numeric status
 * code from the status line, or 0 if it could not be parsed. The response is
 * read until the server closes the connection (mnet_call always sends
 * "Connection: close"), bounded to max_body bytes of body.
 *
 * Returns 0 on success, -1 on failure. */
static int mnet_read_response(mnet_socket_t sock,
    char **out_body, size_t *out_body_len, int *out_status, size_t max_body)
{
    char buf[8192];
    char *raw = NULL;
    size_t raw_len = 0, raw_cap = 0;
    size_t cap = max_body + sizeof(buf);
    ssize_t n = 0;

    *out_body = NULL;
    *out_body_len = 0;
    *out_status = 0;

    while (raw_len < cap) {
        n = mnet_recv(sock, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && mnet_socket_errno() == MNET_EINTR) {
                continue;
            }
            break;
        }
        if (raw_len + (size_t)n > raw_cap) {
            size_t ncap = raw_cap ? raw_cap * 2 : 16384;
            while (ncap < raw_len + (size_t)n) ncap *= 2;
            char *nb = realloc(raw, ncap);
            if (nb == NULL) {
                free(raw);
                return -1;
            }
            raw = nb;
            raw_cap = ncap;
        }
        memcpy(raw + raw_len, buf, (size_t)n);
        raw_len += (size_t)n;
    }

    if (n < 0) {
        free(raw);
        return -1;
    }
    if (raw == NULL) {
        return -1; /* no response at all */
    }

    /* Parse the status code from the first line: "HTTP/1.1 200 OK". */
    if (raw_len >= 12 && strncmp(raw, "HTTP/", 5) == 0) {
        const char *sp = memchr(raw, ' ', raw_len);
        if (sp != NULL) {
            *out_status = atoi(sp + 1);
        }
    }

    /* The body starts after the first blank line. */
    size_t body_off = raw_len; /* default: no body */
    for (size_t i = 0; i + 1 < raw_len; i++) {
        if (raw[i] == '\r' && i + 3 < raw_len &&
            raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
            body_off = i + 4;
            break;
        }
        if (raw[i] == '\n' && raw[i + 1] == '\n') {
            body_off = i + 2;
            break;
        }
    }

    size_t body_len = raw_len - body_off;
    if (body_len > max_body) body_len = max_body;
    char *body = malloc(body_len + 1);
    if (body == NULL) {
        free(raw);
        return -1;
    }
    memcpy(body, raw + body_off, body_len);
    body[body_len] = '\0';
    free(raw);

    *out_body = body;
    *out_body_len = body_len;
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
#ifdef _WIN32
        /* Windows setsockopt takes optval as const char *. */
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    /* Build and send the HTTP request */
    {
        char req[4096];
        int req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: mnet/0.2.5\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host);

        if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
            mnet_close(sock);
            return NULL;
        }

        if (mnet_send(sock, req, (size_t)req_len) < 0) {
            mnet_close(sock);
            return NULL;
        }
    }

    /* Read the response (headers are stripped; status is parsed by the
       reader, since the body alone no longer carries it) */
    if (mnet_read_response(sock, &body, &body_len, &status,
            MNET_MAX_BODY_SIZE) != 0) {
        mnet_close(sock);
        return NULL;
    }

    mnet_close(sock);

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
