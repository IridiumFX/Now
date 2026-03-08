/*
 * now_procure.c — Dependency procurement
 *
 * Resolves deps via registry, downloads, verifies, and installs
 * to ~/.now/repo/.
 */

#include "now_procure.h"
#include "now_pom.h"
#include "now_fs.h"
#include "now_version.h"
#include "now_manifest.h"
#include "pico_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <direct.h>
  #include <shlobj.h>
  #define mkdir_compat(p) _mkdir(p)
#else
  #include <sys/stat.h>
  #include <pwd.h>
  #include <unistd.h>
  #define mkdir_compat(p) mkdir((p), 0755)
#endif

/* ---- Helpers ---- */

/* Get default repo root: ~/.now/repo */
static char *default_repo_root(void) {
    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
#endif
    if (!home) return NULL;

    char *dot_now = now_path_join(home, ".now");
    if (!dot_now) return NULL;
    char *repo = now_path_join(dot_now, "repo");
    free(dot_now);
    return repo;
}

/* Convert group "org.acme" → "org/acme" (malloc'd) */
static char *group_to_path(const char *group) {
    if (!group) return NULL;
    size_t len = strlen(group);
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    for (size_t i = 0; i < len; i++)
        p[i] = (group[i] == '.') ? '/' : group[i];
    p[len] = '\0';
    return p;
}

/* Parse JSON like {"versions":[{"version":"1.2.3","snapshot":false,"triples":["noarch"]}]}
 * Minimal JSON parsing — we know the exact format from cookbook. */
static int parse_resolve_response(const char *json, size_t len,
                                   NowRegistryVersion **out, int *count) {
    *out = NULL;
    *count = 0;

    /* Find "versions":[ */
    const char *arr = strstr(json, "\"versions\":[");
    if (!arr) return -1;
    arr = strchr(arr, '[');
    if (!arr) return -1;
    arr++;

    /* Count entries (count '{' at this nesting level) */
    int n = 0;
    int depth = 0;
    for (const char *p = arr; *p && !(*p == ']' && depth == 0); p++) {
        if (*p == '{') { if (depth == 0) n++; depth++; }
        else if (*p == '}') depth--;
    }

    if (n == 0) return 0;

    NowRegistryVersion *versions = (NowRegistryVersion *)calloc((size_t)n,
                                        sizeof(NowRegistryVersion));
    if (!versions) return -1;

    /* Parse each {"version":"...","snapshot":...,"triples":["..."]} */
    const char *p = arr;
    for (int i = 0; i < n; i++) {
        /* Find start of object */
        p = strchr(p, '{');
        if (!p) break;
        p++;

        /* Find "version":"..." */
        const char *vkey = strstr(p, "\"version\":\"");
        if (vkey) {
            vkey += 11; /* skip "version":" */
            const char *vend = strchr(vkey, '"');
            if (vend) {
                size_t vlen = (size_t)(vend - vkey);
                versions[i].version = (char *)malloc(vlen + 1);
                if (versions[i].version) {
                    memcpy(versions[i].version, vkey, vlen);
                    versions[i].version[vlen] = '\0';
                }
            }
        }

        /* Find "snapshot":true/false */
        const char *skey = strstr(p, "\"snapshot\":");
        if (skey) {
            skey += 11;
            versions[i].snapshot = (*skey == 't') ? 1 : 0;
        }

        /* Find "triples":["..."] — take first triple */
        const char *tkey = strstr(p, "\"triples\":[\"");
        if (tkey) {
            tkey += 12;
            const char *tend = strchr(tkey, '"');
            if (tend) {
                size_t tlen = (size_t)(tend - tkey);
                versions[i].triple = (char *)malloc(tlen + 1);
                if (versions[i].triple) {
                    memcpy(versions[i].triple, tkey, tlen);
                    versions[i].triple[tlen] = '\0';
                }
            }
        }

        /* Advance past this object */
        p = strchr(p, '}');
        if (p) p++;
    }

    *out = versions;
    *count = n;
    return 0;
}

/* ---- Dep confusion protection ---- */

NOW_API int now_group_is_private(const NowStrArray *private_groups,
                                  const char *group) {
    if (!private_groups || !group) return 0;
    for (size_t i = 0; i < private_groups->count; i++) {
        const char *prefix = private_groups->items[i];
        if (!prefix) continue;
        size_t plen = strlen(prefix);
        size_t glen = strlen(group);
        if (glen < plen) continue;
        if (strncmp(group, prefix, plen) != 0) continue;
        /* Exact match or dot-boundary: "org.acme" matches "org.acme" and
         * "org.acme.foo" but NOT "org.acmecorp" */
        if (glen == plen || group[plen] == '.')
            return 1;
    }
    return 0;
}

