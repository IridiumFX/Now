/*
 * now_fs.c — Filesystem utilities
 */
#include "now_fs.h"
#include "now_pom.h"   /* NowArchDict, now_arch_dict_is_gate */
#include "now_arch.h"  /* NowTagSet, now_tagset_has */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>

/* MinGW PATH_MAX = 260 (MAX_PATH); Linux/macOS = 4096. We need ≥ 4096 to
 * satisfy glibc fortify's __realpath_chk and to avoid Windows-side truncation
 * when paths use the \\?\ extended syntax. */
#if !defined(PATH_MAX) || PATH_MAX < 4096
  #undef PATH_MAX
  #define PATH_MAX 4096
#endif

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
  #define mkdir_one(p) _mkdir(p)
  #ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
  #endif
#else
  #include <dirent.h>
  #include <unistd.h>
  #define mkdir_one(p) mkdir(p, 0755)
#endif

/* ---- Path utilities ---- */

NOW_API const char *now_home_dir(void) {
    const char *home = NULL;
#ifdef _WIN32
    /* USERPROFILE first: it is the native Windows home and the one the
     * majority of call sites already used. Git Bash also sets HOME, and
     * when the two disagree the split is invisible until something
     * written under one home is read from the other. */
    home = getenv("USERPROFILE");
    if (!home || !*home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    return (home && *home) ? home : NULL;
}

NOW_API char *now_path_join(const char *a, const char *b) {
    if (!a || !*a) return b ? strdup(b) : NULL;
    if (!b || !*b) return strdup(a);

    /* If b is already absolute, return it unchanged — don't double-prefix
     * with a. Handles POSIX leading-slash and Windows drive-letter forms
     * ("C:\..." or "C:/..."). */
    int b_abs = (b[0] == '/' || b[0] == '\\' ||
                 (((b[0] >= 'A' && b[0] <= 'Z') ||
                   (b[0] >= 'a' && b[0] <= 'z')) && b[1] == ':'));
    if (b_abs) return strdup(b);

    size_t la = strlen(a);
    size_t lb = strlen(b);

    /* Check if a already ends with separator */
    int has_sep = (a[la - 1] == '/' || a[la - 1] == '\\');

    size_t total = la + lb + (has_sep ? 1 : 2);
    char *out = malloc(total);
    if (!out) return NULL;

    if (has_sep)
        snprintf(out, total, "%s%s", a, b);
    else
        snprintf(out, total, "%s/%s", a, b);

    return out;
}

NOW_API const char *now_path_ext(const char *path) {
    if (!path) return "";
    const char *dot = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '.') dot = p;
        else if (*p == '/' || *p == '\\') dot = NULL;
    }
    return dot ? dot : "";
}

NOW_API const char *now_path_basename(const char *path) {
    if (!path) return "";
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

NOW_API int now_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

NOW_API int now_is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

NOW_API int now_mkdir_p(const char *path) {
    if (!path || !*path) return -1;

    char *tmp = strdup(path);
    if (!tmp) return -1;

    /* Normalize separators to / */
    for (char *p = tmp; *p; p++) {
        if (*p == '\\') *p = '/';
    }

    /* Skip drive letter on Windows (e.g. "C:/") */
    char *start = tmp;
#ifdef _WIN32
    if (start[0] && start[1] == ':' && start[2] == '/')
        start += 3;
    else
        start += 1;
#else
    start += 1;
#endif

    /* Create each component */
    for (char *p = start; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!now_path_exists(tmp))
                mkdir_one(tmp);  /* ignore errors on intermediate dirs */
            *p = '/';
        }
    }
    if (!now_path_exists(tmp)) {
        if (mkdir_one(tmp) != 0) { free(tmp); return -1; }
    }

    free(tmp);
    return 0;
}

/* ---- Object path derivation (§3.2) ---- */

