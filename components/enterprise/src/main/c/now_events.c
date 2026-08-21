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
 * apennines has no datagram primitive yet; only dns.c uses SOCK_DGRAM
 * and it does so privately. A `t3/net/udp.h` is the natural home and
 * has been raised with them — until it exists this mirrors dns.c's
 * platform guards rather than inventing a different shape.
 */
#include "now_events.h"
#include "now_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
        if (mark_truncated) APPEND(",\"detail_truncated\":true");
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

NOW_API size_t now_event_encode(char *out, size_t out_cap, const NowEvent *ev) {
    size_t detail_len, n;

    if (!out || !ev || out_cap == 0) return 0;
    if (out_cap > NOW_EVENTS_MAX_DATAGRAM) out_cap = NOW_EVENTS_MAX_DATAGRAM;

    detail_len = strlen(ev->detail);
    n = encode_attempt(out, out_cap, ev, detail_len, ev->detail_truncated);
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
        bare.detail_truncated = 1;
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

NOW_API int now_event_decode(NowEvent *ev, const char *json, size_t len) {
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
    json_get_bool(json, len, "detail_truncated", &ev->detail_truncated);

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

/* ==== rendering ==== */

NOW_API size_t now_event_render(char *out, size_t out_cap,
                                const NowEvent *ev, const char *fmt) {
    if (!out || !ev || out_cap == 0) return 0;
    if (!fmt) fmt = "json";

    if (strcmp(fmt, "json") == 0)
        return now_event_encode(out, out_cap, ev);

    if (strcmp(fmt, "pasta") == 0) {
        /* Pasta strings carry no escapes, so anything that would need one
         * cannot appear. `detail` is the field that would, and it is
         * dropped rather than mangled — the JSON form is there for
         * callers who need it. This is the same constraint that kept
         * Pasta off the wire (§3). */
        int w = snprintf(out, out_cap,
            "{ v: %d, run: \"%s\", seq: %ld, ts: \"%s\", event: \"%s\", "
            "phase: \"%s\", ok: %s }",
            ev->v, ev->run, ev->seq, ev->ts, now_event_name(ev->event),
            ev->phase, ev->ok ? "true" : "false");
        return (w < 0 || (size_t)w >= out_cap) ? 0 : (size_t)w;
    }

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
};

NOW_API NowEventSink *now_event_sink_open(const char *url) {
    char host[128];
    int port;
    NowEventSink *s;

    if (parse_events_url(url, host, sizeof(host), &port) != 0) return NULL;
    if (sockets_up() != 0) return NULL;

    s = (NowEventSink *)calloc(1, sizeof(*s));
    if (!s) return NULL;

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

    n = now_event_encode(buf, sizeof(buf), ev);
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
            size_t n = now_event_render(line, sizeof(line), &ev, opts->output);
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
