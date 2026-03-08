# pico — Library Specification

**Version**: 0.3.0 (pico_http) / 0.1.0 (pico_ws)
**Status**: Implemented
**License**: MIT
**Language**: C11
**Dependencies**: Platform sockets; optional mbedTLS for TLS

---

## 1. Overview

pico is a minimal networking library for C providing:

- **pico_http** — synchronous HTTP/1.1 client (GET, HEAD, POST, PUT, PATCH, DELETE)
- **pico_ws** — synchronous WebSocket client (RFC 6455, text/binary frames)

Both share a common transport layer (`PicoConn`) supporting raw TCP and
optional TLS via mbedTLS. The library is designed for embedding into larger
projects that need HTTP and WebSocket without pulling in libcurl or
libwebsockets.

The library is:
- **Small**: ~1700 lines of C across three files
- **Portable**: Windows (Winsock2), Linux, macOS, FreeBSD, any POSIX
- **Self-contained**: Only optional dependency is mbedTLS for TLS
- **MIT licensed**: Suitable for any project, commercial or open source

### Design Philosophy

1. Do one thing well: synchronous HTTP/1.1 and WebSocket client requests.
2. One optional dependency (mbedTLS for TLS). Plain HTTP/WS works with
   zero dependencies beyond platform sockets.
3. Compile-time feature flags: `PICO_HTTP_TLS` for HTTPS/WSS support.
4. All memory management uses `malloc`/`realloc`/`free`. No global state
   beyond a one-time Winsock initialisation flag on Windows.

---

## 2. File Manifest

```
pico_http.h          HTTP public API header
pico_http.c          HTTP implementation + shared transport
pico_ws.h            WebSocket public API header
pico_ws.c            WebSocket implementation (RFC 6455)
pico_internal.h      Shared transport (PicoConn, sockets, TLS) — internal
```

For embedding without TLS, copy the five files above. For TLS, also
provide mbedTLS headers and link against mbedtls/mbedx509/mbedcrypto.

---

## 3. Integration

### 3.1 Source Integration (Recommended for Embedding)

Copy the five pico files into your project. Compile `pico_http.c` and
`pico_ws.c` as part of your build. For static linking, define
`PICO_HTTP_STATIC` before including either header.

### 3.2 Shared Library

Define `PICO_HTTP_BUILDING` when compiling the source files. Consumers
include headers without defining anything.

On Windows, link against `ws2_32.lib`.

### 3.3 CMake

```cmake
option(PICO_HTTP_TLS "Enable HTTPS/WSS via mbedTLS" ON)

add_library(pico pico_http.c pico_ws.c)
target_include_directories(pico PUBLIC .)

if(WIN32)
    target_link_libraries(pico PRIVATE ws2_32)
endif()

if(PICO_HTTP_TLS)
    target_link_libraries(pico PRIVATE mbedtls mbedx509 mbedcrypto)
    target_compile_definitions(pico PRIVATE PICO_HTTP_TLS)
endif()

if(BUILD_SHARED_LIBS)
    target_compile_definitions(pico PRIVATE PICO_HTTP_BUILDING)
    set_target_properties(pico PROPERTIES C_VISIBILITY_PRESET hidden)
else()
    target_compile_definitions(pico PUBLIC PICO_HTTP_STATIC)
endif()
```

---

## 4. HTTP API (pico_http.h)

All public symbols are prefixed `pico_http_` or `PicoHttp`. Export macro:
`PICO_API`.

### 4.1 Error Codes

```c
typedef enum {
    PICO_OK             =  0,   /* Success */
    PICO_ERR_INVALID    = -1,   /* NULL or invalid arguments */
    PICO_ERR_DNS        = -2,   /* DNS resolution failed */
    PICO_ERR_CONNECT    = -3,   /* TCP connect failed/refused/timeout */
    PICO_ERR_SEND       = -4,   /* Socket write error */
    PICO_ERR_RECV       = -5,   /* Socket read error or timeout */
    PICO_ERR_PARSE      = -6,   /* Malformed status line */
    PICO_ERR_ALLOC      = -7,   /* malloc/realloc failed */
    PICO_ERR_WINSOCK    = -8,   /* WSAStartup failed (Windows) */
    PICO_ERR_TOO_MANY_REDIRECTS = -9,
    PICO_ERR_TLS        = -10   /* TLS handshake or I/O error */
} PicoHttpError;
```

