/*
 * Regression tests for the request-handling security fixes:
 *
 *   - mnet_jsonf: every %s variant is escaped, nothing reads past the format
 *     string, large values are not truncated or over-read.
 *   - Header parsing is confined to the header block: request body content
 *     can never appear as a request header.
 *   - Content-Length / Transfer-Encoding handling.
 *   - A client that disconnects mid-response cannot kill the server (SIGPIPE).
 *   - Slow clients are cut off by an absolute deadline, not a per-recv() one.
 *
 * The jsonf tests are pure functions and run everywhere. The server tests
 * drive a real server over loopback and are skipped on Windows.
 *
 * Build: see the Makefile (make test_security).
 */
#define _GNU_SOURCE
#include <mnet/mnet.h>
#include <mnet/mnet_app.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
/* mnet_jsonf                                                          */
/* ------------------------------------------------------------------ */

/* True if r is a 200 whose body equals `expect`. Frees r. */
static int body_is(mnet_response_t r, const char *expect)
{
    int ok = r.status == 200 && r.body != NULL &&
        r.body_length == strlen(expect) &&
        memcmp(r.body, expect, r.body_length) == 0 &&
        ((const char *)r.body)[r.body_length] == '\0';

    mnet_response_free(&r);
    return ok;
}

/* True if r is the 500 produced for a rejected format. Frees r. */
static int is_rejected(mnet_response_t r)
{
    int ok = r.status == 500;

    mnet_response_free(&r);
    return ok;
}

static mnet_response_t fmt_str(const char *fmt, const char *arg)
{
    return mnet_jsonf(fmt, arg);
}

