/*
 * now_events.c — optional build lifecycle event stream
 *
 * Schema and the reasoning behind it: specs/now-events-v1.md.
 *
 * Sits beside now_audit.c rather than in the build path because it is
 * the same kind of thing — opt-in observability that must never be able
 * to change what the build does. Every send here is best effort and
 * every failure is discarded.
 *
 * The UDP shim at the bottom is written against raw sockets because
 * apennines has no datagram primitive: it exposes tcp.h but nothing for
 * SOCK_DGRAM, and only dns.c uses one, privately. `t3/net/udp.h` is the
 * natural home for it and has been requested (to-apennines.md,
 * 2026-08-21). Until it exists this mirrors dns.c's platform guards
 * rather than inventing a third shape; when it lands, this section is
 * what should be deleted.
 */
#include "now_events.h"
#include "now_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET now_sock;
  #define NOW_SOCK_BAD  INVALID_SOCKET
  #define now_sock_close closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  typedef int now_sock;
  #define NOW_SOCK_BAD  (-1)
  #define now_sock_close close
#endif

/* ==== names ==== */

static const char *event_names[NOW_EVENT__COUNT] = {
    "run.started",
    "phase.started",
    "run.progress",
    "module.failed",
    "test.failed",
    "phase.finished",
    "run.finished"
};

NOW_API const char *now_event_name(NowEventType e) {
    if ((int)e >= 0 && (int)e < (int)NOW_EVENT__COUNT) return event_names[(int)e];
    return "unknown";
}

NOW_API NowEventType now_event_parse_name(const char *name) {
    int i;
    if (!name) return NOW_EVENT__COUNT;
    for (i = 0; i < (int)NOW_EVENT__COUNT; i++)
        if (strcmp(name, event_names[i]) == 0) return (NowEventType)i;
    return NOW_EVENT__COUNT;
}

NOW_API int now_event_is_terminal(NowEventType e) {
    return e == NOW_EVENT_RUN_FINISHED;
}

/* ==== JSON ==== */

/* Escape into a JSON string body. Returns bytes written, or (size_t)-1 if
 * it would not fit — callers shorten and retry rather than emitting a
 * half-escaped string. */
static size_t json_escape(char *out, size_t cap, const char *in) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;

    for (; *p; p++) {
        char tmp[8];
        size_t n;
        switch (*p) {
        case '"':  memcpy(tmp, "\\\"", 2); n = 2; break;
        case '\\': memcpy(tmp, "\\\\", 2); n = 2; break;
        case '\n': memcpy(tmp, "\\n", 2);  n = 2; break;
        case '\r': memcpy(tmp, "\\r", 2);  n = 2; break;
        case '\t': memcpy(tmp, "\\t", 2);  n = 2; break;
        default:
            if (*p < 0x20) {
                n = (size_t)snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)*p);
            } else {
                tmp[0] = (char)*p; n = 1;
            }
        }
        if (o + n >= cap) return (size_t)-1;
        memcpy(out + o, tmp, n);
        o += n;
    }
    if (o >= cap) return (size_t)-1;
    out[o] = '\0';
    return o;
}

/* One attempt at the whole object with `detail` already cut to
 * detail_len bytes. Returns bytes written, 0 if it does not fit. */
static size_t encode_attempt(char *out, size_t cap, const NowEvent *ev,
                             size_t detail_len, int mark_truncated) {
    char esc_detail[1600];
    char esc_module[600];
    char esc_project[420];
    char cut[768];
    size_t n = 0;
    int w;

    esc_detail[0] = esc_module[0] = esc_project[0] = '\0';

    if (ev->detail[0]) {
        if (detail_len >= sizeof(cut)) detail_len = sizeof(cut) - 1;
        memcpy(cut, ev->detail, detail_len);
        cut[detail_len] = '\0';
        if (json_escape(esc_detail, sizeof(esc_detail), cut) == (size_t)-1)
            return 0;
    }
    if (ev->module[0] &&
        json_escape(esc_module, sizeof(esc_module), ev->module) == (size_t)-1)
        return 0;
    if (ev->project[0] &&
        json_escape(esc_project, sizeof(esc_project), ev->project) == (size_t)-1)
        return 0;

    /* Envelope first, in the order specs/now-events-v1.md §4 fixes. */
    w = snprintf(out, cap,
        "{\"v\":%d,\"run\":\"%s\",\"seq\":%ld,\"ts\":\"%s\","
        "\"event\":\"%s\",\"phase\":\"%s\",\"ok\":%s",
        ev->v, ev->run, ev->seq, ev->ts,
        now_event_name(ev->event), ev->phase, ev->ok ? "true" : "false");
    if (w < 0 || (size_t)w >= cap) return 0;
    n = (size_t)w;

#define APPEND(...) do {                                   \
        w = snprintf(out + n, cap - n, __VA_ARGS__);       \
        if (w < 0 || (size_t)w >= cap - n) return 0;       \
        n += (size_t)w;                                    \
    } while (0)

    if (esc_project[0]) APPEND(",\"project\":\"%s\"", esc_project);
    if (esc_module[0])  APPEND(",\"module\":\"%s\"", esc_module);
    if (esc_detail[0]) {
        APPEND(",\"detail\":\"%s\"", esc_detail);
        if (mark_truncated) APPEND(",\"detail_lossy\":true");
    }
    if (ev->code >= 0)       APPEND(",\"code\":%d", ev->code);
    if (ev->elapsed_ms >= 0) APPEND(",\"elapsed_ms\":%ld", ev->elapsed_ms);

    if (ev->counts.compiled >= 0 || ev->counts.skipped >= 0 ||
        ev->counts.failed   >= 0 || ev->counts.passed  >= 0 ||
        ev->counts.total    >= 0) {
        int first = 1;
        APPEND(",\"counts\":{");
        if (ev->counts.compiled >= 0) { APPEND("%s\"compiled\":%d", first ? "" : ",", ev->counts.compiled); first = 0; }
        if (ev->counts.skipped  >= 0) { APPEND("%s\"skipped\":%d",  first ? "" : ",", ev->counts.skipped);  first = 0; }
        if (ev->counts.failed   >= 0) { APPEND("%s\"failed\":%d",   first ? "" : ",", ev->counts.failed);   first = 0; }
        if (ev->counts.passed   >= 0) { APPEND("%s\"passed\":%d",   first ? "" : ",", ev->counts.passed);   first = 0; }
        if (ev->counts.total    >= 0) { APPEND("%s\"total\":%d",    first ? "" : ",", ev->counts.total);    first = 0; }
        APPEND("}");
    }

    if (ev->host[0]) APPEND(",\"host\":\"%s\"", ev->host);
    if (ev->pid > 0) APPEND(",\"pid\":%d", ev->pid);

    APPEND("}");
