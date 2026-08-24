/*
 * now_tell.c — `now tell` and `now tool:*`
 *
 * See now_tell.h for why these are two commands and not one family.
 */
#include "now_tell.h"
#include "now_build.h"
#include "now_procure.h"   /* now_repo_dep_path — where a dep landed */
#include "now_fs.h"
#include "pasta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>   /* offsetof — the field table is offset-based */

/* ---- output helpers ---- */

static int is_json(const char *f)  { return f && strcmp(f, "json") == 0; }
static int is_pasta(const char *f) { return f && strcmp(f, "pasta") == 0; }

static void esc_json(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out);  break;
        case '\r': fputs("\\r", out);  break;
        case '\t': fputs("\\t", out);  break;
        default:
            if (*p < 0x20) fprintf(out, "\\u%04x", (unsigned)*p);
            else           fputc((int)*p, out);
        }
    }
    fputc('"', out);
}

/* A scalar prints BARE in text mode — `now tell sources.dir` is going to
 * end up inside `$(...)` far more often than it is going to be read, and
 * a quoted string there is a bug waiting to happen. */
static void emit_scalar(FILE *out, const char *v, const char *fmt) {
    if (is_json(fmt))       { esc_json(out, v); fputc('\n', out); }
    else if (is_pasta(fmt)) { fprintf(out, "\"%s\"\n", v ? v : ""); }
    else                    { fprintf(out, "%s\n", v ? v : ""); }
}

/* A list prints one per line in text mode, for the same reason: that is
 * what `while read` consumes. */
static void emit_list(FILE *out, const char *const *items, size_t n,
                      const char *fmt) {
    if (is_json(fmt)) {
        fputs("[", out);
        for (size_t i = 0; i < n; i++) {
            if (i) fputs(", ", out);
            esc_json(out, items[i]);
        }
        fputs("]\n", out);
    } else if (is_pasta(fmt)) {
        fputs("[", out);
        for (size_t i = 0; i < n; i++)
            fprintf(out, "%s\"%s\"", i ? ", " : "", items[i]);
        fputs("]\n", out);
    } else {
        for (size_t i = 0; i < n; i++) fprintf(out, "%s\n", items[i]);
    }
}

static void emit_strarray(FILE *out, const NowStrArray *a, const char *fmt) {
    emit_list(out, (const char *const *)a->items, a->count, fmt);
}

/* ---- descriptor fields ----
 *
 * An explicit table rather than a walk of the raw Pasta tree, and
 * deliberately: the useful answer is the EFFECTIVE value, after the
 * loader has applied its language-aware defaults. A project that never
 * wrote `sources.dir` still compiles `src/main/c`, and a caller asking
 * where the sources are wants that answer, not an empty string. */
typedef enum { F_STR, F_ARR } FieldKind;

typedef struct {
    const char *path;
    FieldKind   kind;
    size_t      offset;   /* into NowProject */
} Field;

#define OFF(m) offsetof(struct NowProject, m)

static const Field k_fields[] = {
    { "group",             F_STR, OFF(group) },
    { "artifact",          F_STR, OFF(artifact) },
    { "version",           F_STR, OFF(version) },
    { "name",              F_STR, OFF(name) },
    { "description",       F_STR, OFF(description) },
    { "url",               F_STR, OFF(url) },
    { "license",           F_STR, OFF(license) },
    { "std",               F_STR, OFF(std) },
    { "convergence",       F_STR, OFF(convergence) },
    { "langs",             F_ARR, OFF(langs) },
    { "sources.dir",       F_STR, OFF(sources.dir) },
    { "sources.headers",   F_STR, OFF(sources.headers) },
    { "sources.private",   F_STR, OFF(sources.private_headers) },
    { "sources.pattern",   F_STR, OFF(sources.pattern) },
    { "sources.exclude",   F_ARR, OFF(sources.exclude) },
    { "sources.include",   F_ARR, OFF(sources.include) },
    { "tests.dir",         F_STR, OFF(tests.dir) },
    { "tests.exclude",     F_ARR, OFF(tests.exclude) },
    { "tests.defines",     F_ARR, OFF(tests.defines) },
    { "output.type",       F_STR, OFF(output.type) },
    { "output.name",       F_STR, OFF(output.name) },
    { "compile.flags",     F_ARR, OFF(compile.flags) },
    { "compile.defines",   F_ARR, OFF(compile.defines) },
    { "compile.includes",  F_ARR, OFF(compile.includes) },
    { "compile.warnings",  F_ARR, OFF(compile.warnings) },
    { "compile.std",       F_STR, OFF(compile.std) },
    { "compile.opt",       F_STR, OFF(compile.opt) },
    { "link.flags",        F_ARR, OFF(link.flags) },
    { "link.libs",         F_ARR, OFF(link.libs) },
    { "link.libdirs",      F_ARR, OFF(link.libdirs) },
    { "link.archives",     F_ARR, OFF(link.archives) },
    { "link.script",       F_STR, OFF(link.script) },
    { "components",        F_ARR, OFF(components) },
    { "vendored",          F_ARR, OFF(vendored) },
    { "modules",           F_ARR, OFF(modules) },
    { "private_groups",    F_ARR, OFF(private_groups) },
    { "arch.tags",         F_ARR, OFF(arch.tags) },
    { NULL, F_STR, 0 }
};