`pico_http_strerror(int err)` returns a human-readable static string.

### 4.2 Types

```c
typedef struct { char *name; char *value; } PicoHttpHeader;

typedef struct {
    int              status;
    char            *status_text;
    PicoHttpHeader  *headers;
    size_t           header_count;
    char            *body;
    size_t           body_len;
} PicoHttpResponse;

typedef struct {
    const PicoHttpHeader *headers;
    size_t                header_count;
    int                   connect_timeout_ms; /* 0 = 5000 */
    int                   timeout_ms;         /* 0 = 30000 */
    int                   max_redirects;      /* 0 = 10, -1 = disable */
} PicoHttpOptions;
```

### 4.3 Request Functions

All return `PICO_OK` (0) or a negative `PicoHttpError`. A return of
`PICO_OK` does **not** imply 2xx — check `out->status`.

```c
/* No-body methods */
int pico_http_get(host, port, path, opts, out);
int pico_http_head(host, port, path, opts, out);
int pico_http_delete(host, port, path, opts, out);

/* Body methods (content_type may be NULL → "application/octet-stream") */
int pico_http_put(host, port, path, content_type, body, body_len, opts, out);
int pico_http_post(host, port, path, content_type, body, body_len, opts, out);
int pico_http_patch(host, port, path, content_type, body, body_len, opts, out);

/* URL-based convenience (parses http:// or https:// URL) */
int pico_http_request(method, url, content_type, body, body_len, opts, out);
```

### 4.4 Streaming API

For downloading large responses without buffering the entire body in memory.

```c
/* Callback invoked for each chunk of response body data.
 * Return 0 to continue, non-zero to abort the download. */
typedef int (*PicoHttpWriteFn)(const void *data, size_t chunk_len,
                                void *userdata);

/* Streaming GET — headers parsed into *out, body delivered via callback.
 * out->body will be NULL on return. Caller must still call
 * pico_http_response_free(out) to free headers.
 * Supports Content-Length and chunked TE.
 * Follows redirects (same rules as pico_http_get). */
int pico_http_get_stream(host, port, path, opts, out, write_fn, userdata);
```

**Behaviour**:
- Headers are parsed normally and placed into `out->headers`.
- Response body is **not** accumulated in memory. Instead, `write_fn` is
  invoked for each received chunk (typically 4 KiB from the socket buffer).
- If `write_fn` returns non-zero, the download is aborted and
  `pico_http_get_stream` returns `PICO_ERR_RECV`.
- Redirect responses (301/302/303/307/308) are followed automatically.
  The callback is only invoked for the final (non-redirect) response body.
- `out->body` is always `NULL` and `out->body_len` is always `0`.

**Typical usage — streaming to file**:

```c
static int write_to_file(const void *data, size_t len, void *ud) {
    return fwrite(data, 1, len, (FILE *)ud) == len ? 0 : -1;
}

FILE *f = fopen("artifact.tar.gz", "wb");
PicoHttpResponse res;
int rc = pico_http_get_stream("registry.example.com", 443, "/artifact/...",
                               NULL, &res, write_to_file, f);
fclose(f);
if (rc == PICO_OK && res.status == 200)
    printf("Downloaded successfully\n");
pico_http_response_free(&res);
```

### 4.5 Other Functions

```c
void        pico_http_response_free(PicoHttpResponse *res);
const char *pico_http_find_header(const PicoHttpResponse *res, const char *name);
int         pico_http_parse_url(url, &host, &port, &path);
int         pico_http_parse_url_ex(url, &host, &port, &path, &tls);
const char *pico_http_version(void);  /* Returns "0.3.0" */
```

---

## 5. WebSocket API (pico_ws.h)

All public symbols are prefixed `pico_ws_` or `PicoWs`. Export macro:
`PICO_WS_API`.