#undef APPEND
    return n;
}

NOW_API size_t now_event_encode_json(char *out, size_t out_cap, const NowEvent *ev) {
    size_t detail_len, n;

    if (!out || !ev || out_cap == 0) return 0;
    if (out_cap > NOW_EVENTS_MAX_DATAGRAM) out_cap = NOW_EVENTS_MAX_DATAGRAM;

    detail_len = strlen(ev->detail);
    n = encode_attempt(out, out_cap, ev, detail_len, ev->detail_lossy);
    if (n) return n;

    /* Too big. Shorten `detail` until it fits — it is the only field
     * that can be arbitrarily long, and a truncated diagnostic is worth
     * more than a dropped event. Halve rather than step: a 60 KB
     * compiler dump should not cost 60 000 attempts. */
    while (detail_len > 0) {
        detail_len /= 2;
        n = encode_attempt(out, out_cap, ev, detail_len, 1);
        if (n) return n;
    }

    /* Even with no detail at all it does not fit; drop module too. */
    {
        NowEvent bare = *ev;
        bare.detail[0] = '\0';
        bare.module[0] = '\0';
        bare.detail_lossy = 1;
        return encode_attempt(out, out_cap, &bare, 0, 1);
    }
}

/* ---- a JSON reader just wide enough for §4 ----
 *
 * Deliberately not a general JSON parser: this reads a flat object whose
 * shape this same file writes, and refusing anything else is a feature.
 * A full parser here would be a much larger attack surface reachable
 * from a socket. */

static const char *json_find_key(const char *json, size_t len, const char *key) {
    char pat[64];
    size_t plen = (size_t)snprintf(pat, sizeof(pat), "\"%s\":", key);
    size_t i;
    if (plen >= sizeof(pat)) return NULL;
    for (i = 0; i + plen <= len; i++) {
        if (memcmp(json + i, pat, plen) != 0) continue;
        {
            /* Skip the whitespace almost every other JSON writer puts
             * after the colon. Our own encoder emits none, so a decoder
             * checked only against it round-trips perfectly and still
             * cannot read anything else — which is exactly what
             * happened: the first datagram from a Python sender was
             * rejected outright, and the listener reported it as
             * silence. */
            const char *p = json + i + plen;
            const char *end = json + len;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            return p;
        }
    }
    return NULL;
}

static int json_get_int(const char *json, size_t len, const char *key,
                        long *out) {
    const char *p = json_find_key(json, len, key);
    if (!p) return -1;
    *out = strtol(p, NULL, 10);
    return 0;
}

static int json_get_bool(const char *json, size_t len, const char *key,
                         int *out) {
    const char *p = json_find_key(json, len, key);
    if (!p) return -1;
    *out = (strncmp(p, "true", 4) == 0);
    return 0;
}

/* Unescape a JSON string value into `out`. Returns 0 on success. */
static int json_get_str(const char *json, size_t len, const char *key,
                        char *out, size_t cap) {
    const char *p = json_find_key(json, len, key);
    const char *end = json + len;
    size_t o = 0;
    if (!p || p >= end || *p != '"') return -1;
    p++;
    while (p < end && *p != '"') {
        char c = *p;
        if (c == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
                /* Only the control-character form this file emits. */
                if (p + 4 < end) {
                    char hex[5];
                    memcpy(hex, p + 1, 4); hex[4] = '\0';
                    c = (char)strtol(hex, NULL, 16);
                    p += 4;
                }
                break;
            default: c = *p; break;
            }
        }
        if (o + 1 >= cap) return -1;   /* refuse rather than silently cut */
        out[o++] = c;
        p++;
    }
    if (p >= end) return -1;           /* unterminated */
    out[o] = '\0';
    return 0;
}

