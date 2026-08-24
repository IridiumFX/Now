/*
 * now_tell.h — `now tell` (introspection) and `now tool:*` (declared tooling)
 *
 * These were one family in the spec — nine `tool:*` commands — and they
 * are two unrelated features that happened to share a prefix:
 *
 *   ASKING NOW SOMETHING     `tool:query`, `tool:sources`, `tool:includes`,
 *                            `tool:compile-cmd`, `tool:compile-flags`,
 *                            `tool:dep-path`
 *   RUNNING SOMETHING YOU    `tool:run`, `tool:list`
 *   DECLARED
 *
 * The first group is a verb: it answers questions about the project and
 * changes nothing. It is now `now tell`, which reads as what it does and
 * sits beside `build` / `test` / `clean` rather than hiding behind a
 * colon. The second keeps `tool:`, which then means exactly one thing —
 * tooling this project declares — instead of two.
 *
 * `tell` distinguishes its two kinds of question SYNTACTICALLY, so no
 * name can be ambiguous:
 *
 *   dotted or bare  -> a descriptor field:  now tell sources.dir
 *   hyphenated      -> a computed answer:   now tell compile-cmd F
 *
 * Descriptor keys are `[a-z_]+` throughout the format, so a hyphen can
 * never be one and the two sets cannot collide as the spec's
 * `tool:query sources` / `tool:sources` pair could.
 */
#ifndef NOW_TELL_H
#define NOW_TELL_H

#include "now.h"
#include "now_pom.h"

#include <stdio.h>

/* Answer one question about `project`.
 *
 * `what` is a descriptor path ("sources.dir", "deps") or a computed
 * query ("source-files", "compile-cmd", "compile-flags", "include-paths",
 * "dep-path"). `argv`/`argc` carry any extra arguments the query needs.
 * `format` is "text" (default), "json" or "pasta".
 *
 * Returns 0 on success, non-zero if the question could not be answered
 * — which includes an unknown field name, because silently printing
 * nothing for a typo is how a script ends up branching on an empty
 * string it thinks means "not set". */
NOW_API int now_tell(FILE *out, const NowProject *project,
                     const char *basedir, const char *what,
                     const char *const *argv, int argc,
                     const char *format, NowResult *result);

/* ---- `tools:` — project-local declared tooling ---- */

typedef struct {
    char *name;
    char *description;
    char *run;
    char *hook;        /* lifecycle phase to attach to, or NULL */
} NowTool;

typedef struct {
    NowTool *items;
    size_t   count;
    size_t   cap;
} NowToolArray;

NOW_API void now_toolarray_free(NowToolArray *a);

/* Parse a `tools:` block. Declared here rather than in now_pom.h because
 * nothing in the build reads it — only these two commands do. */
NOW_API void now_tools_load(NowToolArray *dst, const void *pasta_map);

/* `now tool:list` — every declared tool and its description. */
NOW_API int now_tool_list(FILE *out, const NowProject *project,
                          const char *basedir, NowResult *result);

/* `now tool:run <name>` — run one. Returns the tool's exit code, or a
 * negative value if it could not be started. */
NOW_API int now_tool_run(const NowProject *project, const char *basedir,
                         const char *name, int verbose, NowResult *result);

#endif /* NOW_TELL_H */