static void test_jsonf(void)
{
    mnet_response_t r;


    /* A precision on %s must not skip escaping (previously it was passed
       straight to vsnprintf, which allowed breaking out of the string). */
    CHECK(body_is(fmt_str("{\"a\":\"%.40s\"}", "x\",\"admin\":true,\"y\":\""),
        "{\"a\":\"x\\\",\\\"admin\\\":true,\\\"y\\\":\\\"\"}"),
        "jsonf: %.Ns escapes quotes (no JSON injection)");

    CHECK(body_is(fmt_str("{\"a\":\"%.3s\"}", "abcdef"), "{\"a\":\"abc\"}"),
        "jsonf: %.Ns truncates the source");

    CHECK(body_is(fmt_str("[\"%.1s\"]", "\xC3\xA9"), "[\"\"]"),
        "jsonf: precision never splits a UTF-8 sequence");

    CHECK(body_is(fmt_str("[\"%.0s\"]", "abc"), "[\"\"]"),
        "jsonf: %.0s is empty");

    CHECK(is_rejected(fmt_str("{\"a\":\"%5s\"}", "x")),
        "jsonf: width on %s is rejected");
    CHECK(is_rejected(fmt_str("{\"a\":\"%-8s\"}", "x")),
        "jsonf: flags on %s are rejected");
    CHECK(is_rejected(fmt_str("{\"a\":\"%ls\"}", "x")),
        "jsonf: length modifier on %s is rejected");

    /* Format strings that used to run off the end of the format. */
    CHECK(is_rejected(mnet_jsonf("{\"a\":%")), "jsonf: trailing lone % is rejected");
    CHECK(is_rejected(mnet_jsonf("{\"a\":%5")), "jsonf: truncated spec is rejected");
    CHECK(is_rejected(mnet_jsonf("{\"a\":%.")), "jsonf: truncated precision is rejected");

    CHECK(is_rejected(mnet_jsonf("%n", (int *)NULL)), "jsonf: %n is rejected");
    CHECK(is_rejected(mnet_jsonf("%p", (void *)NULL)), "jsonf: %p is rejected");
    CHECK(is_rejected(mnet_jsonf("%*d", 5, 1)), "jsonf: '*' width is rejected");
    CHECK(is_rejected(mnet_jsonf("%a", 1.0)), "jsonf: %a is rejected");
    CHECK(is_rejected(mnet_jsonf("%123456d", 1)), "jsonf: absurd width is rejected");
    CHECK(is_rejected(mnet_jsonf(NULL)), "jsonf: NULL format is rejected");

    /* %f of a huge double is 308 bytes: larger than the old 256-byte buffer,
       which made the old code copy 308 bytes out of a 256-byte array. */
    r = mnet_jsonf("{\"d\":%f}", 1e300);
    CHECK(r.status == 200 && r.body_length == 5 + 308 + 1 &&
        memcmp((const char *)r.body + r.body_length - 8, ".000000}", 8) == 0 &&
        ((const char *)r.body)[r.body_length] == '\0',
        "jsonf: %f of 1e300 is complete, not truncated or over-read");
    mnet_response_free(&r);

    CHECK(body_is(mnet_jsonf("{\"d\":%.2f}", 3.14159), "{\"d\":3.14}"),
        "jsonf: %.2f");
    CHECK(body_is(mnet_jsonf("{\"d\":%g}", 0.5), "{\"d\":0.5}"), "jsonf: %g");
    CHECK(body_is(mnet_jsonf("{\"d\":%f}", NAN), "{\"d\":null}"),
        "jsonf: NaN becomes null (valid JSON)");
    CHECK(body_is(mnet_jsonf("{\"d\":%f}", INFINITY), "{\"d\":null}"),
        "jsonf: Inf becomes null (valid JSON)");
    CHECK(body_is(mnet_jsonf("{\"d\":%Lf}", (long double)1.5L), "{\"d\":1.500000}"),
        "jsonf: %Lf");

    CHECK(body_is(mnet_jsonf("{\"n\":%d}", -42), "{\"n\":-42}"), "jsonf: %d");
    CHECK(body_is(mnet_jsonf("{\"n\":%5d}", 42), "{\"n\":   42}"), "jsonf: %5d");
    CHECK(body_is(mnet_jsonf("{\"n\":%05d}", 42), "{\"n\":00042}"), "jsonf: %05d");
    CHECK(body_is(mnet_jsonf("{\"n\":%ld}", -5000000000L), "{\"n\":-5000000000}"),
        "jsonf: %ld");
    CHECK(body_is(mnet_jsonf("{\"n\":%lld}", 9000000000000000000LL),
        "{\"n\":9000000000000000000}"), "jsonf: %lld");
    CHECK(body_is(mnet_jsonf("{\"n\":%zu}", (size_t)123), "{\"n\":123}"), "jsonf: %zu");
    CHECK(body_is(mnet_jsonf("{\"n\":%u}", 7u), "{\"n\":7}"), "jsonf: %u");
    CHECK(body_is(mnet_jsonf("{\"n\":%x}", 255u), "{\"n\":ff}"), "jsonf: %x");
    CHECK(body_is(mnet_jsonf("{\"n\":%hhd}", 300), "{\"n\":44}"), "jsonf: %hhd");

    CHECK(body_is(mnet_jsonf("{\"p\":\"100%%\"}"), "{\"p\":\"100%\"}"), "jsonf: %%");
    CHECK(body_is(fmt_str("{\"a\":\"%s\"}", NULL), "{\"a\":\"(null)\"}"),
        "jsonf: NULL %s argument");
    CHECK(body_is(mnet_jsonf("{\"c\":\"%c\"}", 'z'), "{\"c\":\"z\"}"), "jsonf: %c");
    CHECK(body_is(mnet_jsonf("{\"c\":\"%c\"}", '"'), "{\"c\":\"\\\"\"}"),
        "jsonf: %c escapes a quote");
    CHECK(body_is(mnet_jsonf("{\"c\":\"%c\"}", 0), "{\"c\":\"\\u0000\"}"),
        "jsonf: %c with NUL");

    /* Mixed arguments must stay aligned: an earlier conversion must consume
       exactly its own argument. */
    CHECK(body_is(mnet_jsonf("{\"a\":\"%s\",\"n\":%d,\"d\":%.1f,\"b\":\"%.2s\"}",
        "q\"", 9, 2.5, "wxyz"),
        "{\"a\":\"q\\\"\",\"n\":9,\"d\":2.5,\"b\":\"wx\"}"),
        "jsonf: mixed conversions stay aligned");

    {
        char big[100001];

        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        r = mnet_jsonf("{\"d\":\"%s\"}", big);
        CHECK(r.status == 200 && r.body_length == 6 + 100000 + 2,
            "jsonf: 100 KB string");
        mnet_response_free(&r);
    }
}