NOW_API char *now_obj_path_ex(const char *basedir, const char *src_path,
                      const char *src_root, const char *target,
                      const char *obj_ext) {
    /*
     * src/main/c/net/parser.c with src_root="src/main/c"
     * → relative = "net/parser.c"
     * → target/obj/main/net/parser.c.o   (or .obj for MSVC)
     */
    const char *relative = src_path;

    /* Strip src_root prefix if present */
    if (src_root) {
        size_t root_len = strlen(src_root);
        if (strncmp(src_path, src_root, root_len) == 0) {
            relative = src_path + root_len;
            while (*relative == '/' || *relative == '\\') relative++;
        }
    }

    /* Build: basedir/target/obj/main/<relative><obj_ext> */
    char *target_dir = now_path_join(basedir, target);
    char *obj_dir = now_path_join(target_dir, "obj/main");
    free(target_dir);

    char *rel_copy = strdup(relative);
    if (!rel_copy) { free(obj_dir); return NULL; }

    char *obj_base = now_path_join(obj_dir, rel_copy);
    free(rel_copy);
    free(obj_dir);

    size_t base_len = strlen(obj_base);
    size_t ext_len = strlen(obj_ext);
    char *result = malloc(base_len + ext_len + 1);
    if (!result) { free(obj_base); return NULL; }
    memcpy(result, obj_base, base_len);
    memcpy(result + base_len, obj_ext, ext_len + 1);
    free(obj_base);

    return result;
}

NOW_API char *now_obj_path(const char *basedir, const char *src_path,
                   const char *src_root, const char *target) {
    return now_obj_path_ex(basedir, src_path, src_root, target, ".o");
}

/* ---- File list ---- */

NOW_API void now_filelist_init(NowFileList *fl) {
    fl->paths = NULL;
    fl->count = 0;
    fl->capacity = 0;
}

NOW_API int now_filelist_push(NowFileList *fl, const char *path) {
    if (fl->count >= fl->capacity) {
        size_t new_cap = fl->capacity ? fl->capacity * 2 : 16;
        char **tmp = realloc(fl->paths, new_cap * sizeof(char *));
        if (!tmp) return -1;
        fl->paths = tmp;
        fl->capacity = new_cap;
    }
    fl->paths[fl->count] = strdup(path);
    if (!fl->paths[fl->count]) return -1;
    fl->count++;
    return 0;
}

NOW_API void now_filelist_free(NowFileList *fl) {
    for (size_t i = 0; i < fl->count; i++)
        free(fl->paths[i]);
    free(fl->paths);
    now_filelist_init(fl);
}

/* ---- Glob matching (spec §26) ---- */

/* Core recursive matcher over a single slash-separated string.
 * Star, question-mark and char classes never cross a slash; ** does. */
static int glob_core(const char *p, const char *s) {
    while (*p) {
        if (p[0] == '*' && p[1] == '*') {
            /* double-star matches any run including slashes. Collapse a
             * run of stars, then match the remainder at every position. */
            while (*p == '*') p++;
            if (*p == '\0') return 1;          /* trailing star: matches rest */
            if (*p == '/') {
                /* a double-star followed by slash may also match zero
                 * segments (so star-star-slash-foo.c matches foo.c):
                 * try skipping the slash here. */
                if (glob_core(p + 1, s)) return 1;
            }
            for (const char *t = s; ; t++) {
                if (glob_core(p, t)) return 1;
                if (!*t) break;
            }
            return 0;
        } else if (*p == '*') {
            /* single star: any run except a slash */
            for (const char *t = s; ; t++) {
                if (glob_core(p + 1, t)) return 1;
                if (!*t || *t == '/') break;
            }
            return 0;
        } else if (*p == '?') {
            if (!*s || *s == '/') return 0;
            p++; s++;
        } else if (*p == '[') {
            /* character class */
            if (!*s || *s == '/') return 0;
            const char *q = p + 1;
            int neg = 0;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            int matched = 0;
            while (*q && *q != ']') {
                if (*q == '\\' && q[1]) {
                    if (*s == q[1]) matched = 1;
                    q += 2;
                } else if (q[1] == '-' && q[2] && q[2] != ']') {
                    unsigned char lo = (unsigned char)q[0];
                    unsigned char hi = (unsigned char)q[2];
                    unsigned char c  = (unsigned char)*s;
                    if (c >= lo && c <= hi) matched = 1;
                    q += 3;
                } else {
                    if (*s == *q) matched = 1;
                    q++;
                }
            }
            if (*q == ']') q++;
            if (matched == neg) return 0;
            p = q; s++;
        } else if (*p == '\\' && p[1]) {
            /* escape — next pattern char is literal */
            p++;
            if (*p != *s) return 0;
            p++; s++;
        } else {
            if (*p != *s) return 0;
            p++; s++;
        }
    }
    return *s == '\0';
}

