#include <mnet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

static const char *users[] = {
    "Alice",
    "Bob",
    "Charlie",
    NULL
};

MNET_HANDLER(home_page)
{
    (void)req;
    return mnet_html(
        "<!doctype html>"
        "<html><head><title>mnet Combined Example</title></head>"
        "<body>"
        "<h1>mnet Combined Example</h1>"
        "<p>This server serves both HTML pages and a JSON API.</p>"
        "<h2>Web Pages</h2>"
        "<ul>"
        "  <li><a href=\"/\">Home</a> (this page)</li>"
        "  <li><a href=\"/users\">Users List</a></li>"
        "  <li><a href=\"/users/0\">User Details (ID 0)</a></li>"
        "</ul>"
        "<h2>API Endpoints</h2>"
        "<ul>"
        "  <li><code>GET /api/users</code> - list users</li>"
        "  <li><code>GET /api/users/:id</code> - get user by ID</li>"
        "  <li><code>POST /api/users</code> - create user (JSON body)</li>"
        "  <li><code>GET /api/search?q=...</code> - search users</li>"
        "  <li><code>GET /api/protected</code> - protected (requires header)</li>"
        "  <li><code>GET /api/health</code> - health check</li>"
        "</ul>"
        "</body></html>"
    );
}

MNET_HANDLER(users_page)
{
    (void)req;
    char html[1024];
    int offset = 0;
    offset += snprintf(html + offset, sizeof(html) - offset,
        "<!doctype html><html><head><title>Users</title></head><body>"
        "<h1>Users</h1><ul>");

    for (int i = 0; users[i] != NULL; i++) {
        offset += snprintf(html + offset, sizeof(html) - offset,
            "<li><a href=\"/users/%d\">%s</a></li>", i, users[i]);
    }

    offset += snprintf(html + offset, sizeof(html) - offset,
        "</ul><p><a href=\"/\">Back home</a></p></body></html>");

    return mnet_html(html);
}

MNET_HANDLER(user_detail_page)
{
    const char *id_str = MNET_PARAM(req, "id");
    if (id_str == NULL) {
        return mnet_error(400, "missing id");
    }

    int id = atoi(id_str);
    if (id < 0 || users[id] == NULL) {
        mnet_response_t r = mnet_html(
            "<!doctype html><html><head><title>404</title></head><body>"
            "<h1>User Not Found</h1><p><a href=\"/users\">Back to users</a></p>"
            "</body></html>"
        );
        r.status = 404;
        return r;
    }

    char html[256];
    snprintf(html, sizeof(html),
        "<!doctype html><html><head><title>User %d</title></head><body>"
        "<h1>User %d: %s</h1>"
        "<p><a href=\"/users\">Back to users</a> | "
        "<a href=\"/api/users/%d\">API view</a></p>"
        "</body></html>",
        id, id, users[id], id
    );

    return mnet_html(html);
}

MNET_HANDLER(api_list_users)
{
    (void)req;
    char json[512];
    int offset = 0;
    offset += snprintf(json + offset, sizeof(json) - offset, "[");
    int first = 1;
    for (int i = 0; users[i] != NULL; i++) {
        if (!first) offset += snprintf(json + offset, sizeof(json) - offset, ",");
        offset += snprintf(json + offset, sizeof(json) - offset,
            "{\"id\":%d,\"name\":\"%s\"}", i, users[i]);
        first = 0;
    }
    offset += snprintf(json + offset, sizeof(json) - offset, "]");
    return mnet_json(json);
}

MNET_HANDLER(api_get_user)
{
    const char *id_str = MNET_PARAM(req, "id");
    if (id_str == NULL) {
        return mnet_error(400, "missing id");
    }

    int id = atoi(id_str);
    if (id < 0 || users[id] == NULL) {
        return mnet_error(404, "user not found");
    }

    return mnet_jsonf("{\"id\":%d,\"name\":\"%s\"}", id, users[id]);
}

MNET_HANDLER(api_create_user)
{
    const char *auth = MNET_HEADER(req, "Authorization");
    if (auth == NULL || strcmp(auth, "Bearer secret") != 0) {
        return mnet_error(401, "unauthorized");
    }

    const char *body = MNET_BODY(req);
    if (body == NULL || strlen(body) == 0) {
        return mnet_error(400, "empty body");
    }

    /* In real code, parse JSON and validate */
    return mnet_jsonf("{\"created\":true,\"name\":\"%s\"}", body);
}

