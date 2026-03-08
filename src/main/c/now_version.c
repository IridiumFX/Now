/*
 * now_version.c — Semantic versioning and range resolution (§6.9-6.10)
 */
#include "now_version.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Windows (MSVC and MinGW) lacks strndup */
#ifdef _WIN32
static char *now_strndup(const char *s, size_t n) {
    size_t len = strlen(s);
    if (n < len) len = n;
    char *p = malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}
#define strndup now_strndup
#endif

/* ---- SemVer parsing ---- */

static int parse_uint(const char **s, int *out) {
    if (!isdigit((unsigned char)**s)) return -1;
    *out = 0;
    while (isdigit((unsigned char)**s)) {
        *out = *out * 10 + (**s - '0');
        (*s)++;
    }
    return 0;
}

NOW_API int now_semver_parse(const char *str, NowSemVer *out) {
    if (!str || !out) return -1;
    memset(out, 0, sizeof(*out));

    const char *p = str;

    if (parse_uint(&p, &out->major) != 0) return -1;
    if (*p != '.') return -1; p++;
    if (parse_uint(&p, &out->minor) != 0) return -1;

    /* patch is optional for range shorthand like ~1.2 */
    if (*p == '.') {
        p++;
        if (parse_uint(&p, &out->patch) != 0) return -1;
    }

    /* pre-release */
    if (*p == '-') {
        p++;
        const char *start = p;
        while (*p && *p != '+') p++;
        if (p > start)
            out->prerelease = strndup(start, (size_t)(p - start));
    }

    /* build metadata */
    if (*p == '+') {
        p++;
        const char *start = p;
        while (*p) p++;
        if (p > start)
            out->build = strndup(start, (size_t)(p - start));
    }

    return (*p == '\0') ? 0 : -1;
}

NOW_API void now_semver_free(NowSemVer *v) {
    if (!v) return;
    free(v->prerelease);
    free(v->build);
    memset(v, 0, sizeof(*v));
}

/* ---- SemVer comparison ---- */

/* Compare pre-release identifiers per SemVer 2.0 §11:
 * - No prerelease > has prerelease (1.0.0 > 1.0.0-alpha)
 * - Numeric identifiers compared as integers
 * - Alphanumeric identifiers compared lexically
 * - Shorter pre-release is less if all preceding identifiers match */
static int compare_prerelease(const char *a, const char *b) {
    if (!a && !b) return 0;
    if (!a) return 1;   /* release > pre-release */
    if (!b) return -1;  /* pre-release < release */

    const char *pa = a, *pb = b;
    while (*pa && *pb) {
        /* Extract one dot-delimited identifier */
        const char *sa = pa, *sb = pb;
        while (*pa && *pa != '.') pa++;
        while (*pb && *pb != '.') pb++;
        size_t la = (size_t)(pa - sa), lb = (size_t)(pb - sb);

        /* Check if both are numeric */
        int a_num = 1, b_num = 1;
        for (size_t i = 0; i < la; i++)
            if (!isdigit((unsigned char)sa[i])) { a_num = 0; break; }
        for (size_t i = 0; i < lb; i++)
            if (!isdigit((unsigned char)sb[i])) { b_num = 0; break; }

        if (a_num && b_num) {
            int na = atoi(sa), nb = atoi(sb);
            if (na != nb) return na < nb ? -1 : 1;
        } else {
            /* Numeric < alphanumeric */
            if (a_num != b_num) return a_num ? -1 : 1;
            /* Lexical comparison */
            size_t min = la < lb ? la : lb;
            int cmp = memcmp(sa, sb, min);
            if (cmp != 0) return cmp < 0 ? -1 : 1;
            if (la != lb) return la < lb ? -1 : 1;
        }

        if (*pa == '.') pa++;
        if (*pb == '.') pb++;
    }
    /* Shorter pre-release < longer if all preceding match */
    if (*pa) return 1;
    if (*pb) return -1;
    return 0;
}

NOW_API int now_semver_compare(const NowSemVer *a, const NowSemVer *b) {
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    return compare_prerelease(a->prerelease, b->prerelease);
}

/* ---- Format ---- */

NOW_API char *now_semver_to_string(const NowSemVer *v) {
    if (!v) return NULL;

    /* Calculate buffer size */
    size_t len = 32; /* major.minor.patch */
    if (v->prerelease) len += strlen(v->prerelease) + 1;
    if (v->build) len += strlen(v->build) + 1;

    char *buf = malloc(len);
    if (!buf) return NULL;

    int n = snprintf(buf, len, "%d.%d.%d", v->major, v->minor, v->patch);
    if (v->prerelease) n += snprintf(buf + n, len - (size_t)n, "-%s", v->prerelease);
    if (v->build) snprintf(buf + n, len - (size_t)n, "+%s", v->build);

    return buf;
}

