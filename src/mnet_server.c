/* src/mnet_server.c - Connection lifecycle, the worker pool and the
 * mnet_run() accept loop. */

#define _GNU_SOURCE
#include "mnet_internal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mnet_handle_client(mnet_app_t *app, mnet_socket_t client)
{
    int keep_alive = 0;
    char buffer[MNET_REQUEST_BUFFER_SIZE];
    size_t used = 0;
    do {
        /* Use an absolute deadline for the entire request, not a per-recv
           timeout. SO_RCVTIMEO applies per recv() call, so a client that
           sends one byte every timeout interval can hold the connection
           forever. */
        int timeout_ms = app->timeout_seconds > 0 ?
            app->timeout_seconds * 1000 : 30000;
        if (keep_alive && app->keep_alive_timeout > 0) {
            timeout_ms = app->keep_alive_timeout * 1000;
        }

        int have_request = 0;
        int too_large = 0;

        /* Check if there's already a complete request in the buffer from
           a previous read (keep-alive or pipelined). */
        if (used > 0 && (strstr(buffer, "\r\n\r\n") != NULL ||
                          strstr(buffer, "\n\n") != NULL)) {
            have_request = 1;
        }

        /* Read until the end of the header block arrives, the buffer is
           full, or the absolute deadline passes. */
        int64_t deadline_ms = now_ms_mono() + timeout_ms;
        while (!have_request && used < sizeof(buffer) - 1) {
            int64_t now_ms = now_ms_mono();
            int64_t left = deadline_ms - now_ms;
            if (left <= 0) break;

            int pr = mnet_poll(client, (int)left);
            if (pr <= 0) break;

            ssize_t n = mnet_recv(client, buffer + used,
                sizeof(buffer) - 1 - used);
            if (n <= 0) break;
            used += (size_t)n;
            buffer[used] = '\0';
            if (strstr(buffer, "\r\n\r\n") != NULL ||
                strstr(buffer, "\n\n") != NULL) {
                have_request = 1;
                break;
            }
        }

        if (!have_request) {
            if (used >= sizeof(buffer) - 1) too_large = 1;
            if (too_large) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: header block exceeds %d bytes",
                    (int)(sizeof(buffer) - 1));
                send_simple_error(client, 431, "Request Header Fields Too Large",
                    "Request headers exceed the maximum size.", app->debug);
            }
            break;
        }

        parsed_request_t parsed;
        size_t max_body = app->max_body_size > 0 ?
            app->max_body_size : MNET_MAX_BODY_SIZE;
        int pr = parse_request(client, buffer, (ssize_t)used, &parsed, max_body, timeout_ms);

        if (pr != MNET_PARSE_OK) {
            if (pr == MNET_PARSE_UNSUPPORTED_METHOD) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: unsupported method '%s'", parsed.method);
                send_method_not_allowed(client, parsed.method, app->debug, 0);
            } else if (pr == MNET_PARSE_TOO_LARGE) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: body exceeds the configured limit");
                send_simple_error(client, 413, "Payload Too Large",
                    "Request body exceeds the maximum size.", app->debug);
            } else if (pr == MNET_PARSE_BAD_CONTENT_LENGTH) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: malformed Content-Length");
                send_simple_error(client, 400, "Bad Request",
                    "The Content-Length header is malformed.", app->debug);
            } else if (pr == MNET_PARSE_DUPLICATE_CONTENT_LENGTH) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: duplicate Content-Length");
                send_simple_error(client, 400, "Bad Request",
                    "Duplicate Content-Length headers.", app->debug);
            } else if (pr == MNET_PARSE_HEADERS_TOO_LARGE) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: header count or line length exceeds the limit");
                send_simple_error(client, 431, "Request Header Fields Too Large",
                    "Request headers exceed the maximum size.", app->debug);
            } else if (pr == MNET_PARSE_UNSUPPORTED_TRANSFER_ENCODING) {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: Transfer-Encoding not supported");
                send_simple_error(client, 501, "Not Implemented",
                    "Transfer-Encoding is not supported.", app->debug);
            } else {
                mnet_app_log(app, MNET_LOG_WARN,
                    "rejected request: could not parse the request line");
                send_simple_error(client, 400, "Bad Request",
                    "The request could not be parsed.", app->debug);
            }
            free_extras(&parsed.extras);
            if (parsed.body_heap) free(parsed.body);
            break;
        }

        keep_alive = parsed.keep_alive;

        const char *param_values[MNET_MAX_PARAMS] = {0};
        size_t param_count = 0;

        mnet_response_t response = dispatch(app, &parsed, param_values,
            MNET_MAX_PARAMS, &param_count);

        if (response.status == 0 && app->not_found_handler == NULL) {
            send_not_found(client, parsed.path_only, app->debug, keep_alive);
        } else {
            if (app->debug) {
                fprintf(stderr, "[mnet] %d %s %s\n",
                    response.status, parsed.method, parsed.path_only);
            }
            send_response(client, &response, parsed.method_enum == MNET_HTTP_HEAD,
                keep_alive);
            mnet_response_free(&response);
        }

        mnet_match_params_free(param_values, (int)param_count);
        free_extras(&parsed.extras);
        if (parsed.body_heap) free(parsed.body);

        /* Check if there's another complete request already in the buffer
           (pipelined). If so, don't read more — just loop and process it. */
        if (keep_alive) {
            const char *sep = strstr(buffer, "\r\n\r\n");
            const char *sep2 = strstr(buffer, "\n\n");
            const char *end_of_headers = sep ? sep : (sep2 ? sep2 : NULL);
            if (end_of_headers) {
                size_t header_len = (size_t)(end_of_headers - buffer) + (sep ? 4 : 2);
                if (header_len < used) {
                    /* There's more data after the headers — check if it
                       contains another complete request. */
                    size_t remaining = used - header_len;
                    if (remaining > 0 && strstr(buffer + header_len, "\r\n\r\n") != NULL) {
                        /* Move the pipelined request to the front of the buffer. */
                        memmove(buffer, buffer + header_len, remaining);
                        used = remaining;
                        buffer[used] = '\0';
                        continue;
                    }
                }
            }
            used = 0;
        }
    } while (keep_alive);
}

