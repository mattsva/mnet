/*
 * mnet concurrency and stress tests.
 *
 * test_mnet_parser.c checks that individual malformed requests are handled.
 * This file is the opposite: it hammers a running server with many clients at
 * once, across every route kind (static, single-param, multi-param, query,
 * body, keep-alive, traversal, malformed) to shake out races, leaks and
 * missed cleanup paths that only show up under load.
 *
 * Build (POSIX):
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200112L
 *       -Iinclude -Isrc -pthread src/mnet.c src/mnet_app.c src/mnet_response.c
 *       src/mnet_request.c src/mnet_router.c test/test_stress.c -o test_stress
 *
 * The socket parts are POSIX-only; on Windows this compiles to a no-op.
 */
#define _GNU_SOURCE
#include <mnet.h>
#include <mnet_app.h>
#include <mnet_router.h>

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

MNET_HANDLER(h_root)
{
    (void)req;
    return mnet_text("root");
}

MNET_HANDLER(h_item)
{
    const char *id = MNET_PARAM(req, "id");
    return mnet_text(id ? id : "none");
}

MNET_HANDLER(h_sub)
{
    const char *id = MNET_PARAM(req, "id");
    const char *sid = MNET_PARAM(req, "sid");
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/%s", id ? id : "?", sid ? sid : "?");
    return mnet_text(buf);
}

MNET_HANDLER(h_echo)
{
    const char *b = MNET_BODY(req);
    size_t n = MNET_BODY_LEN(req);
    char buf[256];
    snprintf(buf, sizeof(buf), "%.*s", (int)(n < 200 ? n : 200), b ? b : "");
    return mnet_text(buf);
}

MNET_HANDLER(h_search)
{
    const char *q = MNET_QUERY(req, "q");
    return mnet_text(q ? q : "none");
}

MNET_HANDLER(h_header)
{
    const char *v = MNET_HEADER(req, "X-Probe");
    return mnet_text(v ? v : "none");
}

MNET_HANDLER(h_cookie)
{
    const char *v = MNET_COOKIE(req, "session");
    return mnet_text(v ? v : "none");
}

