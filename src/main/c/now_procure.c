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
#include "now_resolve.h"
#include "now_manifest.h"
#include "now_package.h"
#include "now_auth.h"
#include "now_trust.h"
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

NOW_API int now_registry_is_public(const char *url) {
    if (!url || !*url) return 0;

    /* Compare on host only. The fence has to hold whether the central
     * registry is written with a path, a trailing slash, http or https,
     * so anchoring on the scheme or on a whole-string match would be a
     * way around it. */
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    const char *end = h;
    while (*end && *end != '/' && *end != ':') end++;
    size_t hlen = (size_t)(end - h);

    /* registry.now.build is the canonical spelling as of 2026-08-12, and
     * is what the built-in layer and the spec now both use.
     * repo.now.build stays recognised **deliberately**: descriptors
     * written against the older spelling are still out there, and a repo
     * list naming the old host must remain fenced. Dropping it would
     * silently convert those into unfenced holes — the one direction
     * this list must never move. */
    static const char *const k_public_hosts[] = {
        "registry.now.build", "repo.now.build", NULL
    };
    for (size_t i = 0; k_public_hosts[i]; i++) {
        size_t plen = strlen(k_public_hosts[i]);
        if (hlen == plen && strncmp(h, k_public_hosts[i], plen) == 0) return 1;
    }
    return 0;
}

NOW_API size_t now_registry_candidates(const NowProject *project,
                                        const NowProcureOpts *opts,
                                        const char *group,
                                        const char **out, size_t max) {
    if (!out || max == 0) return 0;

    int is_private = project &&
                     now_group_is_private(&project->private_groups, group);

    /* --repo replaces the declared list rather than joining it: an
     * operator naming one registry means that registry. The fence still
     * applies, so it cannot be used to pull a private group from a
     * public host. */
    if (opts && opts->registry_url && *opts->registry_url) {
        if (is_private && now_registry_is_public(opts->registry_url))
            return 0;
        out[0] = opts->registry_url;
        return 1;
    }

    size_t n = 0;
    if (project) {
        for (size_t i = 0; i < project->repos.count && n < max; i++) {
            const char *url = project->repos.items[i].url;
            if (!url || !*url) continue;
            if (is_private && now_registry_is_public(url)) {
                /* §25.2: only repos declared *before* any public registry
                 * are tried. Stop rather than skip — a private repo listed
                 * after the public one is deliberately out of scope, so
                 * that declaration order stays the whole policy. */
                break;
            }
            out[n++] = url;
        }
    }

    /* No repos declared at all: the historical default. Never for a
     * private group — that is the case the original check caught, and it
     * stays caught. */
    if (n == 0 && !is_private && max > 0) {
        out[n++] = "http://localhost:8080";
    }
    return n;
}

/* Most registries a single procure run will consult. Declaring more is
 * not an error — the extras are simply never reached, which beats a
 * heap allocation on a path that realistically holds two or three. */
#define NOW_MAX_REGISTRIES 8

typedef struct {
    const char *url;   /* borrowed */
    char       *jwt;   /* owned, may be NULL for an anonymous registry */
    int         tried; /* auth attempted; do not retry per dep */
} NowRegAuth;

/* Credentials are keyed by registry, so a run that touches more than one
 * needs a token per registry rather than one token reused everywhere —
 * presenting registry A's JWT to registry B leaks it and fails anyway. */
static const char *registry_jwt(NowRegAuth *cache, size_t *n,
                                 const char *url, int offline,
                                 NowResult *result) {
    if (!cache || !n || !url) return NULL;
    for (size_t i = 0; i < *n; i++)
        if (cache[i].url && strcmp(cache[i].url, url) == 0)
            return cache[i].jwt;

    if (*n >= NOW_MAX_REGISTRIES) return NULL;
    NowRegAuth *slot = &cache[(*n)++];
    slot->url = url;
    slot->jwt = NULL;
    slot->tried = 1;
    if (offline) return NULL;

    NowCredentials creds;
    if (now_auth_load(url, &creds) == 0) {
        if (creds.username) {
            char *host = NULL, *base_path = NULL;
            int port = 80, tls = 0;
            if (pico_http_parse_url_ex(url, &host, &port, &base_path, &tls) == 0) {
                char *prefix = base_path ? base_path : strdup("");
                size_t plen = prefix ? strlen(prefix) : 0;
                while (plen > 0 && prefix[plen - 1] == '/') prefix[--plen] = '\0';
                now_auth_login(host, port, prefix, &creds, tls, &slot->jwt, result);
                free(prefix);
            }
            free(host);
        }
        /* No username: the raw token is the Bearer value. */
        if (!slot->jwt && creds.token) slot->jwt = strdup(creds.token);
        now_auth_creds_free(&creds);
    }
    return slot->jwt;
}

