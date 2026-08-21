# now events — wire schema v1

Status: **v1 implemented — listener, emitter, sidecar and counts all live; five of the seven event types are wired into the build (§5)**
Written 2026-08-21.

An optional, opt-in stream of build lifecycle events, so that something
waiting on a build learns what happened from a structured message rather
than by tailing a log for a string.

Nothing in `now` emits these by default. There is no listening socket
unless one is asked for, and no datagram leaves the process unless
`--events` names a destination. That is a hard requirement, not a
default: a build tool that opens a socket nobody asked for is
unacceptable in a sandbox.

---

## 1. Why not just read the log

Waiting on `now build` today means matching a string in its output. That
couples the waiter to the exact wording of a human-facing line, gives no
structure to act on, cannot distinguish *"a module just failed"* from
*"the run failed"*, and arrives only when the process exits — so a
30-minute build that broke in its first minute is indistinguishable from
one that is still healthy.

The events below carry the same information as structured data, during
the run.

## 2. Transport

The destination is a URL, so the scheme picks the transport. This is the
existing idiom in this codebase — `url_is_ssh()` in gut, registry scheme
dispatch in `now_procure.c`.

```
now build --events udp://127.0.0.1:9099      # v1
now build --events udp://239.7.7.7:9099      # multicast — v2, see §8
```

**Loopback unicast is the shape v1 supports**, because it covers the case
that motivated this — a process on the same machine waiting on a build —
without the failure modes multicast brings:

- **WSL2 is NAT'd**, and multicast between WSL and the Windows host is
  unreliable. `now` builds and publishes its Linux binary under WSL, so
  this is the first environment it would meet.
- Docker needs explicit configuration; CI runners and corporate networks
  routinely prune multicast with IGMP snooping disabled.
- A multi-homed host makes the sender choose an outgoing interface, and
  the default choice is wrong often enough to become a support burden.
- Anything on the segment can both read the stream and inject into it.
  §8 is what has to be true before that is acceptable.

## 3. Framing

One event per datagram, no trailing newline, UTF-8.

**A datagram MUST NOT exceed 1200 bytes.** That is under the smallest
MTU worth assuming, so no event is ever IP-fragmented — a fragmented
datagram is far likelier to be dropped, and dropping the one that says
the build finished is the failure this design exists to avoid. Emitters
shorten `detail` until the datagram fits and set `detail_lossy`.

### Pasta is the default; json and text are alternatives

**Pasta**, because it has a stricter grammar and one implementation
behind it, so there is far less room for two readers to disagree about
the same bytes. JSON's ambiguities are real — number precision,
duplicate keys, encoding — and this is a stream meant to be acted on
rather than read.

The first draft of this spec chose JSON, on the grounds that Pasta
strings have no escape sequences and a compiler diagnostic is full of
quotes and newlines. That was the wrong trade: it optimised for the
convenience of the *serialiser* over the determinism of the *format*.
Pasta's `"""..."""` form carries quotes and newlines with no escaping
at all, which is a better answer than escaping to begin with.

Selected per sink: `pasta` (default), `json`, `text`.

### A serialiser caveat that is not the format's fault

**`basta_write()` cannot currently serialise these payloads**, measured
2026-08-21 by round-tripping strings through `pasta_write` →
`pasta_parse`:

| input | written as | re-parses |
|---|---|---|
| `error: unknown type name "u64"` | `"error: ... "u64""` | **no** |
| a string ending in `"` | closing quote merges with the delimiter | **no** |
| a string containing `"""` | terminates early | **no** |
| anything with a newline | `"""..."""` | yes |

`basta_writer.c`'s `write_string()` only reaches for the `"""` form when
the string contains a newline, so an ordinary single-line diagnostic
carrying a double quote is emitted unparseably — and `pasta_write()`
returns it without complaint, so only re-parsing catches it.

So `now_events.c` writes its own Pasta rather than building a value tree
and calling the library writer. The rules it follows are:

- free text (`detail`) always goes in `"""..."""`;
- a `"""` inside that text is broken up and `detail_lossy` is set;
- text ending in `"` gets a newline appended and `detail_lossy` is set;
- short fields (ids, names, paths) go in plain `"..."` and cannot
  contain a quote or newline — anything that would is dropped, so a
  corrupt value can never break the framing.

This is reported to the format owner. If the writer is fixed, this
module should move to it — the tests that pin these four cases are what
tells you the move is safe.

### Reading either form

