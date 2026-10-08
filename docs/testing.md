# Testing

mnet has a comprehensive test suite. All tests must pass before a contribution
is accepted.

## Running the tests

```sh
make test
```

The suite has six parts:

- `test_mnet.c` (43 cases) covers the pure functions: routing, path and query
  parameters, headers, cookies, JSON escaping, URL decoding, the header
  validators, chunked responses, response-free paths and the configuration
  limits.
- `test_http.c` (13 cases) drives the real server over a loopback socket:
  request parsing, method validation, body bounds, header limits and static-file
  handling including traversal attempts.
- `test_mnet_parser.c` (17 cases) targets the parser's failure modes and the
  worker pool: oversized headers, a single over-long header line, too many
  headers, missing CRLF, malformed request lines and query strings, traversal
  variants, an incomplete body, and several concurrent slow clients.
- `test_stress.c` (5 cases) hammers the server with 32 concurrent clients and
  1280 mixed requests, verifies keep-alive, and confirms the default worker
  pool actually serves requests concurrently rather than serially.
- `test_features.c` (24 cases) is one end-to-end check per documented feature:
  methods, HEAD, wildcards, middleware, the custom 404 handler, cookies,
  encoded parameters, response helpers, and the body/header boundaries.
- `test_security.c` (61 check cases) is the regression suite for the security
  fixes: `mnet_jsonf` format-string hardening, header/body scoping,
  Content-Length/Transfer-Encoding handling, SIGPIPE survival, and Slowloris /
  idle / slow-body timeouts. The jsonf half runs everywhere; the server half is
  POSIX-only.

## Running tests under sanitizers

```sh
# AddressSanitizer + UndefinedBehaviorSanitizer
make clean && make test
# Or build with sanitizers explicitly:
gcc -fsanitize=address,undefined src/*.c test/*.c -o test_mnet_asan
./test_mnet_asan
```

All tests must pass under AddressSanitizer, UndefinedBehaviorSanitizer and
ThreadSanitizer.

## Fuzz testing

`test/fuzz_http.c` is a libFuzzer/AFL harness that drives the full request path
over a socket. Note that because the server runs in a forked child, the fuzzer's
coverage instrumentation only observes the client side, so it acts as a crash
and sanitizer oracle over the parser rather than a coverage-guided fuzzer.
