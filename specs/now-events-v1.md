# now events — wire schema v1

Status: **v1 complete — all seven event types wired, listener, emitter, sidecar and counts live**
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

One event per datagram, no trailing newline.

**A datagram MUST NOT exceed 1200 bytes.** That is under the smallest
MTU worth assuming, so no event is ever IP-fragmented — a fragmented
datagram is far likelier to be dropped, and dropping the one that says
the build finished is the failure this design exists to avoid. Emitters
shorten `detail` until the datagram fits and set `detail_lossy`.

### Basta is the default; json and text are alternatives

**Basta** is Pasta's grammar plus one production, the blob. Pasta's
reasons for being chosen here are unchanged: a stricter grammar than
JSON with one implementation behind it, so far less room for two readers
to disagree about the same bytes. JSON's ambiguities — number precision,
duplicate keys, encoding — are real, and this is a stream meant to be
acted on rather than read.

Selected per sink: `basta` (default), `json`, `text`.

This spec has now chosen a wire format three times, and the third answer
is the one that stopped arguing about escaping. The first draft chose
JSON, because Pasta strings have no escape sequences and a compiler
diagnostic is full of quotes and newlines. The second chose Pasta,
because its `"""..."""` form carries quotes and newlines with no
escaping at all — which is a better answer than escaping, and still the
wrong question. **`detail` is not text. It is whatever bytes a compiler
wrote**, and the format needed for that is one with a byte count.

### `detail` is a blob

A blob is the byte `0x00`, an 8-byte big-endian length, then exactly
that many bytes. `0x00` is illegal in every other position in Basta and
Pasta — not in a label, not in a string, not in any token — so the
sentinel is unambiguous wherever it appears, and the parser resumes
ordinary text immediately after the last data byte.

Measured against basta `0696908`, in both directions:

| payload | in a `"""` string | in a blob |
|---|---|---|
| a quote at the end | merges with the delimiter | exact |
| an interior `"""` | ends the string early | exact |
| `"""#`, then `"""##` | needs a fence, then a longer one | exact |
| a NUL byte | no representation at all | exact |
| non-UTF-8 bytes | not text | exact |

The last two are the ones that decided it. A fence — `#"""..."""#`,
escalating — was designed and offered by the format owner, and it
handles the first three at any size. It cannot handle the last two at
any size, because the problem there is not the delimiter.

**The property being bought is that a blob cannot refuse.** Refusing to
write a value is the honest answer for a config file, where a human
reads the diagnostic and fixes the file. Here the same refusal is a
*dropped event*, and a watcher that never learns what broke is exactly
the failure §6 exists to prevent. Nothing in the emitter may be able to
decline a payload, and with a blob nothing can — so there is no fallback
path to get wrong either.

Blessed by the format owner on 2026-08-21 as the intended use of a blob
rather than a stretch of one: *a binary escape from the text domain*,
which is what captured process output is. alforno treats a blob as an
opaque atomic leaf — never scanned for `{variable}`, never examined for
`@link` — so a diagnostic containing either cannot be interpreted by
accident.

**Two consequences, both accepted deliberately.**

A consumer needs a Basta parser, not a Pasta one: `pasta_parse` rejects
a blob outright with *"unexpected character"*, measured. An event with
no `detail` is byte-identical under both, so most of the stream parses
either way — which makes this exactly the kind of thing that would be
discovered by a consumer rather than by us if it were not written down
here.

And a `detail` is no longer readable by eye in a tailed stream. That is
what `--output text` is for, and it is what `now events:listen` prints
unless asked otherwise.

### What a reader must do about blobs

**A reader that scans for keys must step over a blob rather than
through it.** A `detail` is somebody else's bytes: a compiler quoting a
line of Pasta back in a diagnostic would otherwise write its own
envelope by accident, and that is before anyone tries it on purpose.
Because `0x00` cannot occur anywhere else, skipping needs no
value-position tracking — read the length, jump, continue.

`now`'s own reader does this, and the test that pins it is a `detail`
containing a plausible envelope: the event decodes with the real
`module` and the real `code`, not the ones in the diagnostic.

### The limit that is ours rather than the format's

