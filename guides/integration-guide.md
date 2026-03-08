# now — Integration Guide

Integrating `now` with CI/CD systems, IDEs, and existing build infrastructure.

---

## Table of Contents

1. [CI/CD Integration](#1-cicd-integration)
2. [Structured Output](#2-structured-output)
3. [Exit Codes](#3-exit-codes)
4. [GitHub Actions](#4-github-actions)
5. [GitLab CI](#5-gitlab-ci)
6. [Jenkins](#6-jenkins)
7. [IDE Integration](#7-ide-integration)
8. [Build System Coexistence](#8-build-system-coexistence)
9. [Docker](#9-docker)
10. [Package Managers](#10-package-managers)

---

## 1. CI/CD Integration

`now` has first-class CI support via the `now ci` command, which runs the full build + test lifecycle with structured output and CI-aware behavior.

```sh
now ci                          # build + test with auto-detected CI settings
now ci --output json            # JSON output for parsing
now ci --output pasta           # Pasta format output
now ci --locked                 # fail if lock file is inconsistent
now ci --offline                # no network access (deps must be cached)
```

### CI Environment Detection

`now ci` auto-detects common CI environments:

| CI System | Detection |
|-----------|-----------|
| GitHub Actions | `GITHUB_ACTIONS=true` |
| GitLab CI | `GITLAB_CI=true` |
| Jenkins | `JENKINS_URL` set |
| Travis CI | `TRAVIS=true` |
| CircleCI | `CIRCLECI=true` |
| Azure Pipelines | `TF_BUILD=True` |

When running in CI, `now` adjusts behavior:
- Enables structured output by default
- Disables interactive prompts
- Reports build/test metrics

---

## 2. Structured Output

### JSON Format

```sh
now ci --output json
```

Build result:

```json
{
  "phase": "build",
  "status": "success",
  "duration_ms": 1234,
  "sources": { "total": 10, "compiled": 3, "skipped": 7 }
}
```

Test result:

```json
{
  "phase": "test",
  "status": "fail",
  "duration_ms": 567,
  "tests": { "total": 42, "passed": 40, "failed": 2, "skipped": 0 }
}
```

### Pasta Format

```sh
now ci --output pasta
```

```pasta
{
  phase: "build",
  status: "success",
  duration_ms: 1234,
  sources: { total: 10, compiled: 3, skipped: 7 }
}
```

### Text Format

```sh
now ci --output text     # default
```

Human-readable output with optional ANSI colors (disable with `--no-color`).

---

## 3. Exit Codes

`now` uses structured exit codes for CI integration:

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Build error (compile/link failure) |
| 2 | Test failure |
| 3 | Dependency resolution failure / config error |
| 4 | Configuration error (invalid now.pasta) |
| 5 | I/O error (disk/network) |
| 6 | Authentication failure |
| 7 | Blocked by security advisory |

Use these codes for conditional CI steps:

```sh
now ci
case $? in
  0) echo "All good" ;;
  1) echo "Build failed" ;;
  2) echo "Tests failed" ;;
  7) echo "Security advisory blocks this build" ;;
esac
```

---

## 4. GitHub Actions

### Basic Workflow

```yaml
name: Build and Test
on: [push, pull_request]

jobs:
  build:
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4

      - name: Install now
        run: |
          curl -LO https://github.com/IridiumFX/now/releases/latest/download/now-${{ runner.os }}-x86_64.tar.gz
          tar xzf now-*.tar.gz
          echo "$PWD" >> $GITHUB_PATH

      - name: Build and Test
        run: now ci --output json --locked

      - name: Package
        if: github.ref == 'refs/heads/main'
        run: now package
```

### Caching Dependencies

```yaml
      - name: Cache now repo
        uses: actions/cache@v4
        with:
          path: ~/.now/repo
          key: now-deps-${{ hashFiles('now.lock.pasta') }}

      - name: Build
        run: now ci --locked
```

### Publishing on Tag

```yaml
  publish:
    if: startsWith(github.ref, 'refs/tags/v')
    needs: build
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install now
        run: |
          curl -LO https://github.com/IridiumFX/now/releases/latest/download/now-linux-x86_64.tar.gz
          tar xzf now-*.tar.gz && echo "$PWD" >> $GITHUB_PATH
      - name: Publish
        env:
          NOW_TOKEN: ${{ secrets.REGISTRY_TOKEN }}
        run: now publish --repo https://registry.now.build/central
```

---

## 5. GitLab CI

```yaml
stages:
  - build
  - test
  - publish

variables:
  NOW_VERSION: "0.1.0"

.install_now: &install_now
  before_script:
    - curl -LO https://github.com/IridiumFX/now/releases/download/v${NOW_VERSION}/now-linux-x86_64.tar.gz
    - tar xzf now-*.tar.gz && mv now /usr/local/bin/

build:
  stage: build
  <<: *install_now
  script:
    - now build --locked -v
  artifacts:
    paths:
      - target/

test:
  stage: test
  <<: *install_now
  script:
    - now test --output json
  dependencies:
    - build

publish:
  stage: publish
  <<: *install_now
  script:
    - now publish --repo $REGISTRY_URL
  only:
    - tags
```

---

## 6. Jenkins

### Jenkinsfile

```groovy
pipeline {
    agent any
    stages {
        stage('Build') {
            steps {
                sh 'now build --locked -v'
            }
        }
        stage('Test') {
            steps {
                sh 'now ci --output json'
            }
        }
        stage('Package') {
            when { branch 'main' }
            steps {
                sh 'now package'
                archiveArtifacts artifacts: 'target/pkg/*.tar.gz'
            }
        }
        stage('Publish') {
            when { buildingTag() }
            steps {
                sh 'now publish --repo https://registry.now.build/central'
            }
        }
    }
    post {
        always {
            sh 'now clean'
        }
    }
}
```

---

## 7. IDE Integration

### CLion

CLion can work with `now` projects via the exported CMakeLists.txt:

```sh
now export:cmake
```

Open the generated `CMakeLists.txt` as a CMake project in CLion. Re-run `now export:cmake` after changing `now.pasta`.

### VS Code

Use `now export:cmake` and the CMake Tools extension, or configure a custom task:

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "now build",
      "type": "shell",
      "command": "now build -v",
      "group": { "kind": "build", "isDefault": true },
      "problemMatcher": "$gcc"
    },
    {
      "label": "now test",
      "type": "shell",
      "command": "now test -v",
      "group": "test",
      "problemMatcher": "$gcc"
    }
  ]
}
```

### Visual Studio

For MSVC projects, use the Developer Command Prompt and `now` directly:

```sh
now build -v        # auto-detects MSVC
now export:cmake    # or export for VS CMake integration
```

### Generic (Makefile)

```sh
now export:make     # generates standard Makefile
```

Any IDE that supports Makefile projects can use this directly.

---

## 8. Build System Coexistence

`now` can coexist with other build systems in the same project. The export commands create standalone files:

| Scenario | Approach |
|----------|----------|
| Migrating from CMake | Keep `CMakeLists.txt`, add `now.pasta`, build with either |
| Migrating from Make | Keep `Makefile`, add `now.pasta`, build with either |
| Dual build system | Use `now` for development, export for release builds |
| Gradual adoption | Start with `now export:cmake`, switch to `now build` when ready |

The `target/` directory is `now`-specific and won't conflict with CMake's `build/` or Make's default output.

---

## 9. Docker

### Multi-Stage Build

```dockerfile
# Build stage
FROM gcc:13 AS builder
COPY --from=now-releases /now /usr/local/bin/now
WORKDIR /app
COPY . .
RUN now build --locked -j$(nproc)

# Runtime stage
FROM debian:bookworm-slim
COPY --from=builder /app/target/bin/myapp /usr/local/bin/
CMD ["myapp"]
```

### Caching Dependencies

```dockerfile
# Cache layer for dependencies
COPY now.pasta now.lock.pasta ./
RUN now procure --locked

# Build layer (only rebuilds when source changes)
COPY src/ src/
RUN now build --locked
```

---

## 10. Package Managers

### System Package Integration

`now install` places artifacts in `~/.now/repo/`. For system-wide installation:

```sh
now build
sudo install -m 755 target/bin/myapp /usr/local/bin/
sudo install -m 644 target/bin/libmylib.so /usr/local/lib/
sudo cp -r src/main/h/* /usr/local/include/
sudo ldconfig  # Linux: update shared library cache
```

Or use the generated Makefile:

```sh
now export:make
make install PREFIX=/usr/local
```

### pkg-config

For libraries, consider generating a `.pc` file alongside your build output. This is not yet automated by `now` but can be added as a generate-phase plugin.
