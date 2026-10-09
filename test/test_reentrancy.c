/* test/test_reentrancy.c
 *
 * Concurrency test for the dev-mode welcome page.
 *
 * The welcome-page handler formats the current time. It runs on worker
 * threads, so the time conversion must be reentrant: localtime() returns
 * a pointer to a process-wide static struct tm and corrupts under
 * concurrent use. This test drives the welcome page from many clients at
 * once and requires every response to carry a well-formed timestamp.
 *
 * Build (POSIX):
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200112L
 *       -Iinclude -Isrc -pthread src/mnet.c src/mnet_app.c src/mnet_response.c
 *       src/mnet_request.c src/mnet_router.c test/test_reentrancy.c -o test_reentrancy
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
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define REENTRANCY_PORT 18779
#define REENTRANCY_CLIENTS 32

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

typedef struct {
    int port;
    int bad_responses;
} client_arg_t;

static int connect_to(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    struct timeval tv;

    if (fd < 0) return -1;

    tv.tv_sec = 30;
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

/* True when s matches "YYYY-MM-DD HH:MM:SS" exactly. */
static int timestamp_well_formed(const char *s)
{
    static const char *pattern = "dddd-dd-dd dd:dd:dd";
    for (const char *p = pattern; *p; p++, s++) {
        if (*p == 'd') {
            if (*s < '0' || *s > '9') return 0;
        } else if (*s != *p) {
            return 0;
        }
    }
    return 1;
}

static void *client_thread(void *arg)
{
    client_arg_t *ca = (client_arg_t *)arg;
    int fd = connect_to(ca->port);

    if (fd < 0) {
        ca->bad_responses++;
        return NULL;
    }

    const char *req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";
    if (send(fd, req, strlen(req), 0) < 0) {
        ca->bad_responses++;
        close(fd);
        return NULL;
    }

    char reply[8192];
    size_t used = 0;
    for (;;) {
        ssize_t n = recv(fd, reply + used, sizeof(reply) - 1 - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        reply[used] = '\0';
        if (strstr(reply, "</html>") != NULL) break;
        if (used >= sizeof(reply) - 1) break;
    }
    close(fd);

    const char *marker = "Build time: ";
    const char *ts = strstr(reply, marker);
    if (ts == NULL) {
        ca->bad_responses++;
        return NULL;
    }
    ts += strlen(marker);
    if (!timestamp_well_formed(ts)) {
        ca->bad_responses++;
    }
    return NULL;
}

int main(void)
{
    printf("Running welcome-page reentrancy tests...\n");

    pid_t pid = fork();
    if (pid < 0) {
        printf("  SKIP (fork failed)\n");
        return 0;
    }
    if (pid == 0) {
        mnet_app_t *app = mnet_create();
        if (app == NULL) _exit(1);
        mnet_set_dev_mode(app, 1);
        mnet_run(app, REENTRANCY_PORT);
        mnet_destroy(app);
        _exit(0);
    }
    usleep(300000);

    pthread_t threads[REENTRANCY_CLIENTS];
    client_arg_t args[REENTRANCY_CLIENTS];
    int created[REENTRANCY_CLIENTS];
    int started = 0;

    for (int i = 0; i < REENTRANCY_CLIENTS; i++) {
        args[i].port = REENTRANCY_PORT;
        args[i].bad_responses = 0;
        created[i] = 0;
        if (pthread_create(&threads[i], NULL, client_thread, &args[i]) == 0) {
            created[i] = 1;
            started++;
        }
    }

    int bad = 0;
    for (int i = 0; i < REENTRANCY_CLIENTS; i++) {
        if (created[i]) pthread_join(threads[i], NULL);
    }
    for (int i = 0; i < started; i++) bad += args[i].bad_responses;

    CHECK(started == REENTRANCY_CLIENTS,
        "all concurrent clients ran");
    CHECK(bad == 0,
        "every concurrent welcome-page response has a well-formed "
        "timestamp (localtime is reentrant)");

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    printf("\n%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}

#else

int main(void)
{
    printf("Reentrancy tests are not run on Windows.\n");
    return 0;
}

#endif
