# mnet - A Simple, Ergonomic Web Framework for C

## The Idea

mnet is a minimal, single-file-style HTTP web framework for C that prioritizes ergonomics and simplicity. It lets you build web servers and REST APIs in pure C without the boilerplate typically associated with socket programming and HTTP parsing.

The core philosophy: **web development in C should feel almost as clean as in higher-level languages**. Handlers are declared with a macro, routes are registered with one-liners, and responses are constructed with simple builder functions - all while staying close to the metal.

## What mnet provides

### Core socket layer
- `mnet_tcp_listen()` / `mnet_tcp_accept()` - TCP server setup
- `mnet_send()` / `mnet_recv()` - safe wrappers around send/recv
- `mnet_close()` - cleanup

### HTTP application framework
- `mnet_create()` / `mnet_run()` / `mnet_destroy()` - application lifecycle
- `mnet_stop()` - graceful shutdown
- `MNET_HANDLER(name)` - declare handlers with clean syntax
- `MNET_GET`, `MNET_POST`, `MNET_PUT`, `MNET_PATCH`, `MNET_DELETE`, `MNET_HEAD`, `MNET_OPTIONS` - route registration macros
- Path parameters (`/api/users/:id`), query strings, headers, body access
- URL decoding for path params and query values
- Cookie parsing (`MNET_COOKIE(req, "session")`)
- Wildcard routes (`/static/*`)
- Static file serving (`mnet_static(app, "/static", "/var/www/files")`)
- Middleware support (`mnet_use(app, middleware_fn)`)
- Keep-alive connections and chunked transfer encoding

### Response helpers
- `mnet_text()`, `mnet_html()`, `mnet_json()`, `mnet_jsonf()` - response builders
- `mnet_error()`, `mnet_status()` - error and custom-status responses
- `mnet_chunked()` - chunked transfer encoding
- `mnet_response_free()` - free response body memory
- `mnet_json_escape()` - safe JSON string escaping

### Request accessors
- `MNET_PARAM(req, "id")` - path parameters
- `MNET_QUERY(req, "search")` - query parameters  
- `MNET_HEADER(req, "Authorization")` - headers (case-insensitive)
- `MNET_BODY(req)`, `MNET_BODY_LEN(req)` - request body
- `MNET_COOKIE(req, "session")` - cookies

### Optional configuration
- `mnet_set_max_connections(app, 100)` - limit concurrent connections (0 = unlimited)
- `mnet_set_keep_alive_timeout(app, 30)` - keep-alive idle timeout (0 = no timeout)
- `mnet_set_max_body_size(app, 1024*1024)` - max request body size (0 = 16 MB default)
- `mnet_set_timeout(app, 30)` - socket timeout in seconds (0 = no timeout)
- `mnet_set_debug(app, 1)` - enable debug logging

### Build systems
- Make (original)
- CMake
- Meson
- Shared library support (`libmnet.so`)

## Design decisions

- **Threaded worker pool by default** - 4 workers serve multiple clients
  concurrently; pass `1` to `mnet_set_workers()` for the single-threaded
  blocking loop (no synchronisation overhead), or `0` for the default.
- **Cross-platform** - Linux, macOS, BSD, and Windows (POSIX sockets and
  Win32 APIs under a thin compatibility layer); no external dependencies.
- **C17** - modern C, compiles with `-Wall -Wextra -Wpedantic -Werror`.
- **Macros for ergonomics** - `MNET_HANDLER` and route macros reduce boilerplate.
- **Clean, modern API** - the legacy `mnet_handler_legacy_t` handler type was
  removed in v0.2.1; only the modern `mnet_handler_t` return-value style is
  supported.
- **Memory-safe** - all response bodies are freed with `mnet_response_free()`.
