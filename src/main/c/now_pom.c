/*
 * now_pom.c — Project Object Model loader
 *
 * Parses now.pasta into NowProject using the Pasta library.
 */
#include "now_pom.h"
#include "now.h"
#include "now_arch.h"
#include "now_fs.h"   /* now_path_join — machine-level config lookup */

#include "pasta.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- String array ---- */

NOW_API void now_strarray_init(NowStrArray *a) {
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

NOW_API int now_strarray_push(NowStrArray *a, const char *s) {
    if (a->count >= a->capacity) {
        size_t new_cap = a->capacity ? a->capacity * 2 : 4;
        char **tmp = realloc(a->items, new_cap * sizeof(char *));
        if (!tmp) return -1;
        a->items = tmp;
        a->capacity = new_cap;
    }
    a->items[a->count] = s ? strdup(s) : NULL;
    if (s && !a->items[a->count]) return -1;
    a->count++;
    return 0;
}

NOW_API void now_strarray_free(NowStrArray *a) {
    for (size_t i = 0; i < a->count; i++)
        free(a->items[i]);
    free(a->items);
    now_strarray_init(a);
}

/* ---- Dep array ---- */

NOW_API void now_deparray_init(NowDepArray *a) {
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

NOW_API int now_deparray_push(NowDepArray *a) {
    if (a->count >= a->capacity) {
        size_t new_cap = a->capacity ? a->capacity * 2 : 4;
        NowDep *tmp = realloc(a->items, new_cap * sizeof(NowDep));
        if (!tmp) return -1;
        a->items = tmp;
        a->capacity = new_cap;
    }
    memset(&a->items[a->count], 0, sizeof(NowDep));
    now_strarray_init(&a->items[a->count].exclude);
    return (int)a->count++;
}

static void now_dep_free(NowDep *d) {
    free(d->id);
    free(d->scope);
    now_strarray_free(&d->exclude);
}

NOW_API void now_deparray_free(NowDepArray *a) {
    for (size_t i = 0; i < a->count; i++)
        now_dep_free(&a->items[i]);
    free(a->items);
    now_deparray_init(a);
}

/* ---- Repo array ---- */

NOW_API void now_repoarray_init(NowRepoArray *a) {
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

NOW_API int now_repoarray_push(NowRepoArray *a) {
    if (a->count >= a->capacity) {
        size_t new_cap = a->capacity ? a->capacity * 2 : 4;
        NowRepo *tmp = realloc(a->items, new_cap * sizeof(NowRepo));
        if (!tmp) return -1;
        a->items = tmp;
        a->capacity = new_cap;
    }
    memset(&a->items[a->count], 0, sizeof(NowRepo));
    a->items[a->count].release = 1;  /* default: release=true */
    return (int)a->count++;
}

NOW_API void now_repoarray_free(NowRepoArray *a) {
    for (size_t i = 0; i < a->count; i++) {
        free(a->items[i].url);
        free(a->items[i].id);
        free(a->items[i].auth);
    }
    free(a->items);
    now_repoarray_init(a);
}

/* ---- Plugin array ---- */

NOW_API void now_pluginarray_init(NowPluginArray *a) {
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

NOW_API int now_pluginarray_push(NowPluginArray *a) {
    if (a->count >= a->capacity) {
        size_t new_cap = a->capacity ? a->capacity * 2 : 4;
        NowPlugin *tmp = realloc(a->items, new_cap * sizeof(NowPlugin));
        if (!tmp) return -1;
        a->items = tmp;
        a->capacity = new_cap;
    }
    memset(&a->items[a->count], 0, sizeof(NowPlugin));
    return (int)a->count++;
}

NOW_API void now_pluginarray_free(NowPluginArray *a) {
    for (size_t i = 0; i < a->count; i++) {
        free(a->items[i].id);
        free(a->items[i].type);
        free(a->items[i].phase);
        free(a->items[i].timeout);
        free(a->items[i].run);
        /* config is a PastaValue* owned by _pasta_root — do not free */
    }
    free(a->items);
    now_pluginarray_init(a);
}

/* ---- Helpers ---- */

static char *dup_string(const PastaValue *v) {
    if (!v || pasta_type(v) != PASTA_STRING) return NULL;
    return strdup(pasta_get_string(v));
}

static char *dup_map_str(const PastaValue *map, const char *key) {
    return dup_string(pasta_map_get(map, key));
}

static int get_map_bool(const PastaValue *map, const char *key, int def) {
    const PastaValue *v = pasta_map_get(map, key);
    if (!v) return def;
    if (pasta_type(v) == PASTA_BOOL) return pasta_get_bool(v);
    return def;
}

/* Load a string array from a Pasta array value */
static void load_strarray(NowStrArray *dst, const PastaValue *arr) {
    if (!arr || pasta_type(arr) != PASTA_ARRAY) return;
    size_t n = pasta_count(arr);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *elem = pasta_array_get(arr, i);
        if (elem && pasta_type(elem) == PASTA_STRING)
            now_strarray_push(dst, pasta_get_string(elem));
    }
}

/* Union in the machine-level private groups from ~/.now/config.pasta.
 *
 * §25.2 states that both the machine-level and the project-level lists
 * are checked and a group in either is private. Only the project level
 * was ever read, so an administrator setting org-wide policy in
 * config.pasta got a file that `now` opens for audit settings and for
 * the remote object cache, but never for this — the policy looked
 * applied and fenced nothing.
 *
 * Silent on every failure: no config file, unreadable, or malformed all
 * mean "no machine-level policy". A missing optional config must not
 * fail a build, and this runs on every descriptor load. */
static void load_machine_private_groups(NowStrArray *dst) {
    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home || !*home) return;

    char *dot_now = now_path_join(home, ".now");
    if (!dot_now) return;
    char *cfg = now_path_join(dot_now, "config.pasta");
    free(dot_now);
    if (!cfg) return;

    FILE *fp = fopen(cfg, "rb");
    free(cfg);
    if (!fp) return;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0) { fclose(fp); return; }

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return; }
    size_t nread = fread(buf, 1, (size_t)len, fp);
    buf[nread] = '\0';
    fclose(fp);

    PastaResult pr;
    PastaValue *root = pasta_parse(buf, nread, &pr);
    free(buf);
    if (!root) return;
    if (pr.code == PASTA_OK && pasta_type(root) == PASTA_MAP)
        load_strarray(dst, pasta_map_get(root, "private_groups"));
    pasta_free(root);
}

