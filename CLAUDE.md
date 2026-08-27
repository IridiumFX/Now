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
self-host path. The presets build into `build/{presetName}`, so it is
`--build --preset`, not `--build build`:

```
cmake --preset default && cmake --build --preset default -j 32
./build/default/bin/now_test.exe
```

`cmake --build build` targeted a pre-relocation cache at `build/` and
failed with *"CMakeCache.txt directory is different"*, which reads like
a corrupted checkout. That cache is gone, along with the ones under
`build/clang`, `build/winci`, `build/apennines` and `build/static`.

A prebuilt bootstrap sits at `build/static/bin/now.exe` for when
`target/bin/now.exe` is missing or broken — **use it to build, never to
`now test`.** It is from May and predates rc6, so it passes neither
`compile.defines` nor `compile.warnings` to the test compile: the suite
then builds `now_test.c` without `NOW_STATIC` and the link dies on a
page of `__imp_` thunks that has nothing to do with whatever you just
changed. Build with the bootstrap because `now` cannot relink the binary
it is running; test with `target/bin/now.exe`.

**Build both ways before believing a build works.** They disagree
regularly — CMake uses explicit source lists while `now build` walks the
tree, so a source added to a vendored lib links fine under one and dies
on an undefined reference under the other. Five instances now: cookbook
had been missing the vendored `tcp_resolve.c` from its CMake list since
the file appeared.

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

## No fixed-size arrays for variable-length data

`now` is a host tool with an allocator. A bound chosen because it looked
big enough is a guess, and **a guess enforced by producing less is the
worst failure mode available**: the build succeeds, against the wrong
inputs, with exit status 0.

We had it in the compile path — `tmp_argv[128]` fed by loops capped at
32 warnings / 64 defines / 32 includes, plus two contributors with no cap
at all. Both halves were wrong. The caps dropped flags silently, which is
how a `-D` goes missing and the objects come out with the wrong macro
world; and the caps were also the only thing holding the writes in
bounds, which they did not — 32 + 64 + 32 is already 128. A descriptor
with 70 defines and 60 flags overflowed the array and reached gcc as a
corrupted command line. The CMake importer had the same shape and would
emit a `now.pasta` missing entries that looked perfectly well-formed.

So:

- **Size to the input.** `malloc(count + headroom)`, or use `NowStrArray`
  / `NowFileList`, which already grow. The allocation is nothing next to
  spawning a compiler.
- **A bound from a platform or a format is a fact, and stays.**
  `MAXIMUM_WAIT_OBJECTS` is 64 because `WaitForMultipleObjects` says so.
  Keep those, name them, and **report** when input exceeds them — never
  absorb it.
- **Never `if (n < CAP) arr[n++] = x;` with no else.** If the bound is
  real, the else is an error path. If there is no error path, the bound
  should not be there.

Credit where due: Amy raised this from their own `.sfd` generator, where
a 40-parameter prototype came out with 24 arguments — and *compiled*.

## Gotchas

- **The spec runs well ahead of the implementation.** `profiles:` and
  `properties:` are parsed by nobody — zero code, no `-p` flag — yet
  every project in the ecosystem ships a `profiles:` block that does
  nothing. `docs/status.md` claiming "all tiers complete, zero backlog"
  is not accurate. **Grep the source before telling a downstream team a
  field works.**
- **`outputs:` — several artifacts from one set of objects.** A module
  produced exactly one thing until 2026-08-25, so two programs sharing a
  directory had to be two modules with two descriptors repeating one
  compile configuration for sources already sitting together.

  ```
  outputs: [
    { type: "executable", name: "server", entry: "src/main/c/server.c" }
    { type: "executable", name: "client", entry: "src/main/c/client.c" }
    { type: "static",     name: "core" }
  ]
  ```

  **Nothing lists object files.** An executable links its `entry`
  object plus every object that defines no entry point; a library takes
  the no-entry-point set. Which is which comes from
  `now_obj_defines_symbol()`, not from the filename — the same rule
  that fixed the entry-point bug the day before. So `server` and
  `client` share `core.c.o` and neither drags in the other's `main()`.

  An object whose symbol table cannot be read is kept in every artifact
  rather than dropped: dropping it would remove a symbol from
  everything with no error anywhere. It warns.

  Everything is validated **before anything links** — an entry that
  produced no object, an entry that defines no `main()`, two outputs
  claiming one entry, and an executable with no `entry:` in a module
  with more than one `main()`. Each is refused with a message naming
  the descriptor, because the alternative is the linker complaining
  about a duplicate `main` the author never wrote. `entry:` may be
  omitted when the module has exactly one `main()`, which is what every
  single-output descriptor has always meant. An entry point no output
  claims warns — it is a program that will not be built.

  **`output:` singular is untouched.** A descriptor that never says
  `outputs:` takes the same code path it always did; `now` itself is
  the standing proof, since its own descriptor is single-output.
  `build_link_body` now takes a `NowLinkTarget` (what to link, what to
  call it) instead of reading `p->output` and `ctx->objects` directly —
  that is the whole reason it can run more than once.

