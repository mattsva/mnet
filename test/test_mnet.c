/*
 * mnet tests -- build with:
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200112L \
 *       -Iinclude -Isrc -Itest \
 *       src/mnet_response.c src/mnet_request.c src/mnet_router.c \
 *       src/mnet_app.c src/mnet.c \
 *       test/test_mnet.c -o test_mnet
 */

#include <mnet.h>
#include <mnet_response.h>
#include <mnet_request.h>
#include <mnet_router.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_mnet_text(void)
{
    mnet_response_t r = mnet_text("hello");
    assert(r.status == 200);
    assert(strcmp(r.content_type, "text/plain; charset=utf-8") == 0);
    assert(strcmp((const char *)r.body, "hello") == 0);
    assert(r.body_length == 5);
    printf("  PASS test_mnet_text\n");
}

static void test_mnet_html(void)
{
    mnet_response_t r = mnet_html("<h1>hi</h1>");
    assert(r.status == 200);
    assert(strcmp(r.content_type, "text/html; charset=utf-8") == 0);
    assert(strcmp((const char *)r.body, "<h1>hi</h1>") == 0);
    printf("  PASS test_mnet_html\n");
}

static void test_mnet_json(void)
{
    mnet_response_t r = mnet_json("{\"ok\":true}");
    assert(r.status == 200);
    assert(strcmp(r.content_type, "application/json") == 0);
    assert(strcmp((const char *)r.body, "{\"ok\":true}") == 0);
    printf("  PASS test_mnet_json\n");
}

static void test_mnet_jsonf(void)
{
    mnet_response_t r = mnet_jsonf("{\"id\":\"%s\"}", "42");
    assert(r.status == 200);
    assert(strcmp(r.content_type, "application/json") == 0);
    assert(strcmp((const char *)r.body, "{\"id\":\"42\"}") == 0);
    printf("  PASS test_mnet_jsonf\n");
}

static void test_mnet_jsonf_escape(void)
{
    mnet_response_t r = mnet_jsonf("{\"name\":\"%s\"}", "he\"llo");
    assert(r.status == 200);
    assert(strstr((const char *)r.body, "\\\"") != NULL);
    printf("  PASS test_mnet_jsonf_escape\n");
}

static void test_mnet_error(void)
{
    mnet_response_t r = mnet_error(400, "bad request");
    assert(r.status == 400);
    assert(strcmp(r.content_type, "text/plain; charset=utf-8") == 0);
    assert(strcmp((const char *)r.body, "bad request") == 0);
    printf("  PASS test_mnet_error\n");
}

static void test_mnet_status(void)
{
    mnet_response_t r = mnet_status(201, "created");
    assert(r.status == 201);
    assert(strcmp(r.content_type, "text/plain; charset=utf-8") == 0);
    assert(strcmp((const char *)r.body, "created") == 0);
    printf("  PASS test_mnet_status\n");
}

static void test_mnet_json_escape(void)
{
    char out[256];

    size_t n = mnet_json_escape(out, sizeof(out), "hello");
    assert(strcmp(out, "hello") == 0);

    n = mnet_json_escape(out, sizeof(out), "he\"llo");
    assert(strcmp(out, "he\\\"llo") == 0);

    n = mnet_json_escape(out, sizeof(out), "a\\b");
    assert(strcmp(out, "a\\\\b") == 0);

    n = mnet_json_escape(out, sizeof(out), "a\nb");
    assert(strcmp(out, "a\\nb") == 0);

    n = mnet_json_escape(out, sizeof(out), "a\rb");
    assert(strcmp(out, "a\\rb") == 0);

    n = mnet_json_escape(out, sizeof(out), "a\tb");
    assert(strcmp(out, "a\\tb") == 0);

    n = mnet_json_escape(out, sizeof(out), "");
    assert(strcmp(out, "") == 0);
    assert(n == 0);

    printf("  PASS test_mnet_json_escape\n");
}

static void test_mnet_request_param(void)
{
    mnet_request_t req = {0};
    req.path_param_names = (const char *[]){"id", "name", NULL};
    req.path_param_values = (const char *[]){"42", "Alice", NULL};
    req.path_param_count = 2;

    assert(strcmp(MNET_PARAM(&req, "id"), "42") == 0);
    assert(strcmp(MNET_PARAM(&req, "name"), "Alice") == 0);
    assert(MNET_PARAM(&req, "missing") == NULL);
    printf("  PASS test_mnet_request_param\n");
}

static void test_mnet_request_query(void)
{
    mnet_request_t req = {0};
    req.query_names = (const char *[]){"q", "page", NULL};
    req.query_values = (const char *[]){"hello", "1", NULL};
    req.query_count = 2;

    assert(strcmp(MNET_QUERY(&req, "q"), "hello") == 0);
    assert(strcmp(MNET_QUERY(&req, "page"), "1") == 0);
    assert(MNET_QUERY(&req, "missing") == NULL);
    printf("  PASS test_mnet_request_query\n");
}