/* ---- Language-aware defaults ----
 *
 * Maven-style directory conventions, keyed off the primary language:
 *   c    -> src/main/c,   src/main/h,   src/main/h/internal,   src/test/c
 *   c++  -> src/main/cpp, src/main/hpp, src/main/hpp/internal, src/test/cpp
 *   java -> src/main/java,            (no headers)           src/test/java
 *   rust -> src/main/rust,            (no headers)           src/test/rust
 *   go   -> src/main/go,              (no headers)           src/test/go
 *
 * C++ gets fully distinct paths from C on purpose — they are different
 * languages with different idioms; sharing .h/.c roots muddles that.
 */
static void apply_maven_defaults(NowProject *p) {
    if (!p) return;
    const char *primary = (p->langs.count > 0) ? p->langs.items[0] : "c";
    int is_cpp  = (strcmp(primary, "c++")  == 0);
    int is_java = (strcmp(primary, "java") == 0);
    int is_rust = (strcmp(primary, "rust") == 0);
    int is_go   = (strcmp(primary, "go")   == 0);

    if (!p->sources.dir) {
        /* Remembered, because after this point the descriptor no longer
         * says whether the author chose this path or we did. A missing
         * directory the author named is a typo worth failing on; a
         * missing directory we invented is not. */
        p->sources.dir_is_default = 1;
        if      (is_cpp)  p->sources.dir = strdup("src/main/cpp");
        else if (is_java) p->sources.dir = strdup("src/main/java");
        else if (is_rust) p->sources.dir = strdup("src/main/rust");
        else if (is_go)   p->sources.dir = strdup("src/main/go");
        else              p->sources.dir = strdup("src/main/c");
    }

    if (!p->sources.headers) {
        if      (is_cpp)                      p->sources.headers = strdup("src/main/hpp");
        else if (is_java || is_rust || is_go) { /* no headers concept */ }
        else                                  p->sources.headers = strdup("src/main/h");
    }

    if (!p->sources.private_headers) {
        if      (is_cpp)                      p->sources.private_headers = strdup("src/main/hpp/internal");
        else if (is_java || is_rust || is_go) { /* none */ }
        else                                  p->sources.private_headers = strdup("src/main/h/internal");
    }

    if (!p->tests.dir) {
        if      (is_cpp)  p->tests.dir = strdup("src/test/cpp");
        else if (is_java) p->tests.dir = strdup("src/test/java");
        else if (is_rust) p->tests.dir = strdup("src/test/rust");
        else if (is_go)   p->tests.dir = strdup("src/test/go");
        else              p->tests.dir = strdup("src/test/c");
    }
}

/* ---- Section loaders ---- */

/* Load a KEY=VAL string array from either an array of "KEY=VAL" strings
 * or a map { KEY: "VAL", ... }. Used for tests.defines / tests.env so
 * users can write either syntax.
 *
 * quote_values: in MAP form only, wrap each value in literal "..." so
 * preprocessor consumers get a C string literal. This is the right
 * semantics for tests.defines (paths going to fopen are strings) and
 * the wrong semantics for tests.env (env vars are raw); the caller
 * picks. Array form ALWAYS passes through unchanged. */
static void load_kv_strarray(NowStrArray *dst, const PastaValue *src,
                              int quote_values) {
    if (!src) return;
    if (pasta_type(src) == PASTA_ARRAY) {
        size_t n = pasta_count(src);
        for (size_t i = 0; i < n; i++) {
            const PastaValue *e = pasta_array_get(src, i);
            if (e && pasta_type(e) == PASTA_STRING)
                now_strarray_push(dst, pasta_get_string(e));
        }
    } else if (pasta_type(src) == PASTA_MAP) {
        size_t n = pasta_count(src);
        for (size_t i = 0; i < n; i++) {
            const char *k = pasta_map_key(src, i);
            const PastaValue *v = pasta_map_value(src, i);
            if (!k || !v || pasta_type(v) != PASTA_STRING) continue;
            size_t klen = strlen(k);
            const char *vs = pasta_get_string(v);
            size_t vlen = vs ? strlen(vs) : 0;
            size_t buflen = klen + 1 + (quote_values ? 2 : 0) + vlen + 1;
            char *buf = (char *)malloc(buflen);
            if (!buf) continue;
            size_t off = 0;
            memcpy(buf + off, k, klen); off += klen;
            buf[off++] = '=';
            if (quote_values) buf[off++] = '"';
            if (vs) { memcpy(buf + off, vs, vlen); off += vlen; }
            if (quote_values) buf[off++] = '"';
            buf[off] = '\0';
            now_strarray_push(dst, buf);
            free(buf);
        }
    }
}

static void load_sources(NowSources *dst, const PastaValue *src) {
    if (!src || pasta_type(src) != PASTA_MAP) return;
    dst->dir             = dup_map_str(src, "dir");
    dst->headers         = dup_map_str(src, "headers");
    dst->private_headers = dup_map_str(src, "private");
    dst->pattern         = dup_map_str(src, "pattern");
    dst->mode            = dup_map_str(src, "mode");
    load_strarray(&dst->include, pasta_map_get(src, "include"));
    load_strarray(&dst->exclude, pasta_map_get(src, "exclude"));
    load_kv_strarray(&dst->defines, pasta_map_get(src, "defines"), 1);
    load_kv_strarray(&dst->env,     pasta_map_get(src, "env"),     0);
}

/* OS-conditional sub-blocks in compile/link/etc.
 *
 * Pasta:
 *   link: {
 *     libs: ["m"],
 *     windows: { libs: ["ole32", "uuid", "shell32", "advapi32"] },
 *     posix:   { libs: ["pthread"] },
 *     linux:   { libs: ["rt"] }
 *   }
 *
 * Resolution: when loading, each sub-block whose key matches the host's
 * OS (or a group alias like 'posix'/'unix') is APPENDED into the parent
 * arrays. Non-matching blocks are ignored.
 *
 * Recognized keys:
 *   windows, linux, macos, freebsd, openbsd, netbsd  (specific OS)
 *   posix, unix                                       (anything not windows) */
static int os_block_matches(const char *key) {
    const NowTriple *host = now_host_triple_parsed();
    if (!host || !host->os[0]) return 0;
    const char *hos = host->os;
    if (strcmp(key, hos) == 0) return 1;
    if ((strcmp(key, "posix") == 0 || strcmp(key, "unix") == 0)
        && strcmp(hos, "windows") != 0)
        return 1;
    return 0;
}

static int is_os_block_key(const char *key) {
    static const char *keys[] = {
        "windows", "linux", "macos", "freebsd", "openbsd", "netbsd",
        "posix", "unix", NULL
    };
    for (const char **k = keys; *k; k++)
        if (strcmp(key, *k) == 0) return 1;
    return 0;
}

/* Forward decls so the OS-merge helpers can call back into the loaders. */
static void load_compile(NowCompile *dst, const PastaValue *src);
static void load_link(NowLink *dst, const PastaValue *src);

/* Walk OS-named sub-blocks; for each one that matches the host, call
 * `merge` to append its contents into dst. */