`now events:listen` accepts both without being told which arrived. That
is a dispatch, not a guess: a Pasta event's first key is bare (`{ v: 1`)
and a JSON event's is quoted (`{"v":1`), so each decoder rejects the
other's output outright rather than half-reading it. There is a test for
exactly that property, because a decoder that *partially* accepted the
other format would turn this into sniffing.

### basta, and what it is for

Named here and deliberately not implemented in v1. Its value is not a
fourth spelling of the same map — it is carrying bytes no text form can:
a small binary payload, or a credential a chained step needs. That wants
its own design (who may read it, how it is bounded, whether it belongs
in an event at all) and it is a v1.1 question, not a v1 one.

## 4. Envelope

Every event has these seven fields, in this order:

| field | type | meaning |
|---|---|---|
| `v` | int | schema version. `1`. |
| `run` | string | 12 lowercase hex, unique per `now` invocation |
| `seq` | int | 0-based, increments by 1 per event within a run |
| `ts` | string | UTC, `YYYY-MM-DDTHH:MM:SSZ` — same as the audit log |
| `event` | string | one of §5 |
| `phase` | string | the `now` phase: `build`, `test`, `package`, `publish`, `procure`, … |
| `ok` | bool | false once anything in the run has failed |

**Key order is normative.** It costs nothing to fix now and it is what
makes a canonical byte sequence available to §8 without a version break.

Optional fields, by event:

| field | type | meaning |
|---|---|---|
| `project` | string | `group:artifact:version` |
| `module` | string | source path or module name |
| `detail` | string | human-readable text, truncated to fit |
| `detail_lossy` | bool | present and true when `detail` is not byte-exact — shortened to fit, or altered to suit the format |
| `code` | int | process exit code (`run.finished` only) |
| `counts` | object | `{compiled, skipped, failed, passed, total}` — any subset |
| `elapsed_ms` | int | milliseconds since `run.started` |
| `host` | string | hostname |
| `pid` | int | emitting process id |

`host` and `pid` exist because several builds may share one destination.

## 5. Event types

Seven. The set is deliberately small: an event per compiled file would
flood the socket on a 32-way build and tell nobody anything they need.

The **wired** column is what `now` emits today, not what the schema
allows. A consumer may rely on the wired ones; the rest are defined so
that adding them later is not a version bump.

| event | when | terminal | wired |
|---|---|---|---|
| `run.started` | a `now` invocation begins | | yes |
| `phase.started` | compile / link / test / package / … begins | | compile, test |
| `run.progress` | heartbeat, **at most once per second**, carries `counts` | | yes |
| `module.failed` | one compile or link unit failed | | yes |
| `test.failed` | one test failed | | no |
| `phase.finished` | a phase ended; `ok` says how | | compile |
| `run.finished` | the invocation ended; carries `code` | ✓ | yes |

`counts` is carried by `run.progress`, `phase.finished` and
`run.finished`, assembled from the same tallies the human-readable
summary line prints — so the stream and the terminal can never drift
into describing the same build differently. A field a phase did not
measure stays absent rather than being sent as zero: *"no tests ran"*
and *"zero tests passed"* are different claims.

### A note on `module.failed` and its detail

The event carries the compiler's own diagnostic, which is the reason to
want it rather than the exit code. Getting that reliably needed one
change to the build: `now` normally skips its worker pool for a single
job and lets the compiler write straight to the terminal, so nothing is
captured to put in the event. That is exactly backwards — a one-file
incremental rebuild is the commonest thing a watcher waits on. With
events enabled the pool is used even for one job; with events off the
shortcut is unchanged, and so is what you see in the terminal.

`module.failed` and `test.failed` are the early-warning events: they are
emitted as the failure happens, not collected at the end, which is what
makes "something started failing" observable while the build is still
running.

### What a real build emits today

A healthy 220-file build, one second apart:

```
run.progress   compile   counts {compiled: 1,   total: 220}
run.progress   compile   counts {compiled: 41,  total: 220}
run.progress   compile   counts {compiled: 98,  total: 220}
run.progress   compile   counts {compiled: 182, total: 220}
phase.finished compile   counts {compiled: 220, failed: 0}
run.finished   ok (exit 0)
```

and the incremental rebuild after breaking one file:

```
module.failed  src/main/c/m7.c
               m7.c:1:20: error: unknown type name 'u64'
                   1 | int broken(void) { u64 x = 0; return x; }
                     |                    ^~~
phase.finished compile   counts {compiled: 0, skipped: 219, failed: 1}
run.finished   FAILED (exit 1)  counts {failed: 1, total: 219}
```