/* ---- Public API ---- */

NOW_API char *now_repo_dep_path(const char *repo_root,
                                 const char *group, const char *artifact,
                                 const char *version) {
    char *gpath = group_to_path(group);
    if (!gpath) return NULL;

    char *p1 = now_path_join(repo_root, gpath);
    free(gpath);
    if (!p1) return NULL;

    char *p2 = now_path_join(p1, artifact);
    free(p1);
    if (!p2) return NULL;

    char *p3 = now_path_join(p2, version);
    free(p2);
    return p3;
}

NOW_API int now_repo_is_installed(const char *repo_root,
                                   const char *group, const char *artifact,
                                   const char *version) {
    char *dep_path = now_repo_dep_path(repo_root, group, artifact, version);
    if (!dep_path) return 0;

    /* Check for now.pasta inside the dep directory */
    char *descriptor = now_path_join(dep_path, "now.pasta");
    free(dep_path);
    if (!descriptor) return 0;

    int exists = now_path_exists(descriptor);
    free(descriptor);
    return exists;
}

NOW_API int now_registry_resolve(const char *registry_url,
                                  const char *group, const char *artifact,
                                  const char *range_str,
                                  NowRegistryVersion **versions_out,
                                  NowResult *result) {
    if (!registry_url || !group || !artifact || !range_str) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "NULL argument to now_registry_resolve");
        return -1;
    }

    /* Parse registry URL */
    char *host = NULL;
    char *base_path = NULL;
    int port = 80;
    if (pico_http_parse_url(registry_url, &host, &port, &base_path) != 0) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Invalid registry URL: %s", registry_url);
        return -1;
    }

    /* Build path: /resolve/{group}/{artifact}/{range} */
    /* Group dots become slashes for the URL path */
    char *gpath = group_to_path(group);
    char path[1024];
    snprintf(path, sizeof(path), "/resolve/%s/%s/%s", gpath, artifact, range_str);
    free(gpath);

    PicoHttpHeader accept_hdr = { "Accept",
        "application/x-pasta, application/json;q=0.9" };
    PicoHttpOptions opts = {0};
    opts.headers = &accept_hdr;
    opts.header_count = 1;

    PicoHttpResponse res;
    int rc = pico_http_get(host, port, path, &opts, &res);
    free(host);
    free(base_path);

    if (rc != 0) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Network error querying registry");
        return -1;
    }

    if (res.status != 200) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Registry returned status %d", res.status);
        pico_http_response_free(&res);
        return -1;
    }

    int count = 0;
    if (parse_resolve_response(res.body, res.body_len, versions_out, &count) != 0) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Failed to parse registry response");
        pico_http_response_free(&res);
        return -1;
    }

    pico_http_response_free(&res);
    return count;
}

/* Stream callback: write chunk to FILE* */
static int write_to_file(const void *data, size_t len, void *userdata) {
    FILE *f = (FILE *)userdata;
    return fwrite(data, 1, len, f) == len ? 0 : -1;
}

NOW_API int now_registry_download(const char *registry_url,
                                   const char *group, const char *artifact,
                                   const char *version, const char *filename,
                                   const char *dest_path,
                                   NowResult *result) {
    char *host = NULL;
    char *base_path = NULL;
    int port = 80;
    if (pico_http_parse_url(registry_url, &host, &port, &base_path) != 0) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Invalid registry URL: %s", registry_url);
        return -1;
    }

    char *gpath = group_to_path(group);
    char path[1024];
    snprintf(path, sizeof(path), "/artifact/%s/%s/%s/%s",
             gpath, artifact, version, filename);
    free(gpath);

    /* Open destination file before starting download */
    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Cannot write to %s", dest_path);
        free(host);
        free(base_path);
        return -1;
    }

    PicoHttpResponse res;
    int rc = pico_http_get_stream(host, port, path, NULL, &res,
                                  write_to_file, f);
    free(host);
    free(base_path);

    if (rc != 0) {
        fclose(f);
        remove(dest_path);
        if (result) snprintf(result->message, sizeof(result->message),
                             "Network error downloading %s", filename);
        return -1;
    }

    if (res.status != 200) {
        fclose(f);
        remove(dest_path);
        if (result) snprintf(result->message, sizeof(result->message),
                             "Registry returned %d for %s", res.status, filename);
        pico_http_response_free(&res);
        return -1;
    }

    fclose(f);

    pico_http_response_free(&res);
    return 0;
}

