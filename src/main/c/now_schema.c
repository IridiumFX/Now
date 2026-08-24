/*
 * now_schema.c — `now schema:check` (§31.19)
 *
 * Validate a descriptor and say what is wrong with it, without building
 * anything.
 *
 * The diagnostics split into two kinds and the split is the point.
 * ERRORS are things that cannot be what the author meant: a version
 * that is not a version, a `std` the language does not have, an output
 * type nothing can produce. WARNINGS are things that parse and then do
 * not matter — an unknown key, a key this build does not implement, a
 * field that is read by nobody. Only the first kind fails the check,
 * because the second kind is how a descriptor written against a newer
 * spec looks, and refusing it would make the tool unusable exactly when
 * someone is trying to adopt it.
 *
 * `--strict` collapses that distinction, for CI that wants a clean
 * descriptor rather than a working one.
 */
#include "now_schema.h"
#include "now_pom.h"
#include "now_version.h"
#include "now_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- diagnostic list ---- */

NOW_API void now_diaglist_init(NowDiagList *l) {
    if (!l) return;
    l->items = NULL;
    l->count = l->cap = 0;
}

NOW_API void now_diaglist_free(NowDiagList *l) {
    if (!l) return;
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

NOW_API int now_diaglist_push(NowDiagList *l, const char *code,
                              const char *path, int line, int col,
                              const char *message, const char *hint) {
    if (!l) return -1;
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        NowDiag *grown = (NowDiag *)realloc(l->items, cap * sizeof(*grown));
        if (!grown) return -1;
        l->items = grown;
        l->cap = cap;
    }
    NowDiag *d = &l->items[l->count];
    memset(d, 0, sizeof(*d));
    snprintf(d->code,    sizeof(d->code),    "%s", code    ? code    : "");
    snprintf(d->path,    sizeof(d->path),    "%s", path    ? path    : "");
    snprintf(d->message, sizeof(d->message), "%s", message ? message : "");
    snprintf(d->hint,    sizeof(d->hint),    "%s", hint    ? hint    : "");
    d->line = line;
    d->col  = col;
    l->count++;
    return 0;
}

/* ---- what a language calls its standards ----
 *
 * Only for languages where a wrong value is silently accepted today:
 * `std` is passed through to the compiler, so `c14` reaches gcc as
 * `-std=c14` and fails there with a diagnostic that names the compiler
 * rather than the descriptor. Catching it here names the line the
 * author wrote. */
typedef struct { const char *lang; const char *const *stds; } LangStds;

static const char *const k_std_c[]   = { "c89", "c90", "c99", "c11", "c17",
                                         "c18", "c23", "gnu89", "gnu99",
                                         "gnu11", "gnu17", "gnu23", NULL };
static const char *const k_std_cpp[] = { "c++98", "c++03", "c++11", "c++14",
                                         "c++17", "c++20", "c++23",
                                         "gnu++11", "gnu++14", "gnu++17",
                                         "gnu++20", "gnu++23", NULL };

static const LangStds k_lang_stds[] = {
    { "c",   k_std_c   },
    { "c++", k_std_cpp },
    { NULL,  NULL      }
};

static int in_list(const char *const *list, const char *s) {
    for (size_t i = 0; list[i]; i++)
        if (strcmp(list[i], s) == 0) return 1;
    return 0;
}

static void join_list(char *out, size_t cap, const char *const *list) {
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; list[i]; i++) {
        int w = snprintf(out + n, cap - n, "%s%s", n ? " " : "", list[i]);
        if (w < 0 || (size_t)w >= cap - n) return;
        n += (size_t)w;
    }
}

/* ---- validation ---- */

static void check_identity(const NowProject *p, NowDiagList *errs) {
    if (!p->group || !*p->group)
        now_diaglist_push(errs, "NOW-E0101", "group", 0, 0,
                          "group is required", "e.g. group: \"org.example\"");
    if (!p->artifact || !*p->artifact)
        now_diaglist_push(errs, "NOW-E0101", "artifact", 0, 0,
                          "artifact is required", "e.g. artifact: \"mylib\"");
    if (!p->version || !*p->version) {
        now_diaglist_push(errs, "NOW-E0101", "version", 0, 0,
                          "version is required", "e.g. version: \"1.0.0\"");
    } else {
        NowSemVer sv;
        if (now_semver_parse(p->version, &sv) != 0) {
            char msg[NOW_DIAG_TEXT_MAX];
            snprintf(msg, sizeof(msg),
                     "version '%s' is not a semantic version", p->version);
            now_diaglist_push(errs, "NOW-E0103", "version", 0, 0, msg,
                              "MAJOR.MINOR.PATCH, optionally -prerelease");
        } else {
            now_semver_free(&sv);
        }
    }
}

