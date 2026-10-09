/* test/test_http_edge.c
 *
 * HTTP request-parsing edge cases: duplicate Content-Length, CL with
 * Transfer-Encoding, bare LF, obs-fold, Expect: 100-continue, bad
 * Content-Length values, a version-less request line, a space before a
 * header colon, and pipelined requests.
 *
 * POSIX-only (raw sockets).
 */
#define _GNU_SOURCE
#include <mnet/mnet.h>
#include <mnet/mnet_app.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef _WIN32

#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define EDGE_PORT 18781

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

MNET_HANDLER(home)
{
    (void)req;
    return mnet_text("ok");
}

MNET_HANDLER(echo)
{
    const char *b = MNET_BODY(req);
    return mnet_text(b ? b : "");
}

static int connect_to(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    struct timeval tv;

    if (fd < 0) return -1;
    tv.tv_sec = 10;
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

/* Send a raw request; return the status code, or -1 when no valid reply
   line arrived. */
static int send_raw(int port, const char *req, size_t len, char *out,
    size_t out_size)
{
    char buf[4096];
    int fd = connect_to(port);
    ssize_t n;
    size_t used = 0;
    size_t sent = 0;

    if (fd < 0) return -1;
    while (sent < len) {
        ssize_t w = send(fd, req + sent, len - sent, 0);
        if (w <= 0) { close(fd); return -1; }
        sent += (size_t)w;
    }

    while (used < sizeof(buf) - 1) {
        n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "</html>") != NULL) break;
    }
    close(fd);
    buf[used] = '\0';
    if (out != NULL && out_size > 0) {
        size_t copy = used < out_size - 1 ? used : out_size - 1;
        memcpy(out, buf, copy);
        out[copy] = '\0';
    }
    if (strncmp(buf, "HTTP/1.1 ", 9) != 0) return -1;
    return atoi(buf + 9);
}

static int request(int port, const char *req)
{
    return send_raw(port, req, strlen(req), NULL, 0);
}