NOW_API int now_event_decode_json(NowEvent *ev, const char *json, size_t len) {
    long v = 0, n = 0;
    char name[64];

    if (!ev || !json || len == 0) return -1;

    memset(ev, 0, sizeof(*ev));
    ev->code = -1;
    ev->elapsed_ms = -1;
    ev->counts.compiled = ev->counts.skipped = ev->counts.failed =
        ev->counts.passed = ev->counts.total = -1;

    if (json_get_int(json, len, "v", &v) != 0) return -1;
    if (v != NOW_EVENTS_SCHEMA_VERSION) return -1;
    ev->v = (int)v;

    if (json_get_str(json, len, "run", ev->run, sizeof(ev->run)) != 0) return -1;
    if (json_get_int(json, len, "seq", &ev->seq) != 0) return -1;
    if (json_get_str(json, len, "ts", ev->ts, sizeof(ev->ts)) != 0) return -1;
    if (json_get_str(json, len, "event", name, sizeof(name)) != 0) return -1;

    ev->event = now_event_parse_name(name);
    if (ev->event == NOW_EVENT__COUNT) return -1;

    if (json_get_str(json, len, "phase", ev->phase, sizeof(ev->phase)) != 0)
        ev->phase[0] = '\0';
    if (json_get_bool(json, len, "ok", &ev->ok) != 0) ev->ok = 1;

    json_get_str(json, len, "project", ev->project, sizeof(ev->project));
    json_get_str(json, len, "module",  ev->module,  sizeof(ev->module));
    json_get_str(json, len, "detail",  ev->detail,  sizeof(ev->detail));
    json_get_bool(json, len, "detail_lossy", &ev->detail_lossy);

    if (json_get_int(json, len, "code", &n) == 0)       ev->code = (int)n;
    if (json_get_int(json, len, "elapsed_ms", &n) == 0) ev->elapsed_ms = n;
    if (json_get_int(json, len, "compiled", &n) == 0)   ev->counts.compiled = (int)n;
    if (json_get_int(json, len, "skipped", &n) == 0)    ev->counts.skipped  = (int)n;
    if (json_get_int(json, len, "failed", &n) == 0)     ev->counts.failed   = (int)n;
    if (json_get_int(json, len, "passed", &n) == 0)     ev->counts.passed   = (int)n;
    if (json_get_int(json, len, "total", &n) == 0)      ev->counts.total    = (int)n;
    if (json_get_int(json, len, "pid", &n) == 0)        ev->pid = (int)n;
    json_get_str(json, len, "host", ev->host, sizeof(ev->host));

    return 0;
}

NOW_API int now_event_decode(NowEvent *ev, const char *buf, size_t len) {
    /* Pasta first, because it is the default on the wire. The two are
     * mutually exclusive by construction — a bare first key versus a
     * quoted one — so this is a dispatch, not a guess. */
    if (now_event_decode_pasta(ev, buf, len) == 0) return 0;
    return now_event_decode_json(ev, buf, len);
}

/* ==== rendering ==== */

NOW_API size_t now_event_render(char *out, size_t out_cap,
                                const NowEvent *ev, const char *fmt) {
    if (!out || !ev || out_cap == 0) return 0;
    if (!fmt) fmt = "pasta";

    if (strcmp(fmt, "json") == 0)
        return now_event_encode_json(out, out_cap, ev);
    if (strcmp(fmt, "pasta") == 0)
        return now_event_encode_pasta(out, out_cap, ev);

    /* text */
    {
        int w;
        if (ev->module[0] && ev->detail[0])
            w = snprintf(out, out_cap, "%s  %-14s %s: %s",
                         ev->ts, now_event_name(ev->event), ev->module, ev->detail);
        else if (ev->module[0])
            w = snprintf(out, out_cap, "%s  %-14s %s",
                         ev->ts, now_event_name(ev->event), ev->module);
        else if (ev->event == NOW_EVENT_RUN_FINISHED)
            w = snprintf(out, out_cap, "%s  %-14s %s (exit %d)",
                         ev->ts, now_event_name(ev->event),
                         ev->ok ? "ok" : "FAILED", ev->code);
        else if (ev->detail[0])
            w = snprintf(out, out_cap, "%s  %-14s %s",
                         ev->ts, now_event_name(ev->event), ev->detail);
        else
            w = snprintf(out, out_cap, "%s  %-14s %s",
                         ev->ts, now_event_name(ev->event), ev->phase);
        return (w < 0 || (size_t)w >= out_cap) ? 0 : (size_t)w;
    }
}

/* ==== Pasta ====
 *
 * Pasta is the wire default: a stricter grammar than JSON with one
 * implementation behind it, so there is far less room for two readers to
 * disagree about the same bytes.
 *
 * It is hand-written rather than built through basta_new_map() +
 * basta_write(), and that is not a preference. Measured 2026-08-21,
 * basta_writer.c's write_string() only reaches for the """ form when a
 * string contains a newline, so it emits unparseable output for three
 * ordinary cases:
 *
 *   error: unknown type name "u64"   ->  {detail: "error: ... "u64""}
 *   a string ending in a quote        ->  closing delimiter merges
 *   a string containing """           ->  terminates early
 *
 * pasta_write() returns those happily; only re-parsing catches them. A
 * compiler diagnostic containing a double quote is the most ordinary
 * payload an event has, so the writer cannot be used here until that is
 * fixed. Reported to the format owner. Meanwhile the rules below are
 * explicit and the round trip is tested.
 */

/* Free-text fields go out as """...""" so quotes need no escaping. Two
 * things still cannot appear inside that form: the delimiter itself, and
 * a trailing quote (which would merge with it). Both are altered and
 * flagged rather than silently mangled. */
