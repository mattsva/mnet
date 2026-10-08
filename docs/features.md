# mnet Features

This document describes all features of mnet and how they work.

## Table of contents

- [Route registration](#route-registration)
- [Path parameters](#path-parameters)
- [Query parameters](#query-parameters)
- [Headers](#headers)
- [Cookies](#cookies)
- [Request body](#request-body)
- [Response helpers](#response-helpers)
- [Middleware](#middleware)
- [Static file serving](#static-file-serving)
- [Wildcard routes](#wildcard-routes)
- [Keep-alive connections](#keep-alive-connections)
- [Chunked transfer encoding](#chunked-transfer-encoding)
- [HTTP methods](#http-methods)
- [Multithreading](#multithreading)
- [MNET_CALL (client-side HTTP)](#mnet_call-client-side-http)
- [Configuration](#configuration)

## Route registration

Routes are registered with one-liners:

```c
MNET_GET(app, "/", home);
MNET_POST(app, "/api/echo", echo);
MNET_PUT(app, "/api/users/:id", update_user);
MNET_PATCH(app, "/api/users/:id", patch_user);
MNET_DELETE(app, "/api/users/:id", delete_user);
```

Each route maps an HTTP method and a path pattern to a handler function.

## Path parameters

Routes like `/api/users/:id` or `/api/posts/:post_id/comments/:comment_id` are
supported. The router extracts the parameter values and makes them available via
`MNET_PARAM(req, "name")`.

```c
MNET_GET(app, "/api/users/:id", get_user);

MNET_HANDLER(get_user)
{
    const char *id = MNET_PARAM(req, "id");
    return mnet_jsonf("{\"id\":%s}", id);
}
```

Path parameters and query values are URL-decoded automatically (`%20` → space,
`+` → space, etc.).

## Query parameters

Query parameters are available via `MNET_QUERY(req, "name")`.

```c
MNET_GET(app, "/api/search", search);

MNET_HANDLER(search)
{
    const char *q = MNET_QUERY(req, "q");
    return mnet_jsonf("{\"q\":\"%s\"}", q ? q : "");
}
```

## Headers

Headers are available via `MNET_HEADER(req, "name")`. Header names are
case-insensitive.

```c
MNET_HANDLER(protected)
{
    const char *auth = MNET_HEADER(req, "Authorization");
    if (auth == NULL || strcmp(auth, "Bearer secret") != 0) {
        return mnet_error(401, "unauthorized");
    }
    return mnet_text("access granted");
}
```

## Cookies

Cookies are available via `MNET_COOKIE(req, "name")`.

```c
MNET_HANDLER(cookie_test)
{
    const char *session = MNET_COOKIE(req, "session");
    return mnet_jsonf("{\"session\":%s}", session ? session : "none");
}
```

## Request body

The request body is available via `MNET_BODY(req)` and `MNET_BODY_LEN(req)`.

```c
MNET_HANDLER(echo)
{
    const char *body = MNET_BODY(req);
    size_t len = MNET_BODY_LEN(req);
    return mnet_text(body ? body : "");
}
```

The body is read in full into a heap buffer sized from the `Content-Length`
header, up to a configurable cap (16 MB by default, see
`mnet_set_max_body_size()`). Requests declaring a larger body are rejected with
`413`.

## Response helpers

mnet provides several response helpers:

```c
mnet_text("plain text");           // 200, text/plain
mnet_html("<h1>hi</h1>");          // 200, text/html
mnet_json("{\"ok\":true}");         // 200, application/json
mnet_jsonf("{\"id\":\"%s\"}", id);   // 200, application/json (printf-style, %s escaped)
mnet_error(400, "bad request");    // 400, text/plain
mnet_status(201, "created");       // 201, text/plain
mnet_chunked(200, "text/html", body, len); // chunked transfer
```

All response helpers allocate the body with `malloc`. During request handling
the server frees the body automatically after sending the response, so handlers
do **not** need to free anything they return. You only need to free manually when
you build a response outside request handling (for example in a test):

```c
mnet_response_t r = mnet_text("hello");
mnet_response_free(&r);
```

`mnet_response_free()` is safe to call on a `NULL` pointer or an already-freed
response, and it is a no-op for chunked responses (which do not own their body).

## Middleware

Middleware wraps handlers and can inspect or modify requests and responses:

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

Static files are served via `mnet_static(app, url_prefix, fs_path)`.

```c
mnet_static(app, "/static", "/var/www/files");
```

Serves files from `/var/www/files` under the `/static` URL prefix. Path
traversal is prevented via `realpath` checks. Common MIME types are detected
from file extensions.

Path resolution:
- The configured root and the requested path are both resolved with
  `realpath()` (POSIX) / `_fullpath()` (Windows) before the file is opened.
- The resolved target must equal the resolved root or lie beneath it, compared
  on a path-separator boundary so that a sibling such as `/var/www2` is not
  accepted as being inside `/var/www`.
- Anything else is refused with `403` and a traversal attempt is logged.
- A path that cannot be resolved (including a missing file) yields `404`.

Because the check happens on the resolved path and the file is opened by that
resolved path, a symlink that leaves the root is rejected too.

## Wildcard routes

Routes ending with `*` match any remaining path. The matched portion is
available as a path parameter:

```c
MNET_GET(app, "/static/*", serve_static);

MNET_HANDLER(serve_static)
{
    const char *p = MNET_PARAM(req, "*");
    return mnet_text(p ? p : "");
}
```

## Keep-alive connections

The server honors `Connection: keep-alive` from HTTP/1.1 clients and reuses the
connection for subsequent requests. Use `mnet_chunked()` for responses where the
body length is not known upfront.

```c
mnet_set_keep_alive_timeout(app, 30);
```

## Chunked transfer encoding

Use `mnet_chunked()` for responses where the body length is not known upfront:

```c
MNET_HANDLER(stream)
{
    return mnet_chunked(200, "text/plain", stream_data(), stream_len());
}
```

The server will send the response as a series of chunks, each prefixed with its
length in hexadecimal.

## HTTP methods

mnet supports the following HTTP methods:

```c
typedef enum {
    MNET_HTTP_GET,      // 0
    MNET_HTTP_POST,     // 1
    MNET_HTTP_PUT,      // 2
    MNET_HTTP_PATCH,    // 3
    MNET_HTTP_DELETE,   // 4
    MNET_HTTP_HEAD,     // 5
    MNET_HTTP_OPTIONS,  // 6
} mnet_http_method_t;
```

`HEAD` requests return headers with no body. `OPTIONS` is useful for CORS
preflight.

## Multithreading

The server is threaded by default with 4 workers. Each accepted connection is
queued and picked up by a fixed pool of worker threads, so multiple clients are
served concurrently.

```c
mnet_set_workers(app, 4);   // default
mnet_set_workers(app, 1);   // single-threaded blocking loop
mnet_set_workers(app, 0);   // restore default (4)
```

### How the worker pool works

1. The `mnet_run()` function creates a bounded queue and a pool of worker threads.
2. The accept loop accepts each incoming connection and pushes it onto the queue.
3. Each worker thread pops a connection from the queue, serves it to
   completion (including keep-alive requests), and loops.
4. All shared state (the queue, the active connection counter, the shutdown flag)
   is guarded by a mutex.
5. When the server stops, the pool is shut down: all threads are joined and all
   queued connections are closed.

### Worker count configuration

```c
/* 0 = default (4 workers) */
mnet_set_workers(app, 0);

/* 1 = single-threaded (no synchronisation overhead) */
mnet_set_workers(app, 1);

/* n = use n worker threads */
mnet_set_workers(app, 8);
```

## MNET_CALL (client-side HTTP)

`MNET_CALL` makes an HTTP request to a URL and returns the response body as a
string. It is a convenience wrapper around the mnet client functionality.

```c
MNET_HANDLER(fetch_google)
{
    (void)req;
    return mnet_json(MNET_CALL("https://google.com"));
}
```

`MNET_CALL` accepts URLs with or without a scheme:

```c
/* With scheme */
mnet_json(MNET_CALL("https://google.com"));

/* Without scheme (http:// is assumed) */
mnet_json(MNET_CALL("google.com"));
```

### How MNET_CALL works

1. The URL is parsed to extract the scheme, host, port, and path.
2. A TCP connection is established to the host.
3. An HTTP GET request is sent with the `Host` and `User-Agent` headers.
4. The response is read (headers and body) with a bounded timeout.
5. The response body is returned as a heap-allocated string.
6. The response and connection are freed.

### Response handling

The response body is returned as a heap-allocated string. You must free it with
`mnet_free()` (or `free()`) when you are done:

```c
char *content = (char *)MNET_CALL("https://example.com");
if (content != NULL) {
    printf("Content: %s\n", content);
    free(content);
}
```

### Error handling

If the request fails (DNS resolution, connection refused, invalid URL, etc.),
`MNET_CALL` returns `NULL`. Always check the return value:

```c
char *content = (char *)MNET_CALL("https://example.com");
if (content == NULL) {
    // Request failed - handle error
    return mnet_error(500, "request failed");
}
```

### Timeout

The default timeout is 30 seconds. You can configure the timeout per application:

```c
mnet_set_timeout(app, 10);  // 10 seconds
```

### Limits

The default maximum response body size is 16 MB. Requests with a larger body are
rejected with `NULL` (and an error is logged). You can change the limit with:

```c
mnet_set_max_body_size(app, 1024 * 1024); /* 1 MB */
```

## Configuration

mnet is designed to be configurable. All configuration options are optional.
Call them before `mnet_run()`.

```c
/* Number of worker threads. 0 = default (4), 1 = single-threaded. */
mnet_set_workers(app, 4);

/* Maximum concurrent connections. 0 = unlimited (default). */
mnet_set_max_connections(app, 100);

/* Keep-alive idle timeout in seconds. 0 = 30 s default. */
mnet_set_keep_alive_timeout(app, 30);

/* Maximum request body size in bytes. 0 = 16 MB (default). */
mnet_set_max_body_size(app, 1024 * 1024); /* 1 MB */

/* Socket read/write timeout in seconds for client connections. */
mnet_set_timeout(app, 30);

/* Enable debug logging. */
mnet_set_debug(app, 1);

/* Custom 404 handler. */
mnet_set_not_found_handler(app, my_not_found_handler);
```

### Worker count and CPU cores

The server uses a thread pool. The default is 4 workers. To use all available
CPU cores, you can query the system and set the worker count accordingly:

```c
#include <unistd.h>

mnet_app_t *app = mnet_create();
mnet_set_workers(app, sysconf(_SC_NPROCESSORS_ONLN));
```

### Memory configuration

You can configure how much memory the server allocates per connection:

```c
/* Maximum request body size in bytes. 0 = 16 MB (default). */
mnet_set_max_body_size(app, 1024 * 1024); /* 1 MB */

/* Maximum concurrent connections. 0 = unlimited (default). */
mnet_set_max_connections(app, 100);
```

### Keep-alive timeout

Keep-alive idle timeout in seconds. `0` means 30 seconds.

```c
mnet_set_keep_alive_timeout(app, 30);
```

### Timeout

Socket read/write timeout in seconds for client connections. A bounded timeout
is what stops a client from holding a connection open forever without sending
anything (Slowloris). The default is 30 seconds.

```c
mnet_set_timeout(app, 30);
```
