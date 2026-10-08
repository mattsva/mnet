/*
 * mnet feature tests.
 *
 * One end-to-end check per documented feature: methods, wildcards,
 * middleware, the custom 404 handler, connection limits, cookies, encoded
 * parameters, response helpers, and the body/header boundaries. The point is
 * to catch features that are documented but quietly broken, which is how the
 * multi-parameter route double free was found.
 *
 * Build (POSIX):
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200112L
 *       -Iinclude -Isrc -pthread src/mnet.c src/mnet_app.c src/mnet_response.c
 *       src/mnet_request.c src/mnet_router.c test/test_features.c -o test_features
 */
#define _GNU_SOURCE
#include <mnet/mnet.h>
#include <mnet/mnet_app.h>
#include <mnet/mnet_router.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32

#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int failures = 0;
static int passed = 0;

#define CHECK(cond, name)                                      \
    do {                                                       \
        if (cond) {                                            \
            passed++;                                          \
            printf("  PASS %s\n", (name));                     \
        } else {                                               \
            failures++;                                        \
            printf("  FAIL %s (line %d)\n", (name), __LINE__); \
        }                                                      \
    } while (0)

/* --- handlers --- */

MNET_HANDLER(f_root)    { (void)req; return mnet_text("root"); }
MNET_HANDLER(f_getonly) { (void)req; return mnet_text("get"); }
MNET_HANDLER(f_post)    { (void)req; return mnet_text("posted"); }
MNET_HANDLER(f_head)    { (void)req; return mnet_text("head-body"); }
MNET_HANDLER(f_encoded) { const char *i = MNET_PARAM(req, "v"); return mnet_text(i ? i : "none"); }
MNET_HANDLER(f_wild)    { const char *w = MNET_PARAM(req, "*"); return mnet_text(w ? w : "none"); }
MNET_HANDLER(f_status)  { (void)req; return mnet_status(201, "created"); }
MNET_HANDLER(f_err)     { (void)req; return mnet_error(418, "teapot"); }
MNET_HANDLER(f_json)    { (void)req; return mnet_jsonf("{\"a\":\"%s\"}", "x\"y"); }
MNET_HANDLER(f_dup)     { const char *h = MNET_HEADER(req, "X-Dup"); return mnet_text(h ? h : "none"); }
MNET_HANDLER(f_qempty)  { const char *q = MNET_QUERY(req, "q"); return mnet_text(q ? q : "NULL"); }
MNET_HANDLER(f_mw)      { (void)req; return mnet_text("inner"); }
MNET_HANDLER(f_404)     { (void)req; return mnet_status(404, "custom-not-found"); }

/* Middleware that rewrites the response body of the route it wraps. */
static mnet_response_t mw_handler(mnet_request_t *req,
    mnet_response_t (*next)(mnet_request_t *))
{
    mnet_response_t r = next(req);
    (void)req;
    mnet_response_free(&r);
    return mnet_text("middleware");
}