`NowEvent.detail` is a C string, so a NUL byte in captured output ends
the detail at the capture site, before any of this is reached. The wire
form would carry it; we do not hand it one. Whoever needs that should
make `detail` length-carrying — the format is not what is in the way.

### Reading either form

`now events:listen` accepts both without being told which arrived. That
is a dispatch, not a guess: a Basta event's first key is bare (`{ v: 1`)
and a JSON event's is quoted (`{"v":1`), so each decoder rejects the
other's output outright rather than half-reading it. There is a test for
exactly that property, because a decoder that *partially* accepted the
other format would turn this into sniffing.

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
| `detail` | blob | captured process output — bytes, not text (§3), shortened to fit |
| `detail_lossy` | bool | present and true when `detail` was shortened to fit. Nothing else can raise it: since `detail` became a blob there is no content the wire form has to alter |
| `code` | int | process exit code (`run.finished` only) |
| `counts` | object | `{compiled, skipped, failed, passed, total}` — any subset |
| `elapsed_ms` | int | milliseconds since `run.started` |
| `host` | string | hostname |
| `pid` | int | emitting process id |

`host` and `pid` exist because several builds may share one destination.

## 5. Event types

Seven. The set is deliberately small: an event per compiled file would
flood the socket on a 32-way build and tell nobody anything they need.

| event | when | terminal |
|---|---|---|
| `run.started` | a `now` invocation begins | |
| `phase.started` | compile / link / test begins | |
| `run.progress` | heartbeat, **at most once per second**, carries `counts` | |
| `module.failed` | one compile or link unit failed | |
| `test.failed` | one test failed | |
| `phase.finished` | a phase ended; `ok` says how | |
| `run.finished` | the invocation ended; carries `code` | ✓ |

All seven are emitted. `package`, `publish` and `procure` run under
`run.started`/`run.finished` but do not yet bracket themselves with
phase events — adding them is not a version bump.

`counts` is carried by `run.progress`, `phase.finished` and
`run.finished`, assembled from the same tallies the human-readable
summary line prints — so the stream and the terminal can never drift
into describing the same build differently. A field a phase did not
measure stays absent rather than being sent as zero: *"no tests ran"*
and *"zero tests passed"* are different claims.

### A phase that starts always finishes

**`phase.started` and `phase.finished` are guaranteed to pair.** A
started phase with no finish is indistinguishable, to a listener, from a
phase that is still running — so a build returning out of the middle of
one would strand a watcher for ever rather than tell it something ended.

The compile phase has several early returns and the link and test bodies
have many. Rather than find every one and hope, the emitter closes an
open phase when the next phase starts and when the run ends, and the link
and test phases are bracketed by thin wrappers around their bodies so
their `ok` is the phase's own rather than the run's. A listener may rely
on the pairing.

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

a `now test` whose suite fails:

```
run.started    test
phase.started  compile
phase.finished compile   counts {compiled: 1}
phase.started  test
test.failed    t.exe: exit 1
phase.finished test      counts {failed: 1, total: 1}
run.finished   FAILED (exit 1)
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
{ v: 1, run: "a3f1c9d2e4b6", seq: 7, ts: "2026-08-21T09:41:02Z", event: "module.failed", phase: "compile", ok: false, project: "dev.iridium:gut:0.1.0", module: "src/main/c/remote.c", detail: ‹00›‹00 00 00 00 00 00 00 2e›remote.c:512:9: error: unknown type name "u64" }
```

The nine bytes in guillemets are the blob's sentinel and its big-endian
length; they are bytes on the wire, not the text shown here. The 46
bytes after them are the diagnostic exactly as the compiler wrote it —
including the closing quote, which is the byte that made this example
unparseable in the previous draft of this spec.

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
events: { url: "udp://127.0.0.1:9099", wire: "basta", file: 1 }
```

Absent all three, nothing is emitted and no socket is created.

## 10. The listener

```
now events:listen <url> [--filter <event>[,<event>...]]
                        [--output basta|json|text]
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

By that rule the move from Pasta to a Basta blob (§3) is a framing
change and would be a version bump — except that there is nothing to be
compatible with. The event stream landed after `1.0.0-rc8` was cut and
has never been in a release, so v1 has had exactly one published
framing. It is worth saying rather than leaving a reader to reconstruct
it from the git log.