- **`now build` with no `now.pasta` compiles to objects and stops.**
  It used to exit 3. It now walks the tree, compiles every source it
  recognises, and says what it did not do:

  ```
  compiled 3 (32-way parallel), skipped 0 (up to date)
  no now.pasta - nothing linked
  2 objects define main
  add a now.pasta to say what to build from them
  ```

  Stopping is the design. **Compiling is the last step whose failure is
  loud** -- a missing include, a bad `-D`, a syntax error, the compiler
  shouts. Everything past it fails quietly: which objects group into
  which artifact, static versus shared, what gets exported, whether a
  symbol resolved from somewhere unintended. Those produce a successful
  build of the wrong thing. So zero-config goes exactly as far as the
  work verifies itself and no further.

  That is also why no directory convention had to be invented. The
  walker never guesses an artifact kind, so `src/main/c` and a flat tree
  compile identically; the difference only appears once something asks
  to be linked, and by then there is a descriptor to ask. Languages are
  *detected* (`now_lang_all_ids` + `now_lang_classify`), only for types
  that produce objects -- a tree of headers is not a C project with
  nothing to compile.

  Configuration is the same three sources as any build. Include paths
  are **not** inferred from the layout: a guessed `-I` that happens to
  work is how a tree acquires a dependency nobody wrote down.

  `sources.exclude` keeps `target/**` out of the walk. Objects there are
  harmless (the walk looks for source extensions), but a *generated*
  `.c` is not -- without the exclusion every run compiles the previous
  run's generated sources plus its own, and it compounds. The test
  plants one deliberately; a version that only checked object counts
  passed while watching nothing.

- **Three sources configure a build, in this order.** Lowest priority
  first, each appended after the last so the compiler's last-flag-wins
  gives the top of the list the final say:

  ```
  now-baseline          (claims nothing)
  .now-layer.pasta ...  (farthest ancestor first)
  now.pasta             (the descriptor, after workspace inheritance)
  CFLAGS / LDFLAGS      (the POSIX names, read for compatibility)
  NOW_CFLAGS / NOW_LDFLAGS
  --cflags / --ldflags
  ```

  Env and CLI are implemented as **layers**, which is most of what made
  wiring the layer stack worth doing: precedence is stack order, the
  merge rules are the ones already tested, and `tell config-origin`
  names them without knowing they exist. One layer per VARIABLE, not
  one per tier -- a value from `LDFLAGS` reported as coming from
  `CFLAGS` sends someone to unset the wrong thing.

  Reading bare `CFLAGS` is a deliberate risk: a shell that has had it
  exported for something else quietly affects a `now` build. The
  mitigation is not that it cannot happen, it is that the answer is one
  command away:

  ```
  now tell config-origin
  now tell config-origin compile.flags
  ```

  Flag strings split on whitespace **honouring double quotes**, because
  `-I"C:\Program Files\x\include"` is an ordinary include path on this
  machine and splitting it on spaces yields three flags, none of which
  is a directory -- which the compiler then reports as a missing header
  rather than as a broken flag.

  `now_layer_set_cli_flags()` is called once from `main.c` alongside the
  other process-wide settings and **before any branch**. The first
  attempt put it in the build path only, so `tell config-origin`
  reported on a stack the build would not have used. For the same
  reason `tell` applies layers to its own project: a report that omits
  two of the three sources describes a build that is not the one that
  runs.

- **`.now-layer.pasta` reaches the build, and the gate is the point.**
  §25 cascading layers configured a report and nothing else until
  2026-08-25 -- `now_layer_merge_section()` had two callers and both
  were the `layers:*` commands. `now_layer_apply_to_project()` is the
  seam now; it is called once in `main.c` after the project loads, and
  once per module in `now_workspace.c` after root inheritance, so
  layers are the outermost ring and the module's own descriptor still
  wins.

  Two rules hold it together, and both are load-bearing:

  1. **No layer file anywhere above the project means nothing happens.**
     Not "an equivalent result" -- the function returns before touching
     anything. Same discipline as the workspace inheritance gate.
  2. **The built-in baseline claims no compile defaults.** It used to
     declare `warnings: [Wall, Wextra]` and `opt: debug`, and
     `now_build.c` applies neither unless the descriptor asks -- it maps
     Wall/Wextra only when they are already in `compile.warnings` and
     emits `-Og` only when `opt` is set. Wiring layers in while the
     baseline still said that would have handed three unrequested flags,
     including an optimisation level, to every project on the machine.
     A baseline is documentation; documentation that is false is worse
     than none.

  These two are redundant *by design*, and that has a testing
  consequence worth knowing: with an honest baseline, removing the gate
  changes no observable behaviour, so a control that only removes the
  gate comes back green. The control that proves the gate does anything
  has to dirty the baseline **and** remove the gate. If you touch either
  mechanism, run that conjunction.

  `_policy: "locked"` in a layer section makes it additive: lower
  layers may add, the project may not remove or replace, and every
  attempt is recorded as NOW-W0401. A build warns and proceeds;
  `--strict` refuses. Locked never silently discards the project's own
  values -- a locked section that broke builds instead of governing
  them would just get deleted.