static void check_std(const NowProject *p, NowDiagList *errs) {
    const char *primary = (p->langs.count > 0) ? p->langs.items[0] : "c";
    /* compile.std overrides the top-level std; check whichever is set. */
    const char *std = p->compile.std ? p->compile.std : p->std;
    const char *field = p->compile.std ? "compile.std" : "std";
    if (!std || !*std) return;

    for (size_t i = 0; k_lang_stds[i].lang; i++) {
        if (strcmp(k_lang_stds[i].lang, primary) != 0) continue;
        if (in_list(k_lang_stds[i].stds, std)) return;
        {
            char msg[NOW_DIAG_TEXT_MAX], hint[NOW_DIAG_TEXT_MAX];
            char joined[NOW_DIAG_TEXT_MAX];
            join_list(joined, sizeof(joined), k_lang_stds[i].stds);
            snprintf(msg, sizeof(msg), "std '%s' is not valid for lang '%s'",
                     std, primary);
            snprintf(hint, sizeof(hint), "Expected one of: %s", joined);
            now_diaglist_push(errs, "NOW-E0102", field, 0, 0, msg, hint);
        }
        return;
    }
    /* A language we have no table for is not an error — it means we do
     * not know, and guessing would refuse valid descriptors. */
}

static void check_output(const NowProject *p, NowDiagList *errs) {
    static const char *const k_types[] = { "executable", "static", "shared",
                                           "header-only", "jar", NULL };
    if (!p->output.type || !*p->output.type) return;
    if (in_list(k_types, p->output.type)) return;
    {
        char msg[NOW_DIAG_TEXT_MAX], hint[NOW_DIAG_TEXT_MAX], joined[128];
        join_list(joined, sizeof(joined), k_types);
        snprintf(msg, sizeof(msg), "output.type '%s' is not a known type",
                 p->output.type);
        snprintf(hint, sizeof(hint), "Expected one of: %s", joined);
        now_diaglist_push(errs, "NOW-E0104", "output.type", 0, 0, msg, hint);
    }
}

static void check_deps(const NowProject *p, NowDiagList *errs) {
    for (size_t i = 0; i < p->deps.count; i++) {
        const NowDep *d = &p->deps.items[i];
        if (!d->id || !*d->id) {
            now_diaglist_push(errs, "NOW-E0105", "deps[]", 0, 0,
                              "dependency has no coordinate",
                              "group:artifact:version");
            continue;
        }
        /* Two colons, three non-empty parts. A coordinate that is not
         * one resolves to nothing and the failure surfaces much later,
         * from the registry, naming a thing the author never typed. */
        {
            const char *c1 = strchr(d->id, ':');
            const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            int ok = c1 && c2 && c1 != d->id && c2 > c1 + 1 && *(c2 + 1);
            if (!ok) {
                char msg[NOW_DIAG_TEXT_MAX];
                snprintf(msg, sizeof(msg),
                         "dependency '%s' is not a group:artifact:version "
                         "coordinate", d->id);
                now_diaglist_push(errs, "NOW-E0105", "deps[].id", 0, 0, msg,
                                  "e.g. org.example:core:1.2.3");
            }
        }
    }
}

/* ---- entry point ---- */