static void test_mnet_request_header(void)
{
    mnet_request_t req = {0};
    req.header_names = (const char *[]){"Authorization", "Content-Type", NULL};
    req.header_values = (const char *[]){"Bearer xyz", "application/json", NULL};
    req.header_count = 2;

    assert(strcmp(MNET_HEADER(&req, "Authorization"), "Bearer xyz") == 0);
    assert(strcmp(MNET_HEADER(&req, "authorization"), "Bearer xyz") == 0);
    assert(strcmp(MNET_HEADER(&req, "CONTENT-TYPE"), "application/json") == 0);
    assert(MNET_HEADER(&req, "X-Missing") == NULL);
    printf("  PASS test_mnet_request_header\n");
}

static void test_mnet_request_body(void)
{
    mnet_request_t req = {0};
    const char *body = "hello body";
    req.body = body;
    req.body_length = strlen(body);

    assert(strcmp(MNET_BODY(&req), body) == 0);
    assert(MNET_BODY_LEN(&req) == strlen(body));
    printf("  PASS test_mnet_request_body\n");
}


static void test_mnet_route_match_simple(void)
{
    mnet_route_t route = {
        .method = MNET_HTTP_GET,
        .path = "/api/users/:id",
        .param_names = (const char *[]){"id", NULL},
    };

    const char *values[4] = {0};
    int n = mnet_route_match(&route, "/api/users/42", values, 4);
    assert(n == 1);
    assert(strcmp(values[0], "42") == 0);
    mnet_match_params_free(values, n);
    printf("  PASS test_mnet_route_match_simple\n");
}

static void test_mnet_route_match_multi(void)
{
    mnet_route_t route = {
        .method = MNET_HTTP_GET,
        .path = "/api/posts/:post_id/comments/:comment_id",
        .param_names = (const char *[]){"post_id", "comment_id", NULL},
    };

    const char *values[4] = {0};
    int n = mnet_route_match(&route, "/api/posts/5/comments/99", values, 4);
    assert(n == 2);
    assert(strcmp(values[0], "5") == 0);
    assert(strcmp(values[1], "99") == 0);
    mnet_match_params_free(values, n);
    printf("  PASS test_mnet_route_match_multi\n");
}

static void test_mnet_route_match_no_match(void)
{
    mnet_route_t route = {
        .method = MNET_HTTP_GET,
        .path = "/api/users/:id",
        .param_names = (const char *[]){"id", NULL},
    };

    const char *values[4] = {0};
    int n = mnet_route_match(&route, "/api/users", values, 4);
    assert(n == 0);
    printf("  PASS test_mnet_route_match_no_match\n");
}

static void test_mnet_route_match_static(void)
{
    mnet_route_t route = {
        .method = MNET_HTTP_GET,
        .path = "/",
        .param_names = NULL,
    };

    const char *values[4] = {0};
    int n = mnet_route_match(&route, "/", values, 4);
    assert(n == 0); /* no params extracted */
    printf("  PASS test_mnet_route_match_static\n");
}

MNET_HANDLER(test_handler)
{
    (void)req;
    return mnet_text("ok");
}

static void test_handler_macro(void)
{
    mnet_request_t req = {0};
    mnet_response_t r = test_handler(&req);
    assert(r.status == 200);
    assert(strcmp((const char *)r.body, "ok") == 0);
    printf("  PASS test_handler_macro\n");
}

static void test_route_macros_compile(void)
{
    /* This function just verifies the macros compile.
       We create a fake app pointer and call the macros.
       The macros call mnet_route() which we can't easily test
       without a real app, but compilation is the test. */
    mnet_app_t *app = NULL; /* NULL is fine for compile test,
                               * mnet_route will just return -1 */
    MNET_GET(app, "/", test_handler);
    MNET_POST(app, "/", test_handler);
    MNET_PUT(app, "/", test_handler);
    MNET_PATCH(app, "/", test_handler);
    MNET_DELETE(app, "/", test_handler);
    printf("  PASS test_route_macros_compile\n");
}

static void test_http_method_enum(void)
{
    assert(MNET_HTTP_GET == 0);
    assert(MNET_HTTP_POST == 1);
    assert(MNET_HTTP_PUT == 2);
    assert(MNET_HTTP_PATCH == 3);
    assert(MNET_HTTP_DELETE == 4);
    printf("  PASS test_http_method_enum\n");
}

int main(void)
{
    printf("Running mnet tests...\n");

    test_mnet_text();
    test_mnet_html();
    test_mnet_json();
    test_mnet_jsonf();
    test_mnet_jsonf_escape();
    test_mnet_error();
    test_mnet_status();
    test_mnet_json_escape();

    test_mnet_request_param();
    test_mnet_request_query();
    test_mnet_request_header();
    test_mnet_request_body();

    test_mnet_route_match_simple();
    test_mnet_route_match_multi();
    test_mnet_route_match_no_match();
    test_mnet_route_match_static();

    test_handler_macro();
    test_route_macros_compile();

    test_http_method_enum();

    printf("\nAll tests passed!\n");
    return 0;
}