/* ---- Range parsing (§6.10) ---- */

/* Helper: parse version at *p, advance p past it */
static int parse_version_at(const char **p, NowSemVer *out) {
    const char *start = *p;
    /* Find end of version token */
    while (**p && !isspace((unsigned char)**p)) (*p)++;
    size_t len = (size_t)(*p - start);
    char *tmp = malloc(len + 1);
    if (!tmp) return -1;
    memcpy(tmp, start, len);
    tmp[len] = '\0';
    int rc = now_semver_parse(tmp, out);
    free(tmp);
    return rc;
}

static void skip_spaces(const char **p) {
    while (isspace((unsigned char)**p)) (*p)++;
}

NOW_API int now_range_parse(const char *str, NowVersionRange *out) {
    if (!str || !out) return -1;
    memset(out, 0, sizeof(*out));

    const char *p = str;
    skip_spaces(&p);

    if (*p == '\0') return -1;

    /* Wildcard */
    if (*p == '*' && (p[1] == '\0' || isspace((unsigned char)p[1]))) {
        out->kind = NOW_RANGE_ANY;
        return 0;
    }

    /* Caret range: ^1.2.3 */
    if (*p == '^') {
        p++;
        if (now_semver_parse(p, &out->floor) != 0) return -1;
        out->kind = NOW_RANGE_CARET;
        out->has_ceiling = 1;
        memset(&out->ceiling, 0, sizeof(out->ceiling));

        if (out->floor.major != 0) {
            /* ^1.2.3 → <2.0.0 */
            out->ceiling.major = out->floor.major + 1;
        } else {
            /* ^0.9.3 → <0.10.0 (minor is breaking for pre-1.0) */
            out->ceiling.minor = out->floor.minor + 1;
        }
        return 0;
    }

    /* Tilde range: ~1.2.3 or ~1.2 */
    if (*p == '~') {
        p++;
        if (now_semver_parse(p, &out->floor) != 0) return -1;
        out->kind = NOW_RANGE_TILDE;
        out->has_ceiling = 1;
        memset(&out->ceiling, 0, sizeof(out->ceiling));
        out->ceiling.major = out->floor.major;
        out->ceiling.minor = out->floor.minor + 1;
        return 0;
    }

    /* >=version  or  >=version <version (compound) */
    if (p[0] == '>' && p[1] == '=') {
        p += 2;
        skip_spaces(&p);
        if (parse_version_at(&p, &out->floor) != 0) return -1;
        skip_spaces(&p);

        if (*p == '<') {
            /* Compound: >=1.2.0 <2.0.0 */
            p++;
            skip_spaces(&p);
            if (parse_version_at(&p, &out->ceiling) != 0) {
                now_semver_free(&out->floor);
                return -1;
            }
            out->kind = NOW_RANGE_COMPOUND;
            out->has_ceiling = 1;
        } else if (*p == '\0') {
            out->kind = NOW_RANGE_GTE;
            out->has_ceiling = 0;
        } else {
            now_semver_free(&out->floor);
            return -1;
        }
        return 0;
    }

    /* Exact version: 1.2.3 */
    if (now_semver_parse(p, &out->floor) != 0) return -1;
    out->kind = NOW_RANGE_EXACT;
    out->has_ceiling = 0;
    return 0;
}

NOW_API void now_range_free(NowVersionRange *r) {
    if (!r) return;
    now_semver_free(&r->floor);
    now_semver_free(&r->ceiling);
    memset(r, 0, sizeof(*r));
}

/* ---- Range satisfaction ---- */

NOW_API int now_range_satisfies(const NowVersionRange *range, const NowSemVer *version) {
    if (!range || !version) return 0;

    switch (range->kind) {
    case NOW_RANGE_ANY:
        return 1;

    case NOW_RANGE_EXACT:
        return now_semver_compare(&range->floor, version) == 0;

    case NOW_RANGE_GTE:
        return now_semver_compare(version, &range->floor) >= 0;

    case NOW_RANGE_CARET:
    case NOW_RANGE_TILDE:
    case NOW_RANGE_COMPOUND:
        /* version >= floor AND version < ceiling */
        return now_semver_compare(version, &range->floor) >= 0 &&
               now_semver_compare(version, &range->ceiling) < 0;
    }
    return 0;
}