and `now events:listen --until run.finished` exits **1**, matching the
build, so a shell branches on it without parsing a line.

### Example

```
{ v: 1, run: "a3f1c9d2e4b6", seq: 7, ts: "2026-08-21T09:41:02Z", event: "module.failed", phase: "compile", ok: false, project: "dev.iridium:gut:0.1.0", module: "src/main/c/remote.c", detail: """remote.c:512:9: error: unknown type name "u64"""" }
```

The same event with `--wire json`:

```json
{"v":1,"run":"a3f1c9d2e4b6","seq":7,"ts":"2026-08-21T09:41:02Z","event":"module.failed","phase":"compile","ok":false,"project":"dev.iridium:gut:0.1.0","module":"src/main/c/remote.c","detail":"remote.c:512:9: error: unknown type name \"u64\""}
```

## 6. Loss, ordering and the rule that matters

UDP does not retransmit and does not order. A chain that waits on
`run.finished` and never receives it waits forever — **worse than
tailing a log, because a log persists.** So:

> **The datagram is a doorbell, not a delivery.**

Three consequences, all normative:

1. **Every event is also appended to `target/.now-events.jsonl`**, one
   JSON object per line, same bytes. That file is the record; the
   datagram is only the notification that it grew. A listener that
   suspects it missed something reads the file. This is the shape
   Bazel's Build Event Protocol settled on too — a stream plus
   `--build_event_json_file`, and the file is what people depend on.
2. **`seq` is contiguous.** A listener seeing 5 then 7 knows it missed 6
   and can say so. `now events:listen` reports gaps on stderr.
3. **Terminal events are repeated.** `run.finished` is sent three times,
   ~50 ms apart, with an identical `run`/`seq`. Listeners MUST dedupe on
   `(run, seq)`. This turns a 1-in-N drop into roughly 1-in-N³ for the
   one event whose loss actually strands a waiter.

## 7. Emission must never affect the build

A `sendto` that fails, blocks or has no listener MUST NOT change the
outcome or the timing of the build. Sockets are non-blocking, errors are
discarded, and there is no retry beyond §6.3. This mirrors the rule
`now_audit.h` already states for the audit log — *"record() never causes
the calling operation to fail"* — and is why this belongs beside audit
rather than in the build path proper.

## 8. Security: what v1 does not do

**v1 events are unsigned and unauthenticated.** Anything that can send
to the destination can forge any event.

That is survivable on loopback, where the sender is already a local
process, and it is not survivable anywhere else. So v1 enforces it
mechanically rather than documenting it:

> `now events:listen` **refuses to bind a non-loopback address** unless
> `--insecure` is given, and refuses multicast entirely.

Signing is v2, and the reason it is not v1 is that a field reserved
before its canonicalisation is designed is a field that gets designed
wrong. The intended shape: Ed25519 over the canonical bytes of §4 using
the publisher key `now keygen` already writes, with the listener pinning
a public key — the same primitive `now publish` and cookbook already
use, measured against RFC 8032. §4's fixed key order exists so that can
land as v2 without re-cutting v1.

**Do not build anything destructive on a v1 event.** Reporting,
waiting, and triggering work that a human would be happy to see happen
twice are fine.

## 9. Configuration

Three ways in, most specific first:

```
now build --events udp://127.0.0.1:9099     # flag
NOW_EVENTS=udp://127.0.0.1:9099             # environment
```
```pasta
; ~/.now/config.pasta
events: { url: "udp://127.0.0.1:9099", wire: "pasta", file: 1 }
```

Absent all three, nothing is emitted and no socket is created.

## 10. The listener

```
now events:listen <url> [--filter <event>[,<event>...]]
                        [--output pasta|json|text]
                        [--until <event>] [--timeout <sec>]
                        [--insecure]
```

Prints one event per line to stdout and exits 0 when `--until` matches
(default: run forever). `--until run.finished --timeout 1800` is the
"wait for the build" primitive; the exit code is the build's `code` when
the run finished, so a shell can branch on it directly.

Deliberately not a job runner: it prints, and a chain is a loop over its
stdout. Keeping `now` out of orchestration is what stops this becoming a
second CI system.

## 11. Compatibility

`v` is the whole compatibility story. A listener MUST ignore unknown
fields and MUST refuse an event whose `v` it does not know. Adding a
field is not a version bump; changing the meaning of one, the key order,
or the framing is.
