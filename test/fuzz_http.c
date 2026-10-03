/*
 * libFuzzer harness for the mnet HTTP request parser.
 *
 * The parser entry point (parse_request) is static, so the harness drives the
 * whole request path through the public API instead: it starts a server in a
 * child process and feeds the fuzzer's bytes to it over a loopback socket. That
 * covers the request line, headers, query string, body handling and static-file
 * resolution exactly as a real client would reach them.
 *
 * Building
 * --------
 * clang:
 *   clang -std=c17 -g -O1 -fsanitize=fuzzer,address,undefined \
 *       -D_POSIX_C_SOURCE=200112L -Iinclude -Isrc -pthread \
 *       src/mnet.c src/mnet_app.c src/mnet_response.c src/mnet_request.c \
 *       src/mnet_router.c test/fuzz_http.c -o fuzz_http
 *   ./fuzz_http -max_total_time=60 corpus/
 *
 * AFL++ (as a persistent-mode standalone binary):
 *   afl-clang-fast -std=c17 -g -O1 -D_POSIX_C_SOURCE=200112L
 *       -Iinclude -Isrc -pthread src/mnet.c src/mnet_app.c src/mnet_response.c
 *       src/mnet_request.c src/mnet_router.c test/fuzz_http.c -o fuzz_http_afl
 *
 * The target never blocks: the client socket carries a receive timeout and the
 * connection is closed once the reply arrives or the timeout fires.
 *
 * Coverage caveat
 * ---------------
 * The server runs in a forked child, so the fuzzer's coverage instrumentation
 * only observes this client-side harness, not the parser inside the child
 * (libFuzzer reports a low "cov" as a result). The harness is therefore a
 * crash and sanitizer oracle over the real request path - every input does
 * reach the actual parser - but it is not coverage-guided through the server
 * code. To fuzz the parser with real coverage feedback, build the parser into
 * a single-process target that calls parse_request() directly.
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* One server per fuzz run: forking per input would be far too slow, so the
   child is started once and fed many inputs. */
static pid_t g_server_pid = -1;
static int g_port = 0;

MNET_HANDLER(fuzz_home)
{
    (void)req;
    return mnet_text("ok");
}

MNET_HANDLER(fuzz_echo)
{
    const char *b = MNET_BODY(req);
    return mnet_text(b ? b : "");
}

MNET_HANDLER(fuzz_param)
{
    const char *v = MNET_PARAM(req, "id");
    const char *q = MNET_QUERY(req, "q");
    return mnet_text(v ? v : (q ? q : "none"));
}

static void make_fixtures(void)
{
    mkdir("/tmp/mnet-fuzz-root", 0755);
    FILE *f = fopen("/tmp/mnet-fuzz-root/ok.txt", "wb");
    if (f != NULL) { fputs("legit", f); fclose(f); }
}

static int connect_to(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    struct timeval tv;

    if (fd < 0) return -1;

    tv.tv_sec = 1;
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

static void start_server(int port)
{
    pid_t pid = fork();

    if (pid == 0) {
        /* Detach the child's stdio: the fuzz harness's output is piped, and a
           forked server holding that pipe open would stop the reader from ever
           seeing EOF. */
        if (freopen("/dev/null", "r", stdin) == NULL ||
            freopen("/dev/null", "w", stdout) == NULL ||
            freopen("/dev/null", "w", stderr) == NULL) {
            _exit(1);
        }

        mnet_app_t *app = mnet_create();
        if (app == NULL) _exit(1);
        mnet_set_workers(app, 2);
        MNET_GET(app, "/", fuzz_home);
        MNET_POST(app, "/echo", fuzz_echo);
        MNET_GET(app, "/items/:id", fuzz_param);
        mnet_static(app, "/static", "/tmp/mnet-fuzz-root");
        mnet_run(app, (uint16_t)port);
        mnet_destroy(app);
        _exit(0);
    }

    for (int i = 0; i < 200; i++) {
        int fd = connect_to(port);
        if (fd >= 0) { close(fd); break; }
        usleep(20000);
    }
    g_server_pid = pid;
}

static void stop_server(void)
{
    if (g_server_pid > 0) {
        kill(g_server_pid, SIGTERM);
        waitpid(g_server_pid, NULL, 0);
        g_server_pid = -1;
    }
}

/*
 * The fuzz target: feed the bytes to the server and read back whatever comes.
 * Returns 0 always; the value only matters to libFuzzer as "did not crash".
 */
static int run_one(const uint8_t *data, size_t size)
{
    char buf[4096];
    int fd = connect_to(g_port);

    if (fd < 0) return 0;

    /* Cap what we send so a huge input does not stall the target. */
    size_t to_send = size > 8192 ? 8192 : size;
    size_t sent = 0;

    while (sent < to_send) {
        ssize_t w = send(fd, data + sent, to_send - sent, 0);
        if (w <= 0) break;
        sent += (size_t)w;
    }

    /* Drain the reply so the server side completes and cleans up. */
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (n < (ssize_t)sizeof(buf)) break;
    }

    close(fd);
    return 0;
}

/*
 * Entry point used by libFuzzer. When building with -fsanitize=fuzzer the
 * fuzzer supplies main(), so this file's main() is compiled out by defining
 * MNET_FUZZ_LIBFUZZER. Without it the binary is standalone and reads input
 * from stdin (which is also the mode AFL++ uses).
 */
static void fuzz_ensure_server(void)
{
    if (g_server_pid < 0) {
        make_fixtures();
        start_server(18889);
        g_port = 18889;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_ensure_server();
    return run_one(data, size);
}

#ifndef MNET_FUZZ_LIBFUZZER

int main(void)
{
    static uint8_t buf[1 << 16];

    fuzz_ensure_server();

    /* Standalone mode: each stdin read is one input, so AFL++ and a manual
       smoke test can both drive it. */
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), stdin);
        if (n == 0) break;
        run_one(buf, n);
        if (n < sizeof(buf)) break;
    }

    stop_server();
    return 0;
}

#endif
#else /* _WIN32 */

int main(void)
{
    printf("The fuzz harness is POSIX-only.\n");
    return 0;
}

#endif
