# now — session context

Auto-loaded at the start of every Claude Code session in this project.

## What now is

A native build tool + package manager for C/C++/Rust/Go/Julia, written
in C11 and delivered as a single binary. Maven's model — declarative
descriptor, fixed lifecycle, `group:artifact:version` coordinates,
SemVer resolution, `~/.now/repo` local repo, hosted registry — but
targeting machine code instead of bytecode. Fast because it drives
compilers directly (no Make/Ninja generation), runs a parallel process
pool, and does incremental builds off a SHA-256 + mtime manifest with a
local and optional remote cache.

Self-hosting: `now` builds itself from its own `now.pasta`.

## Team

now, **gut**, **cookbook** and **now-action** are one team, not four.
Sibling checkouts live at `../gut`, `../cookbook`, `../now-action`.
External peers we coordinate with but do not own: **apennines** (the
vendored runtime, canonical source at
`C:/Users/Iridium/Projects/apennines`), **basta** / **alforno** (the
pasta parser and processor, vendored under `lib/`), plus lab, vulpes,
starletc, archaeovm, megano, granzeboma, and **Amy** (AmigaOS for
RISC-V — our most demanding freestanding consumer).

## Build

The toolchain is bundled inside CLion. `C:\Users\Iridium\toolchain\mingw`
is a junction to it and sits first on the user PATH, so a plain shell
gets **gcc 15.2.0**. After a CLion upgrade the junction points at the
old install — repoint it (unelevated):

```
cmd /c rmdir C:\Users\Iridium\toolchain\mingw && cmd /c mklink /J C:\Users\Iridium\toolchain\mingw "C:\Program Files\JetBrains\CLion <ver>\bin\mingw"
```

To resolve it directly without depending on the junction:

```
export PATH="$(ls -d '/c/Program Files/JetBrains/CLion '*/bin/mingw/bin | tail -1):$PATH"
```

**A WinLibs gcc 14.2.0 is still later on PATH** (winget). Anything that
resets PATH gets that one instead, and mixing the two across builds is
a real failure mode — 14 and 15 disagree on how `stat` maps to a CRT
symbol, so a mixed link dies on `undefined reference to stat64i32`.
`now` now recompiles on a compiler change rather than reusing the
objects, but a *partial* PATH is still worth checking first when a
build fails oddly.

```
now build -j 32          # 16C/32T machine — use it
now test  -j 32
```

CMake + Ninja is the canonical build (CI uses it); `now build` is the
self-host path. A prebuilt bootstrap sits at `build/static/bin/now.exe`
for when `target/bin/now.exe` is missing or broken.

**Build both ways before believing a build works.** They disagree
regularly — CMake uses explicit source lists while `now build` walks the
tree, so a source added to a vendored lib links fine under one and dies
on an undefined reference under the other. Four instances in three days.

## Installing

`../toolkit` is on the user PATH, so whatever sits there is the `now`
every project on this machine gets. Do not copy it by hand:

```
tools/install-toolkit.sh          # builds static, verifies, installs
```

It refuses to install if the binary is not self-contained, is missing a
command, or fails its own suite — a stale toolkit is better than a
half-updated one.

**Peers who cross-compile cannot use that binary.** Amy builds a RISC-V
freestanding kernel under WSL; a Windows `now.exe` cannot drive a
toolchain that does not exist on Windows. For them:

```
tools/install-wsl.sh              # run inside WSL; installs ~/.local/bin/now
```

It stamps the source revision into `~/.local/bin/.now-revision`. That
exists because their `now` used to be a one-time `cp -r` with no way to
show its age — they sat six days behind and filed a bug that was already
fixed, and it read as a live defect until someone checked the date.

**CMake defaults `BUILD_SHARED_LIBS` to ON here** (`CMakeLists.txt:8`),
and the shared build hides vendored symbols. Anything the CLI needs must
cross the boundary through a `NOW_API` function of ours — `main.c` once
called apennines' `entropy_get_system` directly and the default Linux
build would not link.

## Gotchas