static size_t pasta_escape_free_text(char *out, size_t cap, const char *in,
                                     int *lossy) {
    size_t o = 0;
    const char *p = in;

    while (*p) {
        if (p[0] == '"' && p[1] == '"' && p[2] == '"') {
            /* Break the delimiter with a space; the text stays readable
             * and the reader is told it is not byte-exact. */
            if (o + 4 >= cap) return (size_t)-1;
            out[o++] = '"'; out[o++] = '"'; out[o++] = ' '; out[o++] = '"';
            *lossy = 1;
            p += 3;
            continue;
        }
        if (o + 1 >= cap) return (size_t)-1;
        out[o++] = *p++;
    }
    /* A trailing quote would close the delimiter one byte early. */
    if (o > 0 && out[o - 1] == '"') {
        if (o + 1 >= cap) return (size_t)-1;
        out[o++] = '\n';
        *lossy = 1;
    }
    out[o] = '\0';
    return o;
}

/* Short fields (ids, names, paths) go out as plain "..." strings. They
 * cannot contain a quote or a newline by construction, but a hostile or
 * corrupt one must not be able to break the framing, so anything that
 * would is dropped. */
static size_t pasta_escape_plain(char *out, size_t cap, const char *in) {
    size_t o = 0;
    const char *p = in;
    for (; *p; p++) {
        if (*p == '"' || *p == '\n' || *p == '\r') continue;
        if (o + 1 >= cap) return (size_t)-1;
        out[o++] = *p;
    }
    out[o] = '\0';
    return o;
}

static size_t encode_pasta_attempt(char *out, size_t cap, const NowEvent *ev,
                                   size_t detail_len, int mark_lossy) {
    char det[1600], mod[600], proj[420], hst[128], cut[768];
    size_t n = 0;
    int w;
    int lossy = mark_lossy;

    det[0] = mod[0] = proj[0] = hst[0] = '\0';

    if (ev->detail[0]) {
        if (detail_len >= sizeof(cut)) detail_len = sizeof(cut) - 1;
        memcpy(cut, ev->detail, detail_len);
        cut[detail_len] = '\0';
        if (pasta_escape_free_text(det, sizeof(det), cut, &lossy) == (size_t)-1)
            return 0;
    }
    if (ev->module[0] &&
        pasta_escape_plain(mod, sizeof(mod), ev->module) == (size_t)-1) return 0;
    if (ev->project[0] &&
        pasta_escape_plain(proj, sizeof(proj), ev->project) == (size_t)-1) return 0;
    if (ev->host[0] &&
        pasta_escape_plain(hst, sizeof(hst), ev->host) == (size_t)-1) return 0;

    w = snprintf(out, cap,
        "{ v: %d, run: \"%s\", seq: %ld, ts: \"%s\", event: \"%s\", "
        "phase: \"%s\", ok: %s",
        ev->v, ev->run, ev->seq, ev->ts,
        now_event_name(ev->event), ev->phase, ev->ok ? "true" : "false");
    if (w < 0 || (size_t)w >= cap) return 0;
    n = (size_t)w;

#define PAPPEND(...) do {                                  \
        w = snprintf(out + n, cap - n, __VA_ARGS__);       \
        if (w < 0 || (size_t)w >= cap - n) return 0;       \
        n += (size_t)w;                                    \
    } while (0)

    if (proj[0]) PAPPEND(", project: \"%s\"", proj);
    if (mod[0])  PAPPEND(", module: \"%s\"", mod);
    if (det[0]) {
        PAPPEND(", detail: \"\"\"%s\"\"\"", det);
        if (lossy) PAPPEND(", detail_lossy: true");
    }
    if (ev->code >= 0)       PAPPEND(", code: %d", ev->code);
    if (ev->elapsed_ms >= 0) PAPPEND(", elapsed_ms: %ld", ev->elapsed_ms);

    if (ev->counts.compiled >= 0) PAPPEND(", compiled: %d", ev->counts.compiled);
    if (ev->counts.skipped  >= 0) PAPPEND(", skipped: %d",  ev->counts.skipped);
    if (ev->counts.failed   >= 0) PAPPEND(", failed: %d",   ev->counts.failed);
    if (ev->counts.passed   >= 0) PAPPEND(", passed: %d",   ev->counts.passed);
    if (ev->counts.total    >= 0) PAPPEND(", total: %d",    ev->counts.total);

    if (hst[0])      PAPPEND(", host: \"%s\"", hst);
    if (ev->pid > 0) PAPPEND(", pid: %d", ev->pid);

    PAPPEND(" }");
#undef PAPPEND
    return n;
}

NOW_API size_t now_event_encode_pasta(char *out, size_t out_cap,
                                      const NowEvent *ev) {
    size_t detail_len, n;

    if (!out || !ev || out_cap == 0) return 0;
    if (out_cap > NOW_EVENTS_MAX_DATAGRAM) out_cap = NOW_EVENTS_MAX_DATAGRAM;

    detail_len = strlen(ev->detail);
    n = encode_pasta_attempt(out, out_cap, ev, detail_len, ev->detail_lossy);
    if (n) return n;

    while (detail_len > 0) {
        detail_len /= 2;
        n = encode_pasta_attempt(out, out_cap, ev, detail_len, 1);
        if (n) return n;
    }
    {
        NowEvent bare = *ev;
        bare.detail[0] = '\0';
        bare.module[0] = '\0';
        bare.detail_lossy = 1;
        return encode_pasta_attempt(out, out_cap, &bare, 0, 1);
    }
}

/* ---- reading Pasta back ---- */