NOW_API NowSchemaResult now_schema_check(const char *path,
                                          NowDiagList *errors,
                                          NowDiagList *warnings) {
    const char *desc = path && *path ? path : "now.pasta";

    if (!now_path_exists(desc))
        return NOW_SCHEMA_NOT_FOUND;

    /* Capture the loader's key diagnostics instead of letting them
     * print. They are warnings, every one — a descriptor naming a field
     * this build does not implement is a descriptor written against a
     * newer spec, not a broken one. */
    now_pom_set_diag_collector(warnings);
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load(desc, &res);
    now_pom_set_diag_collector(NULL);

    if (!p) {
        /* A parse failure carries a line and column; a schema failure
         * does not. Both are errors and both are reported here so the
         * caller has one list to render. */
        /* The loader formats a syntax error as "LINE:COL: text", and the
         * renderer prints the location itself — so pass only the text,
         * or the position appears twice in one line. */
        {
            const char *text = res.message[0] ? res.message
                                              : "descriptor did not load";
            if (res.line > 0) {
                char prefix[32];
                int n = snprintf(prefix, sizeof(prefix), "%d:%d: ",
                                 res.line, res.col);
                if (n > 0 && (size_t)n < sizeof(prefix) &&
                    strncmp(text, prefix, (size_t)n) == 0)
                    text += n;
            }
            now_diaglist_push(errors,
                              res.code == NOW_ERR_SYNTAX ? "NOW-E0001"
                                                         : "NOW-E0002",
                              "", res.line, res.col, text, "");
        }
        return NOW_SCHEMA_INVALID;
    }

    check_identity(p, errors);
    check_std(p, errors);
    check_output(p, errors);
    check_deps(p, errors);

    now_project_free(p);
    return errors->count ? NOW_SCHEMA_INVALID : NOW_SCHEMA_VALID;
}

/* ---- reporting ---- */

static void json_escape_into(FILE *out, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n",  out); break;
        case '\r': fputs("\\r",  out); break;
        case '\t': fputs("\\t",  out); break;
        default:
            if (*p < 0x20) fprintf(out, "\\u%04x", (unsigned)*p);
            else           fputc((int)*p, out);
        }
    }
}

static void json_diags(FILE *out, const char *name, const NowDiagList *l) {
    fprintf(out, "  \"%s\": [", name);
    for (size_t i = 0; i < l->count; i++) {
        const NowDiag *d = &l->items[i];
        fprintf(out, "%s\n    {\n", i ? "," : "");
        fprintf(out, "      \"code\": \"");    json_escape_into(out, d->code);
        fprintf(out, "\",\n      \"path\": \""); json_escape_into(out, d->path);
        fprintf(out, "\",\n      \"line\": %d,\n      \"col\": %d,\n",
                d->line, d->col);
        fprintf(out, "      \"message\": \""); json_escape_into(out, d->message);
        fprintf(out, "\",\n      \"hint\": \"");  json_escape_into(out, d->hint);
        fprintf(out, "\"\n    }");
    }
    fprintf(out, "%s]", l->count ? "\n  " : "");
}

NOW_API void now_schema_report(FILE *out, const char *path,
                               NowSchemaResult res,
                               const NowDiagList *errors,
                               const NowDiagList *warnings,
                               const char *format) {
    const char *fmt = format ? format : "text";
    const char *desc = path && *path ? path : "now.pasta";

    if (strcmp(fmt, "json") == 0) {
        fprintf(out, "{\n  \"valid\": %s,\n",
                res == NOW_SCHEMA_VALID ? "true" : "false");
        json_diags(out, "errors", errors);
        fprintf(out, ",\n");
        json_diags(out, "warnings", warnings);
        fprintf(out, "\n}\n");
        return;
    }

    if (strcmp(fmt, "pasta") == 0) {
        fprintf(out, "{\n  valid: %s,\n",
                res == NOW_SCHEMA_VALID ? "true" : "false");
        fprintf(out, "  errors: %zu,\n  warnings: %zu\n}\n",
                errors->count, warnings->count);
        return;
    }

    /* text */
    if (res == NOW_SCHEMA_NOT_FOUND) {
        fprintf(out, "%s: not found\n", desc);
        return;
    }
    for (size_t i = 0; i < errors->count; i++) {
        const NowDiag *d = &errors->items[i];
        if (d->line)
            fprintf(out, "%s:%d:%d: error: %s", desc, d->line, d->col, d->message);
        else if (d->path[0])
            fprintf(out, "%s: error: %s: %s", desc, d->path, d->message);
        else
            fprintf(out, "%s: error: %s", desc, d->message);
        fprintf(out, "  [%s]\n", d->code);
        if (d->hint[0]) fprintf(out, "  hint: %s\n", d->hint);
    }
    for (size_t i = 0; i < warnings->count; i++) {
        const NowDiag *d = &warnings->items[i];
        fprintf(out, "%s: warning: %s  [%s]\n", desc, d->message, d->code);
    }
    fprintf(out, "%s: %zu error(s), %zu warning(s)\n",
            desc, errors->count, warnings->count);
}
