/*
 * now_objsym.h — does this object file DEFINE a given symbol?
 *
 * Exists because `now` was answering that question by looking at the
 * filename. `is_entry_point_obj()` matched `main.c.o` and friends, so
 * an executable whose entry point lived in `app.c` linked its own
 * `main` into the test binary alongside the test's, and `now test`
 * died with `multiple definition of 'main'`. The code comment there
 * recorded the assumption plainly — "library scaffolds have no main()
 * in production objects" — and nobody had tried an executable that did
 * not use `main.c`.
 *
 * A filename is a convention. A symbol table is a fact.
 *
 * Reads COFF (MinGW and MSVC) and ELF64 directly rather than shelling
 * out to `nm` or `dumpbin`: those are not guaranteed present, differ
 * between toolchains, and would make a link-time decision depend on a
 * tool the build never otherwise needs.
 */
#ifndef NOW_OBJSYM_H
#define NOW_OBJSYM_H

#include "now.h"

/* Does `obj_path` define `name` as a global symbol?
 *
 *   1  yes — defined here, externally visible
 *   0  no  — absent, or referenced but not defined
 *  -1  cannot tell — unreadable, or a format this does not parse
 *
 * The three-way return is the point. A caller must not read "cannot
 * tell" as "no": for the entry-point case those two answers lead to
 * opposite link lines, and guessing wrong in one direction produces
 * undefined references while guessing wrong in the other produces a
 * duplicate `main`. Callers fall back to something conservative on -1
 * rather than pretending they know. */
NOW_API int now_obj_defines_symbol(const char *obj_path, const char *name);

/* How many OTHER global symbols does this object define, besides
 * `except`? -1 if it cannot tell.
 *
 * Used to explain a link failure before it happens. Excluding an
 * entry-point object from the test link also excludes everything else
 * that translation unit defined, which is invisible until the linker
 * reports an undefined reference to a function the author can plainly
 * see in their own source file. */
NOW_API int now_obj_other_global_count(const char *obj_path, const char *except);

#endif /* NOW_OBJSYM_H */
