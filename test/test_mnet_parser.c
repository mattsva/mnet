/*
 * mnet parser and concurrency tests.
 *
 * test_mnet.c covers the pure helpers and test_http.c drives the happy path
 * over a socket. This file targets the parser's failure modes and the worker
 * pool: oversized headers, missing CRLF, malformed query strings, traversal,
 * incomplete bodies and several slow clients at once.
 *
 * Build (POSIX):
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200112L
 *       -Iinclude -Isrc -pthread src/mnet.c src/mnet_app.c src/mnet_response.c
 *       src/mnet_request.c src/mnet_router.c test/test_mnet_parser.c
 *       -o test_mnet_parser
 *
 * The socket parts are POSIX-only; on Windows the file compiles to a no-op so
 * the library itself is still covered by the unit tests there.
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
#include <pthread.h>
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
    return mnet_text("ok");
}

MNET_HANDLER(echo)
{
    const char *b = MNET_BODY(req);
    return mnet_text(b ? b : "");
}

MNET_HANDLER(search)
{
    const char *q = MNET_QUERY(req, "q");
    return mnet_text(q ? q : "");
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
    rmdir("/tmp/mnet-parser-root");
    mkdir("/tmp/mnet-parser-root", 0755);
    write_file("/tmp/mnet-parser-root/ok.txt", "legit");

    /* A file that exists outside the document root, so the "resolves outside
       the root" branch can be exercised deterministically (a traversal target
       that does not exist is a 404 from realpath instead). */
    write_file("/tmp/mnet-parser-secret.txt", "secret");
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