static const char *pasta_find_key(const char *doc, size_t len, const char *key) {
    char pat[64];
    size_t plen = (size_t)snprintf(pat, sizeof(pat), "%s:", key);
    size_t i;
    if (plen >= sizeof(pat)) return NULL;
    for (i = 0; i + plen <= len; i++) {
        /* A key is preceded by '{' or ', ' — without that check, "total"
         * would also match the tail of "subtotal". */
        if (i > 0 && doc[i - 1] != ' ' && doc[i - 1] != '{') continue;
        if (memcmp(doc + i, pat, plen) != 0) continue;
        {
            const char *p = doc + i + plen;
            const char *end = doc + len;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            return p;
        }
    }
    return NULL;
}

static int pasta_get_int(const char *doc, size_t len, const char *key, long *out) {
    const char *p = pasta_find_key(doc, len, key);
    if (!p) return -1;
    *out = strtol(p, NULL, 10);
    return 0;
}

static int pasta_get_bool(const char *doc, size_t len, const char *key, int *out) {
    const char *p = pasta_find_key(doc, len, key);
    if (!p) return -1;
    *out = (strncmp(p, "true", 4) == 0);
    return 0;
}

static int pasta_get_str(const char *doc, size_t len, const char *key,
                         char *out, size_t cap) {
    const char *p = pasta_find_key(doc, len, key);
    const char *end = doc + len;
    const char *stop;
    size_t n;

    if (!p || p >= end || *p != '"') return -1;

    if (p + 2 < end && p[1] == '"' && p[2] == '"') {
        /* """ ... """ — everything up to the next delimiter, verbatim. */
        p += 3;
        stop = p;
        while (stop + 2 < end &&
               !(stop[0] == '"' && stop[1] == '"' && stop[2] == '"')) stop++;
        if (stop + 2 >= end) return -1;
    } else {
        p += 1;
        stop = p;
        while (stop < end && *stop != '"') stop++;
        if (stop >= end) return -1;
    }

    n = (size_t)(stop - p);
    if (n + 1 > cap) return -1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

NOW_API int now_event_decode_pasta(NowEvent *ev, const char *doc, size_t len) {
    long v = 0, n = 0;
    char name[64];

    if (!ev || !doc || len == 0) return -1;

    memset(ev, 0, sizeof(*ev));
    ev->code = -1;
    ev->elapsed_ms = -1;
    ev->counts.compiled = ev->counts.skipped = ev->counts.failed =
        ev->counts.passed = ev->counts.total = -1;

    if (pasta_get_int(doc, len, "v", &v) != 0) return -1;
    if (v != NOW_EVENTS_SCHEMA_VERSION) return -1;
    ev->v = (int)v;

    if (pasta_get_str(doc, len, "run", ev->run, sizeof(ev->run)) != 0) return -1;
    if (pasta_get_int(doc, len, "seq", &ev->seq) != 0) return -1;
    if (pasta_get_str(doc, len, "ts", ev->ts, sizeof(ev->ts)) != 0) return -1;
    if (pasta_get_str(doc, len, "event", name, sizeof(name)) != 0) return -1;

    ev->event = now_event_parse_name(name);
    if (ev->event == NOW_EVENT__COUNT) return -1;

    if (pasta_get_str(doc, len, "phase", ev->phase, sizeof(ev->phase)) != 0)
        ev->phase[0] = '\0';
    if (pasta_get_bool(doc, len, "ok", &ev->ok) != 0) ev->ok = 1;

    pasta_get_str(doc, len, "project", ev->project, sizeof(ev->project));
    pasta_get_str(doc, len, "module",  ev->module,  sizeof(ev->module));
    pasta_get_str(doc, len, "detail",  ev->detail,  sizeof(ev->detail));
    pasta_get_bool(doc, len, "detail_lossy", &ev->detail_lossy);

    if (pasta_get_int(doc, len, "code", &n) == 0)       ev->code = (int)n;
    if (pasta_get_int(doc, len, "elapsed_ms", &n) == 0) ev->elapsed_ms = n;
    if (pasta_get_int(doc, len, "compiled", &n) == 0)   ev->counts.compiled = (int)n;
    if (pasta_get_int(doc, len, "skipped", &n) == 0)    ev->counts.skipped  = (int)n;
    if (pasta_get_int(doc, len, "failed", &n) == 0)     ev->counts.failed   = (int)n;
    if (pasta_get_int(doc, len, "passed", &n) == 0)     ev->counts.passed   = (int)n;
    if (pasta_get_int(doc, len, "total", &n) == 0)      ev->counts.total    = (int)n;
    if (pasta_get_int(doc, len, "pid", &n) == 0)        ev->pid = (int)n;
    pasta_get_str(doc, len, "host", ev->host, sizeof(ev->host));

    return 0;
}

/* ==== UDP ==== */

static int sockets_up(void) {
#ifdef _WIN32
    static int done = 0;
    if (!done) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        done = 1;
    }
#endif
    return 0;
}

/* Parse "udp://host:port". Returns 0 on success. */
static int parse_events_url(const char *url, char *host, size_t host_cap,
                            int *port) {
    const char *p, *colon;
    size_t hlen;

    if (!url) return -1;
    if (strncmp(url, "udp://", 6) != 0) return -1;
    p = url + 6;

    colon = strrchr(p, ':');
    if (!colon || colon == p) return -1;

    hlen = (size_t)(colon - p);
    if (hlen >= host_cap) return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    *port = atoi(colon + 1);
    if (*port <= 0 || *port > 65535) return -1;
    return 0;
}

static int addr_is_loopback(const char *host) {
    return strcmp(host, "127.0.0.1") == 0 ||
           strncmp(host, "127.", 4) == 0 ||
           strcmp(host, "localhost") == 0 ||
           strcmp(host, "::1") == 0;
}