static void apply_os_overrides(void *dst, const PastaValue *src,
                                void (*merge)(void *, const PastaValue *)) {
    if (!src || pasta_type(src) != PASTA_MAP) return;
    size_t n = pasta_count(src);
    for (size_t i = 0; i < n; i++) {
        const char *key = pasta_map_key(src, i);
        const PastaValue *val = pasta_map_value(src, i);
        if (!key || !val || pasta_type(val) != PASTA_MAP) continue;
        if (!is_os_block_key(key)) continue;
        if (!os_block_matches(key))  continue;
        merge(dst, val);
    }
}

/* Type-erased dispatch wrappers for apply_os_overrides. */
static void merge_compile(void *dst, const PastaValue *src) {
    load_compile((NowCompile *)dst, src);
}
static void merge_link(void *dst, const PastaValue *src) {
    load_link((NowLink *)dst, src);
}

static void load_compile(NowCompile *dst, const PastaValue *src) {
    if (!src || pasta_type(src) != PASTA_MAP) return;
    load_strarray(&dst->flags,    pasta_map_get(src, "flags"));
    load_strarray(&dst->warnings, pasta_map_get(src, "warnings"));
    load_strarray(&dst->defines,  pasta_map_get(src, "defines"));
    load_strarray(&dst->includes, pasta_map_get(src, "includes"));
    /* Scalar fields: only set if not already set (so a parent block's
     * std doesn't get clobbered when an OS sub-block is merged in). */
    if (!dst->std) dst->std = dup_map_str(src, "std");
    if (!dst->opt) dst->opt = dup_map_str(src, "opt");
    /* Merge in any OS sub-blocks that match the host. */
    apply_os_overrides(dst, src, merge_compile);
}

static void load_link(NowLink *dst, const PastaValue *src) {
    if (!src || pasta_type(src) != PASTA_MAP) return;
    if (get_map_bool(src, "inherit_target", 0)) dst->inherit_target = 1;
    load_strarray(&dst->flags,    pasta_map_get(src, "flags"));
    load_strarray(&dst->libs,     pasta_map_get(src, "libs"));
    load_strarray(&dst->libdirs,  pasta_map_get(src, "libdirs"));
    load_strarray(&dst->archives, pasta_map_get(src, "archives"));
    if (!dst->script)      dst->script      = dup_map_str(src, "script");
    if (!dst->script_body) dst->script_body = dup_map_str(src, "script_body");
    apply_os_overrides(dst, src, merge_link);
}

/* Parse an inheritance policy block (§1.11).
 *
 * Shapes accepted:
 *   { version: true, compile: false, ... }   per-field
 *   *                                        everything  (string or label)
 *   null                                     nothing
 *   true / false                             everything / nothing
 *
 * Fields left unmentioned stay -1; resolve_inherit() applies the
 * spec's `true` default only after the module block has had its say. */
static void load_inherit(NowInherit *dst, const PastaValue *src) {
    dst->declared = 0;
    dst->all = dst->version = dst->deps = -1;
    dst->compile = dst->link = dst->profiles = dst->properties = -1;
    if (!src) return;
    dst->declared = 1;

    int t = pasta_type(src);
    if (t == PASTA_NULL) { dst->all = 0; return; }
    if (t == PASTA_BOOL) { dst->all = pasta_get_bool(src) ? 1 : 0; return; }
    if (t == PASTA_STRING || t == PASTA_LABEL) {
        const char *s = pasta_get_string(src);
        if (s && strcmp(s, "*") == 0) dst->all = 1;
        return;
    }
    if (t != PASTA_MAP) return;

    dst->version    = get_map_bool(src, "version",    -1);
    dst->deps       = get_map_bool(src, "deps",       -1);
    dst->compile    = get_map_bool(src, "compile",    -1);
    dst->link       = get_map_bool(src, "link",       -1);
    dst->profiles   = get_map_bool(src, "profiles",   -1);
    dst->properties = get_map_bool(src, "properties", -1);
}

/* ---- Descriptor key diagnostics ----
 *
 * A descriptor key that neither list below mentions is either a typo or
 * a field from a newer spec than this binary. Either way the loader
 * drops it, and dropping it silently is how a correct-looking
 * `inherit_defaults` block can sit in a workspace root for weeks doing
 * nothing. Say so instead. */

/* Top-level keys the loader reads and acts on. */
static const char *const k_known_keys[] = {
    "group", "artifact", "version", "name", "description", "url", "license",
    "lang", "langs", "std", "sources", "tests", "output", "compile", "link",
    "deps", "depends", "repos", "convergence", "private_groups", "resolve",
    "plugins",
    "components", "vendored", "modules", "java", "arch", "target_flags",
    "assembly",
    /* Read out-of-band from the raw Pasta tree rather than into
     * NowProject, so they have no struct field to grep for — but they
     * are live config and must not be reported as unknown:
     *   reproducible -> now_repro.c   trust -> now_trust.c
     *   advisories   -> now_advisory.c */
    "reproducible", "trust", "advisories",
    "inherit_defaults", "inherit", "workspace_mode", NULL
};

/* Top-level keys the spec defines but this build does not act on. These
 * parse fine and are then ignored — warn so nobody plans around them. */
static const char *const k_inert_keys[] = {
    "profiles", "properties", "walk_boundary", "inheritance",
    /* Parsed by nobody: reported as "recognized but not implemented"
     * rather than "unknown", which is the difference between a user
     * checking their spelling and a user checking the roadmap. */
    "allow_prerelease",
    /* Documented in the spec's own top-level field table (§1.13) and
     * implemented by nothing. They were reported as *unknown* keys,
     * which tells someone who copied the field straight out of the spec
     * that they misspelled it. "Not implemented" is the true answer.
     *   build_options — advanced build phase options
     *   target/targets — per-triple compile overrides (the --target flag
     *                    and arch: tags exist; the descriptor block does not)
     *   toolchain     — named presets, incl. cross:<triple>; only the CC/CXX
     *                   environment variables and PATH lookup are implemented
     *   toolchains    — the §11.8 per-triple compiler map. Its sibling
     *                   `target_flags` (§11.9) is implemented and lives in
     *                   the known list above; this one is not, and reporting
     *                   it as a typo told a cross-compiling user their
     *                   spelling was wrong when it was the build's coverage
     *                   that was.
     *   parent        — published parent descriptor coordinate
     */
    "build_options", "target", "targets", "toolchain", "toolchains", "parent",
    "publish", NULL
};

static int str_in_list(const char *const *list, const char *s) {
    for (size_t i = 0; list[i]; i++)
        if (strcmp(list[i], s) == 0) return 1;
    return 0;
}

