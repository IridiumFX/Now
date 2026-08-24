/*
 * now_convert.h — `now convert` (§23.4)
 *
 * Convert a descriptor between Pasta and JSON. See now_convert.c for
 * why the reading half needed no new parser and why `--to json5` is
 * refused rather than approximated.
 */
#ifndef NOW_CONVERT_H
#define NOW_CONVERT_H

#include "now.h"

#include <stdio.h>   /* FILE */

/* `in_path` NULL or empty auto-detects now.pasta / now.json / now.json5.
 * `to` is "pasta" or "json". Without `in_place` the result goes to
 * stdout; with it, the sibling file is written and the source removed —
 * written first, so an interrupted run leaves the original. */
/* `out` is a parameter rather than always-stdout so a test can read what
 * was produced without reopening the process's stdout, which on Windows
 * means "CON" and on a CI runner means nothing at all. NULL = stdout.
 * Unused when `in_place`. */
NOW_API int now_convert(const char *in_path, const char *to,
                        int in_place, FILE *out, NowResult *result);

#endif /* NOW_CONVERT_H */