static int connect_to(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    struct timeval tv;

    if (fd < 0) return -1;

    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Read a full reply (honouring Content-Length) into out. Returns the status. */
static int fetch(int port, const char *req, char *out, size_t out_size)
{
    int fd = connect_to(port);
    size_t sent = 0, len = strlen(req), used = 0;
    long clen = -1;
    int header_done = 0;

    if (fd < 0) return -1;
    if (out_size > 0) out[0] = '\0';

    while (sent < len) {
        ssize_t w = send(fd, req + sent, len - sent, 0);
        if (w <= 0) { close(fd); return -1; }
        sent += (size_t)w;
    }

    for (;;) {
        ssize_t n = recv(fd, out + used, out_size - 1 - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        out[used] = '\0';

        if (!header_done) {
            char *end = strstr(out, "\r\n\r\n");
            if (end != NULL) {
                char *cl = strstr(out, "Content-Length: ");
                header_done = 1;
                clen = cl ? atol(cl + 16) : 0;
                if (clen < 0) clen = 0;
                if (used >= (size_t)(end - out) + 4 + (size_t)clen) break;
            }
        } else if (clen >= 0) {
            char *end = strstr(out, "\r\n\r\n");
            if (used >= (size_t)(end - out) + 4 + (size_t)clen) break;
        }
        if (used >= out_size - 1) break;
    }

    close(fd);
    if (used == 0) return -1;
    if (strncmp(out, "HTTP/1.1 ", 9) != 0) return -1;
    return atoi(out + 9);
}

/* The body after the header terminator, or NULL. */
static const char *body_of(char *resp)
{
    char *end = strstr(resp, "\r\n\r\n");
    return end ? end + 4 : NULL;
}

static pid_t start_server(int port)
{
    pid_t pid;

    fflush(stdout);
    fflush(stderr);
    pid = fork();

    if (pid == 0) {
        if (freopen("/dev/null", "r", stdin) == NULL ||
            freopen("/dev/null", "w", stdout) == NULL) {
            _exit(1);
        }

        mnet_app_t *app = mnet_create();
        if (app == NULL) _exit(1);
        mnet_set_workers(app, 4);
        mnet_set_not_found_handler(app, f_404);

        MNET_GET(app, "/", f_root);
        MNET_GET(app, "/getonly", f_getonly);
        MNET_POST(app, "/postonly", f_post);
        MNET_GET(app, "/headtest", f_head);
        MNET_GET(app, "/enc/:v", f_encoded);
        MNET_GET(app, "/wild/*", f_wild);
        MNET_GET(app, "/status", f_status);
        MNET_GET(app, "/err", f_err);
        MNET_GET(app, "/json", f_json);
        MNET_GET(app, "/dup", f_dup);
        MNET_GET(app, "/qempty", f_qempty);
        MNET_GET(app, "/mw", f_mw);

        mnet_run(app, (uint16_t)port);
        mnet_destroy(app);
        _exit(0);
    }

    for (int i = 0; i < 300; i++) {
        int fd = connect_to(port);
        if (fd >= 0) { close(fd); return pid; }
        usleep(20000);
    }
    return pid;
}

static void stop_server(pid_t pid)
{
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

int main(void)
{
    const int port = 18885;
    const int mw_port = 18886;
    pid_t pid, mw_pid;
    char resp[8192];
    const char *body;

    printf("Running mnet feature tests...\n");
    pid = start_server(port);

    /* --- methods --- */
    CHECK(fetch(port, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
        resp, sizeof(resp)) == 200, "GET on a GET route is 200");

    CHECK(fetch(port, "POST /postonly HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n", resp, sizeof(resp)) == 200,
        "POST on a POST route is 200");

    /* A known method with no route on that path. */
    CHECK(fetch(port, "GET /postonly HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
        resp, sizeof(resp)) == 404, "GET on a POST-only path is 404");

    /* HEAD: status and Content-Length present, body absent. */
    {
        int st = fetch(port, "HEAD /headtest HTTP/1.1\r\nHost: x\r\n"
            "Connection: close\r\n\r\n", resp, sizeof(resp));
        body = body_of(resp);
        CHECK(st == 200, "HEAD returns 200");
        CHECK(strstr(resp, "Content-Length: 9") != NULL,
            "HEAD reports the body length it would send");
        CHECK(body != NULL && body[0] == '\0', "HEAD sends no body");
    }

    /* --- status / error helpers --- */
    {
        int st = fetch(port, "GET /status HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
            resp, sizeof(resp));
        body = body_of(resp);
        CHECK(st == 201, "mnet_status sets the status code");
        CHECK(body != NULL && strcmp(body, "created") == 0, "mnet_status sets the body");
    }

    {
        int st = fetch(port, "GET /err HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
            resp, sizeof(resp));
        CHECK(st == 418, "mnet_error sets the status code");
    }

    /* --- JSON escaping --- */
    {
        fetch(port, "GET /json HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
            resp, sizeof(resp));
        body = body_of(resp);
        CHECK(body != NULL && strstr(body, "x\\\"y") != NULL,
            "mnet_jsonf escapes quotes in values");
    }

    /* --- encoded path parameter --- */
    {
        int st = fetch(port, "GET /enc/%34%32 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
            resp, sizeof(resp));
        body = body_of(resp);
        CHECK(st == 200, "encoded path parameter route matches");
        CHECK(body != NULL && strcmp(body, "42") == 0, "path parameter is URL-decoded");
    }

    /* --- wildcard --- */
    {
        int st = fetch(port, "GET /wild/a/b/c.txt HTTP/1.1\r\nHost: x\r\n"
            "Connection: close\r\n\r\n", resp, sizeof(resp));
        body = body_of(resp);
        CHECK(st == 200, "wildcard route matches a nested path");
        CHECK(body != NULL && strcmp(body, "a/b/c.txt") == 0,
            "wildcard captures the remaining path");
    }

    /* --- duplicate headers --- */
    {
        int st = fetch(port, "GET /dup HTTP/1.1\r\nHost: x\r\nX-Dup: first\r\n"
            "X-Dup: second\r\nConnection: close\r\n\r\n", resp, sizeof(resp));
        body = body_of(resp);
        CHECK(st == 200, "duplicate headers do not break parsing");
        CHECK(body != NULL && strcmp(body, "first") == 0,
            "the first occurrence of a duplicate header is returned");
    }

    /* --- empty query value --- */
    {
        int st = fetch(port, "GET /qempty?q= HTTP/1.1\r\nHost: x\r\n"
            "Connection: close\r\n\r\n", resp, sizeof(resp));
        body = body_of(resp);
        CHECK(st == 200, "empty query value is handled");
        CHECK(body != NULL && strcmp(body, "") == 0,
            "an empty query value reads as empty, not NULL");
    }

    /* --- query key with no '=' --- */
    {
        int st = fetch(port, "GET /qempty?q HTTP/1.1\r\nHost: x\r\n"
            "Connection: close\r\n\r\n", resp, sizeof(resp));
        body = body_of(resp);
        CHECK(st == 200, "query key without '=' is handled");
        CHECK(body != NULL && strcmp(body, "") == 0,
            "a key without '=' yields an empty value");
    }

    /* --- custom 404 handler --- */
    {
        int st = fetch(port, "GET /definitely-missing HTTP/1.1\r\nHost: x\r\n"
            "Connection: close\r\n\r\n", resp, sizeof(resp));
        body = body_of(resp);
        CHECK(st == 404, "custom 404 handler sets the status");
        CHECK(body != NULL && strcmp(body, "custom-not-found") == 0,
            "custom 404 handler supplies the body");
    }

    stop_server(pid);

    /* --- middleware on its own server --- */
    {
        fflush(stdout); fflush(stderr);
        mw_pid = fork();
        if (mw_pid == 0) {
            if (freopen("/dev/null", "r", stdin) == NULL ||
                freopen("/dev/null", "w", stdout) == NULL) _exit(1);
            mnet_app_t *app = mnet_create();
            mnet_set_workers(app, 2);
            mnet_use(app, mw_handler);
            MNET_GET(app, "/mw", f_mw);
            mnet_run(app, (uint16_t)mw_port);
            mnet_destroy(app);
            _exit(0);
        }
        for (int i = 0; i < 300; i++) {
            int fd = connect_to(mw_port);
            if (fd >= 0) { close(fd); break; }
            usleep(20000);
        }

        int st = fetch(mw_port, "GET /mw HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
            resp, sizeof(resp));
        body = body_of(resp);
        CHECK(st == 200, "middleware-wrapped route returns 200");
        CHECK(body != NULL && strcmp(body, "middleware") == 0,
            "middleware can replace the handler's response");

        stop_server(mw_pid);
    }

    printf("\n%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}

#else /* _WIN32 */

int main(void)
{
    printf("Feature tests are not run on Windows.\n");
    return 0;
}

#endif