NOW_API void now_registry_versions_free(NowRegistryVersion *versions, int count) {
    if (!versions) return;
    for (int i = 0; i < count; i++) {
        free(versions[i].version);
        free(versions[i].triple);
    }
    free(versions);
}

NOW_API int now_repo_install(const char *repo_root,
                              const char *group, const char *artifact,
                              const char *version,
                              const char *archive_path,
                              NowResult *result) {
    /* Create the target directory */
    char *dep_path = now_repo_dep_path(repo_root, group, artifact, version);
    if (!dep_path) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Cannot compute dep path");
        return -1;
    }

    if (now_mkdir_p(dep_path) != 0) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Cannot create directory %s", dep_path);
        free(dep_path);
        return -1;
    }

    /* For now, just copy the archive into the dep directory.
     * TODO: extract .tar.gz into canonical layout (h/, lib/, etc.)
     * For the initial implementation, we'll handle now.pasta download
     * and plain files separately. */
    char *archive_dest = now_path_join(dep_path, "archive.tar.gz");
    free(dep_path);
    if (!archive_dest) return -1;

    /* Copy file */
    FILE *src = fopen(archive_path, "rb");
    if (!src) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Cannot open archive %s", archive_path);
        free(archive_dest);
        return -1;
    }

    FILE *dst = fopen(archive_dest, "wb");
    if (!dst) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Cannot write archive to repo");
        fclose(src);
        free(archive_dest);
        return -1;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dst);

    fclose(dst);
    fclose(src);
    free(archive_dest);
    return 0;
}

