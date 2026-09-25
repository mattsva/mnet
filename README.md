# mnet

A simple, ergonomic Web Framework for C.

## Building

```sh
make
```

This builds `mnet-server` (the main example) and `test_mnet` (the test runner).

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

### Request parameters

**Path parameters** (`:id`, `:post_id`, etc.):

```c
const char *id = MNET_PARAM(req, "id");
```

**Query parameters** (`?search=hello`):

```c
const char *q = MNET_QUERY(req, "search");
```

**Headers** (case-insensitive):

```c
const char *auth = MNET_HEADER(req, "Authorization");
```

**Body**:

```c
const char *body = MNET_BODY(req);
size_t body_len = MNET_BODY_LEN(req);
```

### Response helpers

```c
return mnet_text("plain text");            // 200, text/plain
return mnet_html("<h1>hi</h1>");           // 200, text/html
return mnet_json("{\"ok\":true}");         // 200, application/json
return mnet_jsonf("{\"id\":\"%s\"}", id);  // 200, application/json (printf-style, %s escaped)
return mnet_error(400, "bad request");     // error status + text body
return mnet_status(201, "created");        // custom status + text body
```

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

### HTTP methods

```c
typedef enum {
    MNET_HTTP_GET,
    MNET_HTTP_POST,
    MNET_HTTP_PUT,
    MNET_HTTP_PATCH,
    MNET_HTTP_DELETE
} mnet_http_method_t;
```

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

## Makefile targets

| Target | Description |
|--------|-------------|
| `make examples` | Build all example binaries |
| `make test` | Build and run the test suite |
| `make clean` | Remove all built binaries |
| `make help` | Show available targets |
| `make <name>` | Build `<name>.c` linked with mnet (e.g. `make main`) |

## Requirements

- GCC with C17 support
- POSIX system (Linux, macOS, BSD)
- No external dependencies