NOW_API int now_glob_match(const char *pattern, const char *path) {
    if (!pattern || !path) return 0;

    /* Normalize separators in the path (patterns keep '\' as escape). */
    char normbuf[PATH_MAX];
    char *norm = normbuf;
    size_t len = strlen(path);
    if (len + 1 > sizeof(normbuf)) {
        norm = (char *)malloc(len + 1);
        if (!norm) return 0;
    }
    for (size_t i = 0; i <= len; i++)
        norm[i] = (path[i] == '\\') ? '/' : path[i];

    /* §26.1: a pattern without '/' matches the basename only. */
    const char *target = norm;
    if (!strchr(pattern, '/')) {
        const char *slash = strrchr(norm, '/');
        if (slash) target = slash + 1;
    }

    int rc = glob_core(pattern, target);
    if (norm != normbuf) free(norm);
    return rc;
}

/* ---- Source discovery ---- */

static int ext_matches(const char *path, const char **exts) {
    const char *ext = now_path_ext(path);
    for (const char **e = exts; *e; e++) {
        if (strcmp(ext, *e) == 0) return 1;
    }
    return 0;
}

#include "now_dirwalk.h"

/* Canonicalize a path for use as cache key. Falls back to the input on error. */
static void canonicalize_into(char *out, size_t outcap, const char *path) {
#ifdef _WIN32
    if (!_fullpath(out, path, outcap)) {
        strncpy(out, path, outcap - 1);
        out[outcap - 1] = '\0';
    }
#else
    if (!realpath(path, out)) {
        strncpy(out, path, outcap - 1);
        out[outcap - 1] = '\0';
    }
#endif
}

/* High-precision modification time. Units differ per platform (Windows:
 * 100ns ticks since 1601; POSIX: ns since 1970) but only same-platform
 * values are compared, so the units don't need to match across OSes.
 * Returns -1 on error. */
static long long dir_mtime_hires(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return -1;
    ULARGE_INTEGER ts;
    ts.LowPart  = data.ftLastWriteTime.dwLowDateTime;
    ts.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return (long long)ts.QuadPart;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
  #if defined(__APPLE__)
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL
         + (long long)st.st_mtimespec.tv_nsec;
  #elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL
         + (long long)st.st_mtimespec.tv_nsec;
  #else
    return (long long)st.st_mtim.tv_sec * 1000000000LL
         + (long long)st.st_mtim.tv_nsec;
  #endif
#endif
}

/* When dict + active are non-NULL, subdirs whose basename (after
 * alias resolution) is in `dict->tags` are platform-gated: descended
 * only if the tag is also in `active`. Subdirs not in the dict are
 * traversed unconditionally. */
static int dir_gate_passes(const NowArchDict *dict, const NowTagSet *active,
                            const char *name) {
    if (!dict || !active) return 1;
    if (!now_arch_dict_is_gate(dict, name)) return 1;
    const char *canon = now_arch_dict_resolve(dict, name);
    return now_tagset_has(active, canon);
}

/* Walk a directory using the global dirwalk cache if available.
 * Writes matching source files to `out` (recursively through subdirs).
 * Updates the cache on miss/mtime mismatch.
 *
 * dict + active are the optional platform-gate context. Passing
 * NULL/NULL disables gating (every subdir is descended). */
