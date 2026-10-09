# mnet API Reference

This document describes the public API of mnet.

## Application lifecycle

```c
mnet_app_t *mnet_create(void);
void mnet_destroy(mnet_app_t *app);
int mnet_run(mnet_app_t *app, uint16_t port);
int mnet_stop(mnet_app_t *app);
```

### mnet_create

Create a new application.

```c
mnet_app_t *app = mnet_create();
if (app == NULL) return 1;
```

### mnet_destroy

Destroy an application. Frees all resources.

```c
mnet_destroy(app);
```

### mnet_run

Run the server. This function blocks until the server is stopped.

```c
mnet_run(app, 8080);
```

### mnet_stop

Stop the server gracefully.

```c
mnet_stop(app);
```

## Handlers

Handlers use the `MNET_HANDLER` macro and return a response directly:

```c
MNET_HANDLER(home)
{
    return mnet_html("<h1>Hello World!</h1>");
}
```

`MNET_HANDLER(name)` expands to a static function:

```c
static mnet_response_t name(mnet_request_t *req);
```

## Route registration

```c
MNET_GET(app, "/", home);
MNET_POST(app, "/api/echo", echo);
MNET_PUT(app, "/api/users/:id", update_user);
MNET_PATCH(app, "/api/users/:id", patch_user);
MNET_DELETE(app, "/api/users/:id", delete_user);
```

Route registration macros:

```c
#define MNET_GET(app, path, handler)     mnet_route((app), MNET_HTTP_GET, (path), (handler))
#define MNET_POST(app, path, handler)    mnet_route((app), MNET_HTTP_POST, (path), (handler))
#define MNET_PUT(app, path, handler)     mnet_route((app), MNET_HTTP_PUT, (path), (handler))
#define MNET_PATCH(app, path, handler)   mnet_route((app), MNET_HTTP_PATCH, (path), (handler))
#define MNET_DELETE(app, path, handler)  mnet_route((app), MNET_HTTP_DELETE, (path), (handler))
#define MNET_HEAD(app, path, handler)    mnet_route((app), MNET_HTTP_HEAD, (path), (handler))
#define MNET_OPTIONS(app, path, handler) mnet_route((app), MNET_HTTP_OPTIONS, (path), (handler))
```

## Request accessors

```c
const char *MNET_PARAM(req, name);        // Path parameters (:id)
const char *MNET_QUERY(req, name);         // Query parameters (?search=hello)
const char *MNET_HEADER(req, name);        // Headers (case-insensitive)
const char *MNET_BODY(req);                // Request body
size_t MNET_BODY_LEN(req);                 // Request body length
const char *MNET_COOKIE(req, name);        // Cookies
```

## Response helpers

```c
mnet_response_t mnet_text(const char *text);
mnet_response_t mnet_html(const char *html);
mnet_response_t mnet_json(const char *json);
mnet_response_t mnet_jsonf(const char *format, ...);
mnet_response_t mnet_error(int status, const char *message);
mnet_response_t mnet_status(int status, const char *body);
mnet_response_t mnet_chunked(int status, const char *content_type, const void *body, size_t body_length);
void mnet_response_free(mnet_response_t *response);
```

All response helpers allocate the body with `malloc`. During request handling the
server frees the body automatically after sending the response, so handlers do
**not** need to free anything they return. You only need to free manually when
you build a response outside request handling (for example in a test):

```c
mnet_response_t r = mnet_text("hello");
mnet_response_free(&r);
```

`mnet_response_free()` is safe to call on a `NULL` pointer or an already-freed
response, and it is a no-op for chunked responses (which do not own their body).

## Middleware

```c
mnet_response_t my_middleware(mnet_request_t *req,
    mnet_response_t (*next)(mnet_request_t *))
{
    // pre-processing
    mnet_response_t resp = next(req);
    // post-processing
    return resp;
}

mnet_use(app, my_middleware);
```

## Static file serving

```c
mnet_static(app, "/static", "/var/www/files");
```

Serves files from `/var/www/files` under the `/static` URL prefix. Path traversal
is prevented via `realpath` checks. Common MIME types are detected from file
extensions.

## Optional configuration

```c
/* Number of worker threads. 0 = default (4), 1 = single-threaded. */
mnet_set_workers(app, 4);

/* Maximum concurrent connections. 0 = unlimited (default). */
mnet_set_max_connections(app, 100);

/* Keep-alive idle timeout in seconds. 0 = 5 s default. */
mnet_set_keep_alive_timeout(app, 30);

/* Maximum request body size in bytes. 0 = 16 MB (default). */
mnet_set_max_body_size(app, 1024 * 1024); /* 1 MB */

/* Socket read/write timeout in seconds for client connections. */
mnet_set_timeout(app, 30);

/* Enable debug logging per app. */
mnet_set_debug(app, 1);

/* Custom 404 handler. */
mnet_set_not_found_handler(app, my_not_found_handler);

/* Log handler. */
mnet_set_log_handler(app, my_log_handler);
```

### Worker count

The server is threaded by default with 4 workers. Each accepted connection is
queued and picked up by a fixed pool of worker threads, so multiple clients are
served concurrently. Pass `1` to `mnet_set_workers()` for the single-threaded
blocking loop (no synchronisation overhead), or `0` to restore the default.

```c
mnet_set_workers(app, 4);
```

### Max connections

Maximum number of concurrent connections. `0` means unlimited.

```c
mnet_set_max_connections(app, 100);
```

### Keep-alive timeout

Keep-alive idle timeout in seconds. `0` means 5 seconds.

```c
mnet_set_keep_alive_timeout(app, 30);
```

### Max body size

Maximum request body size in bytes. `0` means 16 MB.

```c
mnet_set_max_body_size(app, 1024 * 1024); /* 1 MB */
```

### Timeout

Socket read/write timeout in seconds for client connections. A bounded timeout
is what stops a client from holding a connection open forever without sending
anything (Slowloris). The default is 30 seconds.

```c
mnet_set_timeout(app, 30);
```

### Debug

Enable debug logging for a specific application.

```c
mnet_set_debug(app, 1);
```

### Not found handler

Custom 404 handler.

```c
mnet_set_not_found_handler(app, my_not_found_handler);
```

### Log handler

Set the log callback. Without one, messages go to stderr.

```c
mnet_set_log_handler(app, my_log_handler);
```

## Logging

```c
static void my_log(int level, const char *fmt, ...)
{
    /* ... */
}

mnet_set_log_handler(app, my_log);
```

Levels are `MNET_LOG_ERROR`, `MNET_LOG_WARN`, `MNET_LOG_INFO` and
`MNET_LOG_DEBUG`. Rejected requests (oversized headers, unsupported methods,
malformed request lines, path traversal, connection-limit refusals) and
listener failures are all reported.

## HTTP methods

```c
typedef enum {
    MNET_HTTP_GET,
    MNET_HTTP_POST,
    MNET_HTTP_PUT,
    MNET_HTTP_PATCH,
    MNET_HTTP_DELETE,
    MNET_HTTP_HEAD,
    MNET_HTTP_OPTIONS
} mnet_http_method_t;
```

`HEAD` requests return headers with no body. `OPTIONS` is useful for CORS
preflight.
