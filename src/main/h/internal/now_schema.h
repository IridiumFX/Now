/*
 * now_schema.h — descriptor validation (§31.19, `now schema:check`)
 *
 * Answers one question without building anything: is this now.pasta
 * what its author thinks it is?
 *
 * The build already emits these diagnostics — unknown keys, recognized-
 * but-unimplemented keys, fields that parse and do nothing — but it
 * emits them on the way past, mixed into compiler output, on a run the
 * author started for another reason. A descriptor with a typo in a
 * field nobody reads produces one line somewhere in a thousand, and the
 * evidence from this week is that people do not see it: two peer teams
 * were caught by descriptor questions in three days, and one of them
 * found a working field by trying it rather than by being told.
 *
 * So the same diagnostics, gathered, sorted, and delivered as the
 * answer to the question rather than as a side effect of another one.
 */
#ifndef NOW_SCHEMA_H
#define NOW_SCHEMA_H

#include "now.h"

#include <stdio.h>   /* FILE */
#include <stddef.h> /* size_t */

#define NOW_DIAG_CODE_MAX 16
#define NOW_DIAG_PATH_MAX 128
#define NOW_DIAG_TEXT_MAX 256

/* One finding about a descriptor.
 *
 * `code` is stable and greppable — a CI job keys on it, so it is part
 * of the interface and must not be reused for a different meaning.
 * `path` names the field in descriptor terms (`tests.include`), not in
 * struct terms, because that is what the author wrote and what they
 * will search for. */
typedef struct {
    char code[NOW_DIAG_CODE_MAX];
    char path[NOW_DIAG_PATH_MAX];
    int  line;
    int  col;
    char message[NOW_DIAG_TEXT_MAX];
    char hint[NOW_DIAG_TEXT_MAX];
} NowDiag;

typedef struct {
    NowDiag *items;
    size_t   count;
    size_t   cap;
} NowDiagList;

NOW_API void now_diaglist_init(NowDiagList *l);
NOW_API void now_diaglist_free(NowDiagList *l);
NOW_API int  now_diaglist_push(NowDiagList *l, const char *code,
                               const char *path, int line, int col,
                               const char *message, const char *hint);

/* Outcome of a check, mapped to the exit codes §31.19 fixes:
 *   0  valid, or valid with warnings (unless --strict)
 *   1  invalid
 *   2  descriptor not found */
typedef enum {
    NOW_SCHEMA_VALID = 0,
    NOW_SCHEMA_INVALID = 1,
    NOW_SCHEMA_NOT_FOUND = 2
} NowSchemaResult;

/* Validate the descriptor at `path`.
 *
 * Both lists are filled; the caller frees them. `errors` non-empty
 * implies NOW_SCHEMA_INVALID. Warnings never change the result — that
 * is `--strict`'s job, and it belongs to the caller so the library does
 * not have to know about flags. */
NOW_API NowSchemaResult now_schema_check(const char *path,
                                          NowDiagList *errors,
                                          NowDiagList *warnings);

/* Render. `format` is "text", "json" or "pasta"; NULL means text. */
NOW_API void now_schema_report(FILE *out, const char *path,
                               NowSchemaResult res,
                               const NowDiagList *errors,
                               const NowDiagList *warnings,
                               const char *format);

/* ---- the collector the descriptor loader writes into ----
 *
 * now_pom.c's key diagnostics print to stderr as the build runs. When a
 * collector is installed they are captured instead, which is what lets
 * schema:check report them as findings rather than as noise — and is
 * also the only reason those warnings are testable at all. Install,
 * load, uninstall; NULL restores printing. Not thread-safe, and neither
 * is the loader. */
NOW_API void now_pom_set_diag_collector(NowDiagList *warnings);

#endif /* NOW_SCHEMA_H */