/* Nested fields that parse and are then never read.
 *
 * The key diagnostics only ever walked the top level, so every one of
 * these was accepted in silence — which is how they survived: a user
 * writes `output: { dir: "dist" }`, sees no complaint, and reasonably
 * concludes the build honours it. A silently ignored field is worse
 * than a missing one, because nothing tells you it did nothing.
 *
 * Warn rather than reject: these appear in real descriptors already,
 * and failing a build on a field that has always been a no-op would
 * break working projects to no benefit.
 *
 * Verified dead 2026-08-12 — check before adding, and delete an entry
 * the moment it is implemented. */
typedef struct {
    const char        *section;   /* top-level key */
    int                is_array;  /* section holds an array of maps */
    const char *const *dead;      /* NULL-terminated */
} NowDeadNested;

static const char *const k_dead_output[]  = { "dir", NULL };
static const char *const k_dead_link[]    = { "script_body", NULL };
/* `sources:` and `tests:` are read by the SAME loader, so both accept
 * every field either one names — and are then read by different code.
 * The asymmetry is the whole reason these two lists differ:
 * `sources.include` is read in five places and `tests.include` in none;
 * `tests.defines` reaches the test compile and `sources.defines`
 * reaches nothing. A symmetric parse with an asymmetric use is exactly
 * the shape that gets documented wrongly, and was — the spec claimed
 * `tests.include` worked until 2026-08-24. */
static const char *const k_dead_sources[] = { "mode", "defines", "env", NULL };
static const char *const k_dead_tests[]   = { "pattern", "headers",
                                              "private_headers",
                                              "include", NULL };
static const char *const k_dead_deps[]    = { "exclude", "volatile", NULL };
static const char *const k_dead_repos[]   = { "release", "snapshot",
                                              "auth", NULL };
static const char *const k_dead_plugins[] = { "type", "run", NULL };
static const char *const k_dead_inherit[] = { "profiles", "properties", NULL };

static const NowDeadNested k_dead_nested[] = {
    { "output",  0, k_dead_output  },
    { "link",    0, k_dead_link    },
    { "sources", 0, k_dead_sources },
    { "tests",   0, k_dead_tests   },
    { "deps",    1, k_dead_deps    },
    { "repos",   1, k_dead_repos   },
    { "plugins", 1, k_dead_plugins },
    { "inherit", 0, k_dead_inherit },
    { NULL, 0, NULL }
};

static void warn_dead_map(const PastaValue *m, const char *const *dead,
                           const char *section, const char *where) {
    if (!m || pasta_type(m) != PASTA_MAP) return;
    for (size_t i = 0; dead[i]; i++) {
        if (!pasta_map_get(m, dead[i])) continue;
        fprintf(stderr,
                "warning: %s: '%s.%s' is parsed but has no effect in this "
                "build - it is ignored\n", where, section, dead[i]);
    }
}

static void warn_dead_nested_keys(const PastaValue *root, const char *path) {
    if (!root || pasta_type(root) != PASTA_MAP) return;
    const char *where = path ? path : "now.pasta";
    for (size_t s = 0; k_dead_nested[s].section; s++) {
        const PastaValue *sec = pasta_map_get(root, k_dead_nested[s].section);
        if (!sec) continue;
        if (k_dead_nested[s].is_array) {
            if (pasta_type(sec) != PASTA_ARRAY) continue;
            size_t n = pasta_count(sec);
            /* One warning per section, not per element — a dep list with
             * twenty entries carrying `exclude` should say so once. */
            /* One flag per dead key in this section — sized from the
             * table rather than guessed, so adding a ninth entry cannot
             * silently stop reporting. */
            size_t ndead = 0;
            while (k_dead_nested[s].dead[ndead]) ndead++;
            int *said = (int *)calloc(ndead ? ndead : 1, sizeof(int));
            if (!said) continue;
            for (size_t e = 0; e < n; e++) {
                const PastaValue *el = pasta_array_get(sec, e);
                if (!el || pasta_type(el) != PASTA_MAP) continue;
                for (size_t k = 0; k < ndead; k++) {
                    if (said[k]) continue;
                    if (!pasta_map_get(el, k_dead_nested[s].dead[k])) continue;
                    said[k] = 1;
                    fprintf(stderr,
                            "warning: %s: '%s[].%s' is parsed but has no "
                            "effect in this build - it is ignored\n",
                            where, k_dead_nested[s].section,
                            k_dead_nested[s].dead[k]);
                }
            }
            free(said);
        } else {
            warn_dead_map(sec, k_dead_nested[s].dead,
                          k_dead_nested[s].section, where);
        }
    }
}

static void warn_descriptor_keys(const PastaValue *root, const char *path) {
    if (!root || pasta_type(root) != PASTA_MAP) return;
    const char *where = path ? path : "now.pasta";
    size_t n = pasta_count(root);
    for (size_t i = 0; i < n; i++) {
        const char *k = pasta_map_key(root, i);
        if (!k || !*k) continue;
        if (str_in_list(k_known_keys, k)) continue;
        if (str_in_list(k_inert_keys, k)) {
            fprintf(stderr,
                    "warning: %s: '%s' is recognized but not implemented "
                    "in this build - it is ignored\n", where, k);
        } else {
            fprintf(stderr,
                    "warning: %s: unknown key '%s' - ignored\n", where, k);
        }
    }
}

static void load_output(NowOutput *dst, const PastaValue *src) {
    if (!src || pasta_type(src) != PASTA_MAP) return;
    dst->type = dup_map_str(src, "type");
    dst->name = dup_map_str(src, "name");
    dst->dir  = dup_map_str(src, "dir");
}

static void load_arch(NowArchDict *dst, const PastaValue *src) {
    if (!src || pasta_type(src) != PASTA_MAP) return;
    load_strarray(&dst->tags, pasta_map_get(src, "tags"));
    const PastaValue *aliases = pasta_map_get(src, "aliases");
    if (!aliases || pasta_type(aliases) != PASTA_MAP) return;
    size_t n = pasta_count(aliases);
    for (size_t i = 0; i < n; i++) {
        const char *k = pasta_map_key(aliases, i);
        const PastaValue *v = pasta_map_value(aliases, i);
        if (!k || !v || pasta_type(v) != PASTA_STRING) continue;
        now_strarray_push(&dst->alias_keys, k);
        now_strarray_push(&dst->alias_values, pasta_get_string(v));
    }
}

NOW_API const char *now_arch_dict_resolve(const NowArchDict *d, const char *name) {
    if (!d || !name) return name;
    for (size_t i = 0; i < d->alias_keys.count; i++) {
        if (strcmp(d->alias_keys.items[i], name) == 0)
            return d->alias_values.items[i];
    }
    return name;
}

