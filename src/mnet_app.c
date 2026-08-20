#define _POSIX_C_SOURCE 200112L

#include "mnet_app.h"
#include "mnet_socket.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MNET_INITIAL_ROUTE_CAPACITY 8
#define MNET_REQUEST_BUFFER_SIZE 8192

struct mnet_app {
    mnet_route_t *routes;
    size_t route_count;
    size_t route_capacity;
    int running;
};

static int mnet_add_route(
    mnet_app_t *app,
    mnet_method_t method,
    const char *path,
    mnet_handler_t handler)
{
    if (app == NULL || path == NULL || handler == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (app->route_count == app->route_capacity) {
        size_t new_capacity;

        if (app->route_capacity == 0) {
            new_capacity = MNET_INITIAL_ROUTE_CAPACITY;
        } else {
            new_capacity = app->route_capacity * 2;
        }

        mnet_route_t *new_routes = realloc(
            app->routes,
            new_capacity * sizeof(*new_routes)
        );

        if (new_routes == NULL) {
            return -1;
        }

        app->routes = new_routes;
        app->route_capacity = new_capacity;
    }

    app->routes[app->route_count].method = method;
    app->routes[app->route_count].path = path;
    app->routes[app->route_count].handler = handler;

    app->route_count++;

    return 0;
}

static mnet_route_t *mnet_find_route(
    mnet_app_t *app,
    mnet_method_t method,
    const char *path)
{
    for (size_t i = 0; i < app->route_count; i++) {
        mnet_route_t *route = &app->routes[i];

        if (route->method == method &&
            strcmp(route->path, path) == 0) {
            return route;
        }
    }

    return NULL;
}

static mnet_method_t mnet_parse_method(
    const char *method)
{
    if (strcmp(method, "GET") == 0) {
        return MNET_METHOD_GET;
    }

    if (strcmp(method, "POST") == 0) {
        return MNET_METHOD_POST;
    }

    if (strcmp(method, "PUT") == 0) {
        return MNET_METHOD_PUT;
    }

    if (strcmp(method, "DELETE") == 0) {
        return MNET_METHOD_DELETE;
    }

    return -1;
}

static void mnet_handle_client(
    mnet_app_t *app,
    mnet_socket_t client)
{
    char buffer[MNET_REQUEST_BUFFER_SIZE];

    ssize_t received = mnet_recv(
        client,
        buffer,
        sizeof(buffer) - 1
    );

    if (received <= 0) {
        return;
    }

    buffer[received] = '\0';

    char method[16];
    char path[2048];

    if (sscanf(
            buffer,
            "%15s %2047s",
            method,
            path) != 2) {

        const char response[] =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Length: 11\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Bad Request";

        mnet_send(
            client,
            response,
            sizeof(response) - 1
        );

        return;
    }

    mnet_method_t parsed_method = mnet_parse_method(method);

    if (parsed_method < 0) {
        const char response[] =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Length: 18\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Method Not Allowed";

        mnet_send(
            client,
            response,
            sizeof(response) - 1
        );

        return;
    }

    mnet_request_t request = {
        .method = method,
        .path = path,
        .body = NULL,
        .body_length = 0
    };

    mnet_response_t response = {
        .status = 200,
        .content_type = "text/plain; charset=utf-8",
        .body = NULL,
        .body_length = 0
    };

    mnet_route_t *route = mnet_find_route(
        app,
        parsed_method,
        path
    );

    if (route == NULL) {
        mnet_response_status(&response, 404);
        mnet_response_text(&response, "Not Found");
    } else {
        route->handler(&request, &response);
    }

    const char *status_text = "Internal Server Error";

    switch (response.status) {
        case 200:
            status_text = "OK";
            break;
        case 201:
            status_text = "Created";
            break;
        case 204:
            status_text = "No Content";
            break;
        case 400:
            status_text = "Bad Request";
            break;
        case 404:
            status_text = "Not Found";
            break;
        case 500:
            status_text = "Internal Server Error";
            break;
        default:
            break;
    }

    char header[1024];

    int header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        response.status,
        status_text,
        response.body_length,
        response.content_type != NULL
            ? response.content_type
            : "application/octet-stream"
    );

    if (header_length < 0 ||
        (size_t)header_length >= sizeof(header)) {
        return;
    }

    mnet_send(
        client,
        header,
        (size_t)header_length
    );

    if (response.body != NULL &&
        response.body_length > 0) {

        mnet_send(
            client,
            response.body,
            response.body_length
        );
    }
}

mnet_app_t *mnet_app_create(void)
{
    return calloc(1, sizeof(mnet_app_t));
}

void mnet_app_destroy(mnet_app_t *app)
{
    if (app == NULL) {
        return;
    }

    free(app->routes);
    free(app);
}

int mnet_app_run(
    mnet_app_t *app,
    uint16_t port)
{
    if (app == NULL) {
        errno = EINVAL;
        return -1;
    }

    mnet_socket_t server = mnet_tcp_listen(
        port,
        16
    );

    if (server == MNET_INVALID_SOCKET) {
        return -1;
    }

    app->running = 1;

    while (app->running) {
        mnet_socket_t client = mnet_tcp_accept(server);

        if (client == MNET_INVALID_SOCKET) {
            if (errno == EINTR) {
                continue;
            }

            break;
        }

        mnet_handle_client(app, client);

        mnet_close(client);
    }

    mnet_close(server);

    return 0;
}

int mnet_app_stop(
    mnet_app_t *app)
{
    if (app == NULL) {
        errno = EINVAL;
        return -1;
    }

    app->running = 0;

    return 0;
}

int mnet_get(
    mnet_app_t *app,
    const char *path,
    mnet_handler_t handler)
{
    return mnet_add_route(
        app,
        MNET_METHOD_GET,
        path,
        handler
    );
}

int mnet_post(
    mnet_app_t *app,
    const char *path,
    mnet_handler_t handler)
{
    return mnet_add_route(
        app,
        MNET_METHOD_POST,
        path,
        handler
    );
}

int mnet_put(
    mnet_app_t *app,
    const char *path,
    mnet_handler_t handler)
{
    return mnet_add_route(
        app,
        MNET_METHOD_PUT,
        path,
        handler
    );
}

int mnet_delete(
    mnet_app_t *app,
    const char *path,
    mnet_handler_t handler)
{
    return mnet_add_route(
        app,
        MNET_METHOD_DELETE,
        path,
        handler
    );
}
