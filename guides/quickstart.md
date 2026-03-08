# now — Quickstart Guide

Get from zero to a working C/C++ project in under 5 minutes.

---

## Install

Download the `now` binary for your platform from the releases page and add it to your PATH.

```sh
# Linux / macOS
curl -LO https://github.com/IridiumFX/now/releases/latest/download/now-linux-x86_64.tar.gz
tar xzf now-linux-x86_64.tar.gz
sudo mv now /usr/local/bin/

# Windows (PowerShell)
Invoke-WebRequest -Uri https://github.com/IridiumFX/now/releases/latest/download/now-windows-x86_64.zip -OutFile now.zip
Expand-Archive now.zip -DestinationPath .
Move-Item now.exe C:\tools\  # ensure C:\tools is in PATH
```

Verify:

```sh
now version
```

---

## Create a Project

Create a directory with a `now.pasta` descriptor:

```sh
mkdir hello && cd hello
```

Create `now.pasta`:

```pasta
{
  group:    "io.example",
  artifact: "hello",
  version:  "1.0.0",
  langs:    ["c"],
  std:      "c11",
  output:   { type: "executable", name: "hello" },
  compile:  { warnings: ["Wall", "Wextra"] }
}
```

Create the source directory and a source file:

```sh
mkdir -p src/main/c
```

Create `src/main/c/main.c`:

```c
#include <stdio.h>

int main(void) {
    printf("Hello from now!\n");
    return 0;
}
```

---

## Build

```sh
now build
```

That's it. Your binary is at `target/bin/hello` (or `target\bin\hello.exe` on Windows).

Run it:

```sh
./target/bin/hello
```

---

## Add Tests

Create `src/test/c/test.c`:

```c
#include <stdio.h>

int main(void) {
    printf("1 test passed\n");
    return 0;  /* 0 = all tests pass */
}
```

Run tests:

```sh
now test
```

---

## Build a Library

Change `now.pasta` to build a shared library instead:

```pasta
{
  group:    "io.example",
  artifact: "mylib",
  version:  "1.0.0",
  langs:    ["c"],
  std:      "c11",
  output:   { type: "shared", name: "mylib" },
  compile:  { warnings: ["Wall", "Wextra"] }
}
```

Output types: `executable`, `static`, `shared`, `header-only`.

---

## Add Dependencies

```pasta
{
  group:    "io.example",
  artifact: "myapp",
  version:  "1.0.0",
  langs:    ["c"],
  std:      "c11",
  output:   { type: "executable", name: "myapp" },

  deps: [
    { id: "org.acme:core:^1.5.0", scope: "compile" },
    { id: "unity:unity:2.5.2",    scope: "test" }
  ]
}
```

Dependencies are resolved, downloaded, and linked automatically:

```sh
now build     # resolves deps, compiles, links
now procure   # resolve deps only (no build)
```

---

## Parallel Builds

```sh
now build -j8       # 8 parallel jobs
now build -j0       # auto-detect CPU count (default)
```

---

## Export to Other Build Systems

If you ever need to stop using `now`, export to CMake or Make:

```sh
now export:cmake    # generates CMakeLists.txt
now export:make     # generates Makefile
```

The generated files are standalone — no dependency on `now` at build time.

---

## Common Commands

| Command | Description |
|---------|-------------|
| `now build` | Full build (procure + compile + link) |
| `now compile` | Compile only (no link) |
| `now test` | Build and run tests |
| `now clean` | Delete `target/` directory |
| `now package` | Create distributable archive |
| `now install` | Install to local repo (`~/.now/repo/`) |
| `now publish --repo URL` | Upload to registry |
| `now version` | Print now version |
| `now -h` | Show help |

---

## Next Steps

- **[User Guide](user-guide.md)** — Full reference for all features
- **[Integration Guide](integration-guide.md)** — CI/CD setup and IDE support
- **[Admin Guide](admin-guide.md)** — Registry and organization config