NOW_API int now_arch_dict_is_gate(const NowArchDict *d, const char *name) {
    if (!d || !name || d->tags.count == 0) return 0;
    const char *canon = now_arch_dict_resolve(d, name);
    for (size_t i = 0; i < d->tags.count; i++) {
        if (strcmp(d->tags.items[i], canon) == 0) return 1;
    }
    return 0;
}

static void load_deps(NowDepArray *dst, const PastaValue *arr) {
    if (!arr || pasta_type(arr) != PASTA_ARRAY) return;
    size_t n = pasta_count(arr);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *elem = pasta_array_get(arr, i);
        if (!elem || pasta_type(elem) != PASTA_MAP) continue;
        int idx = now_deparray_push(dst);
        if (idx < 0) return;
        NowDep *d = &dst->items[idx];
        d->id          = dup_map_str(elem, "id");
        /* Accept the Maven-style long form `{group, artifact, version}`
         * as an alternative to the `id: "g:a:v"` shorthand — synthesize
         * the id string when the shorthand is absent but the discrete
         * fields are present. Either form alone is sufficient; if the
         * caller supplies both, `id:` wins. */
        if (!d->id) {
            const PastaValue *pg = pasta_map_get(elem, "group");
            const PastaValue *pa = pasta_map_get(elem, "artifact");
            const PastaValue *pv = pasta_map_get(elem, "version");
            const char *g = (pg && pasta_type(pg) == PASTA_STRING) ? pasta_get_string(pg) : NULL;
            const char *a = (pa && pasta_type(pa) == PASTA_STRING) ? pasta_get_string(pa) : NULL;
            const char *v = (pv && pasta_type(pv) == PASTA_STRING) ? pasta_get_string(pv) : NULL;
            if (g && a) {
                size_t need = strlen(g) + strlen(a) + (v ? strlen(v) : 1) + 3;
                char *buf = malloc(need);
                if (buf) {
                    snprintf(buf, need, "%s:%s:%s", g, a, v ? v : "*");
                    d->id = buf;
                }
            }
        }
        d->scope       = dup_map_str(elem, "scope");
        d->optional    = get_map_bool(elem, "optional", 0);
        d->is_volatile = get_map_bool(elem, "volatile", 0);
        d->override    = get_map_bool(elem, "override", 0);
        load_strarray(&d->exclude, pasta_map_get(elem, "exclude"));
    }
}

static void load_repos(NowRepoArray *dst, const PastaValue *arr) {
    if (!arr || pasta_type(arr) != PASTA_ARRAY) return;
    size_t n = pasta_count(arr);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *elem = pasta_array_get(arr, i);
        if (!elem) continue;
        int idx = now_repoarray_push(dst);
        if (idx < 0) return;
        NowRepo *r = &dst->items[idx];
        if (pasta_type(elem) == PASTA_STRING) {
            /* String shorthand: just a URL */
            r->url = strdup(pasta_get_string(elem));
        } else if (pasta_type(elem) == PASTA_MAP) {
            r->url      = dup_map_str(elem, "url");
            r->id       = dup_map_str(elem, "id");
            r->release  = get_map_bool(elem, "release", 1);
            r->snapshot = get_map_bool(elem, "snapshot", 0);
            r->auth     = dup_map_str(elem, "auth");
        }
    }
}

static void load_plugins(NowPluginArray *dst, const PastaValue *arr) {
    if (!arr || pasta_type(arr) != PASTA_ARRAY) return;
    size_t n = pasta_count(arr);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *elem = pasta_array_get(arr, i);
        if (!elem || pasta_type(elem) != PASTA_MAP) continue;
        int idx = now_pluginarray_push(dst);
        if (idx < 0) return;
        NowPlugin *pl = &dst->items[idx];
        pl->id      = dup_map_str(elem, "id");
        pl->type    = dup_map_str(elem, "type");
        pl->phase   = dup_map_str(elem, "phase");
        pl->timeout = dup_map_str(elem, "timeout");
        pl->run     = dup_map_str(elem, "run");
        /* Keep config as raw PastaValue* — owned by _pasta_root */
        pl->config  = (void *)pasta_map_get(elem, "config");
    }
}

/* Load langs — handles both "lang: 'c'" scalar and "langs: [...]" array */
static void load_langs(NowProject *p, const PastaValue *root) {
    const PastaValue *langs_val = pasta_map_get(root, "langs");
    const PastaValue *lang_val  = pasta_map_get(root, "lang");

    if (langs_val && pasta_type(langs_val) == PASTA_ARRAY) {
        /* langs: ["c", "c++"] — only string elements for now */
        size_t n = pasta_count(langs_val);
        for (size_t i = 0; i < n; i++) {
            const PastaValue *elem = pasta_array_get(langs_val, i);
            if (elem && pasta_type(elem) == PASTA_STRING)
                now_strarray_push(&p->langs, pasta_get_string(elem));
        }
    } else if (lang_val && pasta_type(lang_val) == PASTA_STRING) {
        /* lang: "c" or lang: "mixed" */
        const char *s = pasta_get_string(lang_val);
        if (strcmp(s, "mixed") == 0) {
            now_strarray_push(&p->langs, "c");
            now_strarray_push(&p->langs, "c++");
        } else {
            now_strarray_push(&p->langs, s);
        }
    }
}

/* ---- Public API ---- */

static void now_sources_init(NowSources *s) {
    memset(s, 0, sizeof(*s));
    now_strarray_init(&s->include);
    now_strarray_init(&s->exclude);
}

static void now_sources_free(NowSources *s) {
    free(s->dir);
    free(s->headers);
    free(s->private_headers);
    free(s->pattern);
    free(s->mode);
    now_strarray_free(&s->include);
    now_strarray_free(&s->exclude);
    now_strarray_free(&s->defines);
    now_strarray_free(&s->env);
}

static void now_compile_init(NowCompile *c) {
    memset(c, 0, sizeof(*c));
    now_strarray_init(&c->flags);
    now_strarray_init(&c->warnings);
    now_strarray_init(&c->defines);
    now_strarray_init(&c->includes);
}

static void now_compile_free(NowCompile *c) {
    now_strarray_free(&c->flags);
    now_strarray_free(&c->warnings);
    now_strarray_free(&c->defines);
    now_strarray_free(&c->includes);
    free(c->std);
    free(c->opt);
}

static void now_link_init(NowLink *l) {
    memset(l, 0, sizeof(*l));
    now_strarray_init(&l->flags);
    now_strarray_init(&l->libs);
    now_strarray_init(&l->libdirs);
    now_strarray_init(&l->archives);
}

static void now_link_free(NowLink *l) {
    now_strarray_free(&l->flags);
    now_strarray_free(&l->libs);
    now_strarray_free(&l->libdirs);
    now_strarray_free(&l->archives);
    free(l->script);
    free(l->script_body);
}

/* ---- Assembly: extra files a package carries (§24, DRAFT) ---- */

