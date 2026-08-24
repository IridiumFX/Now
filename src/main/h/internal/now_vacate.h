/*
 * now_vacate.h — `now vacate` (§2.2, §3.4)
 *
 * The only thing in `now` that removes anything from `~/.now/repo`.
 * Before this the shared local repo was append-only in practice and had
 * to be pruned by hand.
 *
 * Conservative by construction: an artifact is removed only when no
 * lock file found by the scan references it, and a scan that found no
 * lock files at all refuses rather than treating "I looked nowhere" as
 * "nothing needs it".
 */
#ifndef NOW_VACATE_H
#define NOW_VACATE_H

#include "now.h"

#include <stddef.h>

typedef struct {
    /* Directories to search for `now.lock.pasta`. A dep is kept if any
     * lock file under any of these names it. */
    const char *const *scan_roots;
    size_t             scan_count;

    /* A lock file whose references do NOT count — the project being
     * vacated, so `now vacate` in a project can remove that project's
     * own deps when nothing else wants them. NULL for `--gc`, which
     * counts everything. */
    const char        *exclude_lock;

    int dry_run;   /* print what would go, remove nothing */
    int force;     /* ignore references entirely, and permit an empty scan */
    /* Suppress the progress report. Printing from a library is the
     * caller's job really, and a test that calls this directly should
     * not interleave a page of output into the suite. */
    int quiet;
} NowVacateOpts;

NOW_API int now_vacate(const NowVacateOpts *opts, NowResult *result);

#endif /* NOW_VACATE_H */