static mnet_app_t *g_running_app = NULL;

static void sig_handler(int signum)
{
    (void)signum;
    if (g_running_app) {
        g_running_app->running = 0;
    }
}

/*
 * Worker pool.
 *
 * The accept loop pushes each accepted connection onto a bounded queue; the
 * workers pop one, serve it to completion (including keep-alive requests) and
 * loop. All shared state is guarded by the mutex: the queue itself, the
 * connection counter used to enforce max_connections, and the shutdown flag.
 *
 * app->routes and the handler table are only read once the pool is running, so
 * they need no locking: they are fully populated before mnet_run() is called
 * and the application is not expected to register routes afterwards.
 */
typedef struct {
    mnet_socket_t *slots;
    size_t capacity;
    size_t head;
    size_t count;
    int shutting_down;
    mnet_app_t *app;

    mnet_mutex_t mutex;
    mnet_cond_t not_empty;
    mnet_cond_t not_full;
    mnet_thread_t *threads;
    size_t thread_count;
} mnet_pool_t;

static int mnet_pool_push(mnet_pool_t *pool, mnet_socket_t client)
{
    mnet_mutex_lock(&pool->mutex);

    while (pool->count == pool->capacity && !pool->shutting_down) {
        /* The queue is full: wait for a worker to free a slot. If the server
           is stopping, stop accepting instead. */
        if (!pool->app->running) {
            pool->shutting_down = 1;
            break;
        }
        mnet_cond_wait(&pool->not_full, &pool->mutex);
    }

    if (pool->shutting_down) {
        mnet_mutex_unlock(&pool->mutex);
        return -1;
    }

    pool->slots[(pool->head + pool->count) % pool->capacity] = client;
    pool->count++;
    mnet_cond_signal(&pool->not_empty);
    mnet_mutex_unlock(&pool->mutex);
    return 0;
}

static int mnet_pool_pop(mnet_pool_t *pool, mnet_socket_t *out)
{
    mnet_mutex_lock(&pool->mutex);

    while (pool->count == 0 && !pool->shutting_down) {
        mnet_cond_wait(&pool->not_empty, &pool->mutex);
    }

    if (pool->count == 0) {
        mnet_mutex_unlock(&pool->mutex);
        return -1;
    }

    *out = pool->slots[pool->head];
    pool->head = (pool->head + 1) % pool->capacity;
    pool->count--;
    mnet_cond_signal(&pool->not_full);
    mnet_mutex_unlock(&pool->mutex);
    return 0;
}

MNET_THREAD_FN(mnet_worker_main)
{
    mnet_pool_t *pool = (mnet_pool_t *)arg;
    mnet_socket_t client;

    while (mnet_pool_pop(pool, &client) == 0) {
        mnet_handle_client(pool->app, client);
        mnet_close(client);

        mnet_mutex_lock(&pool->mutex);
        pool->app->active_connections--;
        mnet_mutex_unlock(&pool->mutex);
    }

    MNET_THREAD_RETURN;
}

