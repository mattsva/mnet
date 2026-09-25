#include <mnet.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
        body,
        content_type ? content_type : "none"
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
    mnet_app_t *app = mnet_create();
    if (app == NULL) {
        fprintf(stderr, "Failed to create app\n");
        return 1;
    }

    mnet_set_debug(app, 1);

    /* API routes */
    MNET_GET(app, "/api/items", list_items);
    MNET_GET(app, "/api/items/:id", get_item);
    MNET_POST(app, "/api/items", create_item);
    MNET_GET(app, "/api/search", search_items);
    MNET_POST(app, "/api/echo", api_echo);
    MNET_GET(app, "/api/health", health);

    printf("Starting API server on http://localhost:8080\n");
    printf("Endpoints:\n");
    printf("  GET  /api/items        - list all items\n");
    printf("  GET  /api/items/:id    - get item by ID\n");
    printf("  POST /api/items        - create item\n");
    printf("  GET  /api/search?q=    - search items\n");
    printf("  POST /api/echo         - echo back request\n");
    printf("  GET  /api/health       - health check\n");
    printf("Press Ctrl+C to stop\n");

    int rc = mnet_run(app, 8080);
    mnet_destroy(app);
    return rc;
}