static int tell_field(FILE *out, const NowProject *p, const char *path,
                      const char *fmt) {
    for (size_t i = 0; k_fields[i].path; i++) {
        if (strcmp(k_fields[i].path, path) != 0) continue;
        const char *base = (const char *)p;
        if (k_fields[i].kind == F_STR)
            emit_scalar(out, *(char *const *)(base + k_fields[i].offset), fmt);
        else
            emit_strarray(out, (const NowStrArray *)(base + k_fields[i].offset), fmt);
        return 0;
    }
    return -1;
}

/* `deps` is not a flat array of strings, so it gets its own printer. */
static void tell_deps(FILE *out, const NowProject *p, const char *fmt) {
    if (is_json(fmt)) {
        fputs("[", out);
        for (size_t i = 0; i < p->deps.count; i++) {
            if (i) fputs(", ", out);
            fputs("{\"id\": ", out);
            esc_json(out, p->deps.items[i].id);
            fputs(", \"scope\": ", out);
            esc_json(out, p->deps.items[i].scope ? p->deps.items[i].scope
                                                 : "compile");
            fputs("}", out);
        }
        fputs("]\n", out);
        return;
    }
    for (size_t i = 0; i < p->deps.count; i++) {
        const NowDep *d = &p->deps.items[i];
        if (is_pasta(fmt))
            fprintf(out, "{ id: \"%s\", scope: \"%s\" }\n",
                    d->id ? d->id : "", d->scope ? d->scope : "compile");
        else
            fprintf(out, "%s\n", d->id ? d->id : "");
    }
}

/* ---- computed queries ---- */

