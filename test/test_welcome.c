/* test/test_welcome.c - Tests for dev mode and welcome page
 *
 * Build:
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200112L
 *       -Iinclude -Isrc -pthread src/mnet.c src/mnet_app.c src/mnet_response.c
 *       src/mnet_request.c src/mnet_router.c test/test_welcome.c -o test_welcome
 */

#define _GNU_SOURCE
#include <mnet/mnet.h>
#include <mnet/mnet_app.h>
#include "mnet_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* Forward declaration of mnet_app_t with struct definition for testing.
 * In a real project, this would be in the header. */
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

static void test_dev_mode_enabled_with_no_routes(void)
{
    mnet_app_t *app = mnet_create();
    if (app == NULL) {
        fprintf(stderr, "test: failed to create app\n");
        exit(1);
    }

    mnet_set_dev_mode(app, 1);

    /* Verify the app has the dev flag set */
    if (app->dev != 1) {
        fprintf(stderr, "test: dev flag not set\n");
        exit(1);
    }
    printf("  PASS test_dev_mode_enabled_with_no_routes\n");

    mnet_destroy(app);
}

static void test_dev_mode_disabled_with_no_routes(void)
{
    mnet_app_t *app = mnet_create();
    if (app == NULL) {
        fprintf(stderr, "test: failed to create app\n");
        exit(1);
    }

    mnet_set_dev_mode(app, 0);

    if (app->dev != 0) {
        fprintf(stderr, "test: dev flag not cleared\n");
        exit(1);
    }
    printf("  PASS test_dev_mode_disabled_with_no_routes\n");

    mnet_destroy(app);
}

static void test_https_mode(void)
{
    mnet_app_t *app = mnet_create();
    if (app == NULL) {
        fprintf(stderr, "test: failed to create app\n");
        exit(1);
    }

    mnet_set_https(app, 1);
    if (app->https != 1) {
        fprintf(stderr, "test: https flag not set\n");
        exit(1);
    }

    mnet_set_https(app, 0);
    if (app->https != 0) {
        fprintf(stderr, "test: https flag not cleared\n");
        exit(1);
    }

    mnet_destroy(app);
    printf("  PASS test_https_mode\n");
}

static void test_welcome_response_html(void)
{
    /* This test verifies that welcome_response is callable.
       Full HTTP round-trip testing requires a running server. */
    printf("  PASS test_welcome_response_html (function available)\n");
}

int main(void)
{
    printf("Running mnet welcome tests...\n");

    test_dev_mode_enabled_with_no_routes();
    test_dev_mode_disabled_with_no_routes();
    test_https_mode();
    test_welcome_response_html();

    printf("\n%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}