static int addr_is_multicast(const char *host) {
    int a = atoi(host);
    return a >= 224 && a <= 239;
}

struct NowEventSink {
    now_sock           fd;
    struct sockaddr_in to;
    char               wire[16];
};

NOW_API NowEventSink *now_event_sink_open(const char *url, const char *wire) {
    char host[128];
    int port;
    NowEventSink *s;

    if (parse_events_url(url, host, sizeof(host), &port) != 0) return NULL;
    if (sockets_up() != 0) return NULL;

    s = (NowEventSink *)calloc(1, sizeof(*s));
    if (!s) return NULL;

    snprintf(s->wire, sizeof(s->wire), "%s", (wire && *wire) ? wire : "pasta");

    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd == NOW_SOCK_BAD) { free(s); return NULL; }

    s->to.sin_family = AF_INET;
    s->to.sin_port = htons((unsigned short)port);
    if (strcmp(host, "localhost") == 0)
        s->to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else
        s->to.sin_addr.s_addr = inet_addr(host);

    if (s->to.sin_addr.s_addr == INADDR_NONE) {
        now_sock_close(s->fd);
        free(s);
        return NULL;
    }
    return s;
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

NOW_API void now_event_sink_send(NowEventSink *sink, const NowEvent *ev) {
    char buf[NOW_EVENTS_MAX_DATAGRAM + 1];
    size_t n;
    int repeats, i;

    if (!sink || !ev) return;

    n = now_event_render(buf, sizeof(buf), ev, sink->wire);
    if (n == 0) return;

    repeats = now_event_is_terminal(ev->event) ? NOW_EVENTS_TERMINAL_REPEATS : 1;
    for (i = 0; i < repeats; i++) {
        /* The return value is deliberately ignored. There is nothing a
         * build could usefully do about a datagram that did not leave,
         * and specs/now-events-v1.md §7 makes that a requirement rather
         * than an oversight. */
        (void)sendto(sink->fd, buf, (int)n, 0,
                     (struct sockaddr *)&sink->to, sizeof(sink->to));
        if (i + 1 < repeats) sleep_ms(50);
    }
}

NOW_API void now_event_sink_close(NowEventSink *sink) {
    if (!sink) return;
    if (sink->fd != NOW_SOCK_BAD) now_sock_close(sink->fd);
    free(sink);
}

/* ---- listening ---- */

struct NowEventSource {
    now_sock fd;
};

NOW_API NowEventSource *now_event_source_open(const char *url, int insecure,
                                              NowResult *result) {
    char host[128];
    int port;
    struct sockaddr_in me;
    NowEventSource *s;

    if (parse_events_url(url, host, sizeof(host), &port) != 0) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "events URL must look like udp://host:port (got '%s')",
                     url ? url : "(null)");
        }
        return NULL;
    }

    /* v1 events are unsigned, so anything able to reach this socket can
     * forge any event. On loopback the sender is necessarily a local
     * process; anywhere else it is not. Enforcing that here rather than
     * in the documentation is what makes it a limitation instead of a
     * hole — see specs/now-events-v1.md §8. */
    if (addr_is_multicast(host)) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "multicast is not supported in events v1: events are "
                     "unsigned, so any host on the segment could forge one. "
                     "Signing is v2 (specs/now-events-v1.md section 8)");
        }
        return NULL;
    }
    if (!addr_is_loopback(host) && !insecure) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "refusing to listen on '%s': events v1 are unsigned, so "
                     "anything that can reach a non-loopback socket can forge "
                     "them. Use a loopback address, or --insecure if you have "
                     "decided the network is trusted", host);
        }
        return NULL;
    }

    if (sockets_up() != 0) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot initialise sockets");
        }
        return NULL;
    }

    s = (NowEventSource *)calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd == NOW_SOCK_BAD) {
        free(s);
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot create a UDP socket");
        }
        return NULL;
    }

    {
        int yes = 1;
        setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&yes, sizeof(yes));
    }

    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons((unsigned short)port);
    me.sin_addr.s_addr = addr_is_loopback(host)
                       ? htonl(INADDR_LOOPBACK)
                       : htonl(INADDR_ANY);

    if (bind(s->fd, (struct sockaddr *)&me, sizeof(me)) != 0) {
        now_sock_close(s->fd);
        free(s);
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot bind udp %s:%d — is something already listening?",
                     host, port);
        }
        return NULL;
    }
    return s;
}