static int discover_recursive(const char *basedir, const char *rel_dir,
                               const char **exts,
                               const NowArchDict *dict,
                               const NowTagSet *active,
                               NowFileList *out) {
    char *abs_dir;
    if (rel_dir && *rel_dir)
        abs_dir = now_path_join(basedir, rel_dir);
    else
        abs_dir = strdup(basedir);
    if (!abs_dir) return 0;

    /* Get current mtime (high-precision) */
    long long cur_mtime = dir_mtime_hires(abs_dir);

    /* Try cache. Buffer must be PATH_MAX — glibc fortify's __realpath_chk
     * aborts at compile time if the destination is smaller. */
    char canon[PATH_MAX];
    canonicalize_into(canon, sizeof(canon), abs_dir);

    const NowDirCacheEntry *cached =
        now_dirwalk_cache_global
            ? now_dirwalk_get(now_dirwalk_cache_global, canon, cur_mtime)
            : NULL;

    if (cached) {
        /* Replay from cache — no readdir */
        for (size_t i = 0; i < cached->count; i++) {
            const char *name = cached->entries[i];
            char *rel_path = (rel_dir && *rel_dir)
                ? now_path_join(rel_dir, name)
                : strdup(name);
            if (!rel_path) continue;
            if (cached->is_dir[i]) {
                if (dir_gate_passes(dict, active, name))
                    discover_recursive(basedir, rel_path, exts,
                                       dict, active, out);
            } else if (ext_matches(name, exts)) {
                now_filelist_push(out, rel_path);
            }
            free(rel_path);
        }
        free(abs_dir);
        return 0;
    }

    /* Miss — full readdir, collect entries for both traversal and cache */
    char **entries_list = NULL;
    int   *isdir_list   = NULL;
    size_t entries_count = 0, entries_cap = 0;

#ifdef _WIN32
    char *pattern = now_path_join(abs_dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE hFind = pattern ? FindFirstFileA(pattern, &fd) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (hFind == INVALID_HANDLE_VALUE) { free(abs_dir); return 0; }
    do {
        if (fd.cFileName[0] == '.') continue;
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        if (entries_count >= entries_cap) {
            size_t newcap = entries_cap ? entries_cap * 2 : 16;
            char **ne = (char **)realloc(entries_list, newcap * sizeof(char *));
            int   *id = (int *)realloc(isdir_list,   newcap * sizeof(int));
            if (!ne || !id) { free(ne ? ne : entries_list); free(id ? id : isdir_list); FindClose(hFind); free(abs_dir); return 0; }
            entries_list = ne; isdir_list = id; entries_cap = newcap;
        }
        entries_list[entries_count] = strdup(fd.cFileName);
        isdir_list[entries_count]   = is_dir;
        entries_count++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
#else
    DIR *d = opendir(abs_dir);
    if (!d) { free(abs_dir); return 0; }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char *full = now_path_join(abs_dir, entry->d_name);
        int is_dir = full ? now_is_dir(full) : 0;
        free(full);
        if (entries_count >= entries_cap) {
            size_t newcap = entries_cap ? entries_cap * 2 : 16;
            char **ne = (char **)realloc(entries_list, newcap * sizeof(char *));
            int   *id = (int *)realloc(isdir_list,   newcap * sizeof(int));
            if (!ne || !id) { free(ne ? ne : entries_list); free(id ? id : isdir_list); closedir(d); free(abs_dir); return 0; }
            entries_list = ne; isdir_list = id; entries_cap = newcap;
        }
        entries_list[entries_count] = strdup(entry->d_name);
        isdir_list[entries_count]   = is_dir;
        entries_count++;
    }
    closedir(d);
#endif
    free(abs_dir);

    /* Traverse using the collected list */
    for (size_t i = 0; i < entries_count; i++) {
        char *rel_path = (rel_dir && *rel_dir)
            ? now_path_join(rel_dir, entries_list[i])
            : strdup(entries_list[i]);
        if (!rel_path) continue;
        if (isdir_list[i]) {
            if (dir_gate_passes(dict, active, entries_list[i]))
                discover_recursive(basedir, rel_path, exts,
                                   dict, active, out);
        } else if (ext_matches(entries_list[i], exts)) {
            now_filelist_push(out, rel_path);
        }
        free(rel_path);
    }

    /* Update cache — transfers ownership of entries_list[] and isdir_list[] */
    if (now_dirwalk_cache_global && cur_mtime >= 0) {
        now_dirwalk_put(now_dirwalk_cache_global, canon, cur_mtime,
                         entries_list, isdir_list, entries_count);
    } else {
        for (size_t i = 0; i < entries_count; i++) free(entries_list[i]);
        free(entries_list);
        free(isdir_list);
    }
    return 0;
}

NOW_API int now_discover_sources(const char *basedir, const char *dir,
                         const char **exts, NowFileList *out) {
    /* dir is relative to basedir */
    char *full = now_path_join(basedir, dir);
    if (!now_is_dir(full)) { free(full); return -1; }
    free(full);
    return discover_recursive(basedir, dir, exts, NULL, NULL, out);
}

NOW_API int now_discover_sources_filtered(const char *basedir, const char *dir,
                                           const char **exts,
                                           const NowProject *project,
                                           const NowTagSet *active,
                                           NowFileList *out) {
    char *full = now_path_join(basedir, dir);
    if (!now_is_dir(full)) { free(full); return -1; }
    free(full);
    const NowArchDict *dict = NULL;
    /* No gating if either the project has no dict or no active set
     * was supplied — falls back to the unfiltered walk. */
    if (project && project->arch.tags.count > 0 && active)
        dict = &project->arch;
    return discover_recursive(basedir, dir, exts, dict, active, out);
}

NOW_API int now_file_copy(const char *src, const char *dst) {
    if (!src || !dst) return -1;
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    char buf[8192];
    size_t n;
    int err = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { err = -1; break; }
    }

    fclose(out);
    fclose(in);
    return err;
}

