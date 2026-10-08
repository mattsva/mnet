# clib.md - Welcome Page & C Library Documentation

This document describes the built-in welcome page and how to use the mnet C library.

## Built-in Welcome Page

When the server starts with **development mode enabled** (`mnet_set_dev_mode(app, 1)`)
and **no routes are defined**, mnet serves a built-in welcome page.

### Welcome Page Content

The welcome page displays:

- A congratulations message
- A link to the GitHub README
- A link to this documentation (clib.md)
- **Development state**: `true` or `false`
- **HTTPS state**: `https` or `http`

### When the Welcome Page is Shown

The welcome page is served when:

1. `mnet_set_dev_mode(app, 1)` is called
2. No routes have been registered with `mnet_route()` or the `MNET_GET()`/`MNET_POST()` macros

If any routes are defined, the welcome page is NOT shown, and the server behaves normally
(404 for unknown paths, unless you set a custom `not_found_handler`).

### Example

```c
#include <mnet.h>

int main(void) {
    mnet_app_t *app = mnet_create();
    mnet_set_dev_mode(app, 1);   // Enable welcome page
    mnet_set_https(app, 1);      // Enable HTTPS
    mnet_set_workers(app, 4);    // Worker pool
    mnet_run(app, 8080);
    mnet_destroy(app);
    return 0;
}
```

When accessing `http://localhost:8080/` with no routes defined:

```
HTTP/1.1 200 OK
Content-Type: text/html

<!DOCTYPE html>
<html>
<head><title>mnet Server</title></head>
<body>
  <h1>Congratulations!</h1>
  <p>You successfully started the mnet server.</p>
  <p>Go to <a href="https://github.com/mattsva/mnet/blob/main/README.md">README.md</a>
     for more information about MNET.</p>
  <p>Go to <a href="https://github.com/mattsva/mnet/blob/main/docs/clib.md">clib.md</a>
     for further setup.</p>
  <p>Development state: true</p>
  <p>Current https state: https</p>
</body>
</html>
```

## C Library Overview

### Application Lifecycle

```c
mnet_app_t *app = mnet_create();
mnet_route(app, MNET_HTTP_GET, "/", my_handler);
mnet_run(app, 8080);
mnet_destroy(app);
```

### Route Registration

```c
mnet_route(app, MNET_HTTP_GET, "/users", users_handler);
mnet_route(app, MNET_HTTP_POST, "/users", create_user_handler);
```

### Handler

A handler receives a `mnet_request_t *` and returns a `mnet_response_t`:

```c
static mnet_response_t hello_handler(mnet_request_t *req) {
    (void)req;
    return mnet_text("Hello, World!");
}
```

### Response Helpers

| Function | Description |
|----------|-------------|
| `mnet_text(text)` | Plain text response |
| `mnet_html(html)` | HTML response |
| `mnet_json(json)` | JSON response |
| `mnet_jsonf(format, ...)` | JSON response with printf format |
| `mnet_error(status, msg)` | Error response (404, 500, etc.) |
| `mnet_status(status, body)` | Custom status with body |

### Configuration

| Function | Description |
|----------|-------------|
| `mnet_set_workers(app, n)` | Number of worker threads (0 = default 4) |
| `mnet_set_max_connections(app, n)` | Max concurrent connections (0 = unlimited) |
| `mnet_set_keep_alive_timeout(app, s)` | Keep-alive timeout in seconds |
| `mnet_set_max_body_size(app, bytes)` | Maximum request body size |
| `mnet_set_timeout(app, s)` | Socket read/write timeout in seconds |
| `mnet_set_https(app, 1)` | Enable HTTPS |
| `mnet_set_dev_mode(app, 1)` | Enable welcome page when no routes defined |

## HTTPS Support

mnet supports HTTPS via the `mnet_set_https(app, 1)` configuration.
The server needs a valid certificate and key pair.

## Memory Safety

mnet is designed with memory safety in mind:
- All allocations are checked for NULL
- All memory is freed on all error paths
- No use-after-free detected
- Bounds-checked operations throughout

## Testing

Run the test suite with:

```sh
make test
```

The suite includes 6 test suites covering routing, parsing, security,
stress, features, client-side HTTP, and more.