NOW_API int now_event_source_recv(NowEventSource *src, NowEvent *ev,
                                  int timeout_ms) {
    char buf[NOW_EVENTS_MAX_DATAGRAM + 1];
    int n;

    if (!src || !ev) return -2;

    if (timeout_ms > 0) {
#ifdef _WIN32
        DWORD tv = (DWORD)timeout_ms;
        setsockopt(src->fd, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&tv, sizeof(tv));
#else
        struct timeval tv;
        tv.tv_sec = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
        setsockopt(src->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    n = recvfrom(src->fd, buf, (int)sizeof(buf) - 1, 0, NULL, NULL);
    if (n <= 0) return 0;      /* timeout, or an empty datagram */
    buf[n] = '\0';

    if (now_event_decode(ev, buf, (size_t)n) != 0) return -1;
    return 1;
}

NOW_API void now_event_source_close(NowEventSource *src) {
    if (!src) return;
    if (src->fd != NOW_SOCK_BAD) now_sock_close(src->fd);
    free(src);
}

/* ==== the emitter ====
 *
 * One per process. Off unless a destination was named, and every entry
 * point leaves immediately when it is off — a build that did not ask for
 * events pays one predicate per call site and nothing more.
 */

static struct {
    int            active;
    NowEventSink  *sink;
    FILE          *file;
    char           run[13];
    long           seq;
    char           project[192];
    char           phase[32];
    int            ok;
    time_t         started;
    time_t         last_progress;
    NowEventCounts last_counts;
    int            have_counts;
} g_ev;

static void ev_now_ts(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

NOW_API int now_events_active(void) { return g_ev.active; }

NOW_API void now_events_open(const char *url, const char *wire,
                             const char *file_path) {
    memset(&g_ev, 0, sizeof(g_ev));
    g_ev.ok = 1;
    g_ev.started = time(NULL);

    if (!url || !*url) url = getenv("NOW_EVENTS");
    if (!wire || !*wire) wire = getenv("NOW_EVENTS_WIRE");

    if (url && *url) g_ev.sink = now_event_sink_open(url, wire);

    if (file_path && *file_path) {
        /* The sidecar usually lives under target/, which does not exist
         * yet on a clean tree — the build creates it later. Without this
         * the fopen simply failed and the file half of §6 quietly did
         * not happen, which is the one guarantee a stalled waiter
         * depends on. Measured: a real failing build emitted every
         * datagram correctly and wrote no record at all. */
        char dir[PATH_MAX];
        char *sep;
        snprintf(dir, sizeof(dir), "%s", file_path);
        sep = strrchr(dir, '/');
        {
            char *bsep = strrchr(dir, '\\');
            if (bsep && (!sep || bsep > sep)) sep = bsep;
        }
        if (sep) {
            *sep = '\0';
            now_mkdir_p(dir);
        }
        g_ev.file = fopen(file_path, "ab");
    }

    if (!g_ev.sink && !g_ev.file) return;

    /* An id only has to separate runs that could overlap on one
     * destination. Two builds cannot share a pid in the same second, so
     * the pair is enough without pulling an entropy source into this
     * component. */
    {
        unsigned long a = (unsigned long)g_ev.started;
#ifdef _WIN32
        unsigned long b = (unsigned long)GetCurrentProcessId();
#else
        unsigned long b = (unsigned long)getpid();
#endif
        snprintf(g_ev.run, sizeof(g_ev.run), "%08lx%04lx", a, b & 0xffffUL);
    }
    g_ev.active = 1;
}

/* Fill the envelope, send, append to the sidecar. The sidecar is the
 * record and the datagram is the notification — §6. */
static void ev_emit(NowEventType type, const char *module,
                    const char *detail, int code,
                    const NowEventCounts *counts) {
    NowEvent ev;

    if (!g_ev.active) return;

    memset(&ev, 0, sizeof(ev));
    ev.v = NOW_EVENTS_SCHEMA_VERSION;
    snprintf(ev.run, sizeof(ev.run), "%s", g_ev.run);
    ev.seq = g_ev.seq++;
    ev_now_ts(ev.ts, sizeof(ev.ts));
    ev.event = type;
    snprintf(ev.phase, sizeof(ev.phase), "%s", g_ev.phase);
    ev.ok = g_ev.ok;
    snprintf(ev.project, sizeof(ev.project), "%s", g_ev.project);
    if (module) snprintf(ev.module, sizeof(ev.module), "%s", module);
    if (detail) snprintf(ev.detail, sizeof(ev.detail), "%s", detail);
    ev.code = code;
    ev.elapsed_ms = (type == NOW_EVENT_RUN_STARTED)
                  ? -1
                  : (long)(time(NULL) - g_ev.started) * 1000L;
    ev.counts.compiled = ev.counts.skipped = ev.counts.failed =
        ev.counts.passed = ev.counts.total = -1;
    if (counts) {
        ev.counts = *counts;
        g_ev.last_counts = *counts;
        g_ev.have_counts = 1;
    }
    ev.pid = 0;

    if (g_ev.sink) now_event_sink_send(g_ev.sink, &ev);

    if (g_ev.file) {
        char line[NOW_EVENTS_MAX_DATAGRAM + 2];
        size_t n = now_event_encode_json(line, sizeof(line) - 1, &ev);
        if (n) {
            line[n] = '\n';
            fwrite(line, 1, n + 1, g_ev.file);
            /* Flushed per line on purpose: the whole value of the file is
             * that a listener which missed a datagram can read it *now*,
             * and a buffered record of a build that is still running is
             * no record at all. */
            fflush(g_ev.file);
        }
    }
}

NOW_API void now_events_run_started(const char *phase, const char *project) {
    if (!g_ev.active) return;
    snprintf(g_ev.phase, sizeof(g_ev.phase), "%s", phase ? phase : "");
    if (project) snprintf(g_ev.project, sizeof(g_ev.project), "%s", project);
    ev_emit(NOW_EVENT_RUN_STARTED, NULL, NULL, -1, NULL);
}

NOW_API void now_events_phase_started(const char *phase) {
    if (!g_ev.active) return;
    if (phase) snprintf(g_ev.phase, sizeof(g_ev.phase), "%s", phase);
    ev_emit(NOW_EVENT_PHASE_STARTED, NULL, NULL, -1, NULL);
}

NOW_API void now_events_phase_finished(const char *phase, int ok,
                                       const NowEventCounts *counts) {
    if (!g_ev.active) return;
    if (phase) snprintf(g_ev.phase, sizeof(g_ev.phase), "%s", phase);
    if (!ok) g_ev.ok = 0;
    ev_emit(NOW_EVENT_PHASE_FINISHED, NULL, NULL, -1, counts);
}

NOW_API void now_events_progress(const NowEventCounts *counts) {
    time_t now;
    if (!g_ev.active) return;
    now = time(NULL);
    /* At most one per second (§5). A 32-way build finishing a hundred
     * translation units a second would otherwise say nothing useful very
     * loudly. */
    if (now == g_ev.last_progress) return;
    g_ev.last_progress = now;
    ev_emit(NOW_EVENT_RUN_PROGRESS, NULL, NULL, -1, counts);
}

NOW_API void now_events_module_failed(const char *module, const char *detail) {
    if (!g_ev.active) return;
    g_ev.ok = 0;
    ev_emit(NOW_EVENT_MODULE_FAILED, module, detail, -1, NULL);
}

NOW_API void now_events_test_failed(const char *name, const char *detail) {
    if (!g_ev.active) return;
    g_ev.ok = 0;
    ev_emit(NOW_EVENT_TEST_FAILED, name, detail, -1, NULL);
}

NOW_API void now_events_run_finished(int code, const NowEventCounts *counts) {
    if (!g_ev.active) return;
    if (code != 0) g_ev.ok = 0;
    if (!counts && g_ev.have_counts) counts = &g_ev.last_counts;
    ev_emit(NOW_EVENT_RUN_FINISHED, NULL, NULL, code, counts);
}

NOW_API void now_events_close(void) {
    if (g_ev.sink) now_event_sink_close(g_ev.sink);
    if (g_ev.file) fclose(g_ev.file);
    memset(&g_ev, 0, sizeof(g_ev));
}

/* ==== the listen loop ==== */

static int filter_admits(const char *filter, const char *name) {
    const char *p = filter;
    size_t nlen = strlen(name);

    if (!filter || !*filter) return 1;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t seg = comma ? (size_t)(comma - p) : strlen(p);
        if (seg == nlen && memcmp(p, name, nlen) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

NOW_API int now_events_listen(const NowEventListenOpts *opts,
                              NowResult *result) {
    NowEventSource *src;
    NowEvent ev;
    char line[NOW_EVENTS_MAX_DATAGRAM + 64];
    char cur_run[13] = {0};
    long expect_seq = -1;
    long last_seq = -1;
    int  waited_ms = 0;
    int  exit_code = 0;
    long unreadable = 0;

    if (!opts || !opts->url) return 1;

    src = now_event_source_open(opts->url, opts->insecure, result);
    if (!src) return 1;

    for (;;) {
        int rc = now_event_source_recv(src, &ev, 250);

        if (rc == 0) {
            waited_ms += 250;
            if (opts->timeout_sec > 0 &&
                waited_ms >= opts->timeout_sec * 1000) {
                if (result) {
                    result->code = NOW_ERR_IO;
                    /* Distinguish "nothing arrived" from "things arrived
                     * that I could not read". Reporting the second as
                     * the first sends people to check their network when
                     * the schema is what disagreed. */
                    if (unreadable > 0)
                        snprintf(result->message, sizeof(result->message),
                                 "timed out after %d seconds waiting for %s; "
                                 "%ld datagram(s) did arrive but were not "
                                 "readable as v%d events",
                                 opts->timeout_sec,
                                 opts->until ? opts->until : "events",
                                 unreadable, NOW_EVENTS_SCHEMA_VERSION);
                    else
                        snprintf(result->message, sizeof(result->message),
                                 "timed out after %d seconds waiting for %s; "
                                 "nothing arrived at all",
                                 opts->timeout_sec,
                                 opts->until ? opts->until : "events");
                }
                now_event_source_close(src);
                return 2;
            }
            continue;
        }
        if (rc == -1) { unreadable++; continue; }   /* not a v1 event */
        if (rc < 0) continue;                       /* the socket blinked */

        waited_ms = 0;

        /* A repeated terminal event carries the same (run, seq) on
         * purpose — see §6.3. Dropping the duplicate here is what lets
         * the emitter send it three times. */
        if (strcmp(cur_run, ev.run) == 0 && ev.seq == last_seq) continue;

        if (strcmp(cur_run, ev.run) != 0) {
            snprintf(cur_run, sizeof(cur_run), "%s", ev.run);
            expect_seq = ev.seq;
        }
        if (ev.seq > expect_seq) {
            /* Say it rather than paper over it: the file sidecar has the
             * events that did not arrive here. */
            fprintf(stderr,
                    "warning: missed %ld event(s) from run %s "
                    "(saw seq %ld, expected %ld)\n",
                    ev.seq - expect_seq, ev.run, ev.seq, expect_seq);
        }
        last_seq = ev.seq;
        expect_seq = ev.seq + 1;

        if (filter_admits(opts->filter, now_event_name(ev.event))) {
            size_t n = now_event_render(line, sizeof(line), &ev,
                                        opts->output ? opts->output : "pasta");
            if (n) {
                fwrite(line, 1, n, stdout);
                fputc('\n', stdout);
                fflush(stdout);
            }
        }

        if (opts->until && strcmp(opts->until, now_event_name(ev.event)) == 0) {
            /* Hand the build's own exit code back, so a shell can branch
             * on the result without parsing anything we printed. */
            if (ev.event == NOW_EVENT_RUN_FINISHED)
                exit_code = ev.code >= 0 ? ev.code : (ev.ok ? 0 : 1);
            break;
        }
    }

    now_event_source_close(src);
    if (result) { result->code = NOW_OK; result->message[0] = '\0'; }
    return exit_code;
}