/* ---- Per-build stat() memoization (open-addressed hash table) ---- */

NOW_API void now_stat_cache_init(NowStatCache *c) {
    c->table = NULL;
    c->capacity = 0;
    c->count = 0;
}

NOW_API void now_stat_cache_free(NowStatCache *c) {
    if (!c) return;
    for (size_t i = 0; i < c->capacity; i++)
        free(c->table[i].path);
    free(c->table);
    c->table = NULL;
    c->capacity = c->count = 0;
}

static uint32_t stat_hash_fnv1a(const char *s) {
    uint32_t h = 0x811c9dc5u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193u;
    }
    return h;
}

/* Find the slot for `path` — either holding it, or the first empty
 * slot in its probe chain. Caller checks slot->path to distinguish. */
static NowStatEntry *stat_probe(NowStatEntry *table, size_t mask,
                                  const char *path, uint32_t hash) {
    size_t i = hash & mask;
    for (;;) {
        NowStatEntry *e = &table[i];
        if (!e->path || strcmp(e->path, path) == 0) return e;
        i = (i + 1) & mask;
    }
}

static int stat_grow(NowStatCache *c) {
    size_t old_cap = c->capacity;
    size_t new_cap = old_cap ? old_cap * 2 : 64;
    NowStatEntry *new_table = calloc(new_cap, sizeof(NowStatEntry));
    if (!new_table) return -1;
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < old_cap; i++) {
        NowStatEntry *src = &c->table[i];
        if (!src->path) continue;
        NowStatEntry *dst = stat_probe(new_table, mask, src->path,
                                         stat_hash_fnv1a(src->path));
        *dst = *src;
    }
    free(c->table);
    c->table = new_table;
    c->capacity = new_cap;
    return 0;
}

NOW_API int now_stat_cached(NowStatCache *cache, const char *path,
                             long long *mtime_out) {
    if (!path) return 0;

    /* No cache → direct stat */
    if (!cache) {
        struct stat st;
        if (stat(path, &st) != 0) return 0;
        if (mtime_out) *mtime_out = (long long)st.st_mtime;
        return 1;
    }

    /* Grow when load factor > 0.7 (count + 1 > cap * 7 / 10) */
    if ((cache->count + 1) * 10 > cache->capacity * 7) {
        if (stat_grow(cache) != 0) {
            /* OOM — fall back to uncached stat */
            struct stat st;
            if (stat(path, &st) != 0) return 0;
            if (mtime_out) *mtime_out = (long long)st.st_mtime;
            return 1;
        }
    }

    uint32_t h = stat_hash_fnv1a(path);
    size_t mask = cache->capacity - 1;
    NowStatEntry *slot = stat_probe(cache->table, mask, path, h);
    if (slot->path) {
        /* Hit */
        if (mtime_out && slot->exists) *mtime_out = slot->mtime;
        return slot->exists;
    }

    /* Miss — stat and insert */
    struct stat st;
    int exists = (stat(path, &st) == 0);
    long long mt = exists ? (long long)st.st_mtime : 0;
    slot->path = strdup(path);
    slot->exists = exists;
    slot->mtime = mt;
    cache->count++;

    if (mtime_out && exists) *mtime_out = mt;
    return exists;
}