static void now_assembly_free(NowAssembly *a) {
    if (!a) return;
    for (size_t i = 0; i < a->count; i++) {
        free(a->items[i].src);
        free(a->items[i].dest);
        now_strarray_free(&a->items[i].exclude);
    }
    free(a->items);
    a->items = NULL;
    a->count = a->cap = 0;
}

/* Read one assembly definition's `include:` list into dst. */
static void load_assembly_includes(NowAssembly *dst, const PastaValue *asmdef,
                                    const char *where) {
    if (!asmdef || pasta_type(asmdef) != PASTA_MAP) return;

    /* §17.1 gives an assembly an id, an output format and an output
     * directory as well as its include list. Only the include list is
     * implemented, and a format of "lha" silently producing a `.basta`
     * would be worse than not accepting it — say so once, here, rather
     * than let someone plan an Amiga distribution around it. */
    {
        static const char *const unimpl[] = { "id", "format", "out", NULL };
        for (size_t k = 0; unimpl[k]; k++) {
            if (pasta_map_get(asmdef, unimpl[k]))
                fprintf(stderr,
                        "warning: %s: assembly '%s' is recognized but not "
                        "implemented in this build - the include list is "
                        "honoured, everything else is ignored\n",
                        where ? where : "now.pasta", unimpl[k]);
        }
    }

    const PastaValue *inc = pasta_map_get(asmdef, "include");
    if (!inc || pasta_type(inc) != PASTA_ARRAY) return;

    size_t n = pasta_count(inc);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *e = pasta_array_get(inc, i);
        if (!e || pasta_type(e) != PASTA_MAP) continue;

        char *s = dup_map_str(e, "src");
        if (!s || !*s) { free(s); continue; }   /* an entry with no src selects nothing */

        if (dst->count == dst->cap) {
            size_t cap = dst->cap ? dst->cap * 2 : 4;
            NowAssemblyInclude *grown =
                (NowAssemblyInclude *)realloc(dst->items, cap * sizeof(*grown));
            if (!grown) { free(s); return; }
            dst->items = grown;
            dst->cap   = cap;
        }
        NowAssemblyInclude *a = &dst->items[dst->count];
        memset(a, 0, sizeof(*a));
        now_strarray_init(&a->exclude);
        a->src  = s;
        a->dest = dup_map_str(e, "dest");
        load_strarray(&a->exclude, pasta_map_get(e, "exclude"));
        dst->count++;
    }
}

/* §17.1 makes `assembly:` an ARRAY of assembly definitions, because
 * multiple assemblies may be declared and each produces its own
 * artifact. Only one artifact is produced in this build, so every
 * definition's include list is merged into one — which is right for a
 * tree with one assembly and wrong for a tree with two, and the second
 * case will need `format:`/`out:` to exist before it can mean anything.
 *
 * A bare map is also accepted, and only because §17.1's own prose
 * ("multiple assemblies MAY be declared") makes the single case the
 * common one; it reads as the degenerate array. This is the one place a
 * second spelling is tolerated, and it should not be copied. */
static void load_assembly(NowAssembly *dst, const PastaValue *src,
                           const char *where) {
    if (!dst || !src) return;
    if (pasta_type(src) == PASTA_ARRAY) {
        size_t n = pasta_count(src);
        for (size_t i = 0; i < n; i++)
            load_assembly_includes(dst, pasta_array_get(src, i), where);
    } else if (pasta_type(src) == PASTA_MAP) {
        load_assembly_includes(dst, src, where);
    }
}

/* ---- Per-triple compile/link overrides (§11.9) ---- */

static void now_target_flags_free(NowTargetFlagsArray *a) {
    if (!a) return;
    for (size_t i = 0; i < a->count; i++) {
        free(a->items[i].pattern);
        now_compile_free(&a->items[i].compile);
        now_link_free(&a->items[i].link);
    }
    free(a->items);
    a->items = NULL;
    a->count = a->cap = 0;
}

/* Read `target_flags: { "<pattern>": { compile: {...}, link: {...} } }`.
 *
 * Order is the descriptor's order — Pasta maps preserve it and §11.9
 * makes the merge order-dependent, so this cannot be a lookup table. */
static void load_target_flags(NowTargetFlagsArray *dst, const PastaValue *src) {
    if (!dst || !src || pasta_type(src) != PASTA_MAP) return;
    size_t n = pasta_count(src);
    for (size_t i = 0; i < n; i++) {
        const char       *k = pasta_map_key(src, i);
        const PastaValue *v = pasta_map_value(src, i);
        if (!k || !*k || !v || pasta_type(v) != PASTA_MAP) continue;

        if (dst->count == dst->cap) {
            size_t cap = dst->cap ? dst->cap * 2 : 4;
            NowTargetFlags *grown =
                (NowTargetFlags *)realloc(dst->items, cap * sizeof(*grown));
            if (!grown) return;
            dst->items = grown;
            dst->cap   = cap;
        }

        NowTargetFlags *e = &dst->items[dst->count];
        memset(e, 0, sizeof(*e));
        now_compile_init(&e->compile);
        now_link_init(&e->link);
        e->pattern = strdup(k);
        if (!e->pattern) {
            now_compile_free(&e->compile);
            now_link_free(&e->link);
            return;
        }
        load_compile(&e->compile, pasta_map_get(v, "compile"));
        load_link(&e->link,       pasta_map_get(v, "link"));
        dst->count++;
    }
}

static void append_strarray(NowStrArray *dst, const NowStrArray *src) {
    for (size_t i = 0; i < src->count; i++)
        now_strarray_push(dst, src->items[i]);
}

/* Scalars replace; a pattern that does not state one leaves the base
 * alone. `replace` rather than `fill if empty` is what §11.9 says, and
 * it is the only useful reading: a base `opt: "speed"` that a
 * freestanding target cannot honour has to be overridable by the
 * entry that knows better. */
static void replace_str(char **dst, const char *src) {
    if (!src) return;
    char *copy = strdup(src);
    if (!copy) return;
    free(*dst);
    *dst = copy;
}

/* Merge every entry matching this invocation's target into the base
 * blocks, in declaration order (§11.9): arrays append, scalars replace.
 *
 * Done once, at load, so that the freshness hash, `--explain`, the
 * compile argv and the link argv are all reading one merged block and
 * cannot disagree about what the build's flags are. The cost of the
 * alternative — merging at each emit site — is not the duplication but
 * that the hash is one of those sites, and a flag that changes the
 * compile without changing the hash is a stale object.
 *
 * An entry whose pattern matches nothing is not an error: a descriptor
 * naming four architectures is *expected* to have three inert entries
 * on any given run. */