### 5.1 Error Codes

```c
typedef enum {
    PICO_WS_OK             =  0,
    PICO_WS_ERR_INVALID    = -1,
    PICO_WS_ERR_URL        = -2,
    PICO_WS_ERR_CONNECT    = -3,
    PICO_WS_ERR_TLS        = -4,
    PICO_WS_ERR_HANDSHAKE  = -5,
    PICO_WS_ERR_SEND       = -6,
    PICO_WS_ERR_RECV       = -7,
    PICO_WS_ERR_CLOSED     = -8,
    PICO_WS_ERR_ALLOC      = -9,
    PICO_WS_ERR_NOTSUP     = -10
} PicoWsError;
```

### 5.2 Types

```c
typedef struct PicoWs PicoWs;  /* Opaque connection handle */

typedef struct {
    int         connect_timeout_ms;  /* 0 = 5000 */
    int         recv_timeout_ms;     /* 0 = 30000 */
    const char *protocol;            /* Sub-protocol or NULL */
    const char *const *extra_headers;
} PicoWsOptions;
```

### 5.3 Functions

```c
/* Connect to ws:// or wss:// endpoint. Returns handle or NULL. */
PicoWs *pico_ws_connect(const char *url, const PicoWsOptions *opts, int *err_out);

/* Send text (is_binary=0) or binary (is_binary=1) frame. */
int pico_ws_send(PicoWs *ws, const void *data, size_t len, int is_binary);

/* Receive next data frame. Returns bytes read (>0) or negative error. */
int pico_ws_recv(PicoWs *ws, void *buf, size_t buflen,
                 int timeout_ms, int *is_binary_out);

/* Close connection and free resources. Safe with NULL. */
void pico_ws_close(PicoWs *ws);

const char *pico_ws_strerror(int err);
const char *pico_ws_version(void);  /* Returns "0.1.0" */
```

---

## 6. Protocol Behaviour

### 6.1 HTTP

- **HTTP/1.1** with `Connection: close` (no keep-alive).
- **Host header**: omits port for default (80 for http, 443 for https).
- **Redirects**: 301, 302, 303, 307, 308 followed automatically (default
  max 10). 303 converts POST/PUT to GET. Supports absolute URLs, absolute
  paths, and relative paths in Location header.
- **HEAD/204/304**: body reading is skipped.
- **Chunked TE**: decoded transparently (both buffered and streaming modes).
- **Content-Length**: exact read.
- **No CL, no chunked**: read until connection close.
- **Streaming**: `pico_http_get_stream` delivers body via callback without
  buffering. Supports both Content-Length and chunked TE. Redirect responses
  are followed before streaming begins.

### 6.2 WebSocket (RFC 6455)

- **Upgrade handshake**: HTTP/1.1 GET with `Upgrade: websocket`,
  `Connection: Upgrade`, `Sec-WebSocket-Key` (random base64),
  `Sec-WebSocket-Version: 13`.
- **Frames**: FIN bit always set (no fragmented sends). Client frames
  are masked per RFC requirement.
- **Opcodes handled**: text (0x01), binary (0x02), close (0x08),
  ping (0x09, auto-pong), pong (0x0A, skipped).
- **Extended lengths**: 7-bit, 16-bit, and 64-bit payload lengths.
- **Close**: sends close frame before TCP shutdown.

### 6.3 TLS (when `PICO_HTTP_TLS` defined)

- Backend: mbedTLS 3.x (compile-time flag).
- Client mode, `MBEDTLS_SSL_PRESET_DEFAULT`.
- SNI hostname sent via `mbedtls_ssl_set_hostname`.
- Certificate verification: `MBEDTLS_SSL_VERIFY_NONE` currently.
  CA certificate loading planned for production use.
- BIO callbacks wrap platform sockets — no mbedtls_net dependency.
- Same PicoConn struct handles both plain and TLS connections.

### 6.4 DNS and Connect

- `getaddrinfo` with `AF_UNSPEC` (IPv4/IPv6 dual-stack).
- **Connect timeout**: non-blocking connect with `select` (Windows)
  or `poll` (POSIX). Default 5000ms. Tries all resolved addresses.
