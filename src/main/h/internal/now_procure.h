/*
 * now_procure.h — Dependency procurement (§2.1 step 10)
 *
 * Downloads, verifies, and installs dependencies from registry
 * to the local repo (~/.now/repo/).
 */
#ifndef NOW_PROCURE_H
#define NOW_PROCURE_H

#include "now.h"
#include "now_pom.h"
#include "now_resolve.h"

/* Registry info for a single resolved version */
typedef struct {
    char *version;
    int   snapshot;
    char *triple;
} NowRegistryVersion;

/* Procure options */
typedef struct {
    const char *repo_root;   /* override ~/.now/repo (for testing) */
    const char *cache_root;  /* override ~/.now/cache (for testing) */
    int         offline;     /* 1 = skip downloads, use local only */
} NowProcureOpts;

/* Run the full procure phase for a project:
 * 1. Collect constraints from project deps
 * 2. Resolve version ranges
 * 3. For each resolved dep, check local repo
 * 4. If missing, query registry → download → verify SHA-256 → install
 * 5. Update lock file
 *
 * Returns 0 on success, -1 on error (details in result). */
NOW_API int now_procure(const NowProject *project, const NowProcureOpts *opts,
                        NowResult *result);

/* Query a registry for versions matching a range.
 * Returns count of matching versions, -1 on error.
 * Caller must free each version's fields and the array. */
NOW_API int now_registry_resolve(const char *registry_url,
                                  const char *group, const char *artifact,
                                  const char *range_str,
                                  NowRegistryVersion **versions_out,
                                  NowResult *result);

/* Download an artifact from registry to a local path.
 * auth_token: Bearer JWT (may be NULL for public registries).
 * Returns 0 on success, -1 on error. */
NOW_API int now_registry_download(const char *registry_url,
                                   const char *group, const char *artifact,
                                   const char *version, const char *filename,
                                   const char *dest_path,
                                   const char *auth_token,
                                   NowResult *result);

/* Check if a dependency is installed in the local repo.
 * Returns 1 if installed, 0 if not. */
NOW_API int now_repo_is_installed(const char *repo_root,
                                   const char *group, const char *artifact,
                                   const char *version);

/* Get the local repo path for a dependency.
 * Returns malloc'd string: {repo_root}/{group_path}/{artifact}/{version}/
 * Caller must free. */
NOW_API char *now_repo_dep_path(const char *repo_root,
                                 const char *group, const char *artifact,
                                 const char *version);

/* Install an artifact archive (.tar.gz) into the local repo.
 * Returns 0 on success. */
NOW_API int now_repo_install(const char *repo_root,
                              const char *group, const char *artifact,
                              const char *version,
                              const char *archive_path,
                              NowResult *result);

/* Free a NowRegistryVersion array. */
NOW_API void now_registry_versions_free(NowRegistryVersion *versions, int count);

/* ---- Dep confusion protection (§8) ---- */

/* Check if a group matches any private_groups prefix.
 * Prefix matching uses dot-boundary: "org.acme" matches "org.acme" and
 * "org.acme.internal" but NOT "org.acmecorp".
 * Returns 1 if the group is private, 0 otherwise. */
NOW_API int now_group_is_private(const NowStrArray *private_groups,
                                  const char *group);

/* ---- dep:updates ---- */

/* Check each project dep against the registry for newer versions.
 * Returns the number of deps with available updates, or -1 on error.
 * registry_url may be NULL to use project repos or default. */
NOW_API int now_dep_updates(const NowProject *project, const char *registry_url,
                             int verbose, NowResult *result);

/* ---- cache:mirror ---- */

/* Mirror artifacts from a registry to the local cache.
 * coords: comma-separated "g:a:v,g:a:v,..." or NULL for full manifest.
 * Returns the number of artifacts downloaded, or -1 on error. */
NOW_API int now_cache_mirror(const char *registry_url, const char *coords,
                              int verbose, NowResult *result);

#endif /* NOW_PROCURE_H */
