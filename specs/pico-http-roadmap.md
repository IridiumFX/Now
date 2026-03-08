# pico_http — Evolution Roadmap

**Version**: 0.3.0 (pico_http) / 0.1.0 (pico_ws) — current
**License**: MIT
**Goal**: Evolve from a minimal HTTP/1.1 client into a full HTTP/1.1 + HTTP/2 + HTTP/3
client while keeping the library small, dependency-free at its core, and easy to embed.

---

## Completed

### v0.1.0 — Initial Release
- [x] GET, PUT, POST methods
- [x] Content-Length body reading
- [x] Chunked transfer-encoding decoding
- [x] URL parsing (`pico_http_parse_url`)

### v0.2.0 — Robustness
- [x] Error code enum (`PicoHttpError`) with `pico_http_strerror()`
- [x] HEAD, DELETE, PATCH methods
- [x] `pico_http_request()` — URL-based convenience wrapper
- [x] Redirect following (301/302/303/307/308, configurable max depth)
- [x] 303 converts POST/PUT to GET
- [x] Connect timeout (non-blocking connect with `select`/`poll`)
- [x] Read/write timeout via `SO_RCVTIMEO`/`SO_SNDTIMEO`
- [x] `pico_http_find_header()` — case-insensitive response header lookup
- [x] Custom request headers via `PicoHttpOptions`

### v0.3.0 — TLS + WebSocket
- [x] Optional TLS via mbedTLS (`PICO_HTTP_TLS` compile flag)
- [x] `PicoConn` transport abstraction (raw TCP + optional mbedTLS)
- [x] SNI hostname via `mbedtls_ssl_set_hostname`
- [x] `pico_http_parse_url_ex()` — extended URL parser with TLS flag
- [x] HTTPS support: `https://` URLs, auto port 443
- [x] pico_ws: RFC 6455 WebSocket client (text/binary, ping/pong, masking)
- [x] WSS: WebSocket over TLS via shared `PicoConn`
- [x] Response body streaming (`pico_http_get_stream()`) — callback-based,
  no memory buffering, supports Content-Length and chunked TE
- [x] Content negotiation support (custom Accept headers via `PicoHttpOptions`)

---

## Phase 1: HTTP/1.1 Completion (v0.4 → v0.5)

### v0.4.0 — TLS Hardening + Streaming Polish
- [ ] CA certificate loading from system trust store
- [ ] `MBEDTLS_SSL_VERIFY_REQUIRED` mode (currently `VERIFY_NONE`)
- [ ] TLS backend abstraction: `PicoTlsProvider` with connect/read/write/close
- [ ] SChannel backend (Windows) — zero external deps
- [ ] SecureTransport backend (macOS) — zero external deps
- [ ] `Expect: 100-continue` for large PUT/POST bodies
- [ ] `pico_http_put_stream()` / `pico_http_post_stream()` — streaming uploads
- [ ] Standalone CMake project with install rules
- [ ] pkg-config and CMake `find_package` support

### v0.5.0 — Connection Reuse + Extras
- [ ] `PicoHttpClient` persistent handle (holds socket pool)
- [ ] HTTP/1.1 keep-alive with configurable idle timeout
- [ ] Connection pool keyed by (host, port, tls)
- [ ] Thread safety: per-connection locks or lock-free pool
- [ ] Cookie jar (optional, `PICO_HTTP_COOKIES`)
- [ ] Basic and Bearer authentication helpers
- [ ] Proxy support (HTTP CONNECT for tunneling)
- [ ] Comprehensive test suite (unit + integration against a local test server)
- [ ] Fuzz testing for URL parser and header parser

---

## Phase 2: HTTP/2 (v0.6 → v0.8)

HTTP/2 over TLS (h2) is the primary target. h2c (cleartext) is secondary.

### v0.6.0 — Binary framing + HPACK
- [ ] Frame parser: type, flags, stream ID, length, payload
- [ ] Frame serializer with proper padding
- [ ] Frame types: DATA, HEADERS, PRIORITY, RST_STREAM, SETTINGS,
      PUSH_PROMISE, PING, GOAWAY, WINDOW_UPDATE, CONTINUATION
- [ ] Connection preface (`PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`)
- [ ] HPACK header compression (RFC 7541)
- [ ] Static table (61 entries)
- [ ] Dynamic table with configurable size
- [ ] Huffman encoding/decoding
- [ ] Integer encoding (prefix-coded)

### v0.7.0 — Streams and multiplexing
- [ ] Stream state machine (idle → open → half-closed → closed)
- [ ] Stream priority and dependency (advisory, for future use)
- [ ] Flow control: per-stream and per-connection WINDOW_UPDATE
- [ ] Concurrent streams with configurable limit (SETTINGS_MAX_CONCURRENT_STREAMS)
- [ ] Stream-level error handling (RST_STREAM)
- [ ] Connection-level error handling (GOAWAY)
- [ ] TLS ALPN negotiation ("h2" identifier)
- [ ] Fallback to HTTP/1.1 if server doesn't support h2