- **Read/write timeout**: `SO_RCVTIMEO`/`SO_SNDTIMEO`. Default 30000ms.

---

## 7. Platform Specifics

### Windows (Winsock2)
- `WSAStartup` called once (lazy init, static flag).
- Socket type: `SOCKET`. Close: `closesocket()`.
- Timeout: `DWORD` milliseconds.
- Non-blocking connect: `ioctlsocket(FIONBIO)` + `select`.
- Random bytes: `SystemFunction036` (RtlGenRandom).
- Link: `ws2_32.lib`.

### POSIX
- Socket type: `int`. Close: `close()`.
- Timeout: `struct timeval`.
- Non-blocking connect: `fcntl(O_NONBLOCK)` + `poll`.
- Random bytes: `/dev/urandom`.

---

## 8. Compile-Time Flags

| Flag | Default | Description |
|------|---------|-------------|
| `PICO_HTTP_STATIC` | Off | Suppress export decorations (static linking) |
| `PICO_HTTP_BUILDING` | Off | Enable export decorations (building shared lib) |
| `PICO_HTTP_TLS` | Off* | Enable HTTPS/WSS via mbedTLS |

\* The `now` build system defaults `PICO_HTTP_TLS` to ON when mbedTLS is present.

---

## 9. Examples

### 9.1 Simple GET

```c
PicoHttpResponse res;
int rc = pico_http_get("localhost", 8080, "/healthz", NULL, &res);
if (rc == PICO_OK && res.status == 200)
    printf("%.*s\n", (int)res.body_len, res.body);
pico_http_response_free(&res);
```

### 9.2 HTTPS GET via URL

```c
PicoHttpResponse res;
int rc = pico_http_request("GET", "https://api.example.com/v1/status",
                            NULL, NULL, 0, NULL, &res);
if (rc == PICO_OK)
    printf("HTTP %d: %.*s\n", res.status, (int)res.body_len, res.body);
pico_http_response_free(&res);
```

### 9.3 WebSocket Echo

```c
int err;
PicoWs *ws = pico_ws_connect("wss://echo.example.com/ws", NULL, &err);
if (!ws) { fprintf(stderr, "%s\n", pico_ws_strerror(err)); return 1; }

pico_ws_send(ws, "hello", 5, 0);

char buf[4096];
int n = pico_ws_recv(ws, buf, sizeof(buf), 5000, NULL);
if (n > 0) printf("echo: %.*s\n", n, buf);

pico_ws_close(ws);
```

### 9.4 Streaming GET to File

```c
static int write_chunk(const void *data, size_t len, void *ud) {
    return fwrite(data, 1, len, (FILE *)ud) == len ? 0 : -1;
}

FILE *f = fopen("large-artifact.tar.gz", "wb");
PicoHttpResponse res;
PicoHttpHeader accept = { "Accept", "application/octet-stream" };
PicoHttpOptions opts = { .headers = &accept, .header_count = 1 };
int rc = pico_http_get_stream("registry.example.com", 443,
                               "/artifact/org/acme/core/1.0.0/pkg.tar.gz",
                               &opts, &res, write_chunk, f);
fclose(f);
if (rc != PICO_OK || res.status != 200) {
    fprintf(stderr, "Download failed: %s (HTTP %d)\n",
            pico_http_strerror(rc), res.status);
    remove("large-artifact.tar.gz");
}
pico_http_response_free(&res);
```

### 9.5 GET with Content Negotiation

```c
PicoHttpHeader accept = { "Accept",
    "application/x-pasta, application/json;q=0.9" };
PicoHttpOptions opts = { .headers = &accept, .header_count = 1 };
PicoHttpResponse res;
int rc = pico_http_get("registry.example.com", 443,
                        "/resolve/org/acme/core/^1.0",
                        &opts, &res);
if (rc == PICO_OK && res.status == 200) {
    const char *ct = pico_http_find_header(&res, "Content-Type");
    printf("Got %s: %.*s\n", ct, (int)res.body_len, res.body);
}
pico_http_response_free(&res);
```