/* ---- Range intersection ---- */

/* Returns the higher of two versions */
static const NowSemVer *ver_max(const NowSemVer *a, const NowSemVer *b) {
    return now_semver_compare(a, b) >= 0 ? a : b;
}

/* Returns the lower of two versions */
static const NowSemVer *ver_min(const NowSemVer *a, const NowSemVer *b) {
    return now_semver_compare(a, b) <= 0 ? a : b;
}

/* Helper to copy a semver */
static void ver_copy(NowSemVer *dst, const NowSemVer *src) {
    dst->major = src->major;
    dst->minor = src->minor;
    dst->patch = src->patch;
    dst->prerelease = src->prerelease ? strdup(src->prerelease) : NULL;
    dst->build = src->build ? strdup(src->build) : NULL;
}

/* Effective ceiling for a range (for unbounded, returns a "max" sentinel) */
static NowSemVer MAX_VER = { 999999, 0, 0, NULL, NULL };

static const NowSemVer *effective_ceiling(const NowVersionRange *r) {
    if (r->kind == NOW_RANGE_ANY) return &MAX_VER;
    if (r->kind == NOW_RANGE_GTE) return &MAX_VER;
    if (r->kind == NOW_RANGE_EXACT) return NULL; /* special case */
    return &r->ceiling;
}

NOW_API int now_range_intersect(const NowVersionRange *a, const NowVersionRange *b,
                         NowVersionRange *out) {
    if (!a || !b || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* ANY intersected with anything is the other */
    if (a->kind == NOW_RANGE_ANY) {
        ver_copy(&out->floor, &b->floor);
        if (b->has_ceiling) ver_copy(&out->ceiling, &b->ceiling);
        out->kind = b->kind;
        out->has_ceiling = b->has_ceiling;
        return 0;
    }
    if (b->kind == NOW_RANGE_ANY) {
        ver_copy(&out->floor, &a->floor);
        if (a->has_ceiling) ver_copy(&out->ceiling, &a->ceiling);
        out->kind = a->kind;
        out->has_ceiling = a->has_ceiling;
        return 0;
    }

    /* EXACT intersected with range: check if the exact version satisfies */
    if (a->kind == NOW_RANGE_EXACT) {
        if (now_range_satisfies(b, &a->floor)) {
            ver_copy(&out->floor, &a->floor);
            out->kind = NOW_RANGE_EXACT;
            out->has_ceiling = 0;
            return 0;
        }
        return -1;
    }
    if (b->kind == NOW_RANGE_EXACT) {
        if (now_range_satisfies(a, &b->floor)) {
            ver_copy(&out->floor, &b->floor);
            out->kind = NOW_RANGE_EXACT;
            out->has_ceiling = 0;
            return 0;
        }
        return -1;
    }

    /* General case: both have floor, possibly ceiling */
    const NowSemVer *new_floor = ver_max(&a->floor, &b->floor);
    const NowSemVer *ceil_a = effective_ceiling(a);
    const NowSemVer *ceil_b = effective_ceiling(b);
    const NowSemVer *new_ceil = ver_min(ceil_a, ceil_b);

    int has_ceil = (ceil_a != &MAX_VER || ceil_b != &MAX_VER);

    /* Check for empty intersection */
    if (has_ceil && now_semver_compare(new_floor, new_ceil) >= 0)
        return -1; /* disjoint */

    ver_copy(&out->floor, new_floor);
    if (has_ceil) {
        ver_copy(&out->ceiling, new_ceil);
        out->has_ceiling = 1;
        out->kind = NOW_RANGE_COMPOUND;
    } else {
        out->has_ceiling = 0;
        out->kind = NOW_RANGE_GTE;
    }
    return 0;
}

/* ---- Coordinate parsing ---- */

NOW_API int now_coord_parse(const char *str, NowCoordinate *out) {
    if (!str || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* Format: group:artifact:version */
    const char *first = strchr(str, ':');
    if (!first) return -1;
    const char *second = strchr(first + 1, ':');
    if (!second) return -1;

    out->group    = strndup(str, (size_t)(first - str));
    out->artifact = strndup(first + 1, (size_t)(second - first - 1));
    out->version  = strdup(second + 1);

    if (!out->group || !out->artifact || !out->version) {
        now_coord_free(out);
        return -1;
    }
    return 0;
}

NOW_API void now_coord_free(NowCoordinate *c) {
    if (!c) return;
    free(c->group);
    free(c->artifact);
    free(c->version);
    memset(c, 0, sizeof(*c));
}
