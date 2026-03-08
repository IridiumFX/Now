# now — Architecture Guide

Internal design, module structure, and key design decisions.

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Module Map](#2-module-map)
3. [Data Flow](#3-data-flow)
4. [Build Lifecycle Pipeline](#4-build-lifecycle-pipeline)
5. [Key Data Structures](#5-key-data-structures)
6. [Dependency Resolution Algorithm](#6-dependency-resolution-algorithm)
7. [Parallel Build Architecture](#7-parallel-build-architecture)
8. [Incremental Build Strategy](#8-incremental-build-strategy)
9. [Workspace Build Ordering](#9-workspace-build-ordering)
10. [Layer Merge System](#10-layer-merge-system)
11. [HTTP and WebSocket Clients](#11-http-and-websocket-clients)
12. [Platform Abstraction](#12-platform-abstraction)
13. [Security Architecture](#13-security-architecture)
14. [Error Handling](#14-error-handling)
15. [Design Decisions](#15-design-decisions)

---

## 1. System Overview

`now` is structured as a **shared library** (`libnow`) plus a **CLI binary** (`now`). The library contains all build logic; the CLI is a thin dispatch layer.

```
                    ┌──────────────┐
                    │   CLI (main) │  Phase dispatch, flag parsing
                    └──────┬───────┘
                           │
                    ┌──────▼───────┐
                    │   libnow     │  Public API (now.h)
                    │              │
                    │  ┌─────────┐ │
                    │  │   POM   │ │  Project descriptor parsing
                    │  ├─────────┤ │
                    │  │  Build  │ │  Compile, link, test
                    │  ├─────────┤ │
                    │  │ Resolve │ │  Dependency resolution
                    │  ├─────────┤ │
                    │  │ Procure │ │  Download and install deps
                    │  ├─────────┤ │
                    │  │ Package │ │  Tarball assembly, publish
                    │  ├─────────┤ │
                    │  │ Security│ │  Trust, advisory, repro
                    │  └─────────┘ │
                    └──────┬───────┘
                           │
                    ┌──────▼───────┐
                    │    Pasta     │  Serialization format (submodule)
                    └──────────────┘
```

---

## 2. Module Map

### Core Modules

| Module | Responsibility | Key Types |
|--------|---------------|-----------|
| `now.c` | Public API surface | `NowResult`, `NowError` |
| `now_pom.c` | Project descriptor parser | `NowProject`, `NowDep`, `NowOutput` |
| `now_fs.c` | Filesystem operations | `NowFileList`, path utilities |
| `now_lang.c` | Language classification | Extension-to-language mapping |
| `now_version.c` | SemVer + ranges | `NowSemVer`, `NowVersionRange`, `NowCoordinate` |

### Build Modules

| Module | Responsibility | Key Types |
|--------|---------------|-----------|
| `now_build.c` | Compile, link, test phases | Toolchain detection, flag translation |
| `now_manifest.c` | Incremental build tracking | SHA-256, mtime, flags hash |
| `now_workspace.c` | Multi-module builds | DAG, topo sort, wave execution |

### Dependency Modules

| Module | Responsibility | Key Types |
|--------|---------------|-----------|
| `now_resolve.c` | Version constraint solver | Range intersection, convergence |
| `now_procure.c` | Artifact download/install | Registry client, SHA-256 verify |
| `now_package.c` | Tarball assembly, publish | Staging, upload |

### Security Modules

| Module | Responsibility | Key Types |
|--------|---------------|-----------|
| `now_trust.c` | Signing key management | `NowTrustStore`, `NowTrustKey` |
| `now_advisory.c` | Vulnerability checking | `NowAdvisoryDB`, `NowAdvisoryReport` |
| `now_repro.c` | Reproducible builds | `NowReproConfig`, timebase |

### Infrastructure Modules

| Module | Responsibility | Key Types |
|--------|---------------|-----------|
| `pico_http.c` | HTTP/1.1 client | `PicoHttpResponse`, TLS |
| `pico_ws.c` | WebSocket client | RFC 6455 frames |
| `now_ci.c` | CI integration | `NowCIEnv`, structured output |
| `now_layer.c` | Config layers | `NowLayerStack`, merge/audit |
| `now_arch.c` | Platform triples | `NowTriple`, host detection |
| `now_export.c` | Build system export | CMake + Makefile generators |
| `now_plugin.c` | Plugin system | Hook dispatch, IPC |

### Module Dependency Graph

```
main.c
  └─► now_pom.h ─► now.h
  └─► now_build.h
        └─► now_manifest.h
        └─► now_repro.h
        └─► now_lang.h
        └─► now_fs.h
  └─► now_resolve.h ─► now_version.h
  └─► now_procure.h ─► pico_http.h ─► pico_internal.h
  └─► now_package.h
  └─► now_trust.h
  └─► now_advisory.h ─► now_version.h
  └─► now_workspace.h
  └─► now_plugin.h
  └─► now_ci.h
  └─► now_layer.h
  └─► now_arch.h
  └─► now_export.h
```

---

## 3. Data Flow

### Project Loading

```
now.pasta file
  │  (read + parse)
  ▼
PastaValue tree
  │  (extract fields)
  ▼
NowProject struct
  │  (retained _pasta_root for lazy access)
  ▼
Build phases consume NowProject
```

The raw `PastaValue*` tree is kept alive as `NowProject._pasta_root` so that modules like trust, repro, and advisory can read their sections lazily without duplicating all fields into the struct.

### Build Data Flow

```
NowProject
  │
  ├─► now_procure() ──► registry ──► ~/.now/repo/
  │                                        │
  ├─► now_build_compile() ◄───── dep paths ┘
  │     │
  │     ├─► discover sources (now_fs)
  │     ├─► check manifest (now_manifest)
  │     ├─► spawn compiler processes (parallel pool)
  │     └─► update manifest
  │
  ├─► now_build_link()
  │     └─► link objects into target/bin/
  │
  └─► now_test()
        └─► compile tests, link, execute
```

---

## 4. Build Lifecycle Pipeline

```
┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐
│ procure  │──►│ generate │──►│ compile  │──►│   link   │
│          │   │ (plugins)│   │ (parallel│   │          │
│ resolve  │   │          │   │  pool)   │   │ ar/cc/ld │
│ download │   │ codegen  │   │          │   │          │
│ install  │   │          │   │ manifest │   │          │
└──────────┘   └──────────┘   └──────────┘   └──────────┘
                                                   │
                                              ┌────▼─────┐
                                              │   test   │
                                              │          │
                                              │ compile  │
                                              │ link     │
                                              │ execute  │
                                              └──────────┘
```

---

## 5. Key Data Structures

### NowProject (now_pom.h)

The central data structure. Contains all parsed fields from `now.pasta`:

```c
struct NowProject {
    char *group, *artifact, *version;    // Identity
    NowStrArray langs;                    // Language list
    char *std;                            // Language standard
    NowSources sources, tests;           // Directory config
    NowOutput output;                     // Output type and name
    NowCompile compile;                   // Compile settings
    NowLink link;                         // Link settings
    NowDepArray deps;                     // Dependencies
    NowRepoArray repos;                   // Repositories
    char *convergence;                    // Resolution policy
    NowStrArray private_groups;           // Dep confusion protection
    NowPluginArray plugins;               // Plugins
    NowStrArray modules;                  // Workspace modules
    void *_pasta_root;                    // Raw Pasta tree (kept alive)
};
```

### NowResult

Error reporting structure used throughout:

```c
typedef struct {
    NowError code;     // Error code enum
    int      line;     // Source line (for parse errors)
    int      col;      // Source column
    char     message[512];
} NowResult;
```

### NowFileList (now_fs.h)

Discovered source files:

```c
typedef struct {
    char **paths;
    size_t count;
    size_t capacity;
} NowFileList;
```

---

## 6. Dependency Resolution Algorithm

The resolver uses a constraint-based approach:

1. **Collect constraints**: Parse each dep's `group:artifact:version-range`
2. **Group by coordinate**: Gather all constraints on the same `group:artifact`
3. **Intersect ranges**: For each coordinate, intersect all version ranges
4. **Apply convergence**: Select the resolved version from the intersected range
   - `lowest`: floor of the range
   - `highest`: ceiling (requires registry query)
   - `exact`: all constraints must agree
5. **Apply overrides**: `override: true` deps replace the resolved version
6. **Conflict detection**: Report when ranges have no intersection

The resolved versions are written to `now.lock.pasta` for reproducibility.

---

## 7. Parallel Build Architecture

```
Main thread
  │
  ├─► Discover sources
  ├─► Check manifest (skip up-to-date files)
  ├─► Create process pool (size = min(changed_files, jobs))
  │
  │   ┌─────────────────────────────┐
  │   │ Worker processes            │
  │   │                             │
  │   │  [0] cc -c src/a.c -o a.o  │  stdout/stderr captured via pipes
  │   │  [1] cc -c src/b.c -o b.o  │
  │   │  [2] cc -c src/c.c -o c.o  │
  │   │  ...                        │
  │   └─────────────────────────────┘
  │
  ├─► Wait for batch completion
  │     • WaitForMultipleObjects (Windows)
  │     • waitpid(-1) (POSIX)
  │
  ├─► Print buffered output atomically (no interleaving)
  ├─► Dispatch next batch
  └─► Update manifest with new hashes
```

Key properties:
- **No interleaving**: each worker's output is collected via pipes and printed atomically
- **Single-job fast path**: when `jobs=1`, no pipes — direct execution
- **Incremental**: manifest check before dispatch; unchanged files never spawn a process

---

## 8. Incremental Build Strategy

For each source file, the manifest tracks:

```pasta
{
  "src/main/c/parser.c": {
    source_hash:  "a1b2c3...",    ; SHA-256 of source content
    flags_hash:   "d4e5f6...",    ; SHA-256 of compiler flags
    mtime:        1709654400,      ; file modification time
    obj_path:     "target/obj/main/parser.c.o"
  }
}
```

Rebuild triggers:
1. **mtime changed** → re-hash source → if hash changed → recompile
2. **Flags changed** → recompile (detects flag changes across builds)
3. **Object missing** → recompile
4. **mtime unchanged, hash unchanged** → skip (fast path)

---

## 9. Workspace Build Ordering

```
Kahn's topological sort on the module dependency graph:

1. Build DAG from inter-module deps
2. Find all nodes with in-degree 0 (no deps) → Wave 0
3. Remove Wave 0, reduce in-degrees → find next wave
4. Repeat until all modules scheduled
5. Cycle detection: if nodes remain with no in-degree-0, report cycle

Modules within the same wave are built in parallel.

Wave 0: [core]           ──► build core
Wave 1: [net, tls]       ──► build net + tls in parallel
Wave 2: [cli]            ──► build cli (depends on net + tls)
```

---

## 10. Layer Merge System

Layers are merged bottom-up:

```
Baseline (built-in defaults)
  └─► Enterprise layer (~/.now/layers/*.pasta)
        └─► Project layer (now.pasta)
```

Merge rules per section policy:
- **open**: project values override/extend layer values
  - Scalars: replace
  - Arrays: append (use `!key:` prefix to replace)
  - Maps: deep merge
- **locked**: project values rejected; violations recorded in audit report

The `!exclude:` directive in arrays removes matching entries from lower layers.

---

## 11. HTTP and WebSocket Clients

### pico_http

Standalone HTTP/1.1 client, independently MIT-licensable:

- **Transport**: `PicoConn` abstraction (raw socket + optional mbedTLS)
- **DNS**: `getaddrinfo` for IPv4/IPv6
- **TLS**: mbedTLS 3.6.5, enabled via `PICO_HTTP_TLS` compile flag
- **Response**: streaming parser for headers, Content-Length and chunked TE
- **Streaming download**: `pico_http_get_stream` — callback-based body delivery without memory buffering (used by `now_procure` for large artifacts)
- **Redirects**: automatic 301/302/303/307/308 following
- **Content negotiation**: `Accept: application/x-pasta` header on registry requests (per spec §23.10)

### pico_ws

WebSocket client sharing the `PicoConn` transport:

- **Handshake**: HTTP/1.1 Upgrade with random `Sec-WebSocket-Key`
- **Frames**: text/binary, client masking per RFC 6455
- **Control**: close (reciprocal), ping (auto-pong)

Both clients are synchronous (blocking I/O).

---

## 12. Platform Abstraction

### Compiler Abstraction

| Operation | GCC/Clang | MSVC |
|-----------|-----------|------|
| Compile | `cc -c -o file.o` | `cl.exe /c /Fofile.obj` |
| Warnings | `-Wall -Wextra` | `/W4` |
| Defines | `-DFOO` | `/DFOO` |
| Includes | `-Ipath` | `/Ipath` |
| Shared lib | `cc -shared -o lib.so` | `link.exe /DLL /OUT:lib.dll` |
| Static lib | `ar rcs lib.a` | `lib.exe /OUT:lib.lib` |

### Process Spawning

| Platform | API |
|----------|-----|
| Windows | `CreateProcess` + `WaitForMultipleObjects` |
| POSIX | `fork`/`execvp` + `waitpid` |

### Filesystem

- Path separators handled internally (always `/` internally, converted at OS boundary)
- `mkdir -p` equivalent: `now_mkdir_p()` works on both platforms
- `strndup` compatibility shim for Windows (not available in MSVC or MinGW)

### Platform Triple Detection

`now_host_triple()` detects:
- **OS**: compile-time `#ifdef` (`_WIN32`, `__APPLE__`, `__linux__`, `__FreeBSD__`)
- **Arch**: `_M_X64`/`_M_ARM64` (MSVC), `__x86_64__`/`__aarch64__` (GCC/Clang)
- **Variant**: `msvc` (Windows MSVC), `gnu` (Linux), `apple` (macOS)

---

## 13. Security Architecture

### Trust Chain

```
Registry ──► Package download ──► SHA-256 verify ──► Signature verify ──► Install
                                       │                    │
                                  Hash from registry    Key from trust store
```

### Advisory Pipeline

```
Advisory DB ──► Parse dep coordinate ──► Version range match ──► Severity check
                                                                       │
                                              ┌────────────────────────┤
                                              │                        │
                                         Override?              Blacklisted?
                                              │                        │
                                         Valid + not expired?     Always block
                                              │
                                         Allow (suppress)
```

### Dep Confusion Protection

```
Dep group ──► Match against private_groups ──► If match: reject public registry
                                               If no match: resolve normally
```

---

## 14. Error Handling

All public API functions return `int` (0 = success) and accept a `NowResult*` for error details. The `NowError` enum covers all categories:

| Code | Category |
|------|----------|
| `NOW_OK` | Success |
| `NOW_ERR_ALLOC` | Memory allocation failure |
| `NOW_ERR_IO` | I/O error |
| `NOW_ERR_SYNTAX` | Parse error in Pasta/config |
| `NOW_ERR_SCHEMA` | Invalid field/value in config |
| `NOW_ERR_NOT_FOUND` | Missing file or dep |
| `NOW_ERR_TOOL` | Compiler/linker failure |
| `NOW_ERR_TEST` | Test failure |
| `NOW_ERR_AUTH` | Authentication failure |
| `NOW_ERR_ADVISORY` | Blocked by security advisory |

---

## 15. Design Decisions

### Why Direct Compilation (No Ninja/Make Generation)?

`now` invokes compilers directly rather than generating build system files. This gives:
- Full control over incremental build logic
- No dependency on external build tools
- Simpler mental model (descriptor → binary)
- Exact flag control per-file

The export commands (`export:cmake`, `export:make`) provide escape hatches when integration with other tools is needed.

### Why Pasta Format?

Pasta is a human-friendly serialization format that supports comments, trailing commas, and unquoted keys. It's more readable than JSON and less complex than YAML. The Pasta library is MIT-licensed and has no external dependencies.

### Why Shared Library + CLI?

Separating the library from the CLI enables:
- Direct API access from tests (internal functions exported via `NOW_API`)
- Potential IDE integration via the library
- Embedding in other tools
- Independent versioning of library and CLI

### Why SHA-256 for Everything?

SHA-256 is used for manifest hashing, package integrity, and advisory database hashing. One algorithm simplifies the implementation and the public-domain Brad Conte implementation has no external dependencies.

### Why Minisign for Signatures?

Embedded Ed25519 is complex to implement correctly. Delegating to `minisign` gives us production-quality signature verification with minimal code risk. Embedded Ed25519 is on the post-v1 backlog.