NOW_API int now_lock_differs(const NowLockFile *before, const NowLockFile *after,
                              const char **what, const char **which) {
    if (what) *what = NULL;
    if (which) *which = NULL;
    if (!before || !after) return 0;

    for (size_t i = 0; i < after->count; i++) {
        const NowLockEntry *now_e = &after->entries[i];
        const NowLockEntry *was = now_lock_find(before, now_e->group,
                                                 now_e->artifact);
        if (!was) {
            if (what) *what = "added";
            if (which) *which = now_e->artifact;
            return 1;
        }
        if (now_e->version && was->version &&
            strcmp(now_e->version, was->version) != 0) {
            if (what) *what = "changed version";
            if (which) *which = now_e->artifact;
            return 1;
        }
    }
    for (size_t i = 0; i < before->count; i++) {
        const NowLockEntry *was = &before->entries[i];
        if (!now_lock_find(after, was->group, was->artifact)) {
            if (what) *what = "removed";
            if (which) *which = was->artifact;
            return 1;
        }
    }
    return 0;
}

static void registry_auth_free(NowRegAuth *cache, size_t n) {
    if (!cache) return;
    for (size_t i = 0; i < n; i++) {
        free(cache[i].jwt);
        cache[i].jwt = NULL;
    }
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

    /* The descriptor alone is not evidence of an install.
     *
     * It is downloaded before the checksum and the signature are
     * checked, and it survived a rejection — so a *second* `now procure`
     * after one that refused a tampered artifact saw now.pasta, decided
     * the dependency was already installed, skipped the download, the
     * checksum, the signature and the unpack, and printed "procure:
     * done" with exit 0 while nothing was installed. A retry turned a
     * hard supply-chain failure into a silent success.
     *
     * Require what the compile path actually consumes: the unpacked
     * layout. `now install` and `now_repo_install` both write it, and a
     * rejected fetch never gets that far. */
    char *descriptor = now_path_join(dep_path, "now.pasta");
    int ok = 0;
    if (descriptor && now_path_exists(descriptor)) {
        static const char *const payload[] = { "h", "lib", "bin", NULL };
        for (int i = 0; payload[i] && !ok; i++) {
            char *d = now_path_join(dep_path, payload[i]);
            if (d && now_path_exists(d)) ok = 1;
            free(d);
        }
    }
    free(descriptor);
    free(dep_path);
    return ok;
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
    int reg_tls = 0;
    if (pico_http_parse_url_ex(registry_url, &host, &port, &base_path, &reg_tls) != 0) {
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
    opts.tls = reg_tls ? 1 : -1;   /* scheme from the registry URL, not the port */

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
                                   const char *auth_token,
                                   NowResult *result) {
    char *host = NULL;
    char *base_path = NULL;
    int port = 80;
    int reg_tls = 0;
    if (pico_http_parse_url_ex(registry_url, &host, &port, &base_path, &reg_tls) != 0) {
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

    /* Build options with auth header if provided */
    PicoHttpHeader headers[1];
    int nhdr = 0;
    char auth_buf[1024];
    if (auth_token && *auth_token) {
        snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", auth_token);
        headers[nhdr].name  = "Authorization";
        headers[nhdr].value = auth_buf;
        nhdr++;
    }

    PicoHttpOptions opts = {0};
    opts.headers = headers;
    opts.header_count = (size_t)nhdr;
    /* Artifact bytes plus a Bearer JWT — say the scheme rather than
     * letting the port decide it. */
    opts.tls = reg_tls ? 1 : -1;

    PicoHttpResponse res;
    int rc = pico_http_get_stream(host, port, path, &opts, &res,
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

    /* Check file extension to decide extraction method */
    size_t pathlen = strlen(archive_path);
    if (pathlen > 6 && strcmp(archive_path + pathlen - 6, ".basta") == 0) {
        /* Basta package: extract into canonical layout */
        int rc = now_basta_extract(archive_path, dep_path, 0, result);
        free(dep_path);
        return rc;
    }

    /* Legacy tar.gz: copy the archive as-is */
    char *archive_dest = now_path_join(dep_path, "archive.tar.gz");
    free(dep_path);
    if (!archive_dest) return -1;

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

    /* --locked: a second copy of what was on disk, so resolution can be
     * compared against it afterwards. The flag was parsed and then never
     * read by procure, so `now procure --locked` happily rewrote the
     * lockfile — the one thing the flag exists to prevent. */
    int locked = (opts && opts->locked);
    NowLockFile lock_before;
    now_lock_init(&lock_before);
    if (locked && lock_path) now_lock_load(&lock_before, lock_path);

    if (now_resolver_resolve(&resolver, &lockfile, result) != 0) {
        now_lock_free(&lock_before);
        now_resolver_free(&resolver);
        now_lock_free(&lockfile);
        free(repo_root);
        free(lock_path);
        return -1;
    }

    /* Step 2: registries are now chosen per dependency, because the
     * private-group fence depends on the dep's group. Auth follows the
     * same way — resolved on first use of each registry and cached. */
    int offline = (opts && opts->offline);
    NowRegAuth reg_auth[NOW_MAX_REGISTRIES];
    size_t reg_auth_n = 0;
    memset(reg_auth, 0, sizeof(reg_auth));

    /* Step 3: For each resolved dep, ensure it's installed */
    for (size_t i = 0; i < lockfile.count; i++) {
        NowLockEntry *entry = &lockfile.entries[i];

        /* Dependency-confusion fence (§25.2). The registries this dep may
         * be served by, in declaration order — for a private group the
         * list stops before any public registry and never contains one.
         * An empty list is a hard failure: the spec is explicit that
         * there is no fallback and no "try public anyway" escape. */
        int is_private = now_group_is_private(&project->private_groups,
                                               entry->group);
        const char *cand_urls[NOW_MAX_REGISTRIES];
        size_t cand_n = now_registry_candidates(project, opts, entry->group,
                                                 cand_urls, NOW_MAX_REGISTRIES);
        if (cand_n == 0) {
            if (result) {
                result->code = NOW_ERR_NOT_FOUND;
                if (is_private)
                    snprintf(result->message, sizeof(result->message),
                             "%s:%s is in a private group and no private registry "
                             "is declared ahead of a public one. The public registry "
                             "was NOT consulted (private_groups policy). Declare a "
                             "private repo before any public entry in repos:, or "
                             "remove the group from private_groups",
                             entry->group, entry->artifact);
                else
                    snprintf(result->message, sizeof(result->message),
                             "no registry available for %s:%s",
                             entry->group, entry->artifact);
            }
            now_resolver_free(&resolver);
            now_lock_free(&lockfile);
            free(repo_root);
            free(lock_path);
            return -1;
        }

        /* The registry serving this dep. Provisionally the first
         * candidate; whichever one proves it has the artifact takes over
         * and `reg_locked` stops the archive loop searching again. */
        const char *registry_url = cand_urls[0];
        const char *jwt = NULL;
        int reg_locked = 0;

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

            /* Ask each permitted registry in turn. The first that knows
             * the coordinate owns this dep — every later fetch for it
             * goes to the same one, so a dep is never assembled from a
             * mixture of registries. */
            NowRegistryVersion *versions = NULL;
            int vcount = -1;
            for (size_t ri = 0; ri < cand_n && vcount <= 0; ri++) {
                versions = NULL;
                vcount = now_registry_resolve(cand_urls[ri],
                                              coord.group, coord.artifact,
                                              coord.version,
                                              &versions, result);
                if (vcount > 0) {
                    registry_url = cand_urls[ri];
                    jwt = registry_jwt(reg_auth, &reg_auth_n, registry_url,
                                       offline, result);
                    reg_locked = 1;
                } else if (versions) {
                    now_registry_versions_free(versions, vcount > 0 ? vcount : 0);
                    versions = NULL;
                }
            }
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

        char *dep_dir = now_repo_dep_path(repo_root, entry->group,
                                           entry->artifact, entry->version);
        if (!dep_dir) continue;
        now_mkdir_p(dep_dir);

        /* `now package` names its output {artifact}-{version}-{triple},
         * and that is what lands in the registry — but procure asked for
         * {artifact}-{version}, a name the publisher never creates. Ask
         * for the triple-qualified name the lock entry describes, and
         * fall back to the bare form so registries populated before this
         * (or by hand) still resolve. */
        /* Prefer the triple the registry told us about; otherwise the
         * host's, which is what `now package` stamps into the name for
         * anything that isn't header-only. Defaulting straight to
         * "noarch" asked for a file no publisher had produced. */
        const char *raw_triple = (entry->triple && entry->triple[0])
                                 ? entry->triple : now_host_triple();
        if (!raw_triple || !raw_triple[0]) raw_triple = "noarch";

        /* The registry reports triples colon-separated (windows:x86_64:gnu)
         * while `now package` spells them with dashes in the filename
         * (windows-x86_64-gnu). Normalise, or we ask for a name that
         * cannot exist. */
        char ent_triple[128];
        {
            size_t ti = 0;
            for (; raw_triple[ti] && ti < sizeof(ent_triple) - 1; ti++)
                ent_triple[ti] = (raw_triple[ti] == ':') ? '-' : raw_triple[ti];
            ent_triple[ti] = '\0';
        }

        char sha_name[256];
        snprintf(sha_name, sizeof(sha_name), "%s-%s-%s.sha256",
                 entry->artifact, entry->version, ent_triple);
        char *sha_dest = now_path_join(dep_dir, sha_name);

        /* Download the archive (.basta or legacy .tar.gz) */
        char archive_name[256];
        snprintf(archive_name, sizeof(archive_name), "%s-%s-%s.basta",
                 entry->artifact, entry->version, ent_triple);
        /* Which of the candidate names the registry actually served. The
         * sidecar and the signature belong to *that* file, so they have
         * to be asked for under the same stem — asking under a different
         * one is how require_signatures became unsatisfiable against
         * anything `now publish` uploads. Empty until the archive
         * resolves. */
        char served_stem[256];
        served_stem[0] = '\0';
        char *archive_dest = now_path_join(dep_dir, archive_name);
        if (archive_dest) {
            /* Try, in order: the triple the lock/registry recorded, the
             * host triple, then the bare name. The recorded triple is
             * often "noarch" even for a platform build, so trusting it
             * alone asks for a file that was never published. */
            const char *host_tr = now_host_triple();
            const char *cands[3];
            char c0[256], c1[256], c2[256];
            snprintf(c0, sizeof(c0), "%s-%s-%s.basta",
                     entry->artifact, entry->version, ent_triple);
            snprintf(c1, sizeof(c1), "%s-%s-%s.basta",
                     entry->artifact, entry->version,
                     (host_tr && host_tr[0]) ? host_tr : "noarch");
            snprintf(c2, sizeof(c2), "%s-%s.basta",
                     entry->artifact, entry->version);
            cands[0] = c0; cands[1] = c1; cands[2] = c2;

            /* Two nested searches: which registry has it, and under which
             * of the three names. If the version query already settled
             * the registry, only that one is tried — re-searching could
             * otherwise pull the archive from a different registry than
             * the metadata came from. */
            int adl = -1;
            size_t first_reg = 0, last_reg = cand_n;
            if (reg_locked) {
                for (size_t ri = 0; ri < cand_n; ri++)
                    if (cand_urls[ri] == registry_url) { first_reg = ri; break; }
                last_reg = first_reg + 1;
            }
            for (size_t ri = first_reg; ri < last_reg && adl != 0; ri++) {
                const char *u = cand_urls[ri];
                const char *t = registry_jwt(reg_auth, &reg_auth_n, u,
                                             offline, result);
                for (int ci = 0; ci < 3 && adl != 0; ci++) {
                    if (ci > 0 && strcmp(cands[ci], cands[ci - 1]) == 0) continue;
                    /* Only the remote name varies; the local file is always
                     * archive_dest, so archive_name must keep matching it —
                     * renaming it here made the later signature check look
                     * for a path that was never written. */
                    adl = now_registry_download(u,
                                                entry->group, entry->artifact,
                                                entry->version, cands[ci],
                                                archive_dest, jwt ? jwt : t,
                                                result);
                    if (adl == 0) {
                        /* Remember the stem without the .basta suffix. */
                        snprintf(served_stem, sizeof(served_stem), "%s",
                                 cands[ci]);
                        char *dot = strrchr(served_stem, '.');
                        if (dot) *dot = '\0';
                    }
                }
                if (adl == 0) { registry_url = u; jwt = t; reg_locked = 1; }
            }

            /* Registry settled. The descriptor and the checksum sidecar
             * must come from the same one that served the archive. */
            if (adl == 0) {
                char *desc_dest = now_path_join(dep_dir, "now.pasta");
                if (desc_dest) {
                    now_registry_download(registry_url,
                                          entry->group, entry->artifact,
                                          entry->version, "now.pasta",
                                          desc_dest, jwt, result);
                    free(desc_dest);
                }
                if (sha_dest) {
                    /* Ask under the stem that served the archive first;
                     * the recorded triple is often "noarch" for a
                     * platform build, so the old first guess named a
                     * file no publisher wrote and the bare fallback
                     * missed the host-triple form entirely. */
                    if (served_stem[0])
                        snprintf(sha_name, sizeof(sha_name), "%s.sha256",
                                 served_stem);
                    if (now_registry_download(registry_url,
                                              entry->group, entry->artifact,
                                              entry->version, sha_name,
                                              sha_dest, jwt, result) != 0) {
                        snprintf(sha_name, sizeof(sha_name), "%s-%s.sha256",
                                 entry->artifact, entry->version);
                        now_registry_download(registry_url,
                                              entry->group, entry->artifact,
                                              entry->version, sha_name,
                                              sha_dest, jwt, result);
                    }
                }
            }
            if (adl == 0) {
                /* Verify SHA-256 against sidecar file */
                char *actual = now_sha256_file(archive_dest);
                if (actual) {
                    int sha_ok = 1;

                    /* Check against .sha256 sidecar if downloaded */
                    if (sha_dest && now_path_exists(sha_dest)) {
                        FILE *sf = fopen(sha_dest, "r");
                        if (sf) {
                            char expected[128] = {0};
                            if (fgets(expected, sizeof(expected), sf)) {
                                /* Strip trailing whitespace/newline */
                                size_t elen = strlen(expected);
                                while (elen > 0 && (expected[elen-1] == '\n' ||
                                       expected[elen-1] == '\r' ||
                                       expected[elen-1] == ' '))
                                    expected[--elen] = '\0';

                                if (elen > 0 && strcmp(actual, expected) != 0) {
                                    if (result) {
                                        result->code = NOW_ERR_IO;
                                        snprintf(result->message, sizeof(result->message),
                                                 "SHA-256 mismatch for %s:%s:%s "
                                                 "(sidecar %s, actual %s)",
                                                 entry->group, entry->artifact,
                                                 entry->version, expected, actual);
                                    }
                                    sha_ok = 0;
                                }
                            }
                            fclose(sf);
                        }
                    }

                    /* Also check against lock file SHA if present */
                    if (sha_ok && entry->sha256 && strlen(entry->sha256) > 0) {
                        if (strcmp(actual, entry->sha256) != 0) {
                            if (result) {
                                result->code = NOW_ERR_IO;
                                snprintf(result->message, sizeof(result->message),
                                         "SHA-256 mismatch for %s:%s:%s "
                                         "(lock %s, actual %s)",
                                         entry->group, entry->artifact,
                                         entry->version, entry->sha256, actual);
                            }
                            sha_ok = 0;
                        }
                    }

                    if (!sha_ok) {
                        free(actual);
                        remove(archive_dest);
                        free(archive_dest);
                        free(sha_dest);
                        free(dep_dir);
                        now_resolver_free(&resolver);
                        now_lock_free(&lockfile);
                        free(repo_root);
                        free(lock_path);
                        registry_auth_free(reg_auth, reg_auth_n);
                        return -1;
                    }

                    /* Store SHA-256 in lock entry */
                    if (!entry->sha256 || strlen(entry->sha256) == 0) {
                        free(entry->sha256);
                        entry->sha256 = actual;
                        actual = NULL;
                    }
                    free(actual);
                }
            }
            free(archive_dest);
        }
        free(sha_dest);

        /* Download .sig if require_signatures is set */
        NowTrustPolicy trust_policy = now_trust_policy_from_project(project);
        if (trust_policy.require_signatures) {
            /* `now package` writes {artifact}-{version}-{triple}.sig and
             * `now publish` uploads exactly that. Asking only for the
             * bare {artifact}-{version}.sig meant require_signatures
             * could never be satisfied by anything this tool publishes:
             * a 404, reported as a hard failure, on every dependency.
             * Ask under the stem that served the archive — the signature
             * covers that file — and keep the bare name as a fallback
             * for registries populated by hand. */
            char sig_name[256];
            char sig_alt[256];
            if (served_stem[0])
                snprintf(sig_name, sizeof(sig_name), "%s.sig", served_stem);
            else
                snprintf(sig_name, sizeof(sig_name), "%s-%s.sig",
                         entry->artifact, entry->version);
            snprintf(sig_alt, sizeof(sig_alt), "%s-%s.sig",
                     entry->artifact, entry->version);
            char *sig_dest = now_path_join(dep_dir, sig_name);
            if (sig_dest) {
                int sig_rc = now_registry_download(registry_url,
                                      entry->group, entry->artifact,
                                      entry->version, sig_name,
                                      sig_dest, jwt, result);
                if (sig_rc != 0 && strcmp(sig_name, sig_alt) != 0)
                    sig_rc = now_registry_download(registry_url,
                                      entry->group, entry->artifact,
                                      entry->version, sig_alt,
                                      sig_dest, jwt, result);
                if (sig_rc != 0) {
                    if (result) {
                        /* The download already recorded the HTTP status.
                         * Overwriting it with "has no .sig file" turned a
                         * 401 into a missing-file report and sent people
                         * hunting the publisher instead of their
                         * credentials — keep what actually happened. */
                        char why[256];
                        snprintf(why, sizeof(why), "%s", result->message);
                        result->code = NOW_ERR_AUTH;
                        if (why[0])
                            snprintf(result->message, sizeof(result->message),
                                     "require_signatures is set but the "
                                     "signature for %s:%s:%s could not be "
                                     "fetched (%s)",
                                     entry->group, entry->artifact,
                                     entry->version, why);
                        else
                            snprintf(result->message, sizeof(result->message),
                                     "require_signatures is set but %s:%s:%s "
                                     "has no .sig file on registry",
                                     entry->group, entry->artifact,
                                     entry->version);
                    }
                    free(sig_dest);
                    free(dep_dir);
                    now_resolver_free(&resolver);
                    now_lock_free(&lockfile);
                    free(repo_root);
                    free(lock_path);
                    registry_auth_free(reg_auth, reg_auth_n);
                    return -1;
                }

                /* Actually verify it.
                 *
                 * Downloading the .sig and checking only that the HTTP
                 * GET succeeded is not verification — any registry that
                 * serves any bytes at that URL would have passed. Look
                 * the publisher key up in the trust store and check the
                 * detached Ed25519 signature over the archive we just
                 * fetched. Fail closed: no key, or no signature match,
                 * means the dependency does not get installed. */
                {
                    char *verify_archive = now_path_join(dep_dir, archive_name);
                    NowTrustStore store;
                    now_trust_init(&store);
                    NowResult tr;
                    memset(&tr, 0, sizeof(tr));
                    now_trust_load(&store, &tr);

                    const NowTrustKey *tk =
                        now_trust_find(&store, entry->group, entry->artifact);

                    int vrc = -1;
                    const char *why = NULL;
                    if (!verify_archive) {
                        why = "cannot resolve downloaded archive path";
                    } else if (!tk || !tk->key) {
                        why = "no trusted key for this coordinate in "
                              "~/.now/trust.pasta (add one with 'now trust:add')";
                    } else {
                        NowResult vr;
                        memset(&vr, 0, sizeof(vr));
                        vrc = now_verify_file(verify_archive, sig_dest,
                                              tk->key, &vr);
                        if (vrc != 0)
                            why = vr.message[0] ? vr.message
                                                : "signature does not verify";
                    }

                    now_trust_free(&store);
                    free(verify_archive);

                    if (vrc != 0) {
                        if (result) {
                            result->code = NOW_ERR_AUTH;
                            snprintf(result->message, sizeof(result->message),
                                     "signature verification failed for "
                                     "%s:%s:%s — %s",
                                     entry->group, entry->artifact,
                                     entry->version, why ? why : "unknown");
                        }
                        free(sig_dest);
                        free(dep_dir);
                        now_resolver_free(&resolver);
                        now_lock_free(&lockfile);
                        free(repo_root);
                        free(lock_path);
                        registry_auth_free(reg_auth, reg_auth_n);
                        return -1;
                    }
                }
                free(sig_dest);
            }
        }

        /* Unpack it.
         *
         * `now_repo_install()` extracts the .basta into the layout the
         * compile path reads — <dep>/h for -I and <dep>/lib/<triple> for
         * -L — and it had no callers at all. So procure downloaded a
         * dependency, checksummed it, verified its signature, and left
         * it sitting in the repo as an opaque archive: every registry
         * dependency compiled with no include path and linked with no
         * library. Only `now install` (the local, publisher-side path)
         * ever produced that layout, which is why nobody hit it.
         *
         * After verification, never before — unpacking bytes we have not
         * checked is the one ordering that must not slip. */
        {
            char *installed = now_path_join(dep_dir, archive_name);
            if (installed && now_path_exists(installed)) {
                NowResult ir;
                memset(&ir, 0, sizeof(ir));
                if (now_repo_install(repo_root, entry->group, entry->artifact,
                                     entry->version, installed, &ir) != 0) {
                    if (result) {
                        result->code = NOW_ERR_IO;
                        snprintf(result->message, sizeof(result->message),
                                 "cannot unpack %s:%s:%s — %s",
                                 entry->group, entry->artifact,
                                 entry->version,
                                 ir.message[0] ? ir.message : "unknown");
                    }
                    free(installed);
                    free(dep_dir);
                    now_resolver_free(&resolver);
                    now_lock_free(&lockfile);
                    free(repo_root);
                    free(lock_path);
                    registry_auth_free(reg_auth, reg_auth_n);
                    return -1;
                }
            }
            free(installed);
        }

        /* Build the URL for the lock file.
         *
         * It has to name the remote file that was actually served, not
         * the local one we saved it as — those differ whenever the
         * recorded triple is not the served triple, and the lock was
         * recording a URL that 404s. */
        if (!entry->url) {
            char *gpath = group_to_path(entry->group);
            char url_buf[1024];
            char remote_name[256];
            if (served_stem[0])
                snprintf(remote_name, sizeof(remote_name), "%s.basta",
                         served_stem);
            else
                snprintf(remote_name, sizeof(remote_name), "%s", archive_name);
            snprintf(url_buf, sizeof(url_buf), "%s/artifact/%s/%s/%s/%s",
                     registry_url, gpath, entry->artifact,
                     entry->version, remote_name);
            free(gpath);
            entry->url = (char *)malloc(strlen(url_buf) + 1);
            if (entry->url) strcpy(entry->url, url_buf);
        }

        free(dep_dir);
    }

    /* Step 4: Save updated lock file — unless --locked, in which case
     * any difference from the committed one is the error, and the file
     * is left exactly as it was. */
    if (locked) {
        const char *what = NULL, *which = NULL;
        if (now_lock_differs(&lock_before, &lockfile, &what, &which)) {
            if (result) {
                result->code = NOW_ERR_SCHEMA;
                snprintf(result->message, sizeof(result->message),
                         "--locked: resolution does not match now.lock.pasta "
                         "(%s: %s). Run `now procure` without --locked and "
                         "commit the updated lockfile",
                         what, which ? which : "?");
            }
            now_resolver_free(&resolver);
            now_lock_free(&lockfile);
            now_lock_free(&lock_before);
            free(repo_root);
            free(lock_path);
            registry_auth_free(reg_auth, reg_auth_n);
            return -1;
        }
    } else if (lock_path) {
        now_lock_save(&lockfile, lock_path);
    }

    now_resolver_free(&resolver);
    now_lock_free(&lockfile);
    now_lock_free(&lock_before);
    free(repo_root);
    free(lock_path);
    registry_auth_free(reg_auth, reg_auth_n);
    return 0;
}

/* ---- dep:updates ---- */

NOW_API int now_dep_updates(const NowProject *project, const char *registry_url,
                             int verbose, NowResult *result) {
    if (!project) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "NULL project");
        return -1;
    }

    if (project->deps.count == 0) {
        if (verbose)
            fprintf(stderr, "  no dependencies declared\n");
        return 0;
    }

    /* Determine registry URL */
    const char *url = registry_url;
    if (!url && project->repos.count > 0 && project->repos.items[0].url)
        url = project->repos.items[0].url;
    if (!url) url = "http://localhost:8080";

    /* Load lock file for current versions */
    NowLockFile lockfile;
    now_lock_init(&lockfile);
    now_lock_load(&lockfile, "now.lock.pasta");

    int updates_found = 0;

    for (size_t i = 0; i < project->deps.count; i++) {
        const NowDep *dep = &project->deps.items[i];

        NowCoordinate coord;
        if (now_coord_parse(dep->id, &coord) != 0)
            continue;

        /* Find current locked version */
        const NowLockEntry *locked = now_lock_find(&lockfile,
                                                    coord.group, coord.artifact);
        const char *current_ver = locked ? locked->version : coord.version;

        /* Query registry for all versions */
        NowRegistryVersion *versions = NULL;
        int vcount = now_registry_resolve(url, coord.group, coord.artifact,
                                          "*", &versions, result);

        if (vcount <= 0) {
            if (verbose)
                fprintf(stderr, "  %s:%s — no versions found on registry\n",
                        coord.group, coord.artifact);
            now_coord_free(&coord);
            continue;
        }

        /* versions come sorted descending from registry — first is latest */
        const char *latest = versions[0].version;

        /* Compare current vs latest */
        NowSemVer sv_current, sv_latest;
        int have_current = (current_ver && now_semver_parse(current_ver, &sv_current) == 0);
        int have_latest = (latest && now_semver_parse(latest, &sv_latest) == 0);

        if (have_current && have_latest) {
            int cmp = now_semver_compare(&sv_current, &sv_latest);
            if (cmp < 0) {
                printf("  %s:%s  %s -> %s\n",
                       coord.group, coord.artifact, current_ver, latest);
                updates_found++;
            } else if (verbose) {
                printf("  %s:%s  %s (up to date)\n",
                       coord.group, coord.artifact, current_ver);
            }
            now_semver_free(&sv_current);
            now_semver_free(&sv_latest);
        } else if (have_current) {
            now_semver_free(&sv_current);
        } else if (have_latest) {
            /* No current version, show latest available */
            printf("  %s:%s  (unresolved) -> %s\n",
                   coord.group, coord.artifact, latest);
            updates_found++;
            now_semver_free(&sv_latest);
        }

        now_registry_versions_free(versions, vcount);
        now_coord_free(&coord);
    }

    now_lock_free(&lockfile);

    if (updates_found == 0 && verbose)
        printf("  all dependencies up to date\n");

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return updates_found;
}

/* ---- cache:mirror ---- */

/* Parse JSON manifest: {"artifacts":[{"group":"...","artifact":"...","version":"..."},...]} */
typedef struct {
    char *group;
    char *artifact;
    char *version;
} MirrorEntry;

static int parse_mirror_manifest(const char *json, size_t len,
                                  MirrorEntry **out, int *count) {
    *out = NULL;
    *count = 0;

    const char *arr = strstr(json, "\"artifacts\":[");
    if (!arr) return -1;
    arr = strchr(arr, '[');
    if (!arr) return -1;
    arr++;

    /* Count entries */
    int n = 0;
    int depth = 0;
    for (const char *p = arr; *p && !(*p == ']' && depth == 0); p++) {
        if (*p == '{') { if (depth == 0) n++; depth++; }
        else if (*p == '}') depth--;
    }

    if (n == 0) return 0;

    MirrorEntry *entries = (MirrorEntry *)calloc((size_t)n, sizeof(MirrorEntry));
    if (!entries) return -1;

    const char *p = arr;
    for (int i = 0; i < n; i++) {
        p = strchr(p, '{');
        if (!p) break;
        p++;

        /* Parse "group":"..." */
        const char *gkey = strstr(p, "\"group\":\"");
        if (gkey) {
            gkey += 9;
            const char *gend = strchr(gkey, '"');
            if (gend) {
                size_t glen = (size_t)(gend - gkey);
                entries[i].group = (char *)malloc(glen + 1);
                if (entries[i].group) { memcpy(entries[i].group, gkey, glen); entries[i].group[glen] = '\0'; }
            }
        }

        /* Parse "artifact":"..." */
        const char *akey = strstr(p, "\"artifact\":\"");
        if (akey) {
            akey += 12;
            const char *aend = strchr(akey, '"');
            if (aend) {
                size_t alen = (size_t)(aend - akey);
                entries[i].artifact = (char *)malloc(alen + 1);
                if (entries[i].artifact) { memcpy(entries[i].artifact, akey, alen); entries[i].artifact[alen] = '\0'; }
            }
        }

        /* Parse "version":"..." */
        const char *vkey = strstr(p, "\"version\":\"");
        if (vkey) {
            vkey += 11;
            const char *vend = strchr(vkey, '"');
            if (vend) {
                size_t vlen = (size_t)(vend - vkey);
                entries[i].version = (char *)malloc(vlen + 1);
                if (entries[i].version) { memcpy(entries[i].version, vkey, vlen); entries[i].version[vlen] = '\0'; }
            }
        }

        p = strchr(p, '}');
        if (p) p++;
    }

    *out = entries;
    *count = n;
    return 0;
}

static void mirror_entries_free(MirrorEntry *entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free(entries[i].group);
        free(entries[i].artifact);
        free(entries[i].version);
    }
    free(entries);
}

