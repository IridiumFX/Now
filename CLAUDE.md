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

Toolchain is bundled inside CLion; nothing is on PATH by default.
Resolve it without hardcoding a version — the IDE upgrades break any
pinned path:

```
export PATH="$(ls -d '/c/Program Files/JetBrains/CLion '*/bin/mingw/bin | tail -1):$PATH"
```

```
now build -j 32          # 16C/32T machine — use it
now test  -j 32
```

CMake + Ninja is the canonical build (CI uses it); `now build` is the
self-host path. A prebuilt bootstrap sits at `build/static/bin/now.exe`
for when `target/bin/now.exe` is missing or broken.

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
- **Trailing commas break parsing** even though spec §23.1 lists them
  as legal Pasta. The parser is basta's
  (`lib/basta/src/main/c/basta_parser.c:212`), an external repo. Keep
  descriptors comma-clean until that is fixed upstream. `//` comments
  are JSON5, not Pasta (`;`), though the parser tolerates them.
- **`lib/alforno` cannot be advanced** past `e28f82f`: every commit
  from `f176e12` onward carries a trailing comma in its own `now.pasta`
  that our parser rejects.
- **Changing `struct NowProject`'s layout needs a clean rebuild**
  (`rm -rf target`). A partial rebuild once left stale objects reading
  `_pasta_root` at the old offset, segfaulting in
  `now_repro_from_project`. Header dependency tracking is otherwise
  sound.
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