- **A changed object list forces a relink; a newer artifact does not
  excuse it.** `link_flags_hash` covers the object list, so it is the
  only thing that notices a source being added or removed -- the mtime
  check cannot, because a deleted file has no mtime to be newer than.

  It was neutered from 2026-08-?? to 2026-08-25: compile handed the link
  phase `ctx->last_link_flags_hash` *after* writing the new value into
  the manifest, so the link compared the current hash against itself,
  which is equal by construction. The link then skipped whenever its
  mtime check said "up to date", and the previous artifact survived --
  including the `remove()` before `ar rcs` that exists precisely to stop
  objects accumulating. Deleting a source left its object in the
  archive; the same hole would keep a stale executable.

  Intermittent only because the mtime check decided it: when the
  artifact happened to look older than an object, the link ran and hid
  the bug. It failed about 1 run in 18 of its own test, which is why it
  survived several sessions. **Hand the link phase the PREVIOUS hash**,
  captured before the manifest is updated.

  `test_build_relinks_when_the_object_list_changes` is the guard, and it
  is deliberately not the flaky one: it sets the artifact's mtime 30s
  into the future with `utime()`, so "up to date" is certain and the
  hash comparison is the only thing left. With the fix reverted it fails
  5 runs out of 5. Its first version touched the archive with
  `fopen(..., "ab")`, which does not move the mtime on Windows when
  nothing is written -- so it inherited the timing-dependence it existed
  to remove and caught the bug only 2 in 5. **A test that assumes its
  own setup worked is the same defect as a build that assumes its own
  delete worked.**

- **The dirwalk cache is disabled on Windows, and the reason is
  measured.** It skips `readdir()` when a directory's mtime is
  unchanged, which assumes the mtime moves when the contents change.
  POSIX requires that on link/unlink. Windows does not do it -- on this
  machine, NTFS:

  ```
  deleting a file left the directory mtime unchanged  165/300
  adding   a file left the directory mtime unchanged  112/200
  ```

  and it never catches up (same counts after 50ms and 500ms, so it is
  not a lazy flush to wait out). A stale entry stays stale. That means a
  source ADDED to a directory could be silently not compiled, which is
  the worse half. Turning it off cost 1 second on a 70-second suite
  full of builds. Do not re-enable on mtime alone; a USN-journal or
  change-notification signal would be the way.

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
- **`now test` links and passes now's own suite** — 430/430, re-measured via the self-host path on 2026-08-25 (was 345/345 on
  a clean `target/` on 2026-08-19). This entry used to say it could not,
  because test objects missed `NOW_STATIC` and expected `__imp_` DLL
  imports; `now_build.c:4138` now builds them with production flags plus
  the test-only defines, so they get it. A false gotcha is worse than an
  absent one — it stops people running the self-host suite at all.
- **The unit suite cannot see the registry leg.** Every component on it
  is tested and the seams between them were not: on 2026-08-20, 352
  green tests coexisted with an Ed25519 that got 14% of signatures
  wrong, a `require_signatures` that no publish could satisfy, and a
  `procure` that never unpacked what it downloaded — so **nobody had
  ever compiled against a fetched dependency**. Run
  `tools/registry-roundtrip.sh <artifact-name>` against a live cookbook
  before believing anything about publish/procure. Fresh name per run:
  a re-published coordinate keeps the old artifact and each run makes a
  new signing key, which reads exactly like a signature defect.
- **Register the publisher key before publishing, not after.** cookbook
  verifies a `.sig` against the keys registered for its group, so the
  moment *any* key exists for a group, a publisher whose key it has
  never seen is refused. `now keys:register [--registry URL] [--group G]`
  sends it; both flags default to the `now.pasta` in the directory. This
  is not hypothetical — the round trip registered a key at step 11 and
  the next run's step 3 started failing, because the registry remembered
  and the new run had a new key.
- **Crypto comes from apennines.** `now_ed25519.c` is a thin surface
  over `t2/crypto/ec.c`. Do not reintroduce a second implementation of a
  primitive — the one nobody cross-checks is the one that is wrong.
- **Two loaders duplicate the parsing**: `now_project_load()` and
  `now_project_load_string()`. Only the former parses the workspace
  inheritance fields.
- **`--fail-fast` is compile-only.** `g_fail_fast` is read in the parallel
  compile scheduler and nowhere else — it does not stop a test run at the
  first failing binary, in either test mode. The help wording ("stop
  starting new compiles") is accurate; the flag name is not, and it read
  as a test flag to a peer who needed one.
- **`now test` runs ONE binary by default and counts it as one test.**
  `tests: { mode: "each" }` / `"per-file"` builds one binary per test
  source instead. This decides what a `test.failed` event can name — the
  suite binary, or the file. Both were undocumented until 2026-08-24.
- **Fields that work and were in no document** are as dangerous as fields
  that are documented and do nothing. `tests.exclude` and `tests.mode`
  were both live and unwritten while `tests.pattern` was written and
  dead. A peer found `exclude` by trying it and then checking whether
  the runners had really stopped being built.
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
