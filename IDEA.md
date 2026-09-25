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
- `MNET_GET`, `MNET_POST`, `MNET_PUT`, `MNET_PATCH`, `MNET_DELETE` - route registration macros
- Path parameters (`/api/users/:id`), query strings, headers, body access

### Response helpers
- `mnet_text()`, `mnet_html()`, `mnet_json()`, `mnet_jsonf()` - response builders
- `mnet_error()`, `mnet_status()` - error and custom-status responses
- `mnet_json_escape()` - safe JSON string escaping

### Request accessors
- `MNET_PARAM(req, "id")` - path parameters
- `MNET_QUERY(req, "search")` - query parameters  
- `MNET_HEADER(req, "Authorization")` - headers (case-insensitive)
- `MNET_BODY(req)`, `MNET_BODY_LEN(req)` - request body

## Design decisions

- **Single-threaded, synchronous** - simple and predictable, no async complexity
- **POSIX-only** - uses standard POSIX sockets, no external dependencies
- **C17** - modern C, compiles with `-Wall -Wextra -Wpedantic -Werror`
- **Macros for ergonomics** - `MNET_HANDLER` and route macros reduce boilerplate
- **No legacy baggage** - clean, modern API only
