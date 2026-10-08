# mnet Examples

mnet ships with several examples that demonstrate how to use the library.

## Running the examples

```sh
make examples
./example/example_http_server
./example/example_api_server
./example/example_combined
```

## Basic HTTP server

The basic HTTP server example serves a simple HTML page with links to other
routes.

```c
#include <mnet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "example_common.h"

MNET_HANDLER(home)
{
    (void)req;
    return mnet_html(
        "<!doctype html>"
        "<html>"
        "<head><title>mnet HTTP Server</title></head>"
        "<body>"
        "<h1>Welcome to mnet!</h1>"
        "<p>This is a basic HTTP server example.</p>"
        "<ul>"
        "  <li><a href=\"/about\">About</a></li>"
        "  <li><a href=\"/contact\">Contact</a></li>"
        "  <li><a href=\"/time\">Current Time</a></li>"
        "</ul>"
        "</body></html>"
    );
}

MNET_HANDLER(about)
{
    (void)req;
    return mnet_html(
        "<!doctype html>"
        "<html><head><title>About</title></head>"
        "<body>"
        "<h1>About mnet</h1>"
        "<p>mnet is a simple, ergonomic web framework for C.</p>"
        "<p><a href=\"/\">Back home</a></p>"
        "</body></html>"
    );
}

MNET_HANDLER(contact)
{
    (void)req;
    return mnet_html(
        "<!doctype html>"
        "<html><head><title>Contact</title></head>"
        "<body>"
        "<h1>Contact</h1>"
        "<p>Email: example@example.com</p>"
        "<p><a href=\"/\">Back home</a></p>"
        "</body></html>"
    );
}

MNET_HANDLER(current_time)
{
    (void)req;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    char html[256];
    snprintf(html, sizeof(html),
        "<!doctype html>"
        "<html><head><title>Current Time</title></head>"
        "<body>"
        "<h1>Current Time</h1>"
        "<p>%s</p>"
        "<p><a href=\"/\">Back home</a></p>"
        "</body></html>",
        time_str
    );

    return mnet_html(html);
}

MNET_HANDLER(not_found)
{
    (void)req;
    return mnet_html(
        "<!doctype html>"
        "<html><head><title>404</title></head>"
        "<body>"
        "<h1>404 - Page Not Found</h1>"
        "<p>The page you're looking for doesn't exist.</p>"
        "<p><a href=\"/\">Go home</a></p>"
        "</body></html>"
    );
}

int main(void)
{
    mnet_app_t *app = example_create();
    if (app == NULL) return 1;

    mnet_set_not_found_handler(app, not_found);

    MNET_GET(app, "/", home);
    MNET_GET(app, "/about", about);
    MNET_GET(app, "/contact", contact);
    MNET_GET(app, "/time", current_time);

    return example_serve(app, 8080,
        "Starting HTTP server on http://localhost:8080\n");
}
```

Visit `http://localhost:8080/` to see the home page.

## REST API server

The REST API server example demonstrates a JSON API with CRUD operations.

```c
#include <mnet.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "example_common.h"

/* In-memory "database" */
static const char *items[] = {
    "Item One",
    "Item Two",
    "Item Three",
    NULL
};

/* List all items (GET /api/items) */
MNET_HANDLER(list_items)
{
    (void)req;
    char json[512];
    int offset = 0;
    offset += snprintf(json + offset, sizeof(json) - offset, "[");
    int first = 1;
    for (int i = 0; items[i] != NULL; i++) {
        if (!first) offset += snprintf(json + offset, sizeof(json) - offset, ",");
        offset += snprintf(json + offset, sizeof(json) - offset,
            "\"%s\"", items[i]);
        first = 0;
    }
    offset += snprintf(json + offset, sizeof(json) - offset, "]");
    return mnet_json(json);
}

/* Get single item by ID (GET /api/items/:id) */
MNET_HANDLER(get_item)
{
    const char *id_str = MNET_PARAM(req, "id");
    if (id_str == NULL) {
        return mnet_error(400, "missing id parameter");
    }

    int id = atoi(id_str);
    if (id < 0 || items[id] == NULL) {
        return mnet_error(404, "item not found");
    }

    return mnet_jsonf("{\"id\":%d,\"name\":\"%s\"}", id, items[id]);
}

/* Create item (POST /api/items) */
MNET_HANDLER(create_item)
{
    const char *body = MNET_BODY(req);
    if (body == NULL || strlen(body) == 0) {
        return mnet_error(400, "empty body");
    }
    return mnet_jsonf("{\"created\":true,\"item\":\"%s\"}", body);
}

/* Search items (GET /api/search?q=...) */
MNET_HANDLER(search_items)
{
    const char *query = MNET_QUERY(req, "q");
    if (query == NULL || strlen(query) == 0) {
        return mnet_error(400, "missing query parameter 'q'");
    }

    char json[512];
    int offset = 0;
    offset += snprintf(json + offset, sizeof(json) - offset, "[");
    int first = 1;
    for (int i = 0; items[i] != NULL; i++) {
        if (strstr(items[i], query) != NULL) {
            if (!first) offset += snprintf(json + offset, sizeof(json) - offset, ",");
            offset += snprintf(json + offset, sizeof(json) - offset,
                "{\"id\":%d,\"name\":\"%s\"}", i, items[i]);
            first = 0;
        }
    }
    offset += snprintf(json + offset, sizeof(json) - offset, "]");
    return mnet_json(json);
}

/* Echo endpoint (POST /api/echo) - demonstrates body handling */
MNET_HANDLER(api_echo)
{
    const char *body = MNET_BODY(req);
    if (body == NULL) {
        return mnet_error(400, "empty body");
    }

    const char *content_type = MNET_HEADER(req, "Content-Type");
    return mnet_jsonf(
        "{\"echo\":{\"body\":\"%s\",\"content_type\":\"%s\"}}",
        body, content_type ? content_type : "none"
    );
}

/* Health check (GET /api/health) */
MNET_HANDLER(health)
{
    (void)req;
    return mnet_json("{\"status\":\"ok\"}");
}

int main(void)
{
    mnet_app_t *app = example_create();
    if (app == NULL) return 1;

    /* API routes */
    MNET_GET(app, "/api/items", list_items);
    MNET_GET(app, "/api/items/:id", get_item);
    MNET_POST(app, "/api/items", create_item);
    MNET_GET(app, "/api/search", search_items);
    MNET_POST(app, "/api/echo", api_echo);
    MNET_GET(app, "/api/health", health);

    return example_serve(app, 8080,
        "Starting API server on http://localhost:8080\n"
        "Endpoints:\n"
        "  GET  /api/items        - list all items\n"
        "  GET  /api/items/:id    - get item by ID\n"
        "  POST /api/items        - create item\n"
        "  GET  /api/search?q=    - search items\n"
        "  POST /api/echo         - echo back request\n"
        "  GET  /api/health       - health check\n");
}
```

