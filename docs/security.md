# Security Considerations

mnet is a small framework and leaves several operational concerns to the caller.
If you expose a server to a network you do not fully trust, read this section.

## Timeouts have a default

I/O is blocking, so a client that connects and then sends nothing would otherwise
hold its connection indefinitely. A 30 second timeout is applied by default.
`mnet_set_timeout(app, seconds)` overrides it (use `0` to get the default back),
and `mnet_set_keep_alive_timeout()` sets the separate idle timeout for reused
keep-alive connections. Setting a negative timeout disables the protection and
is strongly discouraged in production.

## Choose a worker count deliberately

The server is threaded by default (4 workers), so multiple clients are served
concurrently. Handlers then run on several threads at once, so anything they
share must be synchronised by the application. Pass `1` to
`mnet_set_workers()` for the single-threaded loop, which has no synchronisation
overhead but handles one connection at a time.

## Cap concurrent connections

`mnet_set_max_connections(app, n)` refuses new connections with `503` once `n`
are active. Without it there is no limit. Note that this is a simple counter,
not per-IP rate limiting — it does not distinguish one abusive client from many
legitimate ones.

## There is no TLS

mnet speaks plaintext HTTP. Terminate TLS in a reverse proxy (nginx, Caddy,
stunnel) in front of it if you need HTTPS.

## Header size is bounded

The request line and headers must fit in the 8 KB read buffer. A request whose
headers do not fit is rejected with `431` and the connection is closed; headers
are never silently truncated, and a header block larger than the buffer is not
parsed as if it were complete. Individual header lines are capped at 4 KB and
the header count at 100; a line whose name is not a valid RFC 7230 token is
skipped rather than stored. Query parameter names and values are capped at 1 KB
each, and a request exceeding either cap is rejected rather than truncated.
These limits are fixed rather than configurable.

## Body size is capped

Request bodies are limited to 16 MB by default and rejected above that with `413`;
adjust with `mnet_set_max_body_size()`. A `Content-Length` that is not a plain
decimal number within the limit is rejected with `400`, and any body bytes
beyond the declared length are ignored rather than copied. Bodies are allocated
on the heap, so the cap also bounds per-request memory use.

## Malformed requests are answered, not ignored

An unparseable request line yields `400`, an unknown method yields `405`, and a
body over the cap yields `413`.

## Response headers cannot be injected

A `Content-Type` containing CR or LF is rejected rather than emitted, so a
handler cannot split the response. If you add your own header-emitting code,
validate values with `mnet_header_value_valid()` and names with
`mnet_header_name_valid()`.

## Static file serving is traversal-checked

`mnet_static()` resolves the requested path with `realpath()` and verifies it
stays under the configured root, rejecting escapes with `403`. The check is
boundary-aware, so a sibling directory whose name merely shares a prefix with
the root (for example `/var/www2` when the root is `/var/www`) is not
reachable. Symlinks are resolved before the check, so a symlink that leaves the
root is rejected too. Do not serve a directory whose contents you would not
expose.

## URL decoding is strict

`mnet_url_decode_ex()` (and its convenience wrapper
`mnet_url_decode_safe()`) rejects malformed or truncated percent escapes and
`%00` instead of truncating, and never writes past the destination capacity.
Path parameters and query values that fail to decode are reported as empty
rather than partially decoded.

## Handlers must not block

A slow handler occupies its worker for as long as it runs. With one worker that
stalls every other client; with several it still ties up a slot.

## Client-side requests are untrusted

When using `MNET_CALL()` to fetch content from external URLs, remember that:

- The remote server can return any content, including malicious HTML or JSON.
- The remote server can set headers that you may not expect.
- A malicious client can redirect the request to another server.
- DNS responses can be spoofed.

Always validate the content you receive and handle errors gracefully.
