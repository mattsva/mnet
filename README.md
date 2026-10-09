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

### Concurrency

```c
mnet_set_workers(app, 4);
```

The server is threaded by default with 4 workers. Each accepted connection is
queued and picked up by a fixed pool of worker threads, so multiple clients are
served concurrently. Pass `1` to `mnet_set_workers()` for the single-threaded
blocking loop (no synchronisation overhead), or `0` to restore the default.

Handlers then run on several threads at once, so any state they share must be
synchronised by the application. Route registration is not affected: routes are
read-only once `mnet_run()` starts.

### Logging

```c
static void my_log(int level, const char *fmt, ...)
{
    /* ... */
}

mnet_set_log_handler(app, my_log);
```

Messages are reported through the handler if one is set, otherwise to `stderr`.
Levels are `MNET_LOG_ERROR`, `MNET_LOG_WARN`, `MNET_LOG_INFO` and
`MNET_LOG_DEBUG`. Rejected requests (oversized headers, unsupported methods,
malformed request lines, path traversal, connection-limit refusals) and
listener failures are all reported.

### Socket timeouts

```c
mnet_set_timeout(app, 30);
```

Sets a read/write timeout in seconds on client connections. Without this, a slow
or malicious client can hang the server indefinitely. A 30 second timeout is
applied by default even if this is never called; pass a value to override it, or
`0` to fall back to the default. A negative value disables the timeout, which is
strongly discouraged in production.

### Keep-alive connections

The server honors `Connection: keep-alive` from HTTP/1.1 clients and reuses the connection for subsequent requests. Use `mnet_chunked()` for responses where the body length is not known upfront.

### Optional configuration

All configuration options are optional. Call them before `mnet_run()`:

```c
/* Number of worker threads. 0 = default (4), 1 = single-threaded. */
mnet_set_workers(app, 4);

/* Maximum concurrent connections. 0 = unlimited (default). */
mnet_set_max_connections(app, 100);

/* Keep-alive idle timeout in seconds. 0 = 30 s default. */
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

The suite has eight parts:

- `test/test_mnet.c` (43 cases) covers the pure functions: routing, path and
  query parameters, headers, cookies, JSON escaping, URL decoding, the header
  validators, chunked responses, response-free paths and the configuration
  limits.
- `test/test_http.c` (13 cases) drives the real server over a loopback socket:
  request parsing, method validation, body bounds, header limits and static-file
  handling including traversal attempts.
- `test/test_mnet_parser.c` (17 cases) targets the parser's failure modes and
  the worker pool: oversized headers, a single over-long header line, too many
  headers, missing CRLF, malformed request lines and query strings, traversal
  variants, an incomplete body, and several concurrent slow clients.
- `test/test_stress.c` (5 cases) hammers the server with 32 concurrent clients
  and 1280 mixed requests, verifies keep-alive, and confirms the default worker
  pool actually serves requests concurrently rather than serially.
- `test/test_features.c` (24 cases) is one end-to-end check per documented
  feature: methods, HEAD, wildcards, middleware, the custom 404 handler,
  cookies, encoded parameters, response helpers, and the body/header
  boundaries.
- `test/test_security.c` (61 check cases) is the regression suite for the
  security fixes: `mnet_jsonf` format-string hardening, header/body scoping,
  Content-Length/Transfer-Encoding handling, SIGPIPE survival, and Slowloris /
  idle / slow-body timeouts. The jsonf half runs everywhere; the server half is
  POSIX-only.
- `test/test_client.c` (6 cases) covers the client-side HTTP API: invalid URLs,
  a request with an explicit port and path (body returned with headers
  stripped), 4xx responses returning NULL, and the async callback.
- `test/test_welcome.c` (4 cases) covers the dev mode and welcome page.

All suites run clean under Valgrind, AddressSanitizer and ThreadSanitizer, and
the same suites run in CI against Make, CMake and Meson on Linux, macOS and
Windows.

`test/fuzz_http.c` is a libFuzzer/AFL harness that drives the full request path
over a socket. Note that because the server runs in a forked child, the fuzzer's
coverage instrumentation only observes the client side, so it acts as a crash
and sanitizer oracle over the parser rather than a coverage-guided fuzzer.

## Makefile targets

| Target | Description |
|--------|-------------|
| `make examples` | Build all example binaries |
| `make libmnet.a` | Build the static library |
| `make libmnet.so` | Build the shared library |
| `make test` | Build and run all test suites |
| `make build FILE=prog SRC=main.c` | Build a single program linked with mnet |
| `make clean` | Remove all built binaries |
| `make help` | Show available targets |

## Requirements

- A C17 compiler (GCC, Clang, or MSVC)
- Linux, macOS, BSD, or Windows
- Threads: pthreads on POSIX, the Win32 thread API on Windows
- No other external dependencies

## Security considerations

mnet is a small framework and leaves several operational concerns to the caller.
If you expose a server to a network you do not fully trust, read this section.

**Timeouts have a default.** I/O is blocking, so a client that connects and then
sends nothing would otherwise hold its connection indefinitely. A 30 second
timeout is applied by default. `mnet_set_timeout(app, seconds)` overrides it (use
`0` to get the default back), and `mnet_set_keep_alive_timeout()` sets the
separate idle timeout for reused keep-alive connections. Setting a negative
timeout disables the protection and is strongly discouraged in production.

**Choose a worker count deliberately.** The server is threaded by default (4
workers), so multiple clients are served concurrently. Handlers then run on
several threads at once, so anything they share must be synchronised by the
application. Pass `1` to `mnet_set_workers()` for the single-threaded loop,
which has no synchronisation overhead but handles one connection at a time.
Neither mode protects against a slow request body: the timeout bounds how long
a client may stall, not how much work it may ask for.

**Cap concurrent connections.** `mnet_set_max_connections(app, n)` refuses new
connections with `503` once `n` are active. Without it there is no limit. Note
that this is a simple counter, not per-IP rate limiting — it does not distinguish
one abusive client from many legitimate ones.

**HTTPS is not implemented.** The `mnet_set_https(app, 1)` flag only controls
the HTTPS state reported on the welcome page; it does not add TLS. Terminate TLS
in a reverse proxy (nginx, Caddy, stunnel) in front of mnet.

**Enable development mode.** `mnet_set_dev_mode(app, 1)` enables a built-in
welcome page. When no routes are registered and dev mode is on, mnet serves a
welcome page at `/` and `/index.html` that shows the current development state
(true/false), the current HTTPS state (https/http), and links to the GitHub
README and to `docs/clib.md` for further setup.

**Header size is bounded.** The request line and headers must fit in the 8 KB read
buffer. A request whose headers do not fit is rejected with `431` and the
connection is closed; headers are never silently truncated, and a header block
larger than the buffer is not parsed as if it were complete. Individual header
lines are capped at 4 KB and the header count at 100; a line whose name is not a
valid RFC 7230 token is skipped rather than stored. Query parameter names and
values are capped at 1 KB each, and a request exceeding either cap is rejected
rather than truncated. These limits are fixed rather than configurable.

**Body size is capped.** Request bodies are limited to 16 MB by default and
rejected above that with `413`; adjust with `mnet_set_max_body_size()`. A
`Content-Length` that is not a plain decimal number within the limit is rejected
with `400`, and any body bytes beyond the declared length are ignored rather than
copied. Bodies are allocated on the heap, so the cap also bounds per-request
memory use.

**Malformed requests are answered, not ignored.** An unparseable request line
yields `400`, an unknown method yields `405`, and a body over the cap yields
`413`.

**Response headers cannot be injected.** A `Content-Type` containing CR or LF is
rejected rather than emitted, so a handler cannot split the response. If you add
your own header-emitting code, validate values with
`mnet_header_value_valid()` and names with `mnet_header_name_valid()`.

**Static file serving is traversal-checked.** `mnet_static()` resolves the
requested path with `realpath()` and verifies it stays under the configured root.
rejecting escapes with `403`. The check is boundary-aware, so a sibling directory
whose name merely shares a prefix with the root (for example `/var/www2` when the
root is `/var/www`) is not reachable. Symlinks are resolved before the check, so
a symlink that leaves the root is rejected too. Do not serve a directory whose
contents you would not expose.

**URL decoding is strict.** `mnet_url_decode_ex()` (and its convenience wrapper
`mnet_url_decode_safe()`) rejects malformed or truncated percent escapes and
`%00` instead of truncating, and never writes past the destination capacity. Path
parameters and query values that fail to decode are reported as empty rather than
partially decoded.

**Handlers must not block.** A slow handler occupies its worker for as long as it
runs. With one worker that stalls every other client; with several it still ties
up a slot.