/* ------------------------------------------------------------------ */
/* Server tests                                                        */
/* ------------------------------------------------------------------ */

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PORT 18779
#define SERVER_TIMEOUT_S 2

MNET_HANDLER(h_hdr)
{
    const char *h = MNET_HEADER(req, "X-Admin");
    const char *a = MNET_HEADER(req, "Authorization");
    return mnet_jsonf("{\"x-admin\":\"%s\",\"auth\":\"%s\"}",
        h ? h : "none", a ? a : "none");
}

MNET_HANDLER(h_len)
{
    return mnet_jsonf("{\"n\":%zu}", MNET_BODY_LEN(req));
}

MNET_HANDLER(h_q)
{
    (void)req;
    return mnet_text("ok");
}

/* A response far larger than the socket buffers, so the server is still
   writing when the client vanishes. */
MNET_HANDLER(h_big)
{
    size_t n = 4 * 1024 * 1024;
    char *b = malloc(n + 1);
    mnet_response_t r;

    (void)req;
    if (b == NULL) return mnet_error(500, "oom");
    memset(b, 'a', n);
    b[n] = '\0';
    r = mnet_text(b);
    free(b);
    return r;
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

static void send_all_fd(int fd, const void *data, size_t len)
{
    const char *p = data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t w = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (w <= 0) return;
        sent += (size_t)w;
    }
}

/* Read until the peer closes or `ms` elapse. Returns bytes read. */
static size_t read_to_close(int fd, char *buf, size_t size, int ms)
{
    size_t used = 0;
    int64_t end = now_ms() + ms;

    while (used < size - 1) {
        int64_t left = end - now_ms();
        struct pollfd p = { fd, POLLIN, 0 };
        ssize_t n;

        if (left <= 0) break;
        if (poll(&p, 1, (int)left) <= 0) break;
        n = recv(fd, buf + used, size - 1 - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
    }
    buf[used] = '\0';
    return used;
}

/* One request on its own connection. Fills `body` (may be NULL) with the
   response body and returns the status code, or -1. */
static int fetch(const char *req, size_t req_len, char *body, size_t body_size)
{
    char buf[8192];
    int fd = connect_to(PORT);
    int status;
    const char *sep;

    if (fd < 0) return -1;
    send_all_fd(fd, req, req_len);
    read_to_close(fd, buf, sizeof(buf), 8000);
    close(fd);

    if (strncmp(buf, "HTTP/1.1 ", 9) != 0) return -1;
    status = atoi(buf + 9);
    if (body != NULL) {
        sep = strstr(buf, "\r\n\r\n");
        snprintf(body, body_size, "%s", sep ? sep + 4 : "");
    }
    return status;
}

static int fetch_str(const char *req, char *body, size_t body_size)
{
    return fetch(req, strlen(req), body, body_size);
}

static pid_t start_server(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        mnet_app_t *app = mnet_create();

        /* The parent ignores SIGPIPE so its own writes cannot kill the test;
           an ignored disposition is inherited across fork(), which would hide
           exactly the bug under test. Restore the default in the server. */
        signal(SIGPIPE, SIG_DFL);

        if (app == NULL) _exit(1);
        MNET_POST(app, "/hdr", h_hdr);
        MNET_POST(app, "/len", h_len);
        MNET_GET(app, "/q", h_q);
        MNET_GET(app, "/big", h_big);
        mnet_set_workers(app, 2);
        mnet_set_timeout(app, SERVER_TIMEOUT_S);
        mnet_set_keep_alive_timeout(app, SERVER_TIMEOUT_S);
        mnet_run(app, PORT);
        mnet_destroy(app);
        _exit(0);
    }

    for (int i = 0; i < 100; i++) {
        int fd = connect_to(PORT);
        if (fd >= 0) { close(fd); return pid; }
        usleep(20000);
    }
    return pid;
}

