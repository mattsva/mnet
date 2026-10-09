/* test/test_starvation.c
 *
 * Worker-pool starvation regression test.
 *
 * The server is threaded with a fixed worker pool (4 by default) and
 * blocking I/O. A worker holds its connection for the whole keep-alive
 * idle wait, so N idle keep-alive connections (N = worker count) occupy
 * every worker and a new client is served only once one of them times
 * out. This test opens exactly that many idle keep-alive connections and
 * requires a new client to still be answered promptly.
 *
 * Build (POSIX):
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200112L
 *       -Iinclude -Isrc -pthread src/mnet.c src/mnet_app.c src/mnet_response.c
 *       src/mnet_request.c src/mnet_router.c test/test_starvation.c -o test_starvation
 *
 * The socket parts are POSIX-only; on Windows this compiles to a no-op.
 */
#define _GNU_SOURCE
#include <mnet/mnet.h>
#include <mnet/mnet_app.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32

#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define STARVATION_PORT 18778

/* A new client must be served well inside the keep-alive idle timeout. */
#define MAX_NEW_CLIENT_WAIT_S 10

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

MNET_HANDLER(h_ok)
{
    (void)req;
    return mnet_text("OK");
}

static double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static int connect_to(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    struct timeval tv;

    if (fd < 0) return -1;

    tv.tv_sec = 60;
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

/* Read until the full response promised by Content-Length has arrived. */
static int read_reply(int fd, char *out, size_t out_size)
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
                content_length = cl != NULL ? atol(cl + 15) : 0;
                if (content_length >= 0) {
                    size_t header_bytes = (size_t)(hdr_end - out) + 4;
                    if (used >= header_bytes + (size_t)content_length) break;
                }
            }
        } else if (content_length >= 0) {
            size_t header_bytes = 0;
            char *hdr_end = strstr(out, "\r\n\r\n");
            if (hdr_end != NULL) {
                header_bytes = (size_t)(hdr_end - out) + 4;
                if (used >= header_bytes + (size_t)content_length) break;
            }
        }
        if (used >= out_size - 1) break;
    }
    return header_done;
}

int main(void)
{
    printf("Running worker-pool starvation tests...\n");

    pid_t pid = fork();
    if (pid < 0) {
        printf("  SKIP (fork failed)\n");
        return 0;
    }
    if (pid == 0) {
        mnet_app_t *app = mnet_create();
        if (app == NULL) _exit(1);
        MNET_GET(app, "/ok", h_ok);
        mnet_run(app, STARVATION_PORT);
        mnet_destroy(app);
        _exit(0);
    }
    usleep(300000);

    /* Occupy every worker: one idle keep-alive connection each. The
       default pool has 4 workers, so open 4 connections. */
    int idle[4];
    int opened = 0;
    for (int i = 0; i < 4; i++) {
        idle[i] = connect_to(STARVATION_PORT);
        if (idle[i] < 0) break;
        const char *req =
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        if (send(idle[i], req, strlen(req), 0) < 0) {
            close(idle[i]);
            break;
        }
        char reply[512];
        if (!read_reply(idle[i], reply, sizeof(reply))) {
            close(idle[i]);
            break;
        }
        opened++;
    }

    CHECK(opened == 4,
        "4 idle keep-alive connections established (one per worker)");

    if (opened == 4) {
        /* A brand-new client while every worker is parked on an idle
           keep-alive connection. */
        int fresh = connect_to(STARVATION_PORT);
        CHECK(fresh >= 0, "new client can still connect");

        if (fresh >= 0) {
            const char *req =
                "GET /ok HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "\r\n";
            double t0 = now_seconds();
            send(fresh, req, strlen(req), 0);

            char reply[512];
            int ok = read_reply(fresh, reply, sizeof(reply));
            double elapsed = now_seconds() - t0;

            CHECK(ok && strstr(reply, "200") != NULL,
                "new client receives a response");
            CHECK(elapsed < (double)MAX_NEW_CLIENT_WAIT_S,
                "new client is served promptly despite idle keep-alive "
                "connections");
            printf("  (new client waited %.1f s)\n", elapsed);
            close(fresh);
        }
    }

    for (int i = 0; i < opened; i++) close(idle[i]);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    printf("\n%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}

#else

int main(void)
{
    printf("Starvation tests are not run on Windows.\n");
    return 0;
}

#endif
