/* src/mnet_app.c - Application lifecycle, configuration and route
 * registration. */

#define _GNU_SOURCE
#include "mnet_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mnet_add_route(
    mnet_app_t *app,
    mnet_http_method_t method,
    const char *path,
    mnet_handler_t handler)
{
    if (app == NULL || path == NULL || handler == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (app->route_count == app->route_capacity) {
        size_t nc;
        if (app->route_capacity == 0) {
            nc = MNET_INITIAL_ROUTE_CAPACITY;
        } else {
            nc = app->route_capacity * 2;
        }
        mnet_route_t *nr = realloc(app->routes, nc * sizeof(*nr));
        if (nr == NULL) return -1;
        app->routes = nr;
        app->route_capacity = nc;
    }

    mnet_route_t *r = &app->routes[app->route_count];
    r->method = method;
    r->path = path;
    r->handler = handler;
    r->param_names = NULL;
    r->user_data = NULL;
    r->path_allocated = 0;

    /* Parse parameter names from the pattern */
    size_t param_count = 0;
    const char *p = path;
    while (*p) {
        if (*p == ':') {
            p++;
            const char *start = p;
            while (*p && *p != '/' && *p != '.') p++;
            size_t len = (size_t)(p - start);
            char *name;
            if (len == 0) {
                /* Unnamed param: generate a name so the matcher's count
                   matches param_names. */
                name = malloc(16);
                if (name == NULL) goto fail;
                snprintf(name, 16, "_%zu", param_count);
            } else {
                name = malloc(len + 1);
                if (name == NULL) goto fail;
                memcpy(name, start, len);
                name[len] = '\0';
            }

            const char **tmp = realloc(r->param_names,
                (param_count + 2) * sizeof(const char *));
            if (tmp == NULL) { free(name); goto fail; }
            r->param_names = tmp;
            r->param_names[param_count] = name;
            param_count++;
            r->param_names[param_count] = NULL;
        } else if (*p == '*') {
            /*
             * A wildcard captures the rest of the path and is exposed under
             * the name "*", so MNET_PARAM(req, "*") returns it. It must be
             * registered here or the matcher's slot would have no name to
             * look up.
             */
            char *name = malloc(2);
            if (name == NULL) goto fail;
            name[0] = '*';
            name[1] = '\0';

            const char **tmp = realloc(r->param_names,
                (param_count + 2) * sizeof(const char *));
            if (tmp == NULL) { free(name); goto fail; }
            r->param_names = tmp;
            r->param_names[param_count] = name;
            param_count++;
            r->param_names[param_count] = NULL;

            /* Nothing after the wildcard is matched, so stop scanning. */
            break;
        } else {
            p++;
        }
    }

    app->route_count++;
    return 0;

fail:
    /* Release whatever was allocated for this route; it is not registered. */
    if (r->param_names != NULL) {
        for (size_t i = 0; i < param_count; i++) {
            free((void *)r->param_names[i]);
        }
        free(r->param_names);
        r->param_names = NULL;
    }
    errno = ENOMEM;
    return -1;
}

static void free_route(mnet_route_t *r)
{
    if (r->param_names) {
        for (size_t i = 0; r->param_names[i] != NULL; i++) {
            free((void *)r->param_names[i]);
        }
        free(r->param_names);
        r->param_names = NULL;
    }
    if (r->path_allocated) {
        free((void *)r->path);
        r->path = NULL;
        r->path_allocated = 0;
    }
    if (r->user_data) {
        static_config_t *cfg = (static_config_t *)r->user_data;
        free(cfg->url_prefix);
        free(cfg->fs_path);
        free(cfg);
        r->user_data = NULL;
    }
}

mnet_app_t *mnet_create(void)
{
    mnet_app_t *app = calloc(1, sizeof(mnet_app_t));

    if (app == NULL) return NULL;

    /*
     * Set the security-relevant defaults explicitly rather than relying on the
     * "0 means default" convention everywhere. A bounded timeout is what keeps
     * a client that connects and sends nothing from pinning the server, so it
     * is on from the start. mnet_set_timeout(app, 0) restores this value.
     */
    app->timeout_seconds = MNET_DEFAULT_TIMEOUT;
    app->keep_alive_timeout = MNET_DEFAULT_KEEP_ALIVE_TIMEOUT;

    /*
     * Threaded by default. A single-threaded blocking server lets one client
     * that connects and then stalls occupy the whole server, which is the
     * thing this library most needs to avoid out of the box.
     * mnet_set_workers(app, 1) selects the single-threaded loop.
     */
    app->workers = MNET_DEFAULT_WORKERS;

    return app;
}

void mnet_destroy(mnet_app_t *app)
{
    if (app == NULL) return;
    for (size_t i = 0; i < app->route_count; i++) {
        free_route(&app->routes[i]);
    }
    free(app->routes);
    free(app);
}

void mnet_set_debug(mnet_app_t *app, int enabled)
{
    if (app != NULL) app->debug = enabled;
}

void mnet_set_not_found_handler(mnet_app_t *app, mnet_response_t (*handler)(mnet_request_t *req))
{
    if (app != NULL) app->not_found_handler = handler;
}

void mnet_use(mnet_app_t *app, mnet_middleware_t middleware)
{
    if (app != NULL) app->middleware = middleware;
}

void mnet_set_timeout(mnet_app_t *app, int seconds)
{
    if (app == NULL) return;

    /* 0 restores the bounded default; a negative value disables the timeout
       entirely, which the documentation discourages for production. */
    app->timeout_seconds = (seconds == 0) ? MNET_DEFAULT_TIMEOUT : seconds;
}

void mnet_set_workers(mnet_app_t *app, int workers)
{
    if (app == NULL) return;

    /* 0 restores the threaded default; 1 selects the single-threaded loop. */
    if (workers == 0) workers = MNET_DEFAULT_WORKERS;
    if (workers < 0) workers = 1;
    if (workers > MNET_MAX_WORKERS) workers = MNET_MAX_WORKERS;
    app->workers = workers;
}

void mnet_set_log_handler(mnet_app_t *app, mnet_log_handler_t handler)
{
    mnet_log_set_handler(handler);
    if (app != NULL) app->log_handler = handler;
}

void mnet_set_https(mnet_app_t *app, int enabled)
{
    if (app != NULL) app->https = enabled;
}

void mnet_set_dev_mode(mnet_app_t *app, int enabled)
{
    if (app != NULL) app->dev = enabled;
}

void mnet_set_max_connections(mnet_app_t *app, int max_connections)
{
    if (app != NULL) app->max_connections = max_connections;
}

void mnet_set_keep_alive_timeout(mnet_app_t *app, int seconds)
{
    if (app == NULL) return;
    app->keep_alive_timeout = (seconds == 0) ?
        MNET_DEFAULT_KEEP_ALIVE_TIMEOUT : seconds;
}

void mnet_set_max_body_size(mnet_app_t *app, size_t max_body_size)
{
    if (app != NULL) app->max_body_size = max_body_size;
}

void mnet_static(mnet_app_t *app, const char *url_prefix,
    const char *fs_path)
{
    if (app == NULL || url_prefix == NULL || fs_path == NULL) return;

    if (app->route_count == app->route_capacity) {
        size_t nc;
        if (app->route_capacity == 0) {
            nc = MNET_INITIAL_ROUTE_CAPACITY;
        } else {
            nc = app->route_capacity * 2;
        }
        mnet_route_t *nr = realloc(app->routes, nc * sizeof(*nr));
        if (nr == NULL) return;
        app->routes = nr;
        app->route_capacity = nc;
    }

    static_config_t *cfg = malloc(sizeof(static_config_t));
    if (cfg == NULL) return;
    cfg->url_prefix = strdup(url_prefix);
    cfg->fs_path = strdup(fs_path);
    if (cfg->url_prefix == NULL || cfg->fs_path == NULL) {
        free(cfg->url_prefix);
        free(cfg->fs_path);
        free(cfg);
        return;
    }

    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s/*", url_prefix);

    char *path_copy = strdup(pattern);
    if (path_copy == NULL) {
        free(cfg->url_prefix);
        free(cfg->fs_path);
        free(cfg);
        return;
    }

    mnet_route_t *r = &app->routes[app->route_count];
    r->method = MNET_HTTP_GET;
    r->path = path_copy;
    r->handler = static_handler;
    r->param_names = NULL;
    r->user_data = cfg;
    r->path_allocated = 1;

    cfg->app = app;

    app->route_count++;
}

int mnet_stop(mnet_app_t *app)
{
    if (app == NULL) {
        errno = EINVAL;
        return -1;
    }
    app->running = 0;
    return 0;
}

int mnet_route(mnet_app_t *app, mnet_http_method_t method,
    const char *path, mnet_handler_t handler)
{
    return mnet_add_route(app, method, path, handler);
}