static int tell_computed(FILE *out, const NowProject *p, const char *basedir,
                         const char *what, const char *const *argv, int argc,
                         const char *fmt, NowResult *result) {
    if (strcmp(what, "source-files") == 0) {
        NowFileList fl;
        if (now_query_sources(p, basedir, &fl, result) < 0) return -1;
        emit_list(out, (const char *const *)fl.paths, fl.count, fmt);
        now_filelist_free(&fl);
        return 0;
    }

    if (strcmp(what, "include-paths") == 0) {
        NowStrArray inc;
        if (now_query_includes(p, basedir, &inc, result) < 0) return -1;
        emit_strarray(out, &inc, fmt);
        now_strarray_free(&inc);
        return 0;
    }

    if (strcmp(what, "compile-cmd") == 0 || strcmp(what, "compile-flags") == 0) {
        int flags_only = (strcmp(what, "compile-flags") == 0);
        char **cmd;
        if (argc < 1) {
            if (result) {
                result->code = NOW_ERR_SCHEMA;
                snprintf(result->message, sizeof(result->message),
                         "%s needs a source file", what);
            }
            return -1;
        }
        cmd = now_query_compile_argv(p, basedir, argv[0], flags_only, result);
        if (!cmd) return -1;
        {
            size_t n = 0;
            while (cmd[n]) n++;
            if (is_json(fmt) || is_pasta(fmt)) {
                emit_list(out, (const char *const *)cmd, n, fmt);
            } else {
                /* One line, shell-shaped: this is meant to be pasted or
                 * piped into a shell, so it is a command and not a list. */
                for (size_t i = 0; i < n; i++)
                    fprintf(out, "%s%s", i ? " " : "", cmd[i]);
                fputc('\n', out);
            }
        }
        now_query_argv_free(cmd);
        return 0;
    }

    if (strcmp(what, "dep-path") == 0) {
        /* `now tell dep-path <group:artifact:version> [h|lib]` */
        char g[256], a[128], v[128];
        const char *coord, *kind;
        const char *c1, *c2;
        const char *home;
        char *repo_root = NULL, *dep = NULL, *sub = NULL;

        if (argc < 1) {
            if (result) {
                result->code = NOW_ERR_SCHEMA;
                snprintf(result->message, sizeof(result->message),
                         "dep-path needs a group:artifact:version coordinate");
            }
            return -1;
        }
        coord = argv[0];
        kind  = argc > 1 ? argv[1] : NULL;
        c1 = strchr(coord, ':');
        c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        if (!c1 || !c2) {
            if (result) {
                result->code = NOW_ERR_SCHEMA;
                snprintf(result->message, sizeof(result->message),
                         "'%s' is not a group:artifact:version coordinate", coord);
            }
            return -1;
        }
        snprintf(g, sizeof(g), "%.*s", (int)(c1 - coord), coord);
        snprintf(a, sizeof(a), "%.*s", (int)(c2 - c1 - 1), c1 + 1);
        snprintf(v, sizeof(v), "%s", c2 + 1);

#ifdef _WIN32
        home = getenv("USERPROFILE");
        if (!home) home = getenv("HOME");
#else
        home = getenv("HOME");
#endif
        if (!home) return -1;
        {
            char *dot = now_path_join(home, ".now");
            if (dot) { repo_root = now_path_join(dot, "repo"); free(dot); }
        }
        if (!repo_root) return -1;
        dep = now_repo_dep_path(repo_root, g, a, v);
        free(repo_root);
        if (!dep) return -1;

        if (kind) sub = now_path_join(dep, kind);

        /* Report whether it is actually there. A path that does not
         * exist is a legitimate answer to "where would it be", but a
         * caller feeding it to a compiler needs to know which it got. */
        {
            const char *ans = sub ? sub : dep;
            if (!now_path_exists(ans)) {
                if (result) {
                    result->code = NOW_ERR_NOT_FOUND;
                    snprintf(result->message, sizeof(result->message),
                             "%s is not installed (looked in %s)", coord, ans);
                }
                free(sub); free(dep);
                return -1;
            }
            emit_scalar(out, ans, fmt);
        }
        free(sub);
        free(dep);
        return 0;
    }

    (void)argc;
    return 1;   /* not a computed query */
}

NOW_API int now_tell(FILE *out, const NowProject *project,
                     const char *basedir, const char *what,
                     const char *const *argv, int argc,
                     const char *format, NowResult *result) {
    int rc;
    if (!what || !*what) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "tell needs something to tell you about");
        }
        return -1;
    }

    /* Hyphen means computed; anything else is a descriptor field. The
     * split is syntactic so the two namespaces can never collide. */
    if (strchr(what, '-')) {
        rc = tell_computed(out, project, basedir, what, argv, argc,
                           format, result);
        if (rc <= 0) return rc;
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "no query called '%s'", what);
        }
        return -1;
    }

    if (strcmp(what, "deps") == 0) { tell_deps(out, project, format); return 0; }

    if (tell_field(out, project, what, format) == 0) return 0;

    /* An unknown field is an ERROR, not an empty answer. Printing
     * nothing for a typo is how a script ends up branching on "" and
     * believing it means the field was unset. */
    if (result) {
        result->code = NOW_ERR_NOT_FOUND;
        snprintf(result->message, sizeof(result->message),
                 "no descriptor field called '%s'", what);
    }
    return -1;
}

/* ==================================================================
 *  tools: — project-local declared tooling
 * ================================================================== */