- **The spec runs well ahead of the implementation.** `profiles:` and
  `properties:` are parsed by nobody — zero code, no `-p` flag — yet
  every project in the ecosystem ships a `profiles:` block that does
  nothing. `docs/status.md` claiming "all tiers complete, zero backlog"
  is not accurate. **Grep the source before telling a downstream team a
  field works.**
- **Descriptor key diagnostics.** `warn_descriptor_keys()` in
  `now_pom.c` warns on unknown keys and, separately, on recognized-but-
  unimplemented ones. Silent acceptance is what let a correct
  `inherit_defaults` block sit inert in Amy's workspace for weeks. When
  you add or implement a field, update `k_known_keys` / `k_inert_keys`.
- **The version lives in two places**: `now.pasta` and
  `now_version()` at `src/main/c/now.c:20`. Bump both — the binary
  reports the latter.
- **Trailing commas are not legal Pasta** — the format owner ruled on
  this, and spec §23.1 was wrong, not the parser. Keep descriptors
  comma-clean. `//` comments are JSON5, not Pasta (`;`), though the
  parser tolerates them.
- **`lib/alforno`** is at `078be82`; the descriptor problems that
  pinned it to `e28f82f` were fixed upstream on 2026-08-11.
- **Changing a struct no longer needs a clean rebuild.** This entry used
  to say it did. That was a bug wearing a rule's clothing: production
  objects tracked headers, test objects did not, so a struct edit
  rebuilt half the test binary and left the other half compiled against
  the old layout — segfault. Fixed 2026-08-16; test objects now have
  their own manifest at `target/.now-manifest-test`. If you see a
  layout-shaped crash after a header edit, it is a real regression now,
  not the tool being like that.
- **`now test` cannot link now's own suite** — test objects don't
  inherit `compile.defines`, so they miss `NOW_STATIC` and expect
  `__imp_` DLL imports. Pre-existing. gut's tests are unaffected.
- **Two loaders duplicate the parsing**: `now_project_load()` and
  `now_project_load_string()`. Only the former parses the workspace
  inheritance fields.
- **Paths inside `link.flags` are passed verbatim** and resolve against
  the *`now` process CWD*, not the module directory. `link.libdirs`,
  `link.archives` and `link.script` are resolved against the module
  dir — prefer them.

## Mailbox

Peer teams coordinate through an append-only Markdown bus at
`~/.claude/mailbox/`. Full spec: `~/.claude/mailbox/interconnection-protocol.md`.

```
~/.claude/mailbox/
  from-now.md         ← we append here for broadcasts
  to-now.md           ← peers append here; our inbox
  to-<peer>.md        ← we append here for directed requests
  from-<peer>.md      ← that peer appends; we read
```

Directed traffic goes in the destination's `to-<peer>.md`; broadcasts
go in `from-now.md`. Major announcements land in both.

```
### [YYYY-MM-DD HH:MM] TOPIC — short summary

Status: INFO | REQUEST | RESPONSE | ACK
Re: (optional — the message being answered)

Body. Concise. What was done / what is needed, file paths and API
signatures, blockers.

— now
```

`INFO` broadcasts state; `REQUEST` expects a `RESPONSE`; `ACK` closes a
thread. Append only — never edit or delete prior messages.

Code handed over for absorption goes in `~/.claude/mailbox/<us>-for-<peer>/`;
the receiver clears it once integrated.

**Since the consolidation, `to-gut` / `to-cookbook` are internal
notes, not cross-team RPC** — don't ceremonially ACK ourselves. Real
negotiation happens with apennines, basta/alforno, and downstream
consumers.

Check cadence: no polling. Check when the user says "mailbox", when a
session opens on a pending thread, or when mid-thread awaiting a reply.
Timestamps UTC — `date -u '+%Y-%m-%d %H:%M'`.

Files are large; use `tail -NN` and `grep -n "^### \["` rather than
reading whole ones.

## Conventions

- Commits: one terse subject line. Branch before committing on `main`.
  Never push without being asked.
- Match surrounding style: C11, 4-space indent, comments explain *why*.
