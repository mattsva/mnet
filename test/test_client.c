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
#include <mnet.h>
#include <mnet_app.h>

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

static void test_mnet_call_invalid_url(void)
{
    char *result;

    /* NULL URL */
    result = mnet_call(NULL);
    assert(result == NULL);

    /* Empty URL */
    result = mnet_call("");
    assert(result == NULL);

    /* URL with no host */
    result = mnet_call("://invalid");
    assert(result == NULL);

    printf("  PASS test_mnet_call_invalid_url\n");
}

static void test_async_callback(char *body)
{
    /* Callback executed; body is freed automatically */
    (void)body;
}

static void test_mnet_call_sync_async(void)
{
    /* Verify that mnet_call and mnet_call_async are declared and callable */
    char *body1 = mnet_call("https://example.com");
    if (body1 != NULL) {
        free(body1);
    }

    /* Async callback */
    mnet_call_async("https://example.com", test_async_callback);

    printf("  PASS test_mnet_call_sync_async\n");
}

int main(void)
{
    printf("Running mnet client tests...\n");

    test_mnet_call_invalid_url();
    test_mnet_call_sync_async();

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