static void apply_target_flags(NowProject *p) {
    if (!p || p->target_flags.count == 0) return;
    const NowTriple *target = now_arch_effective_target();
    if (!target) return;

    int matched = 0;
    for (size_t i = 0; i < p->target_flags.count; i++) {
        NowTargetFlags *e = &p->target_flags.items[i];
        NowTriple pattern;
        now_triple_parse(&pattern, e->pattern);
        /* An empty component matches anything, same as `*` — so
         * "linux" alone is "any linux", and the three-component form
         * is not mandatory. now_triple_match() already reads it that
         * way for the toolchain map. */
        if (!now_triple_match(&pattern, target)) continue;
        matched++;

        append_strarray(&p->compile.flags,    &e->compile.flags);
        append_strarray(&p->compile.warnings, &e->compile.warnings);
        append_strarray(&p->compile.defines,  &e->compile.defines);
        append_strarray(&p->compile.includes, &e->compile.includes);
        replace_str(&p->compile.std, e->compile.std);
        replace_str(&p->compile.opt, e->compile.opt);

        append_strarray(&p->link.flags,    &e->link.flags);
        append_strarray(&p->link.libs,     &e->link.libs);
        append_strarray(&p->link.libdirs,  &e->link.libdirs);
        append_strarray(&p->link.archives, &e->link.archives);
        replace_str(&p->link.script,      e->link.script);
        replace_str(&p->link.script_body, e->link.script_body);
        if (e->link.inherit_target) p->link.inherit_target = 1;
    }

    /* A descriptor that declares per-triple flags and then matches none
     * of them is the CI-matrix typo: `--target linux:amd64:gnuu` builds
     * successfully, silently, with none of the configuration the author
     * wrote for it. `now` cannot validate a triple against a list of
     * real ones — `freestanding:riscv64:none` is a perfectly good target
     * it has never heard of, and a closed list would refuse exactly the
     * users this feature is for. What it CAN say is that the target
     * selected nothing, which is the observable the typo actually
     * changes.
     *
     * Only warned when entries exist: a project with no target_flags is
     * not making a claim about its target and must stay silent. */
    if (matched == 0) {
        char t[NOW_TRIPLE_MAX * 3 + 3];
        now_triple_format(target, t, sizeof(t));
        fprintf(stderr,
                "warning: target '%s' matches no target_flags entry - "
                "building with none of them\n", t);
        for (size_t i = 0; i < p->target_flags.count; i++)
            fprintf(stderr, "         declared: %s\n",
                    p->target_flags.items[i].pattern);
    }
}

NOW_API NowProject *now_project_new(void) {
    NowProject *p = calloc(1, sizeof(NowProject));
    if (!p) return NULL;
    now_strarray_init(&p->langs);
    now_sources_init(&p->sources);
    now_sources_init(&p->tests);
    now_compile_init(&p->compile);
    now_link_init(&p->link);
    now_deparray_init(&p->deps);
    now_repoarray_init(&p->repos);
    now_pluginarray_init(&p->plugins);
    now_strarray_init(&p->components);
    now_strarray_init(&p->vendored);
    now_strarray_init(&p->modules);
    now_strarray_init(&p->private_groups);
    now_strarray_init(&p->arch.tags);
    now_strarray_init(&p->arch.alias_keys);
    now_strarray_init(&p->arch.alias_values);
    return p;
}

NOW_API void now_project_free(NowProject *p) {
    if (!p) return;
    free(p->group);
    free(p->artifact);
    free(p->version);
    free(p->name);
    free(p->description);
    free(p->url);
    free(p->license);
    now_strarray_free(&p->langs);
    free(p->std);
    now_sources_free(&p->sources);
    now_sources_free(&p->tests);
    free(p->output.type);
    free(p->output.name);
    free(p->output.dir);
    now_compile_free(&p->compile);
    now_link_free(&p->link);
    now_target_flags_free(&p->target_flags);
    now_assembly_free(&p->assembly);
    now_deparray_free(&p->deps);
    now_repoarray_free(&p->repos);
    now_pluginarray_free(&p->plugins);
    free(p->convergence);
    now_strarray_free(&p->components);
    now_strarray_free(&p->vendored);
    now_strarray_free(&p->private_groups);
    now_strarray_free(&p->modules);
    free(p->workspace_mode);
    free(p->java.main_class);
    free(p->java.encoding);
    now_strarray_free(&p->java.classpath);
    now_strarray_free(&p->arch.tags);
    now_strarray_free(&p->arch.alias_keys);
    now_strarray_free(&p->arch.alias_values);
    if (p->_pasta_root)
        pasta_free((PastaValue *)p->_pasta_root);
    free(p);
}

