# now — Developer Guide

Contributing to `now`: building from source, running tests, adding features, and coding conventions.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Building from Source](#2-building-from-source)
3. [Running Tests](#3-running-tests)
4. [Project Structure](#4-project-structure)
5. [Adding a New Module](#5-adding-a-new-module)
6. [Coding Conventions](#6-coding-conventions)
7. [Adding a CLI Command](#7-adding-a-cli-command)
8. [Adding Tests](#8-adding-tests)
9. [DLL Export Pattern](#9-dll-export-pattern)
10. [Working with Pasta](#10-working-with-pasta)
11. [Cross-Platform Notes](#11-cross-platform-notes)
12. [Debugging Tips](#12-debugging-tips)
13. [Release Process](#13-release-process)

---

## 1. Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| CMake | 3.20+ | Build system |
| C compiler | C11 | GCC 13+, Clang 15+, or MSVC 2022+ |
| Ninja | Any | Recommended generator |
| Git | Any | Submodule management |

### Clone with Submodules

```sh
git clone --recurse-submodules https://github.com/IridiumFX/now.git
cd now
```

If you already cloned without submodules:

```sh
git submodule update --init --recursive
```

---

## 2. Building from Source

### Debug Build (Default)

```sh
cmake --preset default
cmake --build build/default
```

### Release Build

```sh
cmake --preset release
cmake --build build/release
```

### Static Build

```sh
cmake --preset default -DBUILD_SHARED_LIBS=OFF
cmake --build build/default
```

### Disable TLS

```sh
cmake --preset default -DPICO_HTTP_TLS=OFF
cmake --build build/default
```

### Build Outputs

```
build/default/
  bin/
    now.exe          CLI binary
    now_test.exe     Test binary
    libnow.dll       Shared library (Windows)
  lib/
    libnow.dll.a     Import library (Windows)
```

On Linux/macOS: `libnow.so` / `libnow.dylib` instead of `.dll`.

---

## 3. Running Tests

```sh
# Run all tests
./build/default/bin/now_test

# On Windows with MinGW, ensure gcc is in PATH for the build integration test
PATH="/path/to/mingw/bin:$PATH" ./build/default/bin/now_test
```

Expected output: `167/167 tests passed`

The build integration test (compiles a real "hello" project) requires `gcc` to be in PATH at runtime. In CI, this is 166/167 if gcc isn't available.

### Test Categories

| Category | Count | Description |
|----------|-------|-------------|
| Version | 1 | Library version string |
| POM | 11 | Project descriptor parsing |
| Language | 7 | Extension classification |
| Filesystem | 4 | Path utilities |
| SemVer + Ranges | 14 | Version parsing, ranges, coordinates |
| Manifest | 3 | Incremental build tracking |
| Resolver | 6 | Dependency resolution + lock file |
| HTTP | 8 | pico_http client |
| WebSocket | 6 | pico_ws client |
| Procure | 2 | Dependency procurement |
| Parallel | 1 | CPU detection |
| Toolchain | 3 | Compiler detection |
| Publish | 2 | Package publishing |
| Workspace | 5 | Multi-module builds |
| Plugins | 6 | Plugin system |
| Dep confusion | 7 | Private group matching |
| Repro | 14 | Reproducible builds |
| Trust | 12 | Signing and trust |
| Advisory | 17 | Security advisories |
| CI | 6 | Structured output |
| Layers | 8 | Config layer merge |
| Multi-arch | 9 | Platform triples |
| Export | 9 | CMake + Makefile generation |
| Build integration | 1 | Full compile+link test |

---

## 4. Project Structure

```
now/
  CMakeLists.txt          Build configuration
  CMakePresets.json        CMake presets (default, release)
  src/
    main/
      h/                  Public headers
        now.h             Public API header
        pico_http.h       HTTP client API (standalone MIT)
        pico_ws.h         WebSocket client API (standalone MIT)
        internal/         Internal headers (not installed)
          now_pom.h       Project Object Model
          now_build.h     Build phases
          now_*.h         Feature modules
          pico_internal.h Shared transport layer
      c/                  Source files (.c only)
        now.c             Public API implementation
        now_pom.c         Project Object Model
        now_build.c       Build phases
        now_*.c           Feature modules
        pico_http.c       HTTP client
        pico_ws.c         WebSocket client
        main.c            CLI entry point
    test/
      c/now_test.c        All tests in one file
      resources/          Test data (pasta files, sample projects)
  lib/
    pasta/                Pasta library (git submodule)
    mbedtls/              mbedTLS (git submodule, optional)
    cookbook/              Registry server (git submodule)
  specs/                  Specification documents
  guides/                 User and developer guides
  docs/                   Implementation status
```

---

## 5. Adding a New Module

1. **Create header** `src/main/h/internal/now_feature.h`:
   ```c
   #ifndef NOW_FEATURE_H
   #define NOW_FEATURE_H
   #include "now.h"

   NOW_API int now_feature_do_thing(const NowProject *project, NowResult *result);

   #endif
   ```

2. **Create implementation** `src/main/c/now_feature.c`:
   ```c
   #include "now_feature.h"
   #include "now_pom.h"    /* if you need full NowProject struct */
   #include "now_fs.h"     /* if you need filesystem utilities */
   #include "pasta.h"      /* if you need to read Pasta values */
   ```

3. **Add to CMakeLists.txt**:
   ```cmake
   add_library(now
       ...
       src/main/c/now_feature.c
   )
   ```

4. **Add include to test file** `src/test/c/now_test.c`:
   ```c
   #include "now_feature.h"
   ```

5. **Add tests** (see [Adding Tests](#8-adding-tests))

6. **Add CLI command** if needed (see [Adding a CLI Command](#7-adding-a-cli-command))

---

## 6. Coding Conventions

### General

- **C11 standard** — no compiler-specific extensions
- **4-space indentation**, no tabs
- **Include order**: `now_pom.h` before `now.h` in files needing the full `NowProject` struct
- **Function naming**: `now_module_verb()` (e.g., `now_trust_add`, `now_advisory_check_dep`)
- **Type naming**: `NowPascalCase` for structs/enums (e.g., `NowTrustStore`, `NowSeverity`)
- **Error returns**: `int` (0 = success), with `NowResult*` parameter for details
- **No dynamic allocation in headers** — all memory management in .c files

### Memory Management

- `malloc`/`calloc`/`realloc`/`free` — no custom allocators
- Every `init()` has a matching `free()`
- Dynamic arrays use the pattern: `items`, `count`, `capacity` with doubling growth

### Platform Compatibility

- `strndup` not available on Windows — use `#ifdef _WIN32` compat shim in each file that needs it
- Use `now_path_join()` and `now_path_exists()` instead of platform-specific path operations
- Process spawning: use the existing `now_exec` / parallel pool infrastructure

### Pasta Usage

- `pasta_set()` **appends**, never replaces — `pasta_map_get()` returns the first match
- Maps require braces AND commas: `{ key: "val", key2: "val2" }`
- Use `PASTA_SORTED` when writing deterministic output (manifests, lock files)
- Use `pasta_new_number(double)` / `pasta_get_number()` for numeric values

---

## 7. Adding a CLI Command

Edit `src/main/c/main.c`:

1. **Add to usage text**:
   ```c
   "  feature:do    Description of the command\n"
   ```

2. **Add include** (if new module):
   ```c
   #include "now_feature.h"
   ```

3. **Add handler** in the appropriate section:
   - Before project loading: for commands that don't need `now.pasta` (like `trust:list`)
   - After project loading: for commands that need the project descriptor

   ```c
   } else if (strcmp(phase, "feature:do") == 0) {
       rc = now_feature_do_thing(project, &result);
       if (rc != 0)
           fprintf(stderr, "error: %s\n", result.message);
   ```

---

## 8. Adding Tests

All tests are in `src/test/c/now_test.c`. The test framework is minimal:

### Test Macros

```c
TEST("description")           // start a test (increments counter)
PASS()                        // mark as passed
FAIL("reason")                // mark as failed, return from function
ASSERT_STR(actual, expected)  // string equality
ASSERT_EQ(actual, expected)   // value equality
ASSERT_NOT_NULL(ptr)          // non-null check
```

### Writing a Test

```c
static void test_feature_basic(void) {
    TEST("feature: basic operation");
    /* setup */
    int rc = now_feature_do_thing(NULL, NULL);
    ASSERT_EQ(rc, 0);
    /* cleanup */
    PASS();
}
```

### Registering Tests

Add the call in `main()` under the appropriate section:

```c
printf("\n  Feature:\n");
test_feature_basic();
test_feature_edge_case();
```

### Test Resources

Test data files go in `src/test/resources/`. Access via `NOW_TEST_RESOURCES` macro:

```c
char path[512];
snprintf(path, sizeof(path), "%s/sample.pasta", NOW_TEST_RESOURCES);
```

---

## 9. DLL Export Pattern

`now` uses `NOW_API` for all functions that need to be accessible from the test executable (which links against the shared library):

```c
// In now.h:
#ifdef NOW_STATIC
  #define NOW_API
#elif defined(_WIN32)
  #ifdef NOW_BUILDING
    #define NOW_API __declspec(dllexport)
  #else
    #define NOW_API __declspec(dllimport)
  #endif
#else
  #define NOW_API __attribute__((visibility("default")))
#endif
```

When building the library: `NOW_BUILDING` is defined → functions are exported.
When consuming (tests, CLI): functions are imported.
When building static: `NOW_API` is empty.

**Important**: All functions called from tests must be marked `NOW_API`, even "internal" functions.

The HTTP client has its own independent `PICO_API` macro with the same pattern (`PICO_HTTP_BUILDING` / `PICO_HTTP_STATIC`).

---

## 10. Working with Pasta

### Parsing

```c
PastaResult pr;
PastaValue *root = pasta_parse(input, len, &pr);
if (!root || pr.code != PASTA_OK) {
    /* handle error: pr.message, pr.line */
}
```

### Reading Values

```c
const PastaValue *v = pasta_map_get(root, "key");
if (v && pasta_type(v) == PASTA_STRING)
    printf("%s\n", pasta_get_string(v));
if (v && pasta_type(v) == PASTA_BOOL)
    int b = pasta_get_bool(v);
if (v && pasta_type(v) == PASTA_NUMBER)
    double n = pasta_get_number(v);
```

### Arrays

```c
const PastaValue *arr = pasta_map_get(root, "items");
if (arr && pasta_type(arr) == PASTA_ARRAY) {
    size_t n = pasta_count(arr);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *item = pasta_array_get(arr, i);
        /* use item */
    }
}
```

### Writing

```c
PastaValue *root = pasta_new_map();
pasta_set(root, "key", pasta_new_string("value"));
pasta_set(root, "count", pasta_new_number(42));

PastaValue *arr = pasta_new_array();
pasta_push(arr, pasta_new_string("item1"));
pasta_set(root, "items", arr);

char *text = pasta_write(root, PASTA_PRETTY | PASTA_SORTED);
/* use text */
free(text);
pasta_free(root);
```

**Remember**: `pasta_set()` appends — it never replaces existing keys. Build fresh maps for overlay semantics.

---

## 11. Cross-Platform Notes

### Windows (MinGW)

- `strndup` not available — add compat shim at top of .c file
- MinGW gcc needs its bin dir in PATH for `cc1.exe` to find `libssp-0.dll`
- Use `WaitForMultipleObjects` for parallel process waiting
- Use `_getcwd` instead of `getcwd`

### Windows (MSVC)

- Object extension: `.obj` instead of `.o`
- Static lib tool: `lib.exe` instead of `ar`
- Linker: `link.exe` instead of compiler driver
- Flag translation: `-Wall` → `/W4`, `-O2` → `/O2`, etc.

### macOS

- Use `sysctlbyname("hw.logicalcpu")` for CPU count
- Shared lib extension: `.dylib`
- Clang is the default compiler (Xcode)

### FreeBSD

- Similar to Linux; uses `sysconf(_SC_NPROCESSORS_ONLN)` for CPU count
- GCC or Clang available via ports

---

## 12. Debugging Tips

### Verbose Build Output

```sh
now build -v
```

Shows each compiler invocation, file-by-file status, and timing.

### Build from Clean State

```sh
now clean && now build -v
```

### Examine Manifest

The manifest at `target/.now-manifest` is Pasta format — human-readable:

```sh
cat target/.now-manifest
```

### Debug with Single Job

```sh
now build -j1 -v
```

Disables parallel output buffering, making compiler errors easier to read.

### Test a Single Module

The test executable runs all tests. To focus on a specific area, you can comment out other test sections in `main()` temporarily, or just search the output:

```sh
./build/default/bin/now_test 2>&1 | grep -A1 "FAIL"
```

---

## 13. Release Process

1. **Update version** in `CMakeLists.txt`:
   ```cmake
   project(Now VERSION X.Y.Z LANGUAGES C)
   ```

2. **Update version** in `now.c` (the `now_version()` function)

3. **Run full test suite**:
   ```sh
   cmake --preset default && cmake --build build/default
   ./build/default/bin/now_test
   ```

4. **Build for all platforms** (CI handles this)

5. **Tag the release**:
   ```sh
   git tag -a vX.Y.Z -m "Release X.Y.Z"
   git push origin vX.Y.Z
   ```

6. **Update docs/status.md** with new test counts and feature status