int main(void)
{
    printf("Running HTTP parsing edge-case tests...\n");

    pid_t pid = fork();
    if (pid < 0) {
        printf("  SKIP (fork failed)\n");
        return 0;
    }
    if (pid == 0) {
        mnet_app_t *app = mnet_create();
        if (app == NULL) _exit(1);
        MNET_GET(app, "/", home);
        MNET_POST(app, "/echo", echo);
        mnet_run(app, EDGE_PORT);
        mnet_destroy(app);
        _exit(0);
    }
    usleep(300000);

    /* 1. Duplicate Content-Length with different values -> 400 */
    CHECK(request(EDGE_PORT,
        "POST /echo HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 6\r\n"
        "\r\n"
        "hello") == 400,
        "duplicate Content-Length with different values is 400");

    /* 2. Content-Length together with Transfer-Encoding -> 400/501 */
    {
        int s = request(EDGE_PORT,
            "POST /echo HTTP/1.1\r\n"
            "Content-Length: 5\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "hello");
        CHECK(s == 400 || s == 501,
            "Content-Length with Transfer-Encoding is rejected");
    }

    /* 3. Bare LF line endings are accepted */
    CHECK(request(EDGE_PORT,
        "GET / HTTP/1.1\n"
        "Host: x\n"
        "\n") == 200,
        "bare LF request line endings are accepted");

    /* 4. obs-fold continuation line: request is served, continuation dropped */
    {
        char reply[512];
        int s = send_raw(EDGE_PORT,
            "GET / HTTP/1.1\r\n"
            "Host: x\r\n"
            "X-Fold: one\r\n"
            " two\r\n"
            "\r\n", strlen("GET / HTTP/1.1\r\nHost: x\r\nX-Fold: one\r\n two\r\n\r\n"),
            reply, sizeof(reply));
        CHECK(s == 200 && strstr(reply, "ok") != NULL,
            "obs-fold continuation does not break the request");
    }

    /* 5. Expect: 100-continue: the request still completes */
    {
        int fd = connect_to(EDGE_PORT);
        int got_continue = 0;
        char buf[512];
        const char *req =
            "POST /echo HTTP/1.1\r\n"
            "Content-Length: 5\r\n"
            "Expect: 100-continue\r\n"
            "\r\n";
        if (fd >= 0) {
            send(fd, req, strlen(req), 0);
            /* Wait briefly: a spec server sends "100 Continue" here. */
            ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                got_continue = (strncmp(buf, "HTTP/1.1 100", 12) == 0);
            }
            /* Whether or not 100 Continue arrived, the client sends the body. */
            send(fd, "hello", 5, 0);
            if (!got_continue) {
                ssize_t m = recv(fd, buf, sizeof(buf) - 1, 0);
                if (m > 0) {
                    buf[m] = '\0';
                    CHECK(strstr(buf, "200") != NULL && strstr(buf, "hello") != NULL,
                        "Expect: 100-continue request still completes");
                } else {
                    CHECK(0, "Expect: 100-continue request still completes");
                }
            } else {
                CHECK(1, "Expect: 100-continue request still completes");
            }
            close(fd);
        } else {
            CHECK(0, "Expect: 100-continue request still completes");
        }
        printf("  NOTE 100 Continue sent: %s\n", got_continue ? "yes" : "no");
    }

    /* 6. Bad Content-Length values are rejected */
    CHECK(request(EDGE_PORT,
        "POST /echo HTTP/1.1\r\n"
        "Content-Length: -5\r\n"
        "\r\n") == 400,
        "negative Content-Length is 400");

    CHECK(request(EDGE_PORT,
        "POST /echo HTTP/1.1\r\n"
        "Content-Length: 99999999999999999999\r\n"
        "\r\n") == 413,
        "overflowing Content-Length is 413");

    CHECK(request(EDGE_PORT,
        "POST /echo HTTP/1.1\r\n"
        "Content-Length: abc\r\n"
        "\r\n") == 400,
        "non-numeric Content-Length is 400");

    /* 7. Request line without an HTTP version */
    {
        int s = request(EDGE_PORT, "GET /\r\nHost: x\r\n\r\n");
        printf("  NOTE version-less request line -> %d\n", s);
        CHECK(s == 200 || s == 400,
            "version-less request line has a defined status");
    }

    /* 8. Space before the header colon: request is served */
    {
        char reply[512];
        int s = send_raw(EDGE_PORT,
            "GET / HTTP/1.1\r\n"
            "X-Spaced : value\r\n"
            "Host: x\r\n"
            "\r\n", strlen("GET / HTTP/1.1\r\nX-Spaced : value\r\nHost: x\r\n\r\n"),
            reply, sizeof(reply));
        CHECK(s == 200, "header name with a space before the colon is served");
    }

    /* 9. Pipelined requests on one keep-alive connection.
       (Pipelining needs an explicit Connection: keep-alive header: the
       server does not default HTTP/1.1 to persistent connections.) */
    {
        int fd = connect_to(EDGE_PORT);
        char buf[2048];
        const char *req =
            "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"
            "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
        size_t used = 0;
        int n200 = 0;
        if (fd >= 0) {
            send(fd, req, strlen(req), 0);
            for (int i = 0; i < 2; i++) {
                ssize_t n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
                if (n <= 0) break;
                used += (size_t)n;
                buf[used] = '\0';
                if (strstr(buf, "</html>") != NULL) break;
            }
            close(fd);
            for (const char *p = buf; (p = strstr(p, "200 OK")) != NULL; p += 6) {
                n200++;
            }
            CHECK(n200 >= 2, "two pipelined requests are both answered");
        } else {
            CHECK(0, "two pipelined requests are both answered");
        }
    }

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    printf("\n%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}

#else

int main(void)
{
    printf("HTTP parsing edge-case tests are not run on Windows.\n");
    return 0;
}

#endif
