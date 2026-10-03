# mnet

A simple, ergonomic Web Framework for C.

## Building

### Using Make (default)

```sh
make              # Build examples and test_mnet
make test         # Build and run tests
make clean        # Remove build artifacts
```

### Using CMake

```sh
mkdir build && cd build
cmake ..
make
ctest             # Run tests
```

### Using Meson

```sh
meson setup build
meson compile -C build
meson test -C build
```

### Linking against mnet

After building, you can link against the static or shared library:

```sh
# Static library
gcc -Iinclude myapp.c build/libmnet.a -o myapp

# Shared library
gcc -Iinclude myapp.c -Lbuild -lmnet -o myapp
```

Or use pkg-config (if installed):
```sh
gcc $(pkg-config --cflags --libs mnet) myapp.c -o myapp
```

## API Overview

### Application lifecycle

```c
mnet_app_t *app = mnet_create();
if (app == NULL) return 1;

mnet_run(app, 8080);

mnet_destroy(app);
```

### Handlers

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

### Route registration

```c
MNET_GET(app, "/", home);
MNET_POST(app, "/api/echo", echo);
MNET_PUT(app, "/api/users/:id", update_user);
MNET_PATCH(app, "/api/users/:id", patch_user);
MNET_DELETE(app, "/api/users/:id", delete_user);
```

### Middleware

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

### Static file serving

```c
mnet_static(app, "/static", "/var/www/files");
```

Serves files from `/var/www/files` under the `/static` URL prefix. Path traversal is prevented via `realpath` checks. Common MIME types are detected from file extensions.

### Socket timeouts

```c
mnet_set_timeout(app, 30);
```

Sets a read/write timeout in seconds on client connections. Without this, a slow or malicious client can hang the server indefinitely.

### Keep-alive connections

The server honors `Connection: keep-alive` from HTTP/1.1 clients and reuses the connection for subsequent requests. Use `mnet_chunked()` for responses where the body length is not known upfront.

### Optional configuration

All configuration options are optional. Call them before `mnet_run()`:

```c
/* Maximum concurrent connections. 0 = unlimited (default). */
mnet_set_max_connections(app, 100);

/* Keep-alive idle timeout in seconds. 0 = no timeout (default). */
mnet_set_keep_alive_timeout(app, 30);

/* Maximum request body size in bytes. 0 = 16 MB (default). */
mnet_set_max_body_size(app, 1024 * 1024); /* 1 MB */
```

### Wildcard routes

Routes ending with `*` match any remaining path. The matched portion is available as a path parameter:

```c
MNET_GET(app, "/static/*", serve_static);
// For /static/css/style.css, MNET_PARAM(req, "*") returns "css/style.css"
```

### Request parameters

**Path parameters** (`:id`, `:post_id`, etc.):

```c
const char *id = MNET_PARAM(req, "id");
```

**Query parameters** (`?search=hello`):

```c
const char *q = MNET_QUERY(req, "search");
```

Path parameters and query values are URL-decoded automatically (`%20` → space, `+` → space, etc.).

**Headers** (case-insensitive):

```c
const char *auth = MNET_HEADER(req, "Authorization");
```

**Cookies**:

```c
const char *session = MNET_COOKIE(req, "session");
```

**Body**:

```c
const char *body = MNET_BODY(req);
size_t body_len = MNET_BODY_LEN(req);
```

The request line and headers are parsed from an 8 KB read buffer, so they must fit
within it. The body is not limited by that buffer: it is read in full into a heap
buffer sized from the `Content-Length` header, up to a configurable cap (16 MB by
default, see `mnet_set_max_body_size()`). Requests declaring a larger body are
rejected.

### Response helpers

```c
return mnet_text("plain text");            // 200, text/plain
return mnet_html("<h1>hi</h1>");           // 200, text/html
return mnet_json("{\"ok\":true}");         // 200, application/json
return mnet_jsonf("{\"id\":\"%s\"}", id);  // 200, application/json (printf-style, %s escaped)
return mnet_error(400, "bad request");     // error status + text body
return mnet_status(201, "created");        // custom status + text body
return mnet_chunked(200, "text/html", html, html_len); // chunked transfer
```

All response helpers allocate the body with `malloc`. During request handling the
server frees the body automatically after sending the response, so handlers do
**not** need to free anything they return. You only need to free manually when you
build a response outside request handling (for example in a test):