static void mnet_pool_shutdown(mnet_pool_t *pool)
{
    mnet_mutex_lock(&pool->mutex);
    pool->shutting_down = 1;
    mnet_cond_broadcast(&pool->not_empty);
    mnet_cond_broadcast(&pool->not_full);
    mnet_mutex_unlock(&pool->mutex);

    for (size_t i = 0; i < pool->thread_count; i++) {
        mnet_thread_join(pool->threads[i]);
    }
    free(pool->threads);
    pool->threads = NULL;

    /* Drain anything still queued: no worker will pick it up now. */
    while (pool->count > 0) {
        mnet_socket_t c = pool->slots[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        mnet_close(c);
    }
    free(pool->slots);
    pool->slots = NULL;

    mnet_mutex_destroy(&pool->mutex);
    mnet_cond_destroy(&pool->not_empty);
    mnet_cond_destroy(&pool->not_full);
}

int mnet_run(mnet_app_t *app, uint16_t port)
{
    if (app == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Handle SIGINT/SIGTERM for graceful shutdown. */
#ifndef _WIN32
    struct sigaction sa = {0};
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#else
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#endif

    g_running_app = app;

    app->port = port;

    mnet_socket_t server = mnet_tcp_listen(port, MNET_LISTEN_BACKLOG);
    if (server == MNET_INVALID_SOCKET) {
        mnet_log_msg(MNET_LOG_ERROR, "failed to listen on port %d", port);
        return -1;
    }

    app->running = 1;
    app->active_connections = 0;

    mnet_app_log(app, MNET_LOG_INFO, "listening on port %d", port);

    /*
     * Single-threaded mode is kept for workers <= 1: it has no synchronisation
     * overhead and is what the library did before the pool existed.
     */
    if (app->workers <= 1) {
        while (app->running) {
            mnet_socket_t client = mnet_tcp_accept(server);
            if (client == MNET_INVALID_SOCKET) {
                if (mnet_socket_errno() == MNET_EINTR) continue;
                mnet_app_log(app, MNET_LOG_ERROR,
                    "accept failed, stopping the server");
                break;
            }

            if (app->max_connections > 0 &&
                app->active_connections >= app->max_connections) {
                /* Refuse rather than queue: a plain counter, not per-IP. */
                mnet_app_log(app, MNET_LOG_WARN,
                    "refused a connection: %d active (max_connections)",
                    app->active_connections);
                send_simple_error(client, 503, "Service Unavailable",
                    "The server is at its connection limit.", app->debug);
                mnet_close(client);
                continue;
            }

            app->active_connections++;
            mnet_handle_client(app, client);
            mnet_close(client);
            app->active_connections--;
        }

        mnet_close(server);
        mnet_app_log(app, MNET_LOG_INFO, "server stopped");
        return 0;
    }

    /* Multithreaded: a bounded queue feeding a fixed pool of workers. */
    size_t capacity = app->max_connections > 0 ?
        (size_t)app->max_connections : 128;

    mnet_pool_t pool;
    memset(&pool, 0, sizeof(pool));
    pool.app = app;
    pool.capacity = capacity;
    pool.slots = malloc(capacity * sizeof(mnet_socket_t));
    pool.threads = malloc((size_t)app->workers * sizeof(mnet_thread_t));

    if (pool.slots == NULL || pool.threads == NULL) {
        free(pool.slots);
        free(pool.threads);
        mnet_close(server);
        mnet_log_msg(MNET_LOG_ERROR, "failed to allocate the worker pool");
        return -1;
    }

    mnet_mutex_init(&pool.mutex);
    mnet_cond_init(&pool.not_empty);
    mnet_cond_init(&pool.not_full);

    for (size_t i = 0; i < (size_t)app->workers; i++) {
        if (mnet_thread_create(&pool.threads[i], mnet_worker_main, &pool) != 0) {
            mnet_app_log(app, MNET_LOG_ERROR,
                "failed to start worker %d of %d",
                (int)i, app->workers);
            /* Fall back to serving with the workers that did start. */
            pool.thread_count = i;
            app->workers = (int)i;
            if (i == 0) {
                mnet_pool_shutdown(&pool);
                mnet_close(server);
                return -1;
            }
            break;
        }
        pool.thread_count++;
    }

    mnet_app_log(app, MNET_LOG_INFO, "serving with %d worker threads",
        app->workers);

    while (app->running) {
        mnet_socket_t client = mnet_tcp_accept(server);
        if (client == MNET_INVALID_SOCKET) {
            if (mnet_socket_errno() == MNET_EINTR) continue;
            mnet_app_log(app, MNET_LOG_ERROR,
                "accept failed, stopping the server");
            break;
        }

        mnet_mutex_lock(&pool.mutex);
        int active = app->active_connections;
        mnet_mutex_unlock(&pool.mutex);

        if (app->max_connections > 0 && active >= app->max_connections) {
            mnet_app_log(app, MNET_LOG_WARN,
                "refused a connection: %d active (max_connections)", active);
            send_simple_error(client, 503, "Service Unavailable",
                "The server is at its connection limit.", app->debug);
            mnet_close(client);
            continue;
        }

        mnet_mutex_lock(&pool.mutex);
        app->active_connections++;
        mnet_mutex_unlock(&pool.mutex);

        if (mnet_pool_push(&pool, client) != 0) {
            mnet_mutex_lock(&pool.mutex);
            app->active_connections--;
            mnet_mutex_unlock(&pool.mutex);
            mnet_close(client);
            break;
        }
    }

    mnet_pool_shutdown(&pool);
    mnet_close(server);
    mnet_app_log(app, MNET_LOG_INFO, "server stopped");
    return 0;
}