NOW_API NowProject *now_project_load(const char *path, NowResult *result) {
    /* Read file */
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot open: %s", path);
        }
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        if (result) {
            result->code = NOW_ERR_ALLOC;
            snprintf(result->message, sizeof(result->message), "out of memory");
        }
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)len, fp);
    buf[nread] = '\0';
    fclose(fp);

    /* Parse Pasta */
    PastaResult pr;
    PastaValue *root = pasta_parse(buf, nread, &pr);
    free(buf);

    if (!root || pr.code != PASTA_OK) {
        if (result) {
            result->code = NOW_ERR_SYNTAX;
            result->line = pr.line;
            result->col  = pr.col;
            snprintf(result->message, sizeof(result->message),
                     "%s:%d:%d: %s", path, pr.line, pr.col, pr.message);
        }
        return NULL;
    }

    if (pasta_type(root) != PASTA_MAP) {
        pasta_free(root);
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "now.pasta root must be a map");
        }
        return NULL;
    }

    /* Build project */
    NowProject *p = now_project_new();
    if (!p) {
        pasta_free(root);
        if (result) {
            result->code = NOW_ERR_ALLOC;
            snprintf(result->message, sizeof(result->message), "out of memory");
        }
        return NULL;
    }

    p->_pasta_root = root;

    /* Identity (§1.1) */
    p->group       = dup_map_str(root, "group");
    p->artifact    = dup_map_str(root, "artifact");
    p->version     = dup_map_str(root, "version");
    p->name        = dup_map_str(root, "name");
    p->description = dup_map_str(root, "description");
    p->url         = dup_map_str(root, "url");
    p->license     = dup_map_str(root, "license");

    /* Language (§1.2) */
    load_langs(p, root);
    p->std = dup_map_str(root, "std");

    /* Sources (§1.3) */
    load_sources(&p->sources, pasta_map_get(root, "sources"));
    load_sources(&p->tests,   pasta_map_get(root, "tests"));

    /* Output (§1.4) */
    load_output(&p->output, pasta_map_get(root, "output"));

    /* Compile & link (§1.5) */
    load_compile(&p->compile, pasta_map_get(root, "compile"));
    load_link(&p->link,       pasta_map_get(root, "link"));

    /* Per-triple overrides (§11.9) — read here, merged into the two
     * blocks above by apply_target_flags() once the whole descriptor
     * is in. */
    load_target_flags(&p->target_flags, pasta_map_get(root, "target_flags"));
    load_assembly(&p->assembly, pasta_map_get(root, "assembly"), path);

    /* Dependencies (§1.6). Accept `depends:` as a Maven-friendly
     * alias for `deps:` — `deps:` wins when both are present. */
    {
        const PastaValue *dv = pasta_map_get(root, "deps");
        if (!dv) dv = pasta_map_get(root, "depends");
        load_deps(&p->deps, dv);
    }

    /* Repositories (§1.7) */
    load_repos(&p->repos, pasta_map_get(root, "repos"));

    /* Convergence (§6.11) */
    p->convergence = dup_map_str(root, "convergence");

    /* Plugins (§10) */
    load_plugins(&p->plugins, pasta_map_get(root, "plugins"));

    /* Dep confusion protection (§8, §25.2).
     *
     * Two spellings are documented and both must work. §25.2 shows the
     * project-level form nested under `resolve:`, which was in neither
     * key list and so warned as an unknown key and was dropped — a
     * descriptor written straight from the spec got no fence at all.
     * The flat form is what the implementation has always read. */
    load_strarray(&p->private_groups, pasta_map_get(root, "private_groups"));
    {
        const PastaValue *res = pasta_map_get(root, "resolve");
        if (res && pasta_type(res) == PASTA_MAP)
            load_strarray(&p->private_groups,
                          pasta_map_get(res, "private_groups"));
    }
    load_machine_private_groups(&p->private_groups);

    /* Components (own modules) and vendored (external deps) */
    load_strarray(&p->components, pasta_map_get(root, "components"));
    load_strarray(&p->vendored, pasta_map_get(root, "vendored"));

    /* Workspace (§1.11) */
    load_strarray(&p->modules, pasta_map_get(root, "modules"));
    load_inherit(&p->inherit_defaults, pasta_map_get(root, "inherit_defaults"));
    load_inherit(&p->inherit,          pasta_map_get(root, "inherit"));
    p->workspace_mode = dup_map_str(root, "workspace_mode");

    /* Java-specific config */
    const PastaValue *java_map = pasta_map_get(root, "java");
    if (java_map && pasta_type(java_map) == PASTA_MAP) {
        p->java.main_class = dup_map_str(java_map, "main_class");
        p->java.encoding   = dup_map_str(java_map, "encoding");
        load_strarray(&p->java.classpath, pasta_map_get(java_map, "classpath"));
    }

    /* Platform tag dictionary (§11.x) */
    load_arch(&p->arch, pasta_map_get(root, "arch"));

    apply_maven_defaults(p);
    apply_target_flags(p);

    warn_descriptor_keys(root, path);
    warn_dead_nested_keys(root, path);

    if (result) {
        result->code = NOW_OK;
        result->line = 0;
        result->col  = 0;
        result->message[0] = '\0';
    }

    return p;
}

NOW_API NowProject *now_project_load_string(const char *input, size_t len,
                                     NowResult *result) {
    PastaResult pr;
    PastaValue *root = pasta_parse(input, len, &pr);

    if (!root || pr.code != PASTA_OK) {
        if (result) {
            result->code = NOW_ERR_SYNTAX;
            result->line = pr.line;
            result->col  = pr.col;
            snprintf(result->message, sizeof(result->message),
                     "%d:%d: %s", pr.line, pr.col, pr.message);
        }
        return NULL;
    }

    if (pasta_type(root) != PASTA_MAP) {
        pasta_free(root);
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "now.pasta root must be a map");
        }
        return NULL;
    }

    NowProject *p = now_project_new();
    if (!p) {
        pasta_free(root);
        if (result) {
            result->code = NOW_ERR_ALLOC;
            snprintf(result->message, sizeof(result->message), "out of memory");
        }
        return NULL;
    }

    p->_pasta_root = root;

    /* Load all fields same as file-based loader */
    p->group       = dup_map_str(root, "group");
    p->artifact    = dup_map_str(root, "artifact");
    p->version     = dup_map_str(root, "version");
    p->name        = dup_map_str(root, "name");
    p->description = dup_map_str(root, "description");
    p->url         = dup_map_str(root, "url");
    p->license     = dup_map_str(root, "license");
    load_langs(p, root);
    p->std = dup_map_str(root, "std");
    load_sources(&p->sources, pasta_map_get(root, "sources"));
    load_sources(&p->tests,   pasta_map_get(root, "tests"));
    load_output(&p->output, pasta_map_get(root, "output"));
    load_compile(&p->compile, pasta_map_get(root, "compile"));
    load_link(&p->link,       pasta_map_get(root, "link"));
    load_target_flags(&p->target_flags, pasta_map_get(root, "target_flags"));
    load_assembly(&p->assembly, pasta_map_get(root, "assembly"), NULL);
    {
        const PastaValue *dv = pasta_map_get(root, "deps");
        if (!dv) dv = pasta_map_get(root, "depends");
        load_deps(&p->deps, dv);
    }
    load_repos(&p->repos, pasta_map_get(root, "repos"));
    p->convergence = dup_map_str(root, "convergence");
    load_plugins(&p->plugins, pasta_map_get(root, "plugins"));
    load_strarray(&p->private_groups, pasta_map_get(root, "private_groups"));
    {
        /* §25.2's nested spelling — see the note in now_project_load.
         * The machine-level file is deliberately *not* read here: this
         * loader parses a descriptor held in memory, and reaching out to
         * ~/.now would make the same string parse differently depending
         * on the machine. */
        const PastaValue *res = pasta_map_get(root, "resolve");
        if (res && pasta_type(res) == PASTA_MAP)
            load_strarray(&p->private_groups,
                          pasta_map_get(res, "private_groups"));
    }
    load_strarray(&p->components, pasta_map_get(root, "components"));
    load_strarray(&p->vendored, pasta_map_get(root, "vendored"));
    load_strarray(&p->modules, pasta_map_get(root, "modules"));

    /* Java-specific config */
    {
        const PastaValue *java_map = pasta_map_get(root, "java");
        if (java_map && pasta_type(java_map) == PASTA_MAP) {
            p->java.main_class = dup_map_str(java_map, "main_class");
            p->java.encoding   = dup_map_str(java_map, "encoding");
            load_strarray(&p->java.classpath, pasta_map_get(java_map, "classpath"));
        }
    }

    /* Platform tag dictionary (§11.x) */
    load_arch(&p->arch, pasta_map_get(root, "arch"));

    apply_maven_defaults(p);
    apply_target_flags(p);

    if (result) {
        result->code = NOW_OK;
        result->line = 0;
        result->col  = 0;
        result->message[0] = '\0';
    }

    return p;
}
