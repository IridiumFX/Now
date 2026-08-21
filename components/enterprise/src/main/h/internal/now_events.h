/*
 * now_events.h — optional build lifecycle event stream (specs/now-events-v1.md)
 *
 * A structured alternative to tailing a build log for a string: one JSON
 * object per UDP datagram, emitted as the run progresses, so a waiting
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
    /* Set when `detail` is not byte-exact: shortened to fit the
     * datagram, or altered because the wire format could not carry it
     * verbatim. One flag for both, because a consumer's question is
     * "can I trust these bytes", not "which way did they change". */
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

/* ---- Pasta is the default wire format ----
 *
 * Not JSON: Pasta has a stricter grammar and one implementation behind
 * it, so there is much less room for two readers to disagree about the
 * same bytes. JSON's ambiguities — number precision, duplicate keys,
 * encoding — are real and this stream is meant to be acted on.
 *
 * Free text goes in a """...""" string, which carries quotes and
 * newlines without escaping at all.
 *
 * Encode as Pasta. Returns bytes written, 0 on error. */
NOW_API size_t now_event_encode_pasta(char *out, size_t out_cap,
                                      const NowEvent *ev);

/* Decode a Pasta event. Returns 0 on success. */
NOW_API int now_event_decode_pasta(NowEvent *ev, const char *doc, size_t len);

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

/* Decode an event in whichever of the two textual formats arrived.
 *
 * This is not sniffing: the two forms are mutually exclusive by
 * construction. A Pasta event's first key is bare (`{ v: 1`), a JSON
 * event's is quoted (`{"v":1`), so each decoder rejects the other's
 * output outright rather than half-reading it. Returns 0 on success. */
NOW_API int now_event_decode(NowEvent *ev, const char *buf, size_t len);

/* Render a decoded event for a human or for a pipe. `fmt` is one of
 * "pasta" (default), "json", "text". Returns bytes written, 0 on error.
 *
 * "basta" is named in specs/now-events-v1.md and not implemented here:
 * its value is carrying bytes that no text form can (a small binary
 * payload, a credential for a chained step), and that is a v1.1 job with
 * its own design rather than a fourth spelling of the same map. */
NOW_API size_t now_event_render(char *out, size_t out_cap,
                                const NowEvent *ev, const char *fmt);

/* ---- destinations ---- */

typedef struct NowEventSink NowEventSink;

/* Open a sink for a destination URL ("udp://host:port").
 *
 * `wire` is "pasta" (the default when NULL), "json" or "text". Returns
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
NOW_API void now_events_module_failed(const char *module, const char *detail);
NOW_API void now_events_test_failed(const char *name, const char *detail);
NOW_API void now_events_run_finished(int code, const NowEventCounts *counts);
NOW_API void now_events_close(void);

/* ---- the listener command ---- */

typedef struct {
    const char *url;
    const char *filter;      /* comma-separated event names, NULL = all */
    const char *output;      /* json | pasta | text */
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
