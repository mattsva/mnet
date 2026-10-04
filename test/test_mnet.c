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
#include <mnet_app.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_mnet_text(void)
{
    mnet_response_t r = mnet_text("hello");
    assert(r.status == 200);
    assert(strcmp(r.content_type, "text/plain; charset=utf-8") == 0);
    assert(strcmp((const char *)r.body, "hello") == 0);
    assert(r.body_length == 5);
    mnet_response_free(&r);
    printf("  PASS test_mnet_text\n");
}

static void test_mnet_html(void)
{
    mnet_response_t r = mnet_html("<h1>hi</h1>");
    assert(r.status == 200);
    assert(strcmp(r.content_type, "text/html; charset=utf-8") == 0);
    assert(strcmp((const char *)r.body, "<h1>hi</h1>") == 0);
    mnet_response_free(&r);
    printf("  PASS test_mnet_html\n");
}

static void test_mnet_json(void)
{
    mnet_response_t r = mnet_json("{\"ok\":true}");
    assert(r.status == 200);
    assert(strcmp(r.content_type, "application/json") == 0);
    assert(strcmp((const char *)r.body, "{\"ok\":true}") == 0);
    mnet_response_free(&r);
    printf("  PASS test_mnet_json\n");
}

static void test_mnet_jsonf(void)
{
    mnet_response_t r = mnet_jsonf("{\"id\":\"%s\"}", "42");
    assert(r.status == 200);
    assert(strcmp(r.content_type, "application/json") == 0);
    assert(strcmp((const char *)r.body, "{\"id\":\"42\"}") == 0);
    mnet_response_free(&r);
    printf("  PASS test_mnet_jsonf\n");
}

static void test_mnet_jsonf_escape(void)
{
    mnet_response_t r = mnet_jsonf("{\"name\":\"%s\"}", "he\"llo");
    assert(r.status == 200);
    assert(strstr((const char *)r.body, "\\\"") != NULL);
    mnet_response_free(&r);
    printf("  PASS test_mnet_jsonf_escape\n");
}

static void test_mnet_jsonf_long_string(void)
{
    char long_str[5000];
    memset(long_str, 'a', sizeof(long_str) - 1);
    long_str[sizeof(long_str) - 1] = '\0';

    mnet_response_t r = mnet_jsonf("{\"data\":\"%s\"}", long_str);
    assert(r.status == 200);
    assert(r.body_length > 5000);
    mnet_response_free(&r);
    printf("  PASS test_mnet_jsonf_long_string\n");
}

static void test_mnet_error(void)
{
    mnet_response_t r = mnet_error(400, "bad request");
    assert(r.status == 400);
    assert(strcmp(r.content_type, "text/plain; charset=utf-8") == 0);
    assert(strcmp((const char *)r.body, "bad request") == 0);
    mnet_response_free(&r);
    printf("  PASS test_mnet_error\n");
}

static void test_mnet_status(void)
{
    mnet_response_t r = mnet_status(201, "created");
    assert(r.status == 201);
    assert(strcmp(r.content_type, "text/plain; charset=utf-8") == 0);
    assert(strcmp((const char *)r.body, "created") == 0);
    mnet_response_free(&r);
    printf("  PASS test_mnet_status\n");
}

