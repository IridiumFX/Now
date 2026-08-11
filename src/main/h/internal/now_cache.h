/*
 * now_cache.h — Content-addressable build cache
 *
 * Caches compiled object files keyed by source content, compiler flags,
 * and compiler identity. Survives `now clean` and is shared across projects.
 * Cache location: ~/.now/cache/objects/{ab}/{cd}/{hash}{.o|.obj}
 */
#ifndef NOW_CACHE_H
#define NOW_CACHE_H

#include "now.h"
#include <stddef.h>

/* Compute cache key: SHA-256(source_hash + flags_hash + compiler_path).
 * Returns malloc'd 64-char hex string, or NULL on error. */
NOW_API char *now_cache_key(const char *source_hash,
                            const char *flags_hash,
                            const char *compiler_path);

/* Return the cache root directory (~/.now/cache/objects).
 * Returns malloc'd path, or NULL. Caller frees. */
NOW_API char *now_cache_root(void);

/* Compute the sharded cache path for a key.
 * Returns malloc'd path: {root}/ab/cd/{key}{ext}. Caller frees. */
NOW_API char *now_cache_path(const char *cache_key, const char *obj_ext);

/* Restore a cached object. Copies cached file to dst_path.
 * Returns 0 on hit (file copied), -1 on miss or error. */
NOW_API int now_cache_restore(const char *cache_key,
                              const char *dst_path,
                              const char *obj_ext);

/* Store a compiled object in the cache. Returns 0 on success. */
NOW_API int now_cache_store(const char *cache_key,
                            const char *obj_path,
                            const char *obj_ext);

/* Remove all cached objects. Returns 0 on success, -1 on error. */
NOW_API int now_cache_clean(void);

/* Print cache statistics to stdout. Returns 0 on success. */
NOW_API int now_cache_print_stats(void);

/* ---- Dependency-aware cache (header tracking) ---- */

/* Parsed dependency list from compiler output */
typedef struct {
    char **paths;     /* array of malloc'd file paths */
    size_t count;
    size_t capacity;
} NowDepList;

/* Free all paths in a dep list. */
NOW_API void now_deplist_free(NowDepList *dl);

/* Parse a GCC/Clang .d depfile into a dependency list.
 * source_path is excluded from the output list.
 * Returns 0 on success, -1 on error (e.g. file not found). */
NOW_API int now_depfile_parse(const char *depfile_path,
                               const char *source_path,
                               NowDepList *out);

/* Parse MSVC /showIncludes output into a dependency list.
 * Extracts paths from "Note: including file:" lines.
 * Returns 0 on success. */
NOW_API int now_depfile_parse_msvc(const char *output, size_t output_len,
                                    NowDepList *out);

/* Dep-aware cache restore. Reads .deps sidecar, verifies all header
 * hashes still match, then restores the result object.
 * Returns 0 on hit, -1 on miss. */
NOW_API int now_cache_restore_ex(const char *source_key,
                                  const char *dst_path,
                                  const char *obj_ext);

/* Dep-aware cache store. Stores object under result_key (computed from
 * source_key + dep hashes), and writes a .deps sidecar.
 * If deps is NULL or empty, falls back to now_cache_store(). */
NOW_API int now_cache_store_ex(const char *source_key,
                                const char *obj_path,
                                const char *obj_ext,
                                const NowDepList *deps);

/* Read the header dependencies recorded in a key's .deps sidecar.
 *
 * A cache hit restores an object without compiling, so nothing on that
 * path learns which headers the object was built against — the build
 * needs them to write a manifest entry that can go stale later.
 * Without this, a restored object got a manifest entry with zero deps
 * and was reported "up to date" forever, whatever happened to its
 * headers.
 *
 * On success the caller owns both arrays and every string; free with
 * now_cache_deps_free(). Returns 0 on success, -1 if there is no
 * sidecar (in which case *count is 0 and the arrays are NULL). */
NOW_API int now_cache_deps_for_key(const char *source_key,
                                    char ***dep_paths,
                                    char ***dep_hashes,
                                    size_t *dep_count);

NOW_API void now_cache_deps_free(char **dep_paths, char **dep_hashes,
                                  size_t dep_count);

#endif /* NOW_CACHE_H */
