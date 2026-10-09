/* test/test_client.c
 *
 * Tests for the client-side HTTP functionality (mnet_call).
 *
 * Build:
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200112L
 *       -Iinclude -Isrc -pthread src/mnet.c src/mnet_app.c src/mnet_response.c
 *       src/mnet_request.c src/mnet_router.c test/test_client.c -o test_client
 */

#define _GNU_SOURCE
#include <mnet/mnet.h>
#include <mnet/mnet_app.h>

#include <assert.h>
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

#define CLIENT_TEST_PORT 18777

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
    return mnet_text("BODY-OK");
}

static void test_mnet_call_invalid_url(void)
{
    char *result;

    /* NULL URL */
    result = mnet_call(NULL);
    CHECK(result == NULL, "mnet_call(NULL) returns NULL");

    /* Empty URL */
    result = mnet_call("");
    CHECK(result == NULL, "mnet_call(\"\") returns NULL");

    /* URL with no host */
    result = mnet_call("://invalid");
    CHECK(result == NULL, "mnet_call(\"://invalid\") returns NULL");

    printf("  PASS test_mnet_call_invalid_url\n");
}

/* Drive mnet_call against a live server started in a forked child. The child
   runs the server; the parent performs the requests and reports the child. */
static int run_server_child(pid_t *child_pid)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        mnet_app_t *app = mnet_create();
        if (app == NULL) _exit(1);
        MNET_GET(app, "/ok", h_ok);
        mnet_run(app, CLIENT_TEST_PORT);
        mnet_destroy(app);
        _exit(0);
    }
    *child_pid = pid;
    /* Give the child time to bind and start serving. */
    usleep(300000);
    return 0;
}

static char g_async_body[64];
static int g_async_called = 0;

static void record_async_body(char *body)
{
    g_async_called = 1;
    if (body != NULL) {
        strncpy(g_async_body, body, sizeof(g_async_body) - 1);
        g_async_body[sizeof(g_async_body) - 1] = '\0';
    }
}

static void test_mnet_call_against_server(void)
{
    pid_t child = 0;
    if (run_server_child(&child) != 0) {
        printf("  SKIP server tests (fork failed)\n");
        return;
    }

    /* A URL with an explicit port and a path must connect to that port and
       return the response body with the HTTP headers stripped. */
    char *body = mnet_call("http://127.0.0.1:18777/ok");
    CHECK(body != NULL && strcmp(body, "BODY-OK") == 0,
        "mnet_call returns the body only (headers stripped) for a "
        "URL with port and path");
    free(body);

    /* The documented contract: 4xx/5xx responses return NULL. */
    body = mnet_call("http://127.0.0.1:18777/missing");
    CHECK(body == NULL, "mnet_call returns NULL for a 404 response");

    /* The async variant must invoke the callback with the same body. */
    g_async_called = 0;
    g_async_body[0] = '\0';
    mnet_call_async("http://127.0.0.1:18777/ok", record_async_body);
    CHECK(g_async_called == 1 && strcmp(g_async_body, "BODY-OK") == 0,
        "mnet_call_async invokes the callback with the response body");

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
}

int main(void)
{
    printf("Running mnet client tests...\n");

    test_mnet_call_invalid_url();
    test_mnet_call_against_server();

    printf("\n%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}

#else

int main(void)
{
    printf("Client tests are not run on Windows.\n");
    return 0;
}

#endif