static void test_mnet_url_decode(void)
{
    char out[256];

    mnet_url_decode(out, sizeof(out), "hello");
    assert(strcmp(out, "hello") == 0);

    mnet_url_decode(out, sizeof(out), "hello%20world");
    assert(strcmp(out, "hello world") == 0);

    mnet_url_decode(out, sizeof(out), "a%2Fb");
    assert(strcmp(out, "a/b") == 0);

    mnet_url_decode(out, sizeof(out), "a+b");
    assert(strcmp(out, "a b") == 0);

    mnet_url_decode(out, sizeof(out), "%C3%A9");
    assert(strcmp(out, "\xC3\xA9") == 0);

    mnet_url_decode(out, sizeof(out), "");
    assert(strcmp(out, "") == 0);

    printf("  PASS test_mnet_url_decode\n");
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
    req.extras = NULL;

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

static void test_mnet_request_cookie(void)
{
    mnet_request_t req = {0};
    req.cookie_names = (const char *[]){"session", "theme", NULL};
    req.cookie_values = (const char *[]){"abc123", "dark", NULL};
    req.cookie_count = 2;

    assert(strcmp(MNET_COOKIE(&req, "session"), "abc123") == 0);
    assert(strcmp(MNET_COOKIE(&req, "theme"), "dark") == 0);
    assert(MNET_COOKIE(&req, "missing") == NULL);
    printf("  PASS test_mnet_request_cookie\n");
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
    assert(n == -1); /* no match returns -1, not 0 */
    (void)n;
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
    assert(n == 0); /* matched, no params extracted — returns 0 */
    (void)n;
    printf("  PASS test_mnet_route_match_static\n");
}

static void test_mnet_route_match_partial_free(void)
{
    mnet_route_t route = {
        .method = MNET_HTTP_GET,
        .path = "/api/users/:id/posts/:post_id",
        .param_names = (const char *[]){"id", "post_id", NULL},
    };

    const char *values[4] = {0};
    int n = mnet_route_match(&route, "/api/users/42", values, 4);
    assert(n == -1); /* no match returns -1 */
    (void)n;
    printf("  PASS test_mnet_route_match_partial_free\n");
}

static void test_mnet_route_match_wildcard(void)
{
    mnet_route_t route = {
        .method = MNET_HTTP_GET,
        .path = "/static/*",
        .param_names = NULL,
    };

    const char *values[4] = {0};
    int n = mnet_route_match(&route, "/static/css/style.css", values, 4);
    assert(n == 1);
    assert(strcmp(values[0], "css/style.css") == 0);
    mnet_match_params_free(values, n);
    printf("  PASS test_mnet_route_match_wildcard\n");
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
    mnet_response_free(&r);
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
    assert(MNET_HTTP_HEAD == 5);
    assert(MNET_HTTP_OPTIONS == 6);
    printf("  PASS test_http_method_enum\n");
}

static int middleware_called = 0;

static mnet_response_t sample_middleware(mnet_request_t *req,
    mnet_response_t (*next)(mnet_request_t *))
{
    middleware_called++;
    return next(req);
}

static void test_middleware(void)
{
    mnet_app_t *app = mnet_create();
    assert(app != NULL);

    mnet_use(app, sample_middleware);
    mnet_destroy(app);
    printf("  PASS test_middleware\n");
}

static void test_timeout(void)
{
    mnet_app_t *app = mnet_create();
    assert(app != NULL);

    mnet_set_timeout(app, 30);
    mnet_destroy(app);
    printf("  PASS test_timeout\n");
}

static void test_debug_per_app(void)
{
    mnet_app_t *app = mnet_create();
    assert(app != NULL);

    mnet_set_debug(app, 1);
    mnet_set_debug(app, 0);

    mnet_destroy(app);
    printf("  PASS test_debug_per_app\n");
}

static void test_static_route_cleanup(void)
{
    mnet_app_t *app = mnet_create();
    assert(app != NULL);

    mnet_static(app, "/static", "/tmp");

    mnet_destroy(app);
    printf("  PASS test_static_route_cleanup\n");
}

static void test_mnet_chunked(void)
{
    const char *body = "hello chunked world";
    mnet_response_t r = mnet_chunked(200, "text/plain", body, strlen(body));
    assert(r.status == 200);
    assert(r.chunked == 1);
    assert(r.body_length == strlen(body));
    /* chunked responses don't own the body, so free is a no-op */
    mnet_response_free(&r);
    printf("  PASS test_mnet_chunked\n");
}

static void test_max_connections(void)
{
    mnet_app_t *app = mnet_create();
    assert(app != NULL);

    mnet_set_max_connections(app, 10);
    mnet_set_max_connections(app, 0);

    mnet_destroy(app);
    printf("  PASS test_max_connections\n");
}

static void test_keep_alive_timeout(void)
{
    mnet_app_t *app = mnet_create();
    assert(app != NULL);

    mnet_set_keep_alive_timeout(app, 30);
    mnet_set_keep_alive_timeout(app, 0);

    mnet_destroy(app);
    printf("  PASS test_keep_alive_timeout\n");
}

static void test_max_body_size(void)
{
    mnet_app_t *app = mnet_create();
    assert(app != NULL);

    mnet_set_max_body_size(app, 1024 * 1024);
    mnet_set_max_body_size(app, 0);

    mnet_destroy(app);
    printf("  PASS test_max_body_size\n");
}

static void test_mnet_response_free_null(void)
{
    mnet_response_free(NULL);
    printf("  PASS test_mnet_response_free_null\n");
}

static void test_mnet_response_free_zeroed(void)
{
    mnet_response_t r = {0};
    mnet_response_free(&r);
    assert(r.body == NULL);
    assert(r.body_length == 0);
    printf("  PASS test_mnet_response_free_zeroed\n");
}

static void test_mnet_response_free_owned(void)
{
    mnet_response_t r = mnet_text("free me");
    assert(r.body != NULL);
    mnet_response_free(&r);
    assert(r.body == NULL);
    assert(r.body_length == 0);
    printf("  PASS test_mnet_response_free_owned\n");
}

static void test_mnet_response_free_chunked(void)
{
    const char *body = "no free";
    mnet_response_t r = mnet_chunked(200, "text/plain", body, strlen(body));
    mnet_response_free(&r);
    /* chunked responses don't own the body, so it's not freed or nulled */
    assert(r.body_length == 0);
    printf("  PASS test_mnet_response_free_chunked\n");
}

static void test_mnet_jsonf_very_long_string(void)
{
    size_t len = 10000;
    char *long_str = malloc(len + 1);
    assert(long_str != NULL);
    memset(long_str, 'x', len);
    long_str[len] = '\0';

    mnet_response_t r = mnet_jsonf("{\"data\":\"%s\"}", long_str);
    assert(r.status == 200);
    assert(r.body_length > len);
    mnet_response_free(&r);
    free(long_str);
    printf("  PASS test_mnet_jsonf_very_long_string\n");
}

static void test_mnet_jsonf_unicode_escape(void)
{
    mnet_response_t r = mnet_jsonf("{\"name\":\"%s\"}", "caf\xC3\xA9");
    assert(r.status == 200);
    assert(r.body_length > 0);
    mnet_response_free(&r);
    printf("  PASS test_mnet_jsonf_unicode_escape\n");
}

static void test_mnet_url_decode_ex(void)
{
    char out[64];
    (void)out;

    /* normal */
    assert(mnet_url_decode_ex("hello", 5, out, sizeof(out)) == 5);
    assert(strcmp(out, "hello") == 0);

    /* %20 and + */
    assert(mnet_url_decode_ex("a%20b", 5, out, sizeof(out)) == 3);
    assert(strcmp(out, "a b") == 0);
    assert(mnet_url_decode_ex("a+b", 3, out, sizeof(out)) == 3);
    assert(strcmp(out, "a b") == 0);

    /* %2F decodes to a literal slash */
    assert(mnet_url_decode_ex("a%2Fb", 5, out, sizeof(out)) == 3);
    assert(strcmp(out, "a/b") == 0);

    /* malformed percent escapes are rejected, not passed through */
    assert(mnet_url_decode_ex("a%zzb", 5, out, sizeof(out)) == -1);
    assert(mnet_url_decode_ex("a%2", 3, out, sizeof(out)) == -1);   /* truncated */
    assert(mnet_url_decode_ex("a%", 2, out, sizeof(out)) == -1);

    /* %00 must not silently truncate */
    assert(mnet_url_decode_ex("a%00b", 5, out, sizeof(out)) == -1);

    /* explicit length is honoured (source need not be NUL-terminated) */
    assert(mnet_url_decode_ex("helloXX", 5, out, sizeof(out)) == 5);
    assert(strcmp(out, "hello") == 0);

    /* output exactly filling the buffer (needs room for the NUL too) */
    assert(mnet_url_decode_ex("abc", 3, out, 4) == 3);
    assert(strcmp(out, "abc") == 0);

    /* one byte short: must refuse rather than truncate */
    assert(mnet_url_decode_ex("abc", 3, out, 3) == -1);

    /* zero-size destination is an error, not a write */
    assert(mnet_url_decode_ex("abc", 3, out, 0) == -1);

    /* NULL handling */
    assert(mnet_url_decode_ex(NULL, 0, out, sizeof(out)) == -1);
    assert(mnet_url_decode_ex("a", 1, NULL, 4) == -1);

    printf("  PASS test_mnet_url_decode_ex\n");
}

static void test_mnet_url_decode_malformed(void)
{
    char out[64];

    /* The legacy wrapper must not leave a truncated value behind. */
    memset(out, 'X', sizeof(out));
    size_t n = mnet_url_decode(out, sizeof(out), "a%zzb");
    assert(n == 0);
    assert(out[0] == '\0');

    memset(out, 'X', sizeof(out));
    n = mnet_url_decode(out, sizeof(out), "a%00b");
    assert(n == 0);
    assert(out[0] == '\0');

    /* exact fit succeeds */
    char buf4[4];
    n = mnet_url_decode(buf4, sizeof(buf4), "abc");
    assert(n == 3);
    assert(strcmp(buf4, "abc") == 0);

    /* overflow refuses */
    char buf3[3];
    n = mnet_url_decode(buf3, sizeof(buf3), "abcd");
    assert(n == 0);
    assert(buf3[0] == '\0');

    printf("  PASS test_mnet_url_decode_malformed\n");
}

static void test_mnet_header_value_valid(void)
{
    assert(mnet_header_value_valid("text/plain") == 1);
    assert(mnet_header_value_valid("application/json; charset=utf-8") == 1);
    assert(mnet_header_value_valid("") == 1);
    assert(mnet_header_value_valid(NULL) == 1);

    /* CR / LF / CRLF must be rejected (header injection) */
    assert(mnet_header_value_valid("text/plain\r") == 0);
    assert(mnet_header_value_valid("text/plain\n") == 0);
    assert(mnet_header_value_valid("text/plain\r\nX-Evil: 1") == 0);
    assert(mnet_header_value_valid("a\nb") == 0);

    /* other control characters too */
    assert(mnet_header_value_valid("a\x01b") == 0);
    assert(mnet_header_value_valid("a\x7f" "b") == 0);

    printf("  PASS test_mnet_header_value_valid\n");
}

static void test_mnet_header_name_valid(void)
{
    assert(mnet_header_name_valid("Content-Type") == 1);
    assert(mnet_header_name_valid("X-Custom_Header") == 1);
    assert(mnet_header_name_valid("") == 0);
    assert(mnet_header_name_valid(NULL) == 0);
    assert(mnet_header_name_valid("Bad Name") == 0);   /* space */
    assert(mnet_header_name_valid("Bad:Name") == 0);   /* colon */
    assert(mnet_header_name_valid("Bad\r\nName") == 0);
    assert(mnet_header_name_valid("Bad\nName") == 0);

    printf("  PASS test_mnet_header_name_valid\n");
}

static void test_route_match_allocation_failure_shape(void)
{
    /* A route with more parameters than the caller's slot array must not
       produce more live values than the caller can free. */
    mnet_route_t route = {
        .method = MNET_HTTP_GET,
        .path = "/a/:x/b/:y",
        .param_names = (const char *[]){"x", "y", NULL},
    };

    const char *values[4] = {0};
    int n = mnet_route_match(&route, "/a/1/b/2", values, 1);
    assert(n == 2);              /* both segments matched */
    (void)n;
    assert(values[0] != NULL);   /* only one value stored (max_values=1) */
    assert(values[1] == NULL);
    mnet_match_params_free(values, 1);
    printf("  PASS test_route_match_allocation_failure_shape\n");
}

int main(void)
{
    printf("Running mnet tests...\n");

    test_mnet_text();
    test_mnet_html();
    test_mnet_json();
    test_mnet_jsonf();
    test_mnet_jsonf_escape();
    test_mnet_jsonf_long_string();
    test_mnet_error();
    test_mnet_status();
    test_mnet_url_decode();
    test_mnet_json_escape();

    test_mnet_request_param();
    test_mnet_request_query();
    test_mnet_request_header();
    test_mnet_request_body();
    test_mnet_request_cookie();

    test_mnet_route_match_simple();
    test_mnet_route_match_multi();
    test_mnet_route_match_no_match();
    test_mnet_route_match_static();
    test_mnet_route_match_partial_free();
    test_mnet_route_match_wildcard();

    test_handler_macro();
    test_route_macros_compile();

    test_http_method_enum();
    test_middleware();
    test_timeout();
    test_debug_per_app();
    test_static_route_cleanup();
    test_mnet_chunked();
    test_max_connections();
    test_keep_alive_timeout();
    test_max_body_size();

    test_mnet_response_free_null();
    test_mnet_response_free_zeroed();
    test_mnet_response_free_owned();
    test_mnet_response_free_chunked();
    test_mnet_jsonf_very_long_string();
    test_mnet_jsonf_unicode_escape();

    test_mnet_url_decode_ex();
    test_mnet_url_decode_malformed();
    test_mnet_header_value_valid();
    test_mnet_header_name_valid();
    test_route_match_allocation_failure_shape();

    printf("\nAll tests passed!\n");
    return 0;
}