NOW_API int now_cache_mirror(const char *registry_url, const char *coords,
                              int verbose, NowResult *result) {
    if (!registry_url) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "registry URL required for cache:mirror");
        }
        return -1;
    }

    /* Parse registry URL */
    char *host = NULL;
    char *base_path = NULL;
    int port = 80, tls = 0;
    if (pico_http_parse_url_ex(registry_url, &host, &port, &base_path, &tls) != 0) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Invalid registry URL: %s", registry_url);
        return -1;
    }

    char *prefix = base_path ? base_path : strdup("");
    size_t plen = strlen(prefix);
    while (plen > 0 && prefix[plen - 1] == '/') prefix[--plen] = '\0';

    /* Authenticate */
    NowCredentials creds;
    char *jwt = NULL;
    if (now_auth_load(registry_url, &creds) == 0) {
        if (creds.username)
            now_auth_login(host, port, prefix, &creds, tls, &jwt, result);
        if (!jwt && creds.token)
            jwt = strdup(creds.token);
        now_auth_creds_free(&creds);
    }

    /* Build mirror manifest URL */
    char path[2048];
    if (coords && *coords)
        snprintf(path, sizeof(path), "%s/mirror/manifest?coords=%s", prefix, coords);
    else
        snprintf(path, sizeof(path), "%s/mirror/manifest", prefix);

    /* Build headers */
    PicoHttpHeader headers[2];
    int nhdr = 0;
    char auth_buf[1024];
    if (jwt && *jwt) {
        snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", jwt);
        headers[nhdr].name  = "Authorization";
        headers[nhdr].value = auth_buf;
        nhdr++;
    }

    PicoHttpOptions opts = {0};
    opts.headers = headers;
    opts.header_count = (size_t)nhdr;
    opts.tls = tls ? 1 : -1;   /* scheme from the registry URL, not the port */

    /* Fetch manifest */
    PicoHttpResponse res;
    int rc = pico_http_get(host, port, path, &opts, &res);
    free(host);
    free(prefix);

    if (rc != PICO_OK) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "mirror manifest fetch failed: %s", pico_http_strerror(rc));
        }
        free(jwt);
        return -1;
    }

    if (res.status != 200) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "mirror manifest: registry returned HTTP %d", res.status);
        }
        pico_http_response_free(&res);
        free(jwt);
        return -1;
    }

    /* Parse manifest */
    MirrorEntry *entries = NULL;
    int entry_count = 0;
    if (parse_mirror_manifest(res.body, res.body_len, &entries, &entry_count) != 0) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "failed to parse mirror manifest");
        }
        pico_http_response_free(&res);
        free(jwt);
        return -1;
    }
    pico_http_response_free(&res);

    /* Determine local repo root */
    char *repo_root = default_repo_root();
    if (!repo_root) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "Cannot determine repo root");
        mirror_entries_free(entries, entry_count);
        free(jwt);
        return -1;
    }

    /* Download missing artifacts */
    int downloaded = 0;
    int skipped = 0;

    for (int i = 0; i < entry_count; i++) {
        MirrorEntry *e = &entries[i];
        if (!e->group || !e->artifact || !e->version) continue;

        if (now_repo_is_installed(repo_root, e->group, e->artifact, e->version)) {
            skipped++;
            if (verbose)
                fprintf(stderr, "  %s:%s:%s — already cached\n",
                        e->group, e->artifact, e->version);
            continue;
        }

        /* Create dep directory */
        char *dep_dir = now_repo_dep_path(repo_root, e->group,
                                           e->artifact, e->version);
        if (!dep_dir) continue;
        now_mkdir_p(dep_dir);

        /* Download descriptor */
        char *desc_dest = now_path_join(dep_dir, "now.pasta");
        if (desc_dest) {
            now_registry_download(registry_url, e->group, e->artifact,
                                  e->version, "now.pasta",
                                  desc_dest, jwt, result);
            free(desc_dest);
        }

        /* Download archive (.basta) */
        char archive_name[256];
        snprintf(archive_name, sizeof(archive_name), "%s-%s.basta",
                 e->artifact, e->version);
        char *archive_dest = now_path_join(dep_dir, archive_name);
        if (archive_dest) {
            rc = now_registry_download(registry_url, e->group, e->artifact,
                                       e->version, archive_name,
                                       archive_dest, jwt, result);
            if (rc == 0) {
                downloaded++;
                if (verbose)
                    fprintf(stderr, "  %s:%s:%s — downloaded\n",
                            e->group, e->artifact, e->version);
            }
            free(archive_dest);
        }

        /* Download .sha256 sidecar */
        char sha_name[256];
        snprintf(sha_name, sizeof(sha_name), "%s-%s.sha256",
                 e->artifact, e->version);
        char *sha_dest = now_path_join(dep_dir, sha_name);
        if (sha_dest) {
            now_registry_download(registry_url, e->group, e->artifact,
                                  e->version, sha_name,
                                  sha_dest, jwt, result);
            free(sha_dest);
        }

        free(dep_dir);
    }

    mirror_entries_free(entries, entry_count);
    free(repo_root);
    free(jwt);

    if (verbose)
        fprintf(stderr, "  mirror complete: %d downloaded, %d already cached\n",
                downloaded, skipped);

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return downloaded;
}