### v0.8.0 — HTTP/2 complete
- [ ] Server push handling (PUSH_PROMISE — accept or reject)
- [ ] Header decompression bomb protection
- [ ] Ping/pong keepalive
- [ ] Graceful shutdown with GOAWAY drain
- [ ] h2c upgrade via HTTP/1.1 Upgrade header (optional)
- [ ] Public API additions:
  - `pico_http2_client_new()` — multiplexed client handle
  - `pico_http2_request()` — returns stream handle
  - `pico_http2_stream_read()` — read response on stream
  - Existing `pico_http_get/put/post` auto-negotiate protocol version
- [ ] Integration tests with known HTTP/2 servers

---

## Phase 3: HTTP/3 (v0.9 → v1.0)

HTTP/3 runs over QUIC (UDP). This is the most complex phase.

### v0.9.0 — QUIC transport (or vendor)

Two sub-options:

**Option A: Vendor a QUIC library**
- Evaluate: quiche (Cloudflare, BSD), msquic (Microsoft, MIT), ngtcp2 (MIT)
- Wrap behind `PicoQuicProvider` abstraction
- Pros: proven, handles congestion control and loss recovery
- Cons: significant dependency size

**Option B: Minimal QUIC implementation**
- Initial handshake, 1-RTT only (no 0-RTT in v1)
- TLS 1.3 integration (required by QUIC)
- Loss detection and congestion control (NewReno or Cubic)
- Connection migration (optional, defer to v1.0)
- Pros: self-contained; cons: substantial engineering effort

Recommendation: **Option A** with quiche or ngtcp2 behind an abstraction,
so the QUIC layer can be swapped later.

### v0.9.x — QPACK + HTTP/3 framing
- [ ] QPACK header compression (RFC 9204)
- [ ] Static table (99 entries, superset of HPACK)
- [ ] Dynamic table with encoder/decoder streams
- [ ] Unidirectional streams: control, QPACK encoder, QPACK decoder
- [ ] Request/response on bidirectional QUIC streams
- [ ] Frame types: DATA, HEADERS, CANCEL_PUSH, SETTINGS, PUSH_PROMISE, GOAWAY
- [ ] SETTINGS negotiation (SETTINGS_MAX_FIELD_SECTION_SIZE, etc.)

### v1.0.0 — Unified release
- [ ] Single API surface for HTTP/1.1, HTTP/2, HTTP/3
- [ ] Automatic protocol negotiation (ALPN + Alt-Svc)
- [ ] Connection coalescing (reuse HTTP/2 connection for same-origin)
- [ ] 0-RTT resumption for HTTP/3
- [ ] Connection migration for HTTP/3 (network change resilience)
- [ ] Server push (accept or reject)
- [ ] Graceful shutdown across all protocol versions
- [ ] Comprehensive documentation and examples
- [ ] Fuzz testing (AFL/libFuzzer for all parsers)
- [ ] Performance benchmarks vs curl, hyper, etc.
- [ ] ABI stability guarantees

---

## Architecture Principles

1. **Layered design**: Each protocol version is a layer. Higher layers
   delegate to lower ones. The public API is version-agnostic.

2. **Compile-time features**: Each capability is behind a flag:
   - `PICO_HTTP_TLS` — TLS support (required for h2 and h3)
   - `PICO_HTTP_H2` — HTTP/2 support
   - `PICO_HTTP_H3` — HTTP/3 support
   - Without any flags, you get HTTP/1.1 cleartext only (~500 lines)

3. **Provider pattern**: TLS and QUIC backends are abstracted behind
   function pointer tables. Users can swap implementations without
   modifying pico_http itself.

4. **Zero required dependencies**: The core HTTP/1.1 client uses only
   platform sockets. TLS, HTTP/2, and HTTP/3 are additive.

5. **Memory discipline**: All allocations go through `malloc`/`realloc`/`free`.
   A future `PicoAllocator` hook allows custom allocators.

6. **Thread safety**: The low-level API is stateless (per-request).
   The connection pool (`PicoHttpClient`) uses internal locking.
   No global mutable state beyond one-time Winsock init.

---

## Size Estimates

| Component | Estimated lines | Notes |
|-----------|----------------|-------|
| HTTP/1.1 core | ~1100 | Current implementation (pico_http.c) |
| WebSocket client | ~450 | Current implementation (pico_ws.c) |
| Shared transport | ~150 | PicoConn, sockets, TLS (pico_internal.h) |
| TLS backend abstraction | ~500 | SChannel + SecureTransport wrappers |
| Connection pool | ~300 | Keep-alive, thread safety |
| HPACK | ~600 | Static table, Huffman, dynamic table |
| HTTP/2 framing + streams | ~1200 | State machine, flow control |
| QPACK | ~700 | Based on HPACK with blocking |
| HTTP/3 framing | ~800 | Over QUIC streams |
| QUIC (if custom) | ~5000+ | Transport, crypto, congestion |
| **Total (with vendored QUIC)** | **~5000** | Excluding QUIC library |
| **Total (custom QUIC)** | **~10000** | Fully self-contained |

---

## Versioning and Compatibility

- Public API follows SemVer 2.0
- ABI stability guaranteed from v1.0.0 onward
- Pre-1.0 releases may break API between minor versions
- Header `pico_http.h` remains the single entry point; protocol-specific
  headers are internal
