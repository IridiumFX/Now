/*
 * now_resolve.h — Dependency resolution (§6.2-6.13)
 *
 * Resolves version constraints from the dependency graph,
 * applies convergence policy, and produces a lock file.
 */
#ifndef NOW_RESOLVE_H
#define NOW_RESOLVE_H

#include "now.h"
#include "now_version.h"

/* A resolved dependency entry in the lock file */
typedef struct {
    char *group;
    char *artifact;
    char *version;        /* exact resolved version string */
    char *triple;         /* platform triple or "noarch" */
    char *url;            /* resolved download URL */
    char *sha256;         /* hex SHA-256 of archive */
    char *descriptor_sha256;
    char *scope;          /* compile, test, runtime, provided */
    char **deps;          /* direct dep coordinates of this artifact */
    size_t dep_count;
    int    overridden;    /* 1 if forced by override */
} NowLockEntry;

/* The full lock file */
typedef struct {
    NowLockEntry *entries;
    size_t        count;
    size_t        capacity;
} NowLockFile;

NOW_API void now_lock_init(NowLockFile *lf);
NOW_API void now_lock_free(NowLockFile *lf);

/* Load/save lock file (now.lock.pasta). Returns 0 on success. */
NOW_API int now_lock_load(NowLockFile *lf, const char *path);
NOW_API int now_lock_save(const NowLockFile *lf, const char *path);

/* Find a lock entry by group:artifact. Returns NULL if not found. */
NOW_API const NowLockEntry *now_lock_find(const NowLockFile *lf,
                                           const char *group,
                                           const char *artifact);

/* Add or update a lock entry. Returns 0 on success. */
NOW_API int now_lock_set(NowLockFile *lf, const NowLockEntry *entry);

/* ---- Resolver ---- */

/* A constraint collected from the dependency graph */
typedef struct {
    char            *group;
    char            *artifact;
    NowVersionRange  range;
    char            *scope;
    char            *from;       /* who declared this constraint (for error messages) */
    int              override;   /* 1 if this is a forced override */
} NowConstraint;

/* The resolver context */
typedef struct {
    NowConstraint *constraints;
    size_t         count;
    size_t         capacity;
    const char    *convergence;  /* "lowest", "highest", "exact" */
} NowResolver;

NOW_API void now_resolver_init(NowResolver *r, const char *convergence);
NOW_API void now_resolver_free(NowResolver *r);

/* Add a constraint to the resolver. Returns 0 on success. */
NOW_API int now_resolver_add(NowResolver *r, const char *dep_id,
                              const char *scope, const char *from,
                              int override);

/* Resolve all constraints. Produces intersected ranges per coordinate.
 * Populates the lock file with resolved entries (version = floor of range
 * for "lowest", or placeholder for "highest" — actual version selection
 * happens at procure time when available versions are known).
 * Returns 0 on success, -1 on conflict. */
NOW_API int now_resolver_resolve(NowResolver *r, NowLockFile *lf,
                                  NowResult *result);

#endif /* NOW_RESOLVE_H */
