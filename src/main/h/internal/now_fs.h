/*
 * now_fs.h — Filesystem utilities
 *
 * Path manipulation, directory creation, source file discovery.
 */
#ifndef NOW_FS_H
#define NOW_FS_H

#include <stddef.h>
#include "now.h"  /* for NOW_API */

/* Path separator for the host platform */
#ifdef _WIN32
  #define NOW_SEP '\\'
  #define NOW_SEP_STR "\\"
#else
  #define NOW_SEP '/'
  #define NOW_SEP_STR "/"
#endif

/* The user's home directory — the one `~/.now` hangs off.
 *
 * Exists because the codebase had two conventions. Most sites checked
 * USERPROFILE first on Windows; the trust store, the layer stack and the
 * advisory database checked HOME first. Under Git Bash both are set and
 * can differ, so `now trust:add` could write a key into one home while
 * procure looked for it in the other — signature verification failing
 * with the key sitting right there. Returns a borrowed pointer or NULL. */
NOW_API const char *now_home_dir(void);

NOW_API char       *now_path_join(const char *a, const char *b);
NOW_API const char *now_path_ext(const char *path);
NOW_API const char *now_path_basename(const char *path);
NOW_API int         now_mkdir_p(const char *path);
NOW_API int         now_path_exists(const char *path);
NOW_API int         now_is_dir(const char *path);

NOW_API char *now_obj_path(const char *basedir, const char *src_path,
                            const char *src_root, const char *target);
NOW_API char *now_obj_path_ex(const char *basedir, const char *src_path,
                              const char *src_root, const char *target,
                              const char *obj_ext);

/* Dynamic file list */
typedef struct {
    char  **paths;
    size_t  count;
    size_t  capacity;
} NowFileList;

NOW_API void now_filelist_init(NowFileList *fl);
NOW_API int  now_filelist_push(NowFileList *fl, const char *path);
NOW_API void now_filelist_free(NowFileList *fl);

NOW_API int now_discover_sources(const char *basedir, const char *dir,
                                  const char **exts, NowFileList *out);

/* Glob matcher (spec §26). Returns 1 if `path` matches `pattern`, else 0.
 *
 * Supported syntax:
 *   *        any run of chars except '/'
 *   **       any run of chars including '/' (zero or more segments)
 *   ?        any single char except '/'
 *   [abc]    character class; [a-z] range; [!abc] / [^abc] negation
 *   \x       literal x (escape)
 *
 * Per §26.1, a pattern WITHOUT a '/' is matched against the final path
 * segment (basename); a pattern WITH a '/' is matched against the whole
 * `path`. Backslashes in `path` are normalized to '/' first, so callers
 * may pass platform-native separators. `pattern` keeps '\' as the escape
 * char and is never separator-normalized.
 *
 * `path` is taken as-is — callers that need §26.2 base-path rooting
 * (e.g. exclude rooted at sources.dir) must strip that prefix first. */
NOW_API int now_glob_match(const char *pattern, const char *path);

#include "now_pom.h"  /* NowProject + NowStrArray (NowTagSet alias) */

/* Platform-gated source discovery: walks dir like now_discover_sources
 * but skips subdirs whose name (after alias resolution via
 * project->arch.aliases) is in project->arch.tags unless the tag is
 * in `active`. Passing project=NULL, active=NULL, or an empty dict
 * makes this identical to now_discover_sources. */
NOW_API int now_discover_sources_filtered(const char *basedir, const char *dir,
                                           const char **exts,
                                           const NowProject *project,
                                           const NowStrArray *active,
                                           NowFileList *out);

/* Copy a file from src to dst. Returns 0 on success, -1 on error. */
NOW_API int now_file_copy(const char *src, const char *dst);

/* Per-build stat() memoization. The manifest's dep-check loop stats
 * every header that every source includes — for a 374-source project
 * with ~30 unique headers per source, that's ~11k stat() calls. Most
 * headers (stdio.h, project public headers) are referenced by many
 * sources. Memoizing by path turns the inner stat loop from O(deps)
 * per source to O(1) amortized.
 *
 * Implementation: open-addressed hash table with linear probing,
 * power-of-2 capacity, grow on load factor > 0.7. NULL `path` marks
 * empty slot (we never delete, so no tombstones needed). */
typedef struct {
    char     *path;       /* canonical path string; NULL = empty slot */
    long long mtime;      /* st_mtime, undefined if !exists */
    int       exists;     /* 0/1 */
} NowStatEntry;

typedef struct {
    NowStatEntry *table;  /* power-of-2 sized */
    size_t        capacity;
    size_t        count;
} NowStatCache;

NOW_API void now_stat_cache_init(NowStatCache *c);
NOW_API void now_stat_cache_free(NowStatCache *c);

/* Resolve a path with memoization. Returns 1 if file exists and
 * writes mtime; 0 if it doesn't. cache may be NULL — falls back to
 * an uncached stat(). */
NOW_API int now_stat_cached(NowStatCache *cache, const char *path,
                             long long *mtime_out);

#endif /* NOW_FS_H */