static int server_alive(pid_t pid)
{
    return waitpid(pid, NULL, WNOHANG) == 0;
}

static void test_sigpipe(pid_t pid)
{
    char buf[256];
    int ok = 1;

    for (int i = 0; i < 20; i++) {
        struct linger lg = { 1, 0 }; /* close() sends RST */
        int fd = connect_to(PORT);

        if (fd < 0) { ok = 0; break; }
        send_all_fd(fd,
            "GET /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
            strlen("GET /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
        usleep(30000);
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(fd);
    }
    usleep(200000);

    CHECK(ok && server_alive(pid),
        "server survives clients that reset the connection mid-response");
    CHECK(fetch_str("GET /q HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf)) == 200, "server still answers after the resets");
}

static void test_header_scoping(void)
{
    char out[512];
    int st;

    st = fetch_str("POST /hdr HTTP/1.1\r\nHost: x\r\nContent-Length: 39\r\n"
        "Connection: close\r\n\r\nX-Admin: true\r\nAuthorization: Bearer evil",
        out, sizeof(out));
    CHECK(st == 200 && strstr(out, "\"x-admin\":\"none\"") != NULL &&
        strstr(out, "\"auth\":\"none\"") != NULL,
        "headers written in the request body are not request headers");

    st = fetch_str("POST /hdr HTTP/1.1\r\nHost: x\r\nX-Admin: real\r\n"
        "Content-Length: 13\r\nConnection: close\r\n\r\nX-Admin: fake",
        out, sizeof(out));
    CHECK(st == 200 && strstr(out, "\"x-admin\":\"real\"") != NULL,
        "real headers still parse when the body looks like headers");

    st = fetch_str("POST /hdr HTTP/1.1\nHost: x\nX-Admin: bare\n"
        "Connection: close\n\n", out, sizeof(out));
    CHECK(st == 200 && strstr(out, "\"x-admin\":\"bare\"") != NULL,
        "bare-LF requests have their headers parsed");
}

static void test_body_limits_not_applied_to_body(void)
{
    char req[16384];
    char out[128];
    char body[5001];
    int n, st;

    memset(body, 'a', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';
    n = snprintf(req, sizeof(req),
        "POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 5000\r\n"
        "Connection: close\r\n\r\n%s", body);
    st = fetch(req, (size_t)n, out, sizeof(out));
    CHECK(st == 200 && strstr(out, "\"n\":5000") != NULL,
        "a 5000-byte single-line body is accepted (was 431)");

    n = snprintf(req, sizeof(req),
        "POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 800\r\n"
        "Connection: close\r\n\r\n");
    for (int i = 0; i < 200; i++) {
        n += snprintf(req + n, sizeof(req) - (size_t)n, "a:b\n");
    }
    st = fetch(req, (size_t)n, out, sizeof(out));
    CHECK(st == 200 && strstr(out, "\"n\":800") != NULL,
        "a 200-line body is accepted (was 431)");
}

static void test_content_length(void)
{
    char out[128];

    CHECK(fetch_str("POST /len HTTP/1.1\r\nHost: x\r\nContent-Length:5\r\n"
        "Connection: close\r\n\r\nhello", out, sizeof(out)) == 200 &&
        strstr(out, "\"n\":5") != NULL,
        "Content-Length without a space is read correctly");

    CHECK(fetch_str("POST /len HTTP/1.1\r\nHost: x\r\nContent-Length:100\r\n"
        "Connection: close\r\n\r\nxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", out, sizeof(out))
        == 200 && strstr(out, "\"n\":100") != NULL,
        "Content-Length:100 is not misread as 00");

    CHECK(fetch_str("POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n"
        "Connection: close\r\n\r\nhello", NULL, 0) == 400,
        "non-numeric Content-Length returns 400");

    CHECK(fetch_str("POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
        "Content-Length: 3\r\nConnection: close\r\n\r\nhello", NULL, 0) == 400,
        "duplicate Content-Length returns 400");

    CHECK(fetch_str("POST /len HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
        "5\r\nhello\r\n0\r\n\r\n", NULL, 0) == 501,
        "Transfer-Encoding returns 501 instead of desyncing the stream");

    CHECK(fetch_str("POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999\r\n"
        "Connection: close\r\n\r\n", NULL, 0) == 413,
        "oversized Content-Length still returns 413");

    /* No Content-Length: there is no body, and a "Content-Length:" line that
       appears after the blank line must not be honoured. */
    CHECK(fetch_str("POST /len HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
        "Content-Length: 3\r\nabc", out, sizeof(out)) == 200 &&
        strstr(out, "\"n\":0") != NULL,
        "bytes after the headers are not a body without Content-Length");
}

static void test_keepalive_and_pipelining(void)
{
    char buf[8192];
    const char *one =
        "GET /q HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
    int fd = connect_to(PORT);
    size_t n;
    int responses = 0;

    CHECK(fd >= 0, "connect for keep-alive test");
    if (fd < 0) return;

    /* Two sequential requests on one connection still work. */
    send_all_fd(fd, one, strlen(one));
    n = (size_t)recv(fd, buf, sizeof(buf) - 1, 0);
    buf[n > 0 ? n : 0] = '\0';
    responses += strstr(buf, "HTTP/1.1 200") != NULL;
    send_all_fd(fd, one, strlen(one));
    n = (size_t)recv(fd, buf, sizeof(buf) - 1, 0);
    buf[n > 0 ? n : 0] = '\0';
    responses += strstr(buf, "HTTP/1.1 200") != NULL;
    close(fd);
    CHECK(responses == 2, "keep-alive still serves sequential requests");

    /* Two requests in one packet: both must be served (pipelining). */
    fd = connect_to(PORT);
    if (fd < 0) { CHECK(0, "connect for pipelining test"); return; }
    {
        char two[256];
        int responses = 0;

        snprintf(two, sizeof(two), "%s%s", one, one);
        send_all_fd(fd, two, strlen(two));

        /* Read all responses. TCP may combine multiple responses into
           one segment, so count all "HTTP/1.1 200" occurrences. */
        ssize_t n;
        while ((n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[n] = '\0';
            const char *p = buf;
            while ((p = strstr(p, "HTTP/1.1 200")) != NULL) {
                responses++;
                p += 12;
            }
        }
        close(fd);
        CHECK(responses == 2, "pipelined requests: both served");
    }
}

/* Is the server-side end of `fd` closed (EOF, or a final error response
   followed by EOF)? Drains whatever the server sent. */
static int closed_by_server(int fd)
{
    char buf[1024];
    int64_t end = now_ms() + 500;

    while (now_ms() < end) {
        struct pollfd p = { fd, POLLIN, 0 };
        ssize_t n;

        if (poll(&p, 1, 100) <= 0) continue;
        n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0) return 1;
        if (n < 0) return 1;
    }
    return 0;
}

/*
 * Slowloris. The server has 2 workers and a 2 s timeout. Two connections
 * drip one byte every 300 ms: each recv() would succeed well inside any
 * per-call timeout, so with a per-recv() limit they hold both workers for as
 * long as the attacker likes and a legitimate client is never served.
 */
static void test_slowloris_headers(pid_t pid)
{
    int slow[2];
    int legit;
    int64_t t0, deadline;
    int got = 0;
    char buf[512];
    const char *partial = "GET /q HTTP/1.1\r\nX-Slow: ";
    const char *full = "GET /q HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

    for (int i = 0; i < 2; i++) {
        slow[i] = connect_to(PORT);
        if (slow[i] >= 0) send_all_fd(slow[i], partial, strlen(partial));
    }
    usleep(200000);

    legit = connect_to(PORT);
    send_all_fd(legit, full, strlen(full));

    t0 = now_ms();
    deadline = t0 + 9000;
    while (now_ms() < deadline) {
        struct pollfd p = { legit, POLLIN, 0 };

        for (int i = 0; i < 2; i++) {
            if (slow[i] >= 0) send_all_fd(slow[i], "a", 1);
        }
        if (poll(&p, 1, 300) > 0) {
            ssize_t n = recv(legit, buf, sizeof(buf) - 1, 0);
            if (n > 0) { buf[n] = '\0'; got = strncmp(buf, "HTTP/1.1 200", 12) == 0; }
            break;
        }
    }
    CHECK(got && now_ms() - t0 < 6000,
        "legitimate client is served while others drip header bytes");

    {
        int both = 1;

        for (int i = 0; i < 2; i++) {
            if (slow[i] < 0 || !closed_by_server(slow[i])) both = 0;
        }
        CHECK(both, "header-dripping connections are closed by the server");
    }
    for (int i = 0; i < 2; i++) if (slow[i] >= 0) close(slow[i]);
    close(legit);
    CHECK(server_alive(pid), "server alive after the Slowloris attempt");
}

static void test_slow_body(void)
{
    const char *head =
        "POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n"
        "Connection: close\r\n\r\n0123456789";
    int fd = connect_to(PORT);
    int64_t t0 = now_ms();
    int closed = 0;

    if (fd < 0) { CHECK(0, "connect for slow-body test"); return; }
    send_all_fd(fd, head, strlen(head));

    while (now_ms() - t0 < 7000) {
        struct pollfd p = { fd, POLLIN, 0 };

        send_all_fd(fd, "x", 1);
        if (poll(&p, 1, 300) > 0) { closed = 1; break; }
    }
    CHECK(closed && now_ms() - t0 < 6000,
        "a body dripped below the minimum rate is cut off");
    close(fd);
}

static void test_idle_connection(void)
{
    int fd = connect_to(PORT);
    int64_t t0 = now_ms();
    char buf[64];
    int closed = 0;

    if (fd < 0) { CHECK(0, "connect for idle test"); return; }
    while (now_ms() - t0 < 6000) {
        struct pollfd p = { fd, POLLIN, 0 };

        if (poll(&p, 1, 200) > 0) {
            closed = recv(fd, buf, sizeof(buf), 0) <= 0;
            break;
        }
    }
    CHECK(closed && now_ms() - t0 < 5000,
        "a connection that sends nothing is closed after the timeout");
    close(fd);
}

static void run_server_tests(void)
{
    pid_t pid;

    signal(SIGPIPE, SIG_IGN); /* protects this test process only */

    pid = start_server();
    CHECK(pid > 0, "start server");
    if (pid <= 0) return;

    test_header_scoping();
    test_body_limits_not_applied_to_body();
    test_content_length();
    test_keepalive_and_pipelining();
    test_sigpipe(pid);
    test_slowloris_headers(pid);
    test_slow_body();
    test_idle_connection();

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

#endif /* !_WIN32 */

int main(void)
{
    test_jsonf();
#ifndef _WIN32
    run_server_tests();
#else
    printf("  (server tests are not run on Windows)\n");
#endif

    printf("\n%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}