MNET_HANDLER(api_search)
{
    const char *q = MNET_QUERY(req, "q");
    if (q == NULL || strlen(q) == 0) {
        return mnet_error(400, "missing 'q' parameter");
    }

    char json[512];
    int offset = 0;
    offset += snprintf(json + offset, sizeof(json) - offset, "[");
    int first = 1;
    for (int i = 0; users[i] != NULL; i++) {
        if (strstr(users[i], q) != NULL) {
            if (!first) offset += snprintf(json + offset, sizeof(json) - offset, ",");
            offset += snprintf(json + offset, sizeof(json) - offset,
                "{\"id\":%d,\"name\":\"%s\"}", i, users[i]);
            first = 0;
        }
    }
    offset += snprintf(json + offset, sizeof(json) - offset, "]");
    return mnet_json(json);
}

MNET_HANDLER(api_protected)
{
    const char *auth = MNET_HEADER(req, "Authorization");
    if (auth == NULL) {
        return mnet_error(401, "missing Authorization header");
    }

    return mnet_jsonf("{\"message\":\"access granted\",\"auth\":\"%s\"}", auth);
}

MNET_HANDLER(api_health)
{
    (void)req;
    time_t now = time(NULL);
    return mnet_jsonf(
        "{\"status\":\"ok\",\"timestamp\":%ld,\"users_count\":%d}",
        (long)now,
        3  /* count of users */
    );
}

MNET_HANDLER(api_echo)
{
    const char *body = MNET_BODY(req);
    const char *ct = MNET_HEADER(req, "Content-Type");
    const char *ua = MNET_HEADER(req, "User-Agent");

    return mnet_jsonf(
        "{\"echo\":{\"body\":\"%s\",\"content_type\":\"%s\",\"user_agent\":\"%s\"}}",
        body ? body : "(empty)",
        ct ? ct : "(none)",
        ua ? ua : "(none)"
    );
}

MNET_HANDLER(custom_404)
{
    (void)req;
    return mnet_html(
        "<!doctype html><html><head><title>404</title></head><body>"
        "<h1>404 - Not Found</h1>"
        "<p>The page you requested doesn't exist.</p>"
        "<p><a href=\"/\">Go home</a></p>"
        "</body></html>"
    );
}

int main(void)
{
    mnet_app_t *app = mnet_create();
    if (app == NULL) {
        fprintf(stderr, "Failed to create app\n");
        return 1;
    }

    mnet_set_debug(app, 1);
    mnet_set_not_found_handler(app, custom_404);

    /* HTML pages */
    MNET_GET(app, "/", home_page);
    MNET_GET(app, "/users", users_page);
    MNET_GET(app, "/users/:id", user_detail_page);

    /* JSON API */
    MNET_GET(app, "/api/users", api_list_users);
    MNET_GET(app, "/api/users/:id", api_get_user);
    MNET_POST(app, "/api/users", api_create_user);
    MNET_GET(app, "/api/search", api_search);
    MNET_GET(app, "/api/protected", api_protected);
    MNET_GET(app, "/api/health", api_health);
    MNET_POST(app, "/api/echo", api_echo);

    printf("Starting combined server on http://localhost:8080\n");
    printf("\nWeb pages:\n");
    printf("  GET  /                    - Home page\n");
    printf("  GET  /users               - Users list (HTML)\n");
    printf("  GET  /users/:id           - User detail (HTML)\n");
    printf("\nAPI endpoints:\n");
    printf("  GET  /api/users           - List all users\n");
    printf("  GET  /api/users/:id       - Get user by ID\n");
    printf("  POST /api/users           - Create user (Bearer secret)\n");
    printf("  GET  /api/search?q=       - Search users\n");
    printf("  GET  /api/protected       - Requires Authorization header\n");
    printf("  GET  /api/health          - Health check\n");
    printf("  POST /api/echo            - Echo request details\n");
    printf("\nPress Ctrl+C to stop\n");

    int rc = mnet_run(app, 8080);
    mnet_destroy(app);
    return rc;
}