Test the API with curl:

```sh
curl http://localhost:8080/api/items
curl http://localhost:8080/api/items/0
curl -X POST http://localhost:8080/api/echo -d "hello world"
curl http://localhost:8080/api/search?q=One
```

## Combined HTTP + API server

The combined example serves both HTML pages and a JSON API.

```c
#include <mnet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "example_common.h"

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
            "</body></html>");
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
        id, id, users[id], id);

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
        (long)now, 3);
}

MNET_HANDLER(api_echo)
{
    const char *body = MNET_BODY(req);
    const char *ct = MNET_HEADER(req, "Content-Type");
    const char *ua = MNET_HEADER(req, "User-Agent");

    return mnet_jsonf(
        "{\"echo\":{\"body\":\"%s\",\"content_type\":\"%s\",\"user_agent\":\"%s\"}}",
        body ? body : "(empty)", ct ? ct : "(none)", ua ? ua : "(none)");
}

MNET_HANDLER(custom_404)
{
    (void)req;
    return mnet_html(
        "<!doctype html><html><head><title>404</title></head>"
        "<body><h1>404 - Not Found</h1>"
        "<p>The page you requested doesn't exist.</p>"
        "<p><a href=\"/\">Go home</a></p></body></html>");
}

int main(void)
{
    mnet_app_t *app = example_create();
    if (app == NULL) return 1;

    mnet_set_not_found_handler(app, custom_404);

    /* Optional configuration */
    mnet_set_max_connections(app, 100);
    mnet_set_keep_alive_timeout(app, 30);
    mnet_set_max_body_size(app, 1024 * 1024); /* 1 MB */

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

    return example_serve(app, 8080,
        "Starting combined server on http://localhost:8080\n"
        "\nWeb pages:\n"
        "  GET  /                    - Home page\n"
        "  GET  /users               - Users list (HTML)\n"
        "  GET  /users/:id           - User detail (HTML)\n"
        "\nAPI endpoints:\n"
        "  GET  /api/users           - List all users\n"
        "  GET  /api/users/:id       - Get user by ID\n"
        "  POST /api/users           - Create user (Bearer secret)\n"
        "  GET  /api/search?q=       - Search users\n"
        "  GET  /api/protected       - Requires Authorization header\n"
        "  GET  /api/health          - Health check\n"
        "  POST /api/echo            - Echo request details\n");
}
```

## Example: client-side HTTP with MNET_CALL

```c
#include <mnet.h>

MNET_HANDLER(fetch_google)
{
    (void)req;
    return mnet_json(MNET_CALL("https://google.com"));
}

MNET_HANDLER(fetch_by_path)
{
    (void)req;
    const char *path = MNET_PARAM(req, "host");
    return mnet_json(MNET_CALL(path));
}

int main(void)
{
    mnet_app_t *app = mnet_create();
    if (app == NULL) return 1;

    /* Optional configuration */
    mnet_set_workers(app, 4);
    mnet_set_max_connections(app, 100);
    mnet_set_timeout(app, 30);

    /* Client-side HTTP endpoint */
    MNET_GET(app, "/google", fetch_google);
    MNET_GET(app, "/fetch/:host", fetch_by_path);

    mnet_run(app, 8080);
    mnet_destroy(app);
    return 0;
}
```

Test with curl:

```sh
curl http://localhost:8080/google
curl http://localhost:8080/fetch/google.com
```