```c
mnet_response_t r = mnet_text("hello");
mnet_response_free(&r);
```

`mnet_response_free()` is safe to call on a `NULL` pointer or an already-freed
response, and it is a no-op for chunked responses (which do not own their body).

### Path parameters

Routes like `/api/users/:id` or `/api/posts/:post_id/comments/:comment_id` are supported. The router extracts the parameter values and makes them available via `MNET_PARAM(req, "name")`.

### Request accessors

| Macro | Function |
|-------|----------|
| `MNET_PARAM(req, name)` | `mnet_request_param(req, name)` |
| `MNET_QUERY(req, name)` | `mnet_request_query(req, name)` |
| `MNET_HEADER(req, name)` | `mnet_request_header(req, name)` |
| `MNET_BODY(req)` | `mnet_request_body(req)` |
| `MNET_BODY_LEN(req)` | `mnet_request_body_length(req)` |

The `mnet_request_t` struct includes an `extras` field (a `request_extras_t *`) that holds the underlying name/value arrays for path params, query params, and headers. This is managed internally by the framework and can be ignored in normal handler code.

### HTTP methods

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

`HEAD` requests return headers with no body. `OPTIONS` is useful for CORS preflight.

## Running the examples

### Basic HTTP server

```sh
./example/example_http_server
```

Visit `http://localhost:8080/` to see the home page with links to other routes.

### REST API server

```sh
./example/example_api_server
```

Test the API with curl:
```sh
curl http://localhost:8080/api/items
curl http://localhost:8080/api/items/0
curl -X POST http://localhost:8080/api/echo -d "hello world"
curl http://localhost:8080/api/search?q=One
```

### Combined HTTP + API server

```sh
./example/example_combined
```

A full-featured example with both HTML pages and a JSON API, including authentication via headers.

## Testing

```sh
make test
```

The suite is a single self-contained harness in `test/test_mnet.c` (38 cases) and
covers routing, path and query parameters, headers, cookies, body parsing, JSON
escaping, chunked responses, response-free paths, and the configuration limits.
It runs clean under Valgrind. It does not currently drive a real socket end to
end, so HTTP-level integration behaviour is not covered.

The same suite runs in CI against Make, CMake, and Meson, on Linux, macOS, and
Windows.

## Makefile targets

| Target | Description |
|--------|-------------|
| `make examples` | Build all example binaries |
| `make test` | Build and run the test suite |
| `make clean` | Remove all built binaries |
| `make help` | Show available targets |
| `make <name>` | Build `<name>.c` linked with mnet (e.g. `make main`) |

## Requirements

- A C17 compiler (GCC, Clang, or MSVC)
- Linux, macOS, BSD, or Windows
- No external dependencies

## Security considerations

mnet is a small framework and leaves several operational concerns to the caller.
If you expose a server to a network you do not fully trust, read this section.

**Set a timeout.** I/O is blocking and the server is single-threaded: it handles
one connection at a time. A client that connects and then sends nothing holds the
server for as long as the socket stays open. Call `mnet_set_timeout(app, seconds)`
to bound that. There is no timeout by default.

**Cap concurrent connections.** `mnet_set_max_connections(app, n)` rejects new
connections once `n` are active. Without it there is no limit. Note that this is a
simple counter, not per-IP rate limiting — it does not distinguish one abusive
client from many legitimate ones.

**There is no TLS.** mnet speaks plaintext HTTP. Terminate TLS in a reverse proxy
(nginx, Caddy, stunnel) in front of it if you need HTTPS.

**Header size is bounded by the read buffer.** The request line and headers must
fit in the 8 KB read buffer. A request whose headers exceed it fails to parse and
the connection is closed. This is an implicit limit, not a configurable one; keep
that in mind if you expect very large cookies or a long list of headers.

**Body size is capped.** Request bodies are limited to 16 MB by default and
rejected above that; adjust with `mnet_set_max_body_size()`. Bodies are allocated
on the heap, so the cap also bounds per-request memory use.

**Static file serving is traversal-checked.** `mnet_static()` resolves the
requested path with `realpath()` and verifies it stays under the configured root,
rejecting escapes with 403. Do not serve a directory whose contents you would not
expose.

**Handlers run on a single thread.** Do not block inside a handler — a slow
handler stalls every other client.
