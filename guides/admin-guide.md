# now — Admin Guide

Configuration, registry management, and organizational policy for `now` deployments.

---

## Table of Contents

1. [Installation and Distribution](#1-installation-and-distribution)
2. [Global Configuration](#2-global-configuration)
3. [Registry (Cookbook)](#3-registry-cookbook)
4. [Credentials Management](#4-credentials-management)
5. [Trust Store](#5-trust-store)
6. [Advisory Database](#6-advisory-database)
7. [Configuration Layers (Enterprise Policy)](#7-configuration-layers-enterprise-policy)
8. [Dependency Confusion Protection](#8-dependency-confusion-protection)
9. [Local Repository](#9-local-repository)
10. [Toolchain Management](#10-toolchain-management)

---

## 1. Installation and Distribution

### Binary Distribution

`now` ships as a single binary (`now` / `now.exe`) plus a shared library (`libnow.so` / `libnow.dll`). Install both to a directory on PATH.

### Supported Platforms

| OS | Architecture | Toolchains |
|----|-------------|------------|
| Linux | x86_64, arm64 | GCC, Clang |
| macOS | x86_64, arm64 | Clang (Xcode), GCC |
| Windows | x86_64 | MSVC, MinGW GCC |
| FreeBSD | x86_64 | GCC, Clang |

### Building from Source

See the [Developer Guide](developer-guide.md) for building `now` from source.

---

## 2. Global Configuration

`now` stores user-level configuration under `~/.now/`:

```
~/.now/
  config.pasta            Global configuration
  credentials.pasta       Registry credentials
  trust.pasta             Trusted signing keys
  repo/                   Local artifact repository
  advisories/             Security advisory database
    now-advisory-db.pasta
    local-overrides.pasta
    .last-updated
  layers/                 Enterprise configuration layers
    *.pasta
```

### config.pasta

```pasta
{
  ; Default registry
  default_registry: "https://registry.now.build/central",

  ; Advisory feed configuration
  advisory: {
    feed:              "https://advisories.now.build/db.pasta",
    feed_key:          "RWT...",
    update_policy:     "on-procure",
    severity_threshold: "high"
  },

  ; Offline mode default
  offline: false
}
```

---

## 3. Registry (Cookbook)

`now` uses the **Cookbook** registry server for hosting and resolving artifacts.

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/healthz` | Health check |
| `GET` | `/readyz` | Readiness check |
| `GET` | `/resolve/{group}/{artifact}/{range}` | Resolve version range |
| `GET` | `/artifact/{path}` | Download artifact |
| `PUT` | `/artifact/{path}` | Upload artifact |
| `POST` | `/auth/token` | Obtain auth token |
| `POST` | `/keys` | Register signing key |
| `GET` | `/metrics` | Prometheus metrics |

### Content Negotiation

Cookbook supports multiple formats:

- `application/x-pasta` (default)
- `application/json`
- `?pretty` query parameter for human-readable output

### Storage Backends

| Component | Options |
|-----------|---------|
| Database | SQLite (default), PostgreSQL |
| Artifact storage | Filesystem (default), S3-compatible |

### Running Cookbook

```sh
cookbook serve --port 8080 --db sqlite:///var/lib/cookbook/data.db
```

See the Cookbook documentation for full deployment options.

---

## 4. Credentials Management

Credentials are stored in `~/.now/credentials.pasta`:

```pasta
{
  credentials: [
    {
      url:   "https://registry.now.build",
      token: "eyJhbGciOiJFZERTQSIs..."
    },
    {
      url:   "https://pkg.acme.org/now",
      token: "acme-deploy-token-abc123"
    }
  ]
}
```

Matching is by URL prefix — the first entry whose `url` is a prefix of the registry URL is used. The token is sent as `Authorization: Bearer {token}`.

### Security Notes

- Keep `credentials.pasta` readable only by the current user (`chmod 600`)
- Do not commit credentials to version control
- Use CI-specific tokens with limited scope for automated publishing
- Tokens are obtained via `POST /auth/token` on Cookbook registries

---

## 5. Trust Store

The trust store at `~/.now/trust.pasta` manages signing keys for package verification.

### Managing Keys

```sh
now trust:list                                    # list all keys
now trust:add "*" "RWT..." "Global signing key"   # trust for all packages
now trust:add "org.acme" "RWT..." "Acme Corp"     # trust for org.acme.*
now trust:add "org.acme:core" "RWT..." "Core lib" # trust for exact artifact
```

### Scope Rules

| Scope | Matches |
|-------|---------|
| `*` | All packages |
| `org.acme` | `org.acme`, `org.acme.sub`, `org.acme.deep.nested` |
| `org.acme:core` | Only `org.acme:core` |

Note: `org.acme` does NOT match `org.acmetools` (dot-boundary enforcement).

### Trust Levels

Configure per-project in `now.pasta`:

```pasta
{
  trust: {
    require_signatures: true,    ; SIGNED — reject unsigned packages
    require_known_keys: true     ; TRUSTED — reject unknown publisher keys
  }
}
```

| Level | Behavior |
|-------|----------|
| NONE (default) | SHA-256 integrity only |
| SIGNED | Package must have a valid signature |
| TRUSTED | Signature must be from a key in the trust store |

### Signature Verification

`now` delegates to the `minisign` binary for Ed25519 verification:

```sh
now verify package.tar.gz package.tar.gz.sig
```

---

## 6. Advisory Database

### Database Location

```
~/.now/advisories/
  now-advisory-db.pasta        Main advisory database
  now-advisory-db.pasta.sig    Database signature
  local-overrides.pasta        Local/org-level overrides
  .last-updated                Last pull timestamp
```

### Database Format

```pasta
{
  version:   "1.0.0",
  updated:   "2026-03-05T00:00:00Z",
  source:    "https://advisories.now.build/db.pasta",

  advisories: [
    {
      id:          "NOW-SA-2026-0042",
      cve:         ["CVE-2026-1234"],
      severity:    "critical",
      title:       "Buffer overflow in inflate()",
      affects: [
        { id: "zlib:zlib", versions: [">=1.2.0 <1.3.1"] }
      ],
      fixed_in: [
        { id: "zlib:zlib", version: "1.3.1" }
      ],
      affects_build_time: false,
      affects_runtime:    true
    }
  ]
}
```

### Checking Advisories

```sh
now advisory:check       # check project deps against advisory DB
```

### Severity Blocking

| Severity | Default | Overridable? |
|----------|---------|-------------|
| `blacklisted` | Hard error | No |
| `critical` | Error | Yes |
| `high` | Error | Yes |
| `medium` | Warning | Escalate to error |
| `low` | Warning | Escalate to error |
| `info` | Silent | Escalate to warning |

### Overrides

Projects can acknowledge known advisories in `now.pasta`:

```pasta
{
  advisories: {
    allow: [
      {
        advisory:    "NOW-SA-2026-0042",
        dep:         "zlib:zlib:1.3.0",
        reason:      "inflate() not called — deflate-only usage",
        expires:     "2026-06-01",
        approved_by: "security@acme.org"
      }
    ]
  }
}
```

- `expires` is **mandatory** — overrides without expiry are rejected
- Expired overrides fail the build with a reminder to re-evaluate
- `blacklisted` advisories cannot be overridden

---

## 7. Configuration Layers (Enterprise Policy)

Organization-wide policies are enforced through configuration layers placed in `~/.now/layers/`.

### Creating a Layer

```pasta
; ~/.now/layers/acme-enterprise.pasta
{
  layer: "acme-enterprise",
  sections: [
    {
      name:        "compile",
      policy:      "open",
      description: "Baseline compile settings"
    },
    {
      name:        "toolchain",
      policy:      "locked",
      description: "Standardized toolchain — do not override"
    },
    {
      name:        "advisory",
      policy:      "locked",
      description: "Security policy — advisory checking required"
    }
  ],

  compile: {
    warnings: ["Wall", "Wextra"]
  },

  toolchain: {
    cc: "/opt/gcc-13/bin/gcc"
  }
}
```

### Section Policies

| Policy | Behavior |
|--------|----------|
| `open` | Project can add to or override values |
| `locked` | Project cannot modify; violations reported by `now layers:audit` |

### Inspecting Layers

```sh
now layers:show              # display layer stack
now layers:show --effective  # show merged configuration
now layers:audit             # report policy violations
```

---

## 8. Dependency Confusion Protection

Prevent private packages from being resolved from public registries:

```pasta
{
  private_groups: ["com.acme", "com.acme.internal"]
}
```

When a dependency's group matches a `private_groups` prefix, `now procure` will **only** resolve it from private registries (not the public central registry). This prevents supply chain attacks where an attacker publishes a package with the same name as your internal dependency.

Matching uses dot-boundary rules: `com.acme` matches `com.acme.sub` but NOT `com.acmetools`.

---

## 9. Local Repository

The local repo at `~/.now/repo/` caches installed artifacts:

```
~/.now/repo/
  org/acme/core/1.5.0/
    now.pasta           Descriptor
    h/                  Headers
    lib/linux-x86_64-gnu/  Platform-specific libraries
    bin/linux-x86_64-gnu/  Platform-specific binaries
```

Artifacts are placed here by:
- `now install` — installs from current project
- `now procure` — downloads and extracts from registry

The local repo is always checked before querying remote registries.

---

## 10. Toolchain Management

`now` resolves toolchains from environment variables with sensible defaults:

| Variable | Default (POSIX) | Default (Windows) |
|----------|----------------|-------------------|
| `CC` | `cc` | `cl.exe` (if in VS prompt) |
| `CXX` | `c++` | `cl.exe` |
| `AR` | `ar` | `lib.exe` |
| `AS` | `as` | `ml64.exe` |

### MSVC Detection

`now` auto-detects MSVC when:
- `CC` is set to `cl` or `cl.exe`
- Running inside a Visual Studio Developer Command Prompt

Flag translation happens automatically — you write portable `now.pasta` and `now` generates the correct flags for each toolchain.

### Cross-Compilation

```sh
now build --target linux:arm64:gnu
```

Set `CC` and `CXX` to your cross-compiler toolchain (e.g., `aarch64-linux-gnu-gcc`).