NOW_API int now_procure(const NowProject *project, const NowProcureOpts *opts,
                        NowResult *result) {
    if (!project) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "NULL project");
        return -1;
    }

    /* Determine repo root */
    char *repo_root = NULL;
    if (opts && opts->repo_root) {
        repo_root = (char *)malloc(strlen(opts->repo_root) + 1);
        if (repo_root) strcpy(repo_root, opts->repo_root);
    } else {
        repo_root = default_repo_root();
    }
    if (!repo_root) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Cannot determine repo root");
        return -1;
    }

    /* No deps = nothing to procure */
    if (project->deps.count == 0) {
        free(repo_root);
        return 0;
    }

    /* Step 1: Resolve constraints */
    NowResolver resolver;
    const char *convergence = project->convergence ? project->convergence : "lowest";
    now_resolver_init(&resolver, convergence);

    for (size_t i = 0; i < project->deps.count; i++) {
        const NowDep *dep = &project->deps.items[i];
        const char *scope = dep->scope ? dep->scope : "compile";
        now_resolver_add(&resolver, dep->id, scope, "project", dep->override);
    }

    NowLockFile lockfile;
    now_lock_init(&lockfile);

    /* Try to load existing lock file */
    char *lock_path = now_path_join(".", "now.lock.pasta");
    if (lock_path) {
        now_lock_load(&lockfile, lock_path);
        /* ignore error — may not exist yet */
    }

    if (now_resolver_resolve(&resolver, &lockfile, result) != 0) {
        now_resolver_free(&resolver);
        now_lock_free(&lockfile);
        free(repo_root);
        free(lock_path);
        return -1;
    }

    /* Step 2: Determine registry URL */
    const char *registry_url = "http://localhost:8080";
    if (project->repos.count > 0 && project->repos.items[0].url)
        registry_url = project->repos.items[0].url;

    int offline = (opts && opts->offline);

    /* Step 3: For each resolved dep, ensure it's installed */
    for (size_t i = 0; i < lockfile.count; i++) {
        NowLockEntry *entry = &lockfile.entries[i];

        /* Dep confusion protection: private groups must not resolve
         * from the default public registry — only from project-declared repos */
        int is_private = now_group_is_private(&project->private_groups,
                                               entry->group);
        if (is_private && project->repos.count == 0) {
            if (result) {
                result->code = NOW_ERR_NOT_FOUND;
                snprintf(result->message, sizeof(result->message),
                         "private group '%s' has no declared repositories — "
                         "add a repos: entry or remove from private_groups",
                         entry->group);
            }
            now_resolver_free(&resolver);
            now_lock_free(&lockfile);
            free(repo_root);
            free(lock_path);
            return -1;
        }

        /* Check if already installed locally */
        if (now_repo_is_installed(repo_root, entry->group,
                                  entry->artifact, entry->version)) {
            continue;
        }

        if (offline) {
            if (result) snprintf(result->message, sizeof(result->message),
                                 "Dependency %s:%s:%s not installed and offline mode is set",
                                 entry->group, entry->artifact, entry->version);
            now_resolver_free(&resolver);
            now_lock_free(&lockfile);
            free(repo_root);
            free(lock_path);
            return -1;
        }

        /* If version is from "lowest" convergence (synthetic floor),
         * query registry to find actual available versions */
        if (!entry->version || strlen(entry->version) == 0) {
            /* Need to query registry for actual versions */
            NowCoordinate coord;
            if (now_coord_parse(project->deps.items[i].id, &coord) != 0)
                continue;

            NowRegistryVersion *versions = NULL;
            int vcount = now_registry_resolve(registry_url,
                                              coord.group, coord.artifact,
                                              coord.version,
                                              &versions, result);
            free(coord.group);
            free(coord.artifact);
            free(coord.version);

            if (vcount <= 0) {
                if (result && result->message[0] == '\0')
                    snprintf(result->message, sizeof(result->message),
                             "No versions found for %s:%s",
                             entry->group, entry->artifact);
                now_resolver_free(&resolver);
                now_lock_free(&lockfile);
                free(repo_root);
                free(lock_path);
                return -1;
            }

            /* Pick version based on convergence policy */
            /* versions come sorted descending from registry */
            int pick = (strcmp(convergence, "highest") == 0) ? 0 : vcount - 1;
            free(entry->version);
            entry->version = versions[pick].version;
            versions[pick].version = NULL; /* transferred ownership */

            if (!entry->triple && versions[pick].triple) {
                entry->triple = versions[pick].triple;
                versions[pick].triple = NULL;
            }

            now_registry_versions_free(versions, vcount);
        }

        /* Download the descriptor (now.pasta) */
        char *dep_dir = now_repo_dep_path(repo_root, entry->group,
                                           entry->artifact, entry->version);
        if (!dep_dir) continue;
        now_mkdir_p(dep_dir);

        char *desc_dest = now_path_join(dep_dir, "now.pasta");
        if (desc_dest) {
            now_registry_download(registry_url,
                                  entry->group, entry->artifact,
                                  entry->version, "now.pasta",
                                  desc_dest, result);
            free(desc_dest);
        }

        /* Download the archive */
        char archive_name[256];
        snprintf(archive_name, sizeof(archive_name), "%s-%s.tar.gz",
                 entry->artifact, entry->version);
        char *archive_dest = now_path_join(dep_dir, archive_name);
        if (archive_dest) {
            if (now_registry_download(registry_url,
                                      entry->group, entry->artifact,
                                      entry->version, archive_name,
                                      archive_dest, result) == 0) {
                /* Verify SHA-256 if we have it */
                if (entry->sha256 && strlen(entry->sha256) > 0) {
                    char *actual = now_sha256_file(archive_dest);
                    if (actual && strcmp(actual, entry->sha256) != 0) {
                        if (result)
                            snprintf(result->message, sizeof(result->message),
                                     "SHA-256 mismatch for %s:%s:%s "
                                     "(expected %s, got %s)",
                                     entry->group, entry->artifact,
                                     entry->version, entry->sha256, actual);
                        free(actual);
                        free(archive_dest);
                        free(dep_dir);
                        now_resolver_free(&resolver);
                        now_lock_free(&lockfile);
                        free(repo_root);
                        free(lock_path);
                        return -1;
                    }
                    free(actual);
                }

                /* Compute and store SHA-256 if not already known */
                if (!entry->sha256 || strlen(entry->sha256) == 0) {
                    free(entry->sha256);
                    entry->sha256 = now_sha256_file(archive_dest);
                }
            }
            free(archive_dest);
        }

        /* Download the .sha256 sidecar */
        char sha_name[256];
        snprintf(sha_name, sizeof(sha_name), "%s-%s.sha256",
                 entry->artifact, entry->version);
        char *sha_dest = now_path_join(dep_dir, sha_name);
        if (sha_dest) {
            now_registry_download(registry_url,
                                  entry->group, entry->artifact,
                                  entry->version, sha_name,
                                  sha_dest, result);
            free(sha_dest);
        }

        /* Build the URL for the lock file */
        if (!entry->url) {
            char *gpath = group_to_path(entry->group);
            char url_buf[1024];
            snprintf(url_buf, sizeof(url_buf), "%s/artifact/%s/%s/%s/%s",
                     registry_url, gpath, entry->artifact,
                     entry->version, archive_name);
            free(gpath);
            entry->url = (char *)malloc(strlen(url_buf) + 1);
            if (entry->url) strcpy(entry->url, url_buf);
        }

        free(dep_dir);
    }

    /* Step 4: Save updated lock file */
    if (lock_path) {
        now_lock_save(&lockfile, lock_path);
    }

    now_resolver_free(&resolver);
    now_lock_free(&lockfile);
    free(repo_root);
    free(lock_path);
    return 0;
}