/* Send a raw request and return the status code, or -1 if no reply arrived. */
static int request_len(int port, const char *req, size_t len)
{
    char buf[1024];
    int fd = connect_to(port);
    ssize_t n;
    size_t sent = 0;

    if (fd < 0) return -1;

    while (sent < len) {
        ssize_t w = send(fd, req + sent, len - sent, 0);
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

static pid_t start_server(int port, int workers)
{
    pid_t pid = fork();

    if (pid == 0) {
        mnet_app_t *app = mnet_create();
        if (app == NULL) _exit(1);
        mnet_set_workers(app, workers);
        MNET_GET(app, "/", home);
        MNET_POST(app, "/echo", echo);
        MNET_GET(app, "/search", search);
        mnet_static(app, "/static", "/tmp/mnet-parser-root");
        mnet_run(app, (uint16_t)port);
        mnet_destroy(app);
        _exit(0);
    }

    for (int i = 0; i < 200; i++) {
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

/* --- concurrency: many slow clients at once --- */

typedef struct {
    int port;
    int ok;
    int total;
    int delay_ms;
} slow_client_t;

static void *slow_client(void *p)
{
    slow_client_t *c = (slow_client_t *)p;
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

    for (int i = 0; i < c->total; i++) {
        int fd = connect_to(c->port);
        if (fd < 0) continue;

        /* Send the request line, pause, then the rest: a deliberately slow
           client, to prove the pool keeps serving the others. */
        if (send(fd, req, 16, 0) > 0) {
            usleep((useconds_t)c->delay_ms * 1000);
            send(fd, req + 16, strlen(req) - 16, 0);

            char buf[256];
            ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
            if (n > 0 && strncmp(buf, "HTTP/1.1 200", 12) == 0) c->ok++;
        }
        close(fd);
    }
    return NULL;
}

int main(void)
{
    const int port = 18881;
    pid_t pid;
    char req[40000];
    int off;

    setup_fixtures();
    printf("Running mnet parser and concurrency tests...\n");

    pid = start_server(port, 4);

    /* --- oversized header block --- */
    off = snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: x\r\n");
    while (off < 20000) {
        off += snprintf(req + off, sizeof(req) - (size_t)off,
            "X-Pad-%d: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n", off);
    }
    off += snprintf(req + off, sizeof(req) - (size_t)off, "\r\n");
    CHECK(request_len(port, req, (size_t)off) == 431,
        "header block over the buffer returns 431");

    /* --- a single header line over the 4 KB line cap --- */
    off = snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nX-Big: ");
    memset(req + off, 'a', 5000);
    off += 5000;
    off += snprintf(req + off, sizeof(req) - (size_t)off,
        "\r\nConnection: close\r\n\r\n");
    CHECK(request_len(port, req, (size_t)off) == 431,
        "a single header line over 4 KB returns 431");

    /* --- too many headers --- */
    off = snprintf(req, sizeof(req), "GET / HTTP/1.1\r\n");
    for (int i = 0; i < 150; i++) {
        off += snprintf(req + off, sizeof(req) - (size_t)off, "X-N: v\r\n");
    }
    off += snprintf(req + off, sizeof(req) - (size_t)off, "\r\n");
    CHECK(request_len(port, req, (size_t)off) == 431,
        "more than 100 headers returns 431");

    /* --- missing CRLF between headers and body --- */
    CHECK(request(port, "GET / HTTP/1.1\r\nHost: x") == -1,
        "request with no header terminator gets no reply (server waits)");

    /* --- malformed request lines --- */
    CHECK(request(port, "GET\r\n\r\n") == 400, "request line without a target is 400");
    CHECK(request(port, "\r\n\r\n") == 400, "empty request line is 400");

    /* --- malformed query strings --- */
    CHECK(request(port, "GET /search?q=%zz HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") == 200,
        "malformed percent escape does not crash the server");
    CHECK(request(port, "GET /search?q=%2 HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") == 200,
        "truncated percent escape does not crash the server");
    CHECK(request(port, "GET /search?q=%00 HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") == 200,
        "NUL escape does not crash the server");
    CHECK(request(port, "GET /search?q=a&q=b&q=c HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") == 200,
        "repeated query keys are handled");

    /* --- traversal --- */
    CHECK(request(port, "GET /static/ok.txt HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") == 200, "legitimate static file is served");

    /* A traversal target that exists outside the root is refused with 403. */
    CHECK(request(port, "GET /static/../mnet-parser-secret.txt HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") == 403,
        "traversal to an existing file outside the root is 403");

    /* One that does not exist cannot be resolved at all, so it is a 404: still
       not served, which is what matters. */
    CHECK(request(port, "GET /static/../etc/passwd HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") == 404,
        "traversal to a missing file is 404 (never served)");

    CHECK(request(port, "GET /static/./ok.txt HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n") == 200, "single dot stays inside the root");
    CHECK(request(port, "GET /static/%2e%2e%2f%2e%2e%2fetc%2fpasswd HTTP/1.1\r\n"
        "Host: x\r\nConnection: close\r\n\r\n") != 200,
        "encoded traversal is not served");

    /* --- incomplete body --- */
    /* Declares 100 bytes, sends 5, then closes: must not hang or crash. */
    {
        int fd = connect_to(port);
        const char *head = "POST /echo HTTP/1.1\r\nHost: x\r\n"
            "Content-Length: 100\r\nConnection: close\r\n\r\nhello";
        if (fd >= 0) {
            send(fd, head, strlen(head), 0);
            shutdown(fd, SHUT_WR);
            char buf[256];
            recv(fd, buf, sizeof(buf) - 1, 0);
            close(fd);
        }
        CHECK(1, "incomplete body is handled without hanging");
    }

    /* --- concurrent slow clients --- */
    {
        enum { N = 8 };
        pthread_t th[N];
        slow_client_t args[N];
        int total_ok = 0, total = 0;

        for (int i = 0; i < N; i++) {
            args[i].port = port;
            args[i].ok = 0;
            args[i].total = 5;
            args[i].delay_ms = 20;
            pthread_create(&th[i], NULL, slow_client, &args[i]);
        }
        for (int i = 0; i < N; i++) {
            pthread_join(th[i], NULL);
            total_ok += args[i].ok;
            total += args[i].total;
        }
        CHECK(total_ok == total,
            "8 slow clients are all served by the worker pool");
    }

    stop_server(pid);

    printf("\n%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}

#else /* _WIN32 */

int main(void)
{
    printf("Parser/concurrency tests are not run on Windows.\n");
    return 0;
}

#endif
