/*
 * mnet HTTP integration tests.
 *
 * The unit tests in test_mnet.c cover pure functions. These tests drive the
 * real server over a loopback socket, which is the only way to exercise the
 * request parser, body reading, header limits and static-file handling that
 * the security fixes touch.
 *
 * Build (POSIX):
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200112L
 *       -Iinclude -Isrc src/mnet.c src/mnet_app.c src/mnet_response.c
 *       src/mnet_request.c src/mnet_router.c test/test_http.c -o test_http
 *
 * On Windows the socket setup differs; the test is skipped there (the library
 * itself is still built and unit-tested on Windows).
 *
 * Note: the server is multi-threaded by default, so each connection here
 * sends one complete request and reads the reply. A receive timeout makes a
 * missing answer fail fast instead of hanging the run.
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

MNET_HANDLER(home)
{
    (void)req;
    return mnet_text("home");
}

MNET_HANDLER(echo)
{
    const char *b = MNET_BODY(req);
    return mnet_text(b ? b : "");
}

MNET_HANDLER(user)
{
    const char *id = MNET_PARAM(req, "id");
    return mnet_text(id ? id : "none");
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL) return;
    fputs(content, f);
    fclose(f);
}

static void setup_fixtures(void)
{
    rmdir("/tmp/mnet-http-test-root");
    rmdir("/tmp/mnet-http-test-root2");
    mkdir("/tmp/mnet-http-test-root", 0755);
    mkdir("/tmp/mnet-http-test-root2", 0755);
    write_file("/tmp/mnet-http-test-root/ok.txt", "legit");
    write_file("/tmp/mnet-http-test-root2/secret.txt", "secret");
}

static int connect_to(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    struct timeval tv;

    if (fd < 0) return -1;

    tv.tv_sec = 3;
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

/* Send exactly req_len bytes and return the response status code, or -1. */
static int request_len(int port, const char *req, size_t req_len)
{
    char buf[1024];
    ssize_t n;
    int fd = connect_to(port);

    if (fd < 0) return -1;

    size_t sent = 0;
    while (sent < req_len) {
        ssize_t w = send(fd, req + sent, req_len - sent, 0);
        if (w <= 0) { close(fd); return -1; }
        sent += (size_t)w;
    }

    n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    if (strncmp(buf, "HTTP/1.1 ", 9) != 0) return -1;
    return atoi(buf + 9);
}

static int request(int port, const char *req)
{
    return request_len(port, req, strlen(req));
}

static pid_t start_server(int port)
{
    pid_t pid = fork();

    if (pid == 0) {
        mnet_app_t *app = mnet_create();
        if (app == NULL) _exit(1);
        MNET_GET(app, "/", home);
        MNET_POST(app, "/echo", echo);
        MNET_GET(app, "/users/:id", user);
        mnet_static(app, "/static", "/tmp/mnet-http-test-root");
        mnet_run(app, (uint16_t)port);
        /* Destroy so a leak checker sees a clean exit, then leave without
           flushing the parent's stdio buffers. */
        mnet_destroy(app);
        _exit(0);
    }

    /* Wait for the listener to accept rather than sleeping a fixed time. */
    for (int i = 0; i < 100; i++) {
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
    const int port = 18771;
    pid_t pid;
    char req[40000];
    int off;

    setup_fixtures();

    printf("Running mnet HTTP integration tests...\n");

    pid = start_server(port);

    /* --- normal operation --- */
    CHECK(request(port, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        == 200, "GET / returns 200");

    CHECK(request(port, "POST /echo HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 5\r\nConnection: close\r\n\r\nhello") == 200,
        "POST with body returns 200");

    CHECK(request(port, "GET /users/42 HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") == 200, "routed path param returns 200");

    /* --- request-line / method validation --- */
    CHECK(request(port, "GARBAGE\r\n\r\n") == 400,
        "malformed request line returns 400");

    CHECK(request(port, "BREW / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        == 405, "unsupported method returns 405");

    /* --- body bounds --- */
    /* Content-Length 5 but 200 body bytes sent: must not overflow. */
    off = snprintf(req, sizeof(req),
        "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
        "Connection: close\r\n\r\n");
    memset(req + off, 'A', 200);
    CHECK(request_len(port, req, (size_t)(off + 200)) == 200,
        "body longer than Content-Length does not crash the server");

    /* Declared body larger than the cap must be rejected with 413. */
    CHECK(request(port, "POST /echo HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 99999999999999999999\r\nConnection: close\r\n\r\n")
        == 413, "oversized Content-Length returns 413");

    /* --- header limits --- */
    off = snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: x\r\n");
    while (off < 20000) {
        off += snprintf(req + off, sizeof(req) - (size_t)off,
            "X-Pad-%d: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n", off);
    }
    off += snprintf(req + off, sizeof(req) - (size_t)off, "\r\n");
    CHECK(request_len(port, req, (size_t)off) == 431,
        "header block larger than the buffer returns 431");

    /* --- static files --- */
    CHECK(request(port, "GET /static/ok.txt HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") == 200, "legitimate static file returns 200");

    CHECK(request(port, "GET /static/../../etc/passwd HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") == 403, "plain traversal returns 403");

    CHECK(request(port, "GET /static/%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") != 200, "encoded traversal is not served");

    CHECK(request(port, "GET /static/..%2f..%2fetc%2fpasswd HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") != 200, "mixed encoding traversal is not served");

    /* Prefix boundary: a sibling directory must not be reachable. */
    CHECK(request(port, "GET /static/../mnet-http-test-root2/secret.txt HTTP/1.1\r\n"
        "Host: x\r\nConnection: close\r\n\r\n") != 200,
        "sibling directory with a shared prefix is not served");

    stop_server(pid);

    printf("\n%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}

#else /* _WIN32 */

int main(void)
{
    printf("HTTP integration tests are not run on Windows.\n");
    return 0;
}

#endif