/* Sleeps, so that concurrent completion proves the worker pool is running. */
MNET_HANDLER(h_slow)
{
    (void)req;
    usleep(250 * 1000); /* 250 ms */
    return mnet_text("slow");
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
    rmdir("/tmp/mnet-stress-root");
    mkdir("/tmp/mnet-stress-root", 0755);
    write_file("/tmp/mnet-stress-root/ok.txt", "legit");
    write_file("/tmp/mnet-stress-secret.txt", "secret");
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

/*
 * Read a reply and return its status. The reply may arrive in more than one
 * segment, so keep reading until the full body promised by Content-Length has
 * been seen (or the peer closes / the read times out) instead of trusting a
 * single recv().
 */
static int read_status(int fd, char *out, size_t out_size)
{
    size_t used = 0;
    long content_length = -1;
    int header_done = 0;

    out[0] = '\0';

    for (;;) {
        ssize_t n = recv(fd, out + used, out_size - 1 - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        out[used] = '\0';

        if (!header_done) {
            char *hdr_end = strstr(out, "\r\n\r\n");
            if (hdr_end != NULL) {
                char *cl = strcasestr(out, "content-length:");
                header_done = 1;
                if (cl != NULL) content_length = atol(cl + 15);
                else content_length = 0;
                if (content_length >= 0) {
                    size_t header_bytes = (size_t)(hdr_end - out) + 4;
                    if (used >= header_bytes + (size_t)content_length) break;
                }
            }
        } else if (content_length >= 0) {
            char *hdr_end = strstr(out, "\r\n\r\n");
            size_t header_bytes = (size_t)(hdr_end - out) + 4;
            if (used >= header_bytes + (size_t)content_length) break;
        }

        if (used >= out_size - 1) break;
    }

    if (used == 0) return -1;
    if (strncmp(out, "HTTP/1.1 ", 9) != 0) return -1;
    return atoi(out + 9);
}

/* Read the full reply into buf (best effort) and return the status, or -1. */
static int roundtrip(int port, const char *req, char *out, size_t out_size)
{
    int fd = connect_to(port);
    size_t sent = 0;
    size_t len = strlen(req);

    if (fd < 0) return -1;

    while (sent < len) {
        ssize_t w = send(fd, req + sent, len - sent, 0);
        if (w <= 0) { close(fd); return -1; }
        sent += (size_t)w;
    }

    int status = read_status(fd, out, out_size);
    close(fd);
    return status;
}

static int request(int port, const char *req)
{
    char buf[8192];
    return roundtrip(port, req, buf, sizeof(buf));
}

static pid_t start_server(int port, int workers)
{
    pid_t pid;

    /* Flush before forking: otherwise the child inherits the parent's stdio
       buffer and re-emits anything not yet written when it flushes or exits. */
    fflush(stdout);
    fflush(stderr);

    pid = fork();

    if (pid == 0) {
        /* Detach stdin/stdout so the child's output cannot interleave with the
           test's, but keep stderr: a sanitizer report from the server must not
           be swallowed. */
        if (freopen("/dev/null", "r", stdin) == NULL ||
            freopen("/dev/null", "w", stdout) == NULL) {
            _exit(1);
        }

        mnet_app_t *app = mnet_create();
        if (app == NULL) _exit(1);
        if (workers > 0) mnet_set_workers(app, workers);
        MNET_GET(app, "/", h_root);
        MNET_GET(app, "/items/:id", h_item);
        MNET_GET(app, "/items/:id/sub/:sid", h_sub);
        MNET_POST(app, "/echo", h_echo);
        MNET_GET(app, "/search", h_search);
        MNET_GET(app, "/header", h_header);
        MNET_GET(app, "/cookie", h_cookie);
        MNET_GET(app, "/slow", h_slow);
        mnet_static(app, "/static", "/tmp/mnet-stress-root");
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

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* --- stress worker --- */

typedef struct {
    int port;
    int iterations;
    int ok;
    int total;
    unsigned seed;
} stress_arg_t;

static unsigned next_rand(unsigned *s)
{
    *s = (*s * 1103515245u) + 12345u;
    return (*s >> 16) & 0x7fff;
}

static void *stress_worker(void *p)
{
    stress_arg_t *a = (stress_arg_t *)p;
    char req[512];
    char buf[4096];
    unsigned s = a->seed;

    for (int i = 0; i < a->iterations; i++) {
        int status;
        int expect;

        switch (next_rand(&s) % 10) {
            case 0:
                snprintf(req, sizeof(req),
                    "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
                expect = 200;
                break;
            case 1:
                snprintf(req, sizeof(req),
                    "GET /items/%u HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                    next_rand(&s) % 1000);
                expect = 200;
                break;
            case 2:
                snprintf(req, sizeof(req),
                    "GET /items/%u/sub/%u HTTP/1.1\r\nHost: x\r\n"
                    "Connection: close\r\n\r\n",
                    next_rand(&s) % 100, next_rand(&s) % 100);
                expect = 200;
                break;
            case 3:
                snprintf(req, sizeof(req),
                    "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
                    "Connection: close\r\n\r\nhello");
                expect = 200;
                break;
            case 4:
                snprintf(req, sizeof(req),
                    "GET /search?q=abc%%20def HTTP/1.1\r\nHost: x\r\n"
                    "Connection: close\r\n\r\n");
                expect = 200;
                break;
            case 5:
                snprintf(req, sizeof(req),
                    "GET /header HTTP/1.1\r\nHost: x\r\nX-Probe: %u\r\n"
                    "Connection: close\r\n\r\n", next_rand(&s));
                expect = 200;
                break;
            case 6:
                snprintf(req, sizeof(req),
                    "GET /cookie HTTP/1.1\r\nHost: x\r\n"
                    "Cookie: session=%u; other=1\r\nConnection: close\r\n\r\n",
                    next_rand(&s));
                expect = 200;
                break;
            case 7:
                snprintf(req, sizeof(req),
                    "GET /static/ok.txt HTTP/1.1\r\nHost: x\r\n"
                    "Connection: close\r\n\r\n");
                expect = 200;
                break;
            case 8:
                /* Traversal must never succeed. */
                snprintf(req, sizeof(req),
                    "GET /static/../mnet-stress-secret.txt HTTP/1.1\r\nHost: x\r\n"
                    "Connection: close\r\n\r\n");
                expect = 403;
                break;
            default:
                /* Malformed: must be rejected, not crash. */
                snprintf(req, sizeof(req), "BROKEN\r\n\r\n");
                expect = 400;
                break;
        }

        status = roundtrip(a->port, req, buf, sizeof(buf));
        a->total++;
        if (status == expect) a->ok++;
    }
    return NULL;
}

/* A minimal worker that only hits /slow, used to time concurrency. */
static void *slow_worker(void *p)
{
    stress_arg_t *a = (stress_arg_t *)p;
    char buf[1024];
    int status = roundtrip(a->port,
        "GET /slow HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));

    a->total++;
    if (status == 200) a->ok++;
    return NULL;
}

int main(void)
{
    const int port = 18883;
    const int slow_port = 18884;
    pid_t pid;
    pid_t slow_pid;

    setup_fixtures();
    printf("Running mnet concurrency and stress tests...\n");

    /* --- threaded by default: concurrent slow requests overlap --- */
    slow_pid = start_server(slow_port, 0); /* 0 = leave the default pool size */

    {
        enum { N = 4 };
        pthread_t th[N];
        stress_arg_t args[N];
        double t0, elapsed;
        int ok = 0;

        for (int i = 0; i < N; i++) {
            args[i].port = slow_port;
            args[i].iterations = 1;
            args[i].ok = 0;
            args[i].total = 0;
            args[i].seed = 1;
        }

        t0 = now_ms();
        for (int i = 0; i < N; i++) {
            pthread_create(&th[i], NULL, slow_worker, &args[i]);
        }
        for (int i = 0; i < N; i++) {
            pthread_join(th[i], NULL);
            ok += args[i].ok;
        }
        elapsed = now_ms() - t0;

        CHECK(ok == N, "four parallel /slow requests all succeed");
        /* Each handler sleeps 250 ms. Serialised that is >= 1000 ms; the
           default pool of 4 should finish them together. */
        CHECK(elapsed < 700.0,
            "default worker pool serves them concurrently (not serialised)");
    }

    stop_server(slow_pid);

    /* --- wide mixed stress on a default-configured server --- */
    pid = start_server(port, 0);

    {
        enum { N = 32, ITERS = 40 };
        pthread_t th[N];
        stress_arg_t args[N];
        int ok = 0, total = 0;

        for (int i = 0; i < N; i++) {
            args[i].port = port;
            args[i].iterations = ITERS;
            args[i].ok = 0;
            args[i].total = 0;
            args[i].seed = (unsigned)(i * 7919 + 13);
            pthread_create(&th[i], NULL, stress_worker, &args[i]);
        }
        for (int i = 0; i < N; i++) {
            pthread_join(th[i], NULL);
            ok += args[i].ok;
            total += args[i].total;
        }

        printf("    %d/%d mixed requests correct\n", ok, total);
        CHECK(ok == total, "32 clients x 40 mixed requests all correct");
    }

    /* --- keep-alive: two requests on one connection --- */
    {
        int fd = connect_to(port);
        char buf[4096];
        int ok = 0;

        if (fd >= 0) {
            const char *r1 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
            const char *r2 = "GET /items/42 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

            send(fd, r1, strlen(r1), 0);
            if (read_status(fd, buf, sizeof(buf)) == 200) ok++;

            send(fd, r2, strlen(r2), 0);
            if (read_status(fd, buf, sizeof(buf)) == 200) ok++;

            close(fd);
        }
        CHECK(ok == 2, "keep-alive serves two requests on one connection");
    }

    /* --- repeated requests do not exhaust anything --- */
    {
        int ok = 1;
        for (int i = 0; i < 300; i++) {
            if (request(port, "GET /items/7 HTTP/1.1\r\nHost: x\r\n"
                "Connection: close\r\n\r\n") != 200) {
                ok = 0;
                break;
            }
        }
        CHECK(ok, "300 sequential routed requests all succeed");
    }

    stop_server(pid);

    printf("\n%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}

#else /* _WIN32 */

int main(void)
{
    printf("Concurrency/stress tests are not run on Windows.\n");
    return 0;
}

#endif