### 9.6 PUT with Auth Header

```c
PicoHttpHeader auth = { .name = "Authorization", .value = "Bearer tok123" };
PicoHttpOptions opts = { .headers = &auth, .header_count = 1 };
PicoHttpResponse res;
int rc = pico_http_put("localhost", 8080, "/artifact/org/acme/core/1.0.0",
                        "application/octet-stream", data, len, &opts, &res);
pico_http_response_free(&res);
```

---

## 10. Testing

### Unit Tests (no network)

| Test | Description |
|------|-------------|
| Version string | `pico_http_version()` returns `"0.3.0"` |
| URL parsing | `http://host:port/path` variants |
| HTTPS URL parsing | `https://` → port 443, tls=1 |
| Reject bad schemes | `ftp://` returns -1 |
| Error code strings | All `PicoHttpError` values map to strings |
| NULL argument handling | Returns `PICO_ERR_INVALID` |
| DNS failure | Non-existent host → `PICO_ERR_DNS` |
| Connect failure | Refused port → `PICO_ERR_CONNECT` |
| Header lookup | `pico_http_find_header` on empty response |
| Response free | Safe on zeroed struct and NULL |
| Stream invalid args | NULL host/callback → `PICO_ERR_INVALID` |
| Stream connect fail | Refused port → `PICO_ERR_CONNECT`, zero bytes delivered |
| Stream callback type | `PicoHttpWriteFn` typedef compiles and assigns |
| WS version | `pico_ws_version()` returns `"0.1.0"` |
| WS error strings | All `PicoWsError` values map to strings |
| WS invalid args | NULL → `PICO_WS_ERR_INVALID` |
| WS bad URL | `http://` → `PICO_WS_ERR_URL` |
| WS connect failure | Refused → `PICO_WS_ERR_CONNECT` |
| WS close NULL | No crash |

### Integration Tests (require server)

| Test | Description |
|------|-------------|
| HTTPS GET | `pico_http_request` against `https://httpbin.org/get` |
| WSS connect+send+recv | Against WebSocket echo server |
| Redirect following | 301/302/303 chains |
| HEAD request | No body returned |
| Chunked response | Chunked TE decoded correctly |
| Streaming GET | `pico_http_get_stream` delivers body via callback |
| Streaming + chunked | Streaming with chunked TE |
| Streaming abort | Callback returns non-zero → download stops |

---

## 11. Versioning

Semantic Versioning 2.0. ABI stability not guaranteed before v1.0.0.

### Changelog

| Version | Changes |
|---------|---------|
| 0.1.0 | Initial: GET/PUT/POST, Content-Length, chunked TE, URL parsing |
| 0.2.0 | Error codes (`PicoHttpError`), HEAD/DELETE/PATCH, `pico_http_request`, redirect following, connect timeout, `pico_http_find_header` |
| 0.3.0 | TLS via mbedTLS (`PICO_HTTP_TLS`), HTTPS/WSS support, `pico_http_parse_url_ex`, `PicoConn` transport abstraction, pico_ws WebSocket client (RFC 6455), `pico_http_get_stream` streaming download API, content negotiation via custom request headers |

---

## 12. Roadmap

| Version | Feature |
|---------|---------|
| 0.4.0 | TLS hardening (CA cert loading, `VERIFY_REQUIRED`, TLS backend abstraction), streaming uploads, standalone CMake project |
| 0.5.0 | Connection pooling, keep-alive, proxy support (HTTP CONNECT), cookie jar |
| 0.6.0 | HTTP/2 binary framing + HPACK (RFC 7541) |
| 0.7.0 | HTTP/2 streams, multiplexing, flow control, ALPN negotiation |
| 0.8.0 | HTTP/2 complete (server push, graceful shutdown) |
| 0.9.0 | HTTP/3 via QUIC (vendored transport), QPACK (RFC 9204) |
| 1.0.0 | Unified API across HTTP/1.1, HTTP/2, HTTP/3; stable ABI guarantees |

See `pico-http-roadmap.md` for the full evolution plan.