NOW_API void now_toolarray_free(NowToolArray *a) {
    if (!a) return;
    for (size_t i = 0; i < a->count; i++) {
        free(a->items[i].name);
        free(a->items[i].description);
        free(a->items[i].run);
        free(a->items[i].hook);
    }
    free(a->items);
    a->items = NULL;
    a->count = a->cap = 0;
}

static char *dup_key(const PastaValue *m, const char *k) {
    const PastaValue *v = pasta_map_get(m, k);
    if (!v || pasta_type(v) != PASTA_STRING) return NULL;
    return strdup(pasta_get_string(v));
}

NOW_API void now_tools_load(NowToolArray *dst, const void *pasta_map) {
    const PastaValue *root = (const PastaValue *)pasta_map;
    if (!dst || !root || pasta_type(root) != PASTA_MAP) return;
    {
        const PastaValue *tools = pasta_map_get(root, "tools");
        if (!tools || pasta_type(tools) != PASTA_MAP) return;
        size_t n = pasta_count(tools);
        for (size_t i = 0; i < n; i++) {
            const char *name = pasta_map_key(tools, i);
            const PastaValue *body = pasta_map_value(tools, i);
            if (!name || !body || pasta_type(body) != PASTA_MAP) continue;
            if (dst->count == dst->cap) {
                size_t cap = dst->cap ? dst->cap * 2 : 8;
                NowTool *grown = (NowTool *)realloc(dst->items,
                                                    cap * sizeof(*grown));
                if (!grown) return;
                dst->items = grown;
                dst->cap = cap;
            }
            {
                NowTool *t = &dst->items[dst->count];
                memset(t, 0, sizeof(*t));
                t->name        = strdup(name);
                t->description = dup_key(body, "description");
                t->run         = dup_key(body, "run");
                t->hook        = dup_key(body, "hook");
                dst->count++;
            }
        }
    }
}

NOW_API int now_tool_list(FILE *out, const NowProject *project,
                          const char *basedir, NowResult *result) {
    NowToolArray tools;
    (void)basedir;
    memset(&tools, 0, sizeof(tools));
    now_tools_load(&tools, project->_pasta_root);

    if (tools.count == 0) {
        fprintf(out, "no tools declared (add a `tools:` block to now.pasta)\n");
        now_toolarray_free(&tools);
        (void)result;
        return 0;
    }
    for (size_t i = 0; i < tools.count; i++) {
        const NowTool *t = &tools.items[i];
        fprintf(out, "  %-16s %s", t->name,
                t->description ? t->description : "");
        if (t->hook) fprintf(out, "  [hook: %s]", t->hook);
        fputc('\n', out);
    }
    now_toolarray_free(&tools);
    return 0;
}

NOW_API int now_tool_run(const NowProject *project, const char *basedir,
                         const char *name, int verbose, NowResult *result) {
    NowToolArray tools;
    int rc = -1;

    memset(&tools, 0, sizeof(tools));
    now_tools_load(&tools, project->_pasta_root);

    for (size_t i = 0; i < tools.count; i++) {
        const NowTool *t = &tools.items[i];
        if (strcmp(t->name, name) != 0) continue;
        if (!t->run || !*t->run) {
            if (result) {
                result->code = NOW_ERR_SCHEMA;
                snprintf(result->message, sizeof(result->message),
                         "tool '%s' has no `run` command", name);
            }
            now_toolarray_free(&tools);
            return -1;
        }
        if (verbose) fprintf(stderr, "  tool %s: %s\n", name, t->run);
        /* Through the shell on purpose: a `run` string is written by the
         * project's own author in their own descriptor, and it is
         * expected to contain pipes and redirection. This is not a place
         * untrusted input arrives. */
        {
            char cmd[4096];
            snprintf(cmd, sizeof(cmd), "cd \"%s\" && %s",
                     basedir ? basedir : ".", t->run);
            rc = system(cmd);
        }
        now_toolarray_free(&tools);
        return rc;
    }

    if (result) {
        result->code = NOW_ERR_NOT_FOUND;
        snprintf(result->message, sizeof(result->message),
                 "no tool called '%s'", name);
    }
    now_toolarray_free(&tools);
    return -1;
}
