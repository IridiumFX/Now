/*
 * now_events.h — optional build lifecycle event stream (specs/now-events-v1.md)
 *
 * A structured alternative to tailing a build log for a string: one
 * structured event per UDP datagram, emitted as the run progresses, so a
 * waiting
 * process learns that a module failed while the build is still running
 * rather than when the process exits.
 *
 * Opt-in in the strongest sense: no socket is created and no datagram is
 * sent unless a destination is named by --events, $NOW_EVENTS, or the
 * `events:` section of ~/.now/config.pasta. A build tool that opens a
 * socket nobody asked for has no business in a sandbox.
 *
 * Fire-and-forget, exactly as now_audit.h is: a send that fails, blocks
 * or has nobody listening must not change the outcome or the timing of
 * the build.
 */
#ifndef NOW_EVENTS_H
#define NOW_EVENTS_H

#include "now.h"

#define NOW_EVENTS_SCHEMA_VERSION 1

/* Largest datagram we will put on the wire. Under the smallest MTU worth
 * assuming, so an event is never IP-fragmented — a fragmented datagram is
 * far likelier to be dropped, and the one that says the build finished is
 * the one whose loss strands a waiter. */
#define NOW_EVENTS_MAX_DATAGRAM 1200

/* Terminal events go out this many times, ~50ms apart, with an identical
 * (run, seq). Listeners dedupe. Turns a 1-in-N drop into ~1-in-N^3 for
 * the only event whose loss actually matters. */
#define NOW_EVENTS_TERMINAL_REPEATS 3

typedef enum {
    NOW_EVENT_RUN_STARTED = 0,
    NOW_EVENT_PHASE_STARTED,
    NOW_EVENT_RUN_PROGRESS,
    NOW_EVENT_MODULE_FAILED,
    NOW_EVENT_TEST_FAILED,
    NOW_EVENT_PHASE_FINISHED,
    NOW_EVENT_RUN_FINISHED,
    NOW_EVENT__COUNT
} NowEventType;

/* Counts carried by run.progress / phase.finished / run.finished.
 * A negative value means "not applicable to this event" and is omitted
 * from the wire form rather than sent as -1. */
typedef struct {
    int compiled;
    int skipped;
    int failed;
    int passed;
    int total;
} NowEventCounts;

typedef struct {
    int             v;
    char            run[13];      /* 12 hex + NUL */
    long            seq;
    char            ts[32];       /* YYYY-MM-DDTHH:MM:SSZ */
    NowEventType    event;
    char            phase[32];
    int             ok;

    /* optional — empty string / negative means absent */
    char            project[192];
    char            module[256];
    char            detail[768];
    /* Set when `detail` is not byte-exact. Since detail became a blob
     * the only thing that can raise it is shortening to fit the
     * datagram: there is no longer any content the wire form has to
     * alter to carry. A consumer's question is "can I trust these
     * bytes", so one flag answers it either way. */
    int             detail_lossy;
    int             code;
    NowEventCounts  counts;
    long            elapsed_ms;
    char            host[64];
    int             pid;
} NowEvent;

/* ---- names ---- */

NOW_API const char  *now_event_name(NowEventType e);
/* Returns NOW_EVENT__COUNT when the name is not one of ours. */
NOW_API NowEventType now_event_parse_name(const char *name);
NOW_API int          now_event_is_terminal(NowEventType e);

/* ---- wire form ---- */

/* ---- Basta is the default wire format ----
 *
 * Not JSON: Basta is Pasta's grammar plus blobs, so it has a stricter
 * grammar and one implementation behind it, and there is much less room
 * for two readers to disagree about the same bytes. JSON's ambiguities
 * — number precision, duplicate keys, encoding — are real and this
 * stream is meant to be acted on.
 *
 * `detail` is a blob: a length-prefixed run of bytes with no delimiter
 * to collide with, so no captured process output can fail to be
 * written. That matters more here than anywhere else in `now` — a
 * refused value in a config file is a diagnostic a human fixes, but a
 * refused event is a dropped one, and the watcher never finds out what
 * broke. Blessed by the format owner as the intended use of a blob
 * (2026-08-21).
 *
 * The datagram therefore contains raw bytes and is not text. Its length
 * is the return value; nothing may treat it as a C string.
 *
 * Encode as Basta. Returns bytes written, 0 on error. */
NOW_API size_t now_event_encode_basta(char *out, size_t out_cap,
                                      const NowEvent *ev);

/* Decode a Basta event. Returns 0 on success. */
NOW_API int now_event_decode_basta(NowEvent *ev, const char *doc, size_t len);

/* Encode `ev` as one JSON object into `out` (no trailing newline).
 * Keys are written in the order specs/now-events-v1.md §4 fixes, which
 * is what will make a canonical byte sequence available to v2 signing
 * without re-cutting v1.
 *
 * If the object would exceed NOW_EVENTS_MAX_DATAGRAM, `detail` is
 * truncated until it fits and detail_lossy is set in the output.
 * Returns the number of bytes written, or 0 on error. */
NOW_API size_t now_event_encode_json(char *out, size_t out_cap,
                                     const NowEvent *ev);

/* Decode one JSON object into `ev`. Returns 0 on success.
 *
 * Refuses an event whose `v` is not NOW_EVENTS_SCHEMA_VERSION, and
 * ignores fields it does not know — that pair is the whole compatibility
 * story (§11). */
NOW_API int now_event_decode_json(NowEvent *ev, const char *json, size_t len);

/* Decode an event in whichever of the two forms arrived.
 *
 * This is not sniffing: the two are mutually exclusive by construction.
 * A Basta event's first key is bare (`{ v: 1`), a JSON event's is
 * quoted (`{"v":1`), so each decoder rejects the other's output
 * outright rather than half-reading it. Returns 0 on success. */
NOW_API int now_event_decode(NowEvent *ev, const char *buf, size_t len);

/* Render a decoded event for a human or for a pipe. `fmt` is one of
 * "basta" (default), "json", "text". Returns bytes written, 0 on error.
 *
 * "basta" output contains a blob and is therefore bytes, not text: use
 * the returned length. "text" is the readable one and is what
 * `now events:listen` prints unless asked otherwise. */
NOW_API size_t now_event_render(char *out, size_t out_cap,
                                const NowEvent *ev, const char *fmt);

/* ---- destinations ---- */

typedef struct NowEventSink NowEventSink;

/* Open a sink for a destination URL ("udp://host:port").
 *
 * `wire` is "basta" (the default when NULL), "json" or "text". Returns
 * NULL if the URL is unusable — which the caller treats as "do not
 * emit", never as a build failure. */
NOW_API NowEventSink *now_event_sink_open(const char *url, const char *wire);

/* Send one event. Never blocks, never retries beyond the terminal-event
 * repeat, and never reports failure: there is nothing a build could
 * usefully do about a lost datagram. */
NOW_API void now_event_sink_send(NowEventSink *sink, const NowEvent *ev);

NOW_API void now_event_sink_close(NowEventSink *sink);

/* ---- listening ---- */

typedef struct NowEventSource NowEventSource;

/* Bind and listen for events at `url`.
 *
 * Refuses a non-loopback address unless `insecure` is non-zero, and
 * refuses multicast outright. v1 events are unsigned, so anything able
 * to reach the socket can forge any event; that is survivable when the
 * sender is necessarily a local process and is not survivable otherwise.
 * Enforcing it here rather than in the documentation is the difference
 * between a limitation and a hole. Signing is v2 (§8).
 *
 * Returns NULL on failure, with a reason in `result`. */
NOW_API NowEventSource *now_event_source_open(const char *url, int insecure,
                                              NowResult *result);

/* Wait up to timeout_ms for one datagram.
 * Returns  1 = got an event (in *ev)
 *          0 = timed out
 *         -1 = a datagram arrived that is not a v1 event (skipped)
 *         -2 = the socket failed */
NOW_API int now_event_source_recv(NowEventSource *src, NowEvent *ev,
                                  int timeout_ms);

NOW_API void now_event_source_close(NowEventSource *src);

/* ---- emitting, from inside a run ----
 *
 * One emitter per process, because one `now` invocation is one run. Every
 * call below returns immediately when no destination was configured, so
 * a build that did not ask for events pays a predicate and nothing else.
 *
 * None of these can fail in a way a caller should react to, so none of
 * them return anything: specs/now-events-v1.md §7 makes "emission never
 * changes the build" a requirement, and a return value invites someone
 * to check it and branch. */

/* Resolve a destination and open it. `url` may be NULL, in which case
 * $NOW_EVENTS is consulted. `file_path` may be NULL to skip the sidecar.
 * Safe to call when everything is NULL: the emitter simply stays off. */
NOW_API void now_events_open(const char *url, const char *wire,
                             const char *file_path);

/* Is anything actually being emitted? Only for callers that would do
 * real work to build a detail string. */
NOW_API int now_events_active(void);

NOW_API void now_events_run_started(const char *phase, const char *project);
NOW_API void now_events_phase_started(const char *phase);
NOW_API void now_events_phase_finished(const char *phase, int ok,
                                       const NowEventCounts *counts);
/* Rate-limited to one per second inside the emitter; callers may call it
 * as often as is convenient. */
NOW_API void now_events_progress(const NowEventCounts *counts);
/* Put `detail` into an event, truncating to the struct's capacity and
 * SAYING SO when it does.
 *
 * Named and public because it was neither: the copy was an inline
 * snprintf in ev_emit, so a diagnostic longer than NowEvent.detail lost
 * its tail and the record still claimed to be byte-exact. Nothing could
 * observe that without standing up an emitter and a listener, which is
 * why it survived two handovers that both described `detail_lossy` as
 * having exactly one cause. */
NOW_API void now_event_set_detail(NowEvent *ev, const char *detail);

NOW_API void now_events_module_failed(const char *module, const char *detail);
NOW_API void now_events_test_failed(const char *name, const char *detail);
/* Pass NULL for `counts` to reuse the last set the emitter was given.
 * The totals a run finishes with are the ones its last phase reported,
 * and threading them back out to the caller only to hand them straight
 * back would be ceremony. */
NOW_API void now_events_run_finished(int code, const NowEventCounts *counts);
NOW_API void now_events_close(void);

/* ---- the listener command ---- */

typedef struct {
    const char *url;
    const char *filter;      /* comma-separated event names, NULL = all */
    const char *output;      /* json | basta | text */
    const char *until;       /* stop when this event arrives, NULL = never */
    int         timeout_sec; /* 0 = no timeout */
    int         insecure;
} NowEventListenOpts;

/* Run the listen loop, printing one event per line to stdout.
 *
 * Exits with the build's own exit code when it stops on a `run.finished`
 * named by --until, so a shell can branch on the build's result without
 * parsing anything. Returns 0 otherwise, non-zero on timeout or error. */
NOW_API int now_events_listen(const NowEventListenOpts *opts,
                              NowResult *result);

#endif /* NOW_EVENTS_H */
