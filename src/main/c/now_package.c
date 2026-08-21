/*
 * now_package.c — Package, install, and publish phases
 *
 * Assembles build output into a tarball, installs to local repo,
 * publishes to a remote registry.
 */

#include "now_package.h"
#include "now_pom.h"
#include "now_fs.h"
#include "now_build.h"
#include "now_manifest.h"
#include "now_procure.h"
#include "now_auth.h"
#include "now_trust.h"
#include "now_events.h"
#include "pico_http.h"
#include "pasta.h"
#include "basta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <direct.h>
  #define mkdir_compat(p) _mkdir(p)
#else
  #include <sys/stat.h>
  #include <pwd.h>
  #include <unistd.h>
  #define mkdir_compat(p) mkdir((p), 0755)
#endif

/* ---- Host triple ---- */

NOW_API const char *now_host_triple(void) {
#if defined(_WIN32)
    /* Distinguish MSVC (cl.exe) vs MinGW (gcc/clang via mingw) — both are
     * "Windows" but the resulting binaries are not ABI-compatible.
     * Compile-time defines:
     *   _MSC_VER             — set by cl.exe (and clang-cl)
     *   __MINGW32/64__       — set by MinGW gcc and clang targeting mingw
     * MSVC takes precedence — if both happen to be defined, that's a
     * clang-cl build and -msvc is correct. */
  #if defined(_M_X64) || defined(__x86_64__)
    #if defined(_MSC_VER)
      return "windows-x86_64-msvc";
    #elif defined(__MINGW32__) || defined(__MINGW64__)
      return "windows-x86_64-gnu";
    #else
      return "windows-x86_64-msvc";
    #endif
  #elif defined(_M_ARM64) || defined(__aarch64__)
    #if defined(_MSC_VER)
      return "windows-aarch64-msvc";
    #elif defined(__MINGW32__) || defined(__MINGW64__)
      return "windows-aarch64-gnu";
    #else
      return "windows-aarch64-msvc";
    #endif
  #else
    #if defined(_MSC_VER)
      return "windows-x86-msvc";
    #elif defined(__MINGW32__) || defined(__MINGW64__)
      return "windows-x86-gnu";
    #else
      return "windows-x86-msvc";
    #endif
  #endif
#elif defined(__APPLE__)
  #if defined(__aarch64__)
    return "macos-aarch64-none";
  #else
    return "macos-x86_64-none";
  #endif
#elif defined(__linux__)
  #if defined(__x86_64__)
    return "linux-x86_64-gnu";
  #elif defined(__aarch64__)
    return "linux-aarch64-gnu";
  #else
    return "linux-x86-gnu";
  #endif
#elif defined(__FreeBSD__)
  #if defined(__x86_64__)
    return "freebsd-x86_64-none";
  #elif defined(__aarch64__)
    return "freebsd-aarch64-none";
  #else
    return "freebsd-x86-none";
  #endif
#else
    return "unknown-unknown-none";
#endif
}

/* Copy a single file from src to dst. Returns 0 on success. */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);

    fclose(out);
    fclose(in);
    return 0;
}

/* Copy all files with given extensions from src_dir into dst_dir,
 * preserving subdirectory structure. */
static int copy_tree(const char *basedir, const char *rel_dir,
                     const char *dst_dir, const char **exts) {
    NowFileList files;
    now_filelist_init(&files);

    if (now_discover_sources(basedir, rel_dir, exts, &files) != 0) {
        now_filelist_free(&files);
        return -1;
    }

    for (size_t i = 0; i < files.count; i++) {
        const char *rel = files.paths[i];
        /* Strip the rel_dir prefix to get the sub-path */
        const char *sub = rel;
        size_t dir_len = strlen(rel_dir);
        if (strncmp(rel, rel_dir, dir_len) == 0) {
            sub = rel + dir_len;
            if (*sub == '/' || *sub == '\\') sub++;
        }

        char *dst_file = now_path_join(dst_dir, sub);
        if (!dst_file) continue;

        /* Create parent directory */
        char *parent = strdup(dst_file);
        if (parent) {
            char *sep = strrchr(parent, '/');
            if (!sep) sep = strrchr(parent, '\\');
            if (sep) {
                *sep = '\0';
                now_mkdir_p(parent);
            }
            free(parent);
        }

        char *src_full = now_path_join(basedir, rel);
        if (src_full) {
            copy_file(src_full, dst_file);
            free(src_full);
        }
        free(dst_file);
    }

    now_filelist_free(&files);
    return 0;
}

/* Read an entire file into a malloc'd buffer. Sets *out_len. */
static char *pkg_read_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    *out_len = n;
    return buf;
}

/* Add a file as a named blob entry in a Basta map.
 * Key is the relative filename (e.g. "mylib.h").
 * Returns 0 on success, -1 on failure (file not found is not an error). */
static int add_file_blob(BastaValue *map, const char *key,
                         const char *filepath) {
    size_t len;
    char *data = pkg_read_file(filepath, &len);
    if (!data) return 0;  /* file not found → skip */
    BastaValue *blob = basta_new_blob((const uint8_t *)data, len);
    free(data);
    if (!blob) return -1;
    basta_set(map, key, blob);
    return 0;
}

/* Collect header files into a Basta map: { "name.h": <blob>, ... } */
static BastaValue *collect_headers(const char *basedir, const char *hdr_dir) {
    BastaValue *hmap = basta_new_map();
    if (!hmap) return NULL;

    NowFileList files;
    now_filelist_init(&files);
    const char *hdr_exts[] = {".h", ".hpp", ".hxx", ".hh", ".H", NULL};

    if (now_discover_sources(basedir, hdr_dir, hdr_exts, &files) != 0) {
        now_filelist_free(&files);
        return hmap;  /* empty map is fine */
    }

    size_t dir_len = strlen(hdr_dir);
    for (size_t i = 0; i < files.count; i++) {
        const char *rel = files.paths[i];
        /* Strip hdr_dir prefix to get sub-path */
        const char *sub = rel;
        if (strncmp(rel, hdr_dir, dir_len) == 0) {
            sub = rel + dir_len;
            if (*sub == '/' || *sub == '\\') sub++;
        }
        char *full = now_path_join(basedir, rel);
        if (full) {
            add_file_blob(hmap, sub, full);
            free(full);
        }
    }

    now_filelist_free(&files);
    return hmap;
}

/* ---- Package phase ---- */

/* Phase wrappers, the same shape as now_build_link()'s and for the same
 * reason: these bodies return from a dozen places each, and a
 * `phase.started` with no matching `phase.finished` reads to a listener
 * exactly like a phase still running. Bracketing the call rather than
 * hunting every return is what makes the pairing a guarantee instead of
 * a habit. */
static int package_body(const NowProject *project, const char *basedir,
                        int verbose, NowResult *result);

NOW_API int now_package(const NowProject *project, const char *basedir,
                        int verbose, NowResult *result) {
    int rc;
    now_events_phase_started("package");
    rc = package_body(project, basedir, verbose, result);
    now_events_phase_finished("package", rc == 0, NULL);
    return rc;
}

static int package_body(const NowProject *project, const char *basedir,
                        int verbose, NowResult *result) {
    if (!project || !basedir) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message), "NULL project or basedir");
        }
        return -1;
    }

    const char *group    = project->group    ? project->group    : "unknown";
    const char *artifact = project->artifact ? project->artifact : "unnamed";
    const char *version  = project->version  ? project->version  : "0.0.0";
    const char *triple   = now_host_triple();
    const char *out_type = project->output.type ? project->output.type : "executable";
    int is_header_only = (strcmp(out_type, "header-only") == 0);

    /* Build the root Basta document (sections map) */
    BastaValue *root = basta_new_map();
    if (!root) return -1;

    /* @metadata section */
    {
        BastaValue *meta = basta_new_map();
        basta_set(meta, "format", basta_new_string("basta/1"));
        basta_set(meta, "group",    basta_new_string(group));
        basta_set(meta, "artifact", basta_new_string(artifact));
        basta_set(meta, "version",  basta_new_string(version));
        basta_set(meta, "triple",   basta_new_string(is_header_only ? "noarch" : triple));
        basta_set(meta, "output_type", basta_new_string(out_type));
        if (project->output.name)
            basta_set(meta, "output_name", basta_new_string(project->output.name));
        basta_set(root, "metadata", meta);
    }

    /* @descriptor section: embed now.pasta as a blob */
    {
        char *desc_path = now_path_join(basedir, "now.pasta");
        if (desc_path) {
            BastaValue *desc_sec = basta_new_map();
            add_file_blob(desc_sec, "now.pasta", desc_path);
            /* Lock file if present */
            char *lock_path = now_path_join(basedir, "now.lock.pasta");
            if (lock_path) {
                add_file_blob(desc_sec, "now.lock.pasta", lock_path);
                free(lock_path);
            }
            basta_set(root, "descriptor", desc_sec);
            free(desc_path);
        }
    }

    /* @headers section: embed header files as named blobs */
    if (project->sources.headers) {
        BastaValue *hdrs = collect_headers(basedir, project->sources.headers);
        if (hdrs)
            basta_set(root, "headers", hdrs);
    }

    /* @files section: embed build outputs as named blobs */
    {
        const char *out_name = project->output.name
                               ? project->output.name : artifact;
        char out_file[256];
        if (strcmp(out_type, "static") == 0) {
#ifdef _WIN32
            /* "Windows" is not "MSVC". The bundled toolchain is MinGW
             * gcc/ar and links lib<name>.a, so naming the artifact
             * <name>.lib asked for a file the linker never wrote — and
             * the whole @files section came out empty while `now
             * package` still reported success. Take whichever the
             * toolchain actually produced. */
            snprintf(out_file, sizeof(out_file), "lib%s.a", out_name);
            {
                char *probe_dir = now_path_join(basedir, "target/bin");
                if (probe_dir) {
                    char *probe = now_path_join(probe_dir, out_file);
                    if (probe && !now_path_exists(probe))
                        snprintf(out_file, sizeof(out_file), "%s.lib", out_name);
                    free(probe);
                    free(probe_dir);
                }
            }
#else
            snprintf(out_file, sizeof(out_file), "lib%s.a", out_name);
#endif
        } else if (strcmp(out_type, "shared") == 0) {
#ifdef _WIN32
            snprintf(out_file, sizeof(out_file), "%s.dll", out_name);
#elif defined(__APPLE__)
            snprintf(out_file, sizeof(out_file), "lib%s.dylib", out_name);
#else
            snprintf(out_file, sizeof(out_file), "lib%s.so", out_name);
#endif
        } else {
            /* executable or Java jar */
#ifdef _WIN32
            snprintf(out_file, sizeof(out_file), "%s.exe", out_name);
#else
            snprintf(out_file, sizeof(out_file), "%s", out_name);
#endif
        }

        BastaValue *files = basta_new_map();
        char *bin_dir = now_path_join(basedir, "target/bin");
        if (bin_dir) {
            char *out_path = now_path_join(bin_dir, out_file);
            if (out_path) {
                /* add_file_blob skips a file it cannot read, so a wrong
                 * name produced an archive with no binary in it and a
                 * "packaged" line on stdout. Anything but header-only
                 * has to carry its output or the package is not one. */
                if (!is_header_only && !now_path_exists(out_path)) {
                    if (result) {
                        result->code = NOW_ERR_IO;
                        snprintf(result->message, sizeof(result->message),
                                 "build output %s not found in target/bin "
                                 "— nothing to package for output type '%s'",
                                 out_file, out_type);
                    }
                    free(out_path);
                    free(bin_dir);
                    basta_free(files);
                    basta_free(root);
                    return -1;
                }
                add_file_blob(files, out_file, out_path);
                free(out_path);
            }
            /* For shared libs on Windows, also include the import lib */
            if (strcmp(out_type, "shared") == 0) {
#ifdef _WIN32
                char imp_file[256];
                snprintf(imp_file, sizeof(imp_file), "%s.lib", out_name);
                char *imp_path = now_path_join(bin_dir, imp_file);
                if (imp_path) {
                    if (!now_path_exists(imp_path)) {
                        free(imp_path);
                        char *lib_dir = now_path_join(basedir, "target/lib");
                        if (lib_dir) {
                            imp_path = now_path_join(lib_dir, imp_file);
                            free(lib_dir);
                        } else {
                            imp_path = NULL;
                        }
                    }
                    if (imp_path) {
                        add_file_blob(files, imp_file, imp_path);
                        free(imp_path);
                    }
                }
#endif
            }
            free(bin_dir);
        }
        basta_set(root, "files", files);
    }

    /* @license section */
    {
        char *lic_path = now_path_join(basedir, "LICENSE");
        if (lic_path) {
            if (now_path_exists(lic_path)) {
                BastaValue *lic_sec = basta_new_map();
                add_file_blob(lic_sec, "LICENSE", lic_path);
                basta_set(root, "license", lic_sec);
            }
            free(lic_path);
        }
    }

    /* Write the .basta file */
    char *pkg_dir = now_path_join(basedir, "target/pkg");
    if (!pkg_dir) { basta_free(root); return -1; }
    now_mkdir_p(pkg_dir);

    char basta_name[512];
    snprintf(basta_name, sizeof(basta_name), "%s-%s-%s.basta",
             artifact, version, is_header_only ? "noarch" : triple);

    char *basta_path = now_path_join(pkg_dir, basta_name);
    if (!basta_path) { free(pkg_dir); basta_free(root); return -1; }

    if (verbose)
        fprintf(stderr, "  packaging %s\n", basta_name);

    FILE *fp = fopen(basta_path, "wb");
    if (!fp) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot create %s", basta_path);
        }
        free(basta_path);
        free(pkg_dir);
        basta_free(root);
        return -1;
    }

    int wrc = basta_write_fp(root, BASTA_SECTIONS, fp);
    fclose(fp);
    basta_free(root);

    if (wrc != 0) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "basta_write_fp failed");
        }
        free(basta_path);
        free(pkg_dir);
        return -1;
    }

    /* Compute SHA-256 of the .basta file and store in metadata.
     * Also write a .sha256 sidecar for backward compat. */
    char *sha = now_sha256_file(basta_path);
    if (sha) {
        char sha_name[512];
        snprintf(sha_name, sizeof(sha_name), "%s-%s-%s.sha256",
                 artifact, version, is_header_only ? "noarch" : triple);
        char *sha_path = now_path_join(pkg_dir, sha_name);
        if (sha_path) {
            FILE *sfp = fopen(sha_path, "w");
            if (sfp) {
                fprintf(sfp, "%s\n", sha);
                fclose(sfp);
            }
            free(sha_path);
        }
        free(sha);
    }

    /* Sign the package when a publisher key is configured.
     *
     * `now publish` has always uploaded a .sig "if present" — nothing
     * ever produced one, so require_signatures could not be satisfied
     * by anything now published. Signing here closes that loop. Absence
     * of a key is not an error: unsigned publishing stays the default,
     * and consumers decide whether to demand signatures. */
    {
        unsigned char priv[64];
        if (now_signing_key_load(priv) == 0) {
            char sig_name[512];
            snprintf(sig_name, sizeof(sig_name), "%s-%s-%s.sig",
                     artifact, version, is_header_only ? "noarch" : triple);
            char *sig_path = now_path_join(pkg_dir, sig_name);
            if (sig_path) {
                NowResult sr;
                memset(&sr, 0, sizeof(sr));
                if (now_sign_file(basta_path, sig_path, priv, &sr) == 0) {
                    if (verbose) fprintf(stderr, "  signed %s\n", sig_name);
                } else {
                    fprintf(stderr, "warning: signing failed: %s\n",
                            sr.message[0] ? sr.message : "unknown error");
                }
                free(sig_path);
            }
            memset(priv, 0, sizeof(priv));
        }
    }

    free(basta_path);
    free(pkg_dir);

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}

/* ---- Basta extraction ---- */

/* Write a blob to a file. Returns 0 on success. */
static int write_blob_file(const char *path, const BastaValue *blob) {
    size_t len;
    const uint8_t *data = basta_get_blob(blob, &len);
    if (!data) return -1;

    /* Create parent directories */
    char *parent = strdup(path);
    if (parent) {
        char *sep = strrchr(parent, '/');
        if (!sep) sep = strrchr(parent, '\\');
        if (sep) {
            *sep = '\0';
            now_mkdir_p(parent);
        }
        free(parent);
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);
    return (written == len) ? 0 : -1;
}

NOW_API int now_basta_extract(const char *basta_path, const char *dest_dir,
                               int verbose, NowResult *result) {
    if (!basta_path || !dest_dir) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "NULL basta_path or dest_dir");
        }
        return -1;
    }

    /* Read the .basta file */
    size_t file_len;
    char *file_data = pkg_read_file(basta_path, &file_len);
    if (!file_data) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot read %s", basta_path);
        }
        return -1;
    }

    BastaResult bres;
    BastaValue *root = basta_parse(file_data, file_len, &bres);
    free(file_data);
    if (!root) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "basta parse error: %s", bres.message);
        }
        return -1;
    }

    now_mkdir_p(dest_dir);

    /* Extract @descriptor section → now.pasta, now.lock.pasta */
    const BastaValue *desc = basta_map_get(root, "descriptor");
    if (desc && basta_type(desc) == BASTA_MAP) {
        for (size_t i = 0; i < basta_count(desc); i++) {
            const char *key = basta_map_key(desc, i);
            const BastaValue *val = basta_map_value(desc, i);
            if (key && val && basta_type(val) == BASTA_BLOB) {
                char *path = now_path_join(dest_dir, key);
                if (path) {
                    if (verbose) fprintf(stderr, "  extract %s\n", key);
                    write_blob_file(path, val);
                    free(path);
                }
            }
        }
    }

    /* Extract @headers section → h/{name} */
    const BastaValue *hdrs = basta_map_get(root, "headers");
    if (hdrs && basta_type(hdrs) == BASTA_MAP) {
        char *h_dir = now_path_join(dest_dir, "h");
        if (h_dir) {
            now_mkdir_p(h_dir);
            for (size_t i = 0; i < basta_count(hdrs); i++) {
                const char *key = basta_map_key(hdrs, i);
                const BastaValue *val = basta_map_value(hdrs, i);
                if (key && val && basta_type(val) == BASTA_BLOB) {
                    char *path = now_path_join(h_dir, key);
                    if (path) {
                        if (verbose) fprintf(stderr, "  extract h/%s\n", key);
                        write_blob_file(path, val);
                        free(path);
                    }
                }
            }
            free(h_dir);
        }
    }

    /* Extract @files section → lib/{triple}/ or bin/{triple}/ */
    const BastaValue *files = basta_map_get(root, "files");
    const BastaValue *meta  = basta_map_get(root, "metadata");
    if (files && basta_type(files) == BASTA_MAP && meta) {
        const BastaValue *otype_v  = basta_map_get(meta, "output_type");
        const BastaValue *triple_v = basta_map_get(meta, "triple");
        const char *otype  = otype_v  ? basta_get_string(otype_v)  : "executable";
        const char *tri    = triple_v ? basta_get_string(triple_v) : "unknown";

        int is_exe = (strcmp(otype, "executable") == 0);
        const char *sub = is_exe ? "bin" : "lib";

        char *type_dir = now_path_join(dest_dir, sub);
        if (type_dir) {
            char *tri_dir = now_path_join(type_dir, tri);
            if (tri_dir) {
                now_mkdir_p(tri_dir);
                for (size_t i = 0; i < basta_count(files); i++) {
                    const char *key = basta_map_key(files, i);
                    const BastaValue *val = basta_map_value(files, i);
                    if (key && val && basta_type(val) == BASTA_BLOB) {
                        char *path = now_path_join(tri_dir, key);
                        if (path) {
                            if (verbose)
                                fprintf(stderr, "  extract %s/%s/%s\n", sub, tri, key);
                            write_blob_file(path, val);
                            free(path);
                        }
                    }
                }
                free(tri_dir);
            }
            free(type_dir);
        }
    }

    /* Extract @license section */
    const BastaValue *lic = basta_map_get(root, "license");
    if (lic && basta_type(lic) == BASTA_MAP) {
        for (size_t i = 0; i < basta_count(lic); i++) {
            const char *key = basta_map_key(lic, i);
            const BastaValue *val = basta_map_value(lic, i);
            if (key && val && basta_type(val) == BASTA_BLOB) {
                char *path = now_path_join(dest_dir, key);
                if (path) {
                    if (verbose) fprintf(stderr, "  extract %s\n", key);
                    write_blob_file(path, val);
                    free(path);
                }
            }
        }
    }

    basta_free(root);

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}

/* ---- Install phase ---- */

NOW_API int now_install(const NowProject *project, const char *basedir,
                        int verbose, NowResult *result) {
    if (!project || !basedir) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message), "NULL project or basedir");
        }
        return -1;
    }

    const char *group    = project->group    ? project->group    : "unknown";
    const char *artifact = project->artifact ? project->artifact : "unnamed";
    const char *version  = project->version  ? project->version  : "0.0.0";
    const char *triple   = now_host_triple();
    const char *out_type = project->output.type ? project->output.type : "executable";

    /* Determine repo root */
    const char *home = NULL;
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
                     "Cannot determine home directory");
        }
        return -1;
    }

    char *repo_root = NULL;
    {
        char *dot_now = now_path_join(home, ".now");
        if (dot_now) {
            repo_root = now_path_join(dot_now, "repo");
            free(dot_now);
        }
    }
    if (!repo_root) return -1;

    char *dep_path = now_repo_dep_path(repo_root, group, artifact, version);
    free(repo_root);
    if (!dep_path) return -1;

    now_mkdir_p(dep_path);

    if (verbose)
        fprintf(stderr, "  installing to %s\n", dep_path);

    /* Copy descriptor */
    char *desc_src = now_path_join(basedir, "now.pasta");
    char *desc_dst = now_path_join(dep_path, "now.pasta");
    if (desc_src && desc_dst && now_path_exists(desc_src))
        copy_file(desc_src, desc_dst);
    free(desc_src);
    free(desc_dst);

    /* Copy lock file */
    char *lock_src = now_path_join(basedir, "now.lock.pasta");
    char *lock_dst = now_path_join(dep_path, "now.lock.pasta");
    if (lock_src && lock_dst && now_path_exists(lock_src))
        copy_file(lock_src, lock_dst);
    free(lock_src);
    free(lock_dst);

    /* Copy headers into h/ */
    const char *hdr_dir = project->sources.headers;
    if (hdr_dir) {
        char *h_dst = now_path_join(dep_path, "h");
        if (h_dst) {
            now_mkdir_p(h_dst);
            const char *hdr_exts[] = {".h", ".hpp", ".hxx", ".hh", ".H", NULL};
            copy_tree(basedir, hdr_dir, h_dst, hdr_exts);
            free(h_dst);
        }
    }

    /* Copy build output */
    char *bin_src = now_path_join(basedir, "target/bin");
    if (bin_src) {
        int is_executable = (strcmp(out_type, "executable") == 0);
        const char *dest_sub = is_executable ? "bin" : "lib";

        char *type_dir = now_path_join(dep_path, dest_sub);
        if (type_dir) {
            char *triple_dir = now_path_join(type_dir, triple);
            if (triple_dir) {
                now_mkdir_p(triple_dir);

                const char *out_name = project->output.name
                                       ? project->output.name : artifact;
                char out_file[256];
                if (strcmp(out_type, "static") == 0) {
#ifdef _WIN32
                    snprintf(out_file, sizeof(out_file), "%s.lib", out_name);
#else
                    snprintf(out_file, sizeof(out_file), "lib%s.a", out_name);
#endif
                } else if (strcmp(out_type, "shared") == 0) {
#ifdef _WIN32
                    snprintf(out_file, sizeof(out_file), "%s.dll", out_name);
#elif defined(__APPLE__)
                    snprintf(out_file, sizeof(out_file), "lib%s.dylib", out_name);
#else
                    snprintf(out_file, sizeof(out_file), "lib%s.so", out_name);
#endif
                } else {
#ifdef _WIN32
                    snprintf(out_file, sizeof(out_file), "%s.exe", out_name);
#else
                    snprintf(out_file, sizeof(out_file), "%s", out_name);
#endif
                }

                char *out_src = now_path_join(bin_src, out_file);
                char *out_dst = now_path_join(triple_dir, out_file);
                if (out_src && out_dst && now_path_exists(out_src))
                    copy_file(out_src, out_dst);
                free(out_src);
                free(out_dst);

                free(triple_dir);
            }
            free(type_dir);
        }
        free(bin_src);
    }

    free(dep_path);

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}

/* ---- Publish phase ---- */

/* Authenticate with registry: load credentials, exchange for JWT if possible.
 * Returns malloc'd token string (JWT or raw token), or NULL if no credentials.
 * Caller must free. */
static char *auth_for_registry(const char *registry_url, const char *host,
                                int port, const char *path_prefix, int tls,
                                NowResult *result) {
    NowCredentials creds;
    char *jwt = NULL;
    if (now_auth_load(registry_url, &creds) == 0) {
        if (creds.username) {
            now_auth_login(host, port, path_prefix, &creds, tls,
                           &jwt, result);
        }
        /* Fallback to raw token if no username or login failed */
        if (!jwt && creds.token)
            jwt = strdup(creds.token);
        now_auth_creds_free(&creds);
    }
    return jwt;
}

/* PUT a file to the registry. Returns 0 on success. */
static int publish_put(const char *host, int port, const char *path_prefix,
                       const char *rel_path, const char *data, size_t data_len,
                       const char *content_type, const char *auth_token,
                       int use_tls, int verbose, NowResult *result) {
    /* Build path: {path_prefix}/artifact/{rel_path} */
    char put_path[1024];
    snprintf(put_path, sizeof(put_path), "%s/artifact/%s", path_prefix, rel_path);

    /* Build headers */
    PicoHttpHeader headers[2];
    int nhdr = 0;
    char auth_buf[512];
    if (auth_token && *auth_token) {
        snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", auth_token);
        headers[nhdr].name  = "Authorization";
        headers[nhdr].value = auth_buf;
        nhdr++;
    }

    PicoHttpOptions opts = {0};
    opts.headers = headers;
    opts.header_count = (size_t)nhdr;
    opts.max_redirects = -1;  /* no redirects on PUT */
    /* publish carries a Bearer JWT and the artifact bytes — use the
     * scheme the caller parsed, not the port heuristic. This parameter
     * was previously accepted and ignored. */
    opts.tls = use_tls ? 1 : -1;

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));

    if (verbose)
        fprintf(stderr, "  PUT %s:%d%s (%zu bytes)\n", host, port, put_path, data_len);

    const char *ct = content_type ? content_type : "application/octet-stream";
    int rc = pico_http_put(host, port, put_path,
                           ct, data, data_len,
                           &opts, &res);

    if (rc != PICO_OK) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "HTTP PUT failed: %s", pico_http_strerror(rc));
        }
        pico_http_response_free(&res);
        return -1;
    }

    if (res.status < 200 || res.status >= 300) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "registry returned HTTP %d on PUT %s", res.status, rel_path);
        }
        pico_http_response_free(&res);
        return -1;
    }

    pico_http_response_free(&res);
    return 0;
}

static int publish_body(const NowProject *project, const char *basedir,
                        const char *repo_url, int verbose, NowResult *result);

NOW_API int now_publish(const NowProject *project, const char *basedir,
                        const char *repo_url, int verbose, NowResult *result) {
    int rc;
    now_events_phase_started("publish");
    rc = publish_body(project, basedir, repo_url, verbose, result);
    now_events_phase_finished("publish", rc == 0, NULL);
    return rc;
}

static int publish_body(const NowProject *project, const char *basedir,
                        const char *repo_url, int verbose, NowResult *result) {
    if (!project || !basedir) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "NULL project or basedir");
        }
        return -1;
    }

    const char *group    = project->group;
    const char *artifact = project->artifact;
    const char *version  = project->version;

    if (!group || !artifact || !version) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "project must have group, artifact, and version to publish");
        }
        return -1;
    }

    /* Determine registry URL */
    const char *url = repo_url;
    if (!url && project->repos.count > 0 && project->repos.items[0].url)
        url = project->repos.items[0].url;

    if (!url) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "no registry URL: use --repo or add repos to now.pasta");
        }
        return -1;
    }

    /* Parse registry URL */
    char *host = NULL;
    int   port = 0;
    char *path = NULL;
    int   tls  = 0;

    int rc = pico_http_parse_url_ex(url, &host, &port, &path, &tls);
    if (rc != 0 || !host) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "invalid registry URL: %s", url);
        }
        free(host); free(path);
        return -1;
    }

    /* path prefix: strip trailing slash */
    char *prefix = path ? path : strdup("");
    size_t plen = strlen(prefix);
    while (plen > 0 && prefix[plen - 1] == '/') prefix[--plen] = '\0';

    /* Authenticate with registry */
    char *token = auth_for_registry(url, host, port, prefix, tls, result);

    /* Find .basta package in target/pkg/ */
    const char *triple   = now_host_triple();
    const char *out_type = project->output.type ? project->output.type : "executable";
    int is_header_only = (strcmp(out_type, "header-only") == 0);
    const char *arch_str = is_header_only ? "noarch" : triple;

    char basta_name[512];
    snprintf(basta_name, sizeof(basta_name), "%s-%s-%s.basta",
             artifact, version, arch_str);

    char *pkg_dir = now_path_join(basedir, "target/pkg");
    if (!pkg_dir) { free(host); free(prefix); free(token); return -1; }

    char *basta_path = now_path_join(pkg_dir, basta_name);

    if (!basta_path || !now_path_exists(basta_path)) {
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "package not found: run 'now package' first (%s)",
                     basta_name);
        }
        free(basta_path); free(pkg_dir);
        free(host); free(prefix); free(token);
        return -1;
    }

    /* Build the artifact path: {group}/{artifact}/{version}/{filename}
     * Group dots become slashes: org.acme → org/acme */
    char group_path[256];
    strncpy(group_path, group, sizeof(group_path) - 1);
    group_path[sizeof(group_path) - 1] = '\0';
    for (char *p = group_path; *p; p++)
        if (*p == '.') *p = '/';

    int errors = 0;

    /* 1. PUT the descriptor (now.pasta) — separate from .basta for
     *    registry content negotiation / metadata queries */
    {
        char *desc_path = now_path_join(basedir, "now.pasta");
        if (desc_path && now_path_exists(desc_path)) {
            size_t desc_len;
            char *desc_data = pkg_read_file(desc_path, &desc_len);
            if (desc_data) {
                char rel[512];
                snprintf(rel, sizeof(rel), "%s/%s/%s/now.pasta",
                         group_path, artifact, version);
                rc = publish_put(host, port, prefix, rel,
                                 desc_data, desc_len,
                                 "application/x-pasta",
                                 token, tls, verbose, result);
                free(desc_data);
                if (rc != 0) errors++;
            }
            free(desc_path);
        }
    }

    /* 2. PUT the .basta package */
    if (errors == 0) {
        size_t basta_len;
        char *basta_data = pkg_read_file(basta_path, &basta_len);
        if (basta_data) {
            char rel[512];
            snprintf(rel, sizeof(rel), "%s/%s/%s/%s",
                     group_path, artifact, version, basta_name);
            rc = publish_put(host, port, prefix, rel,
                             basta_data, basta_len,
                             "application/x-basta",
                             token, tls, verbose, result);
            free(basta_data);
            if (rc != 0) errors++;
        } else {
            if (result) {
                result->code = NOW_ERR_IO;
                snprintf(result->message, sizeof(result->message),
                         "cannot read package: %s", basta_path);
            }
            errors++;
        }
    }

    /* 3. PUT the .sig file if present */
    if (errors == 0) {
        char sig_name[512];
        snprintf(sig_name, sizeof(sig_name), "%s-%s-%s.sig",
                 artifact, version, arch_str);
        char *sig_path = now_path_join(pkg_dir, sig_name);
        if (sig_path && now_path_exists(sig_path)) {
            size_t sig_len;
            char *sig_data = pkg_read_file(sig_path, &sig_len);
            if (sig_data) {
                char rel[512];
                snprintf(rel, sizeof(rel), "%s/%s/%s/%s",
                         group_path, artifact, version, sig_name);
                rc = publish_put(host, port, prefix, rel,
                                 sig_data, sig_len, NULL,
                                 token, tls, verbose, result);
                free(sig_data);
                if (rc != 0) errors++;
            }
        }
        free(sig_path);
    }

    /* 4. PUT the .sha256 sidecar.
     *
     * `now package` has always written one and publish never uploaded
     * it, so procure's sidecar check found nothing to compare against on
     * every fetch and silently passed. An integrity check that is
     * skipped whenever the file is absent needs the file to be there. */
    if (errors == 0) {
        char sha_name[512];
        snprintf(sha_name, sizeof(sha_name), "%s-%s-%s.sha256",
                 artifact, version, arch_str);
        char *sha_path = now_path_join(pkg_dir, sha_name);
        if (sha_path && now_path_exists(sha_path)) {
            size_t sha_len;
            char *sha_data = pkg_read_file(sha_path, &sha_len);
            if (sha_data) {
                char rel[512];
                snprintf(rel, sizeof(rel), "%s/%s/%s/%s",
                         group_path, artifact, version, sha_name);
                rc = publish_put(host, port, prefix, rel,
                                 sha_data, sha_len, "text/plain",
                                 token, tls, verbose, result);
                free(sha_data);
                if (rc != 0) errors++;
            }
        }
        free(sha_path);
    }

    free(basta_path);
    free(pkg_dir);
    free(host);
    free(prefix);
    free(token);

    if (errors) return -1;

    if (verbose)
        fprintf(stderr, "  published %s:%s:%s to %s\n",
                group, artifact, version, url);

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}

NOW_API int now_keys_register(const char *registry_url, const char *group,
                               const char *key_id, const char *comment,
                               int verbose, NowResult *result) {
    if (!registry_url || !group) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "keys:register requires a registry URL and a group");
        }
        return -1;
    }

    /* The public half is bytes 32..63 of the key file, but derive it
     * from the seed instead: signing does the same, so a hand-edited
     * file cannot register one key and sign with another. */
    unsigned char priv[64], seed[32], pub[32];
    if (now_signing_key_load(priv) != 0) {
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "no signing key — run 'now keygen' first");
        }
        return -1;
    }
    memcpy(seed, priv, sizeof(seed));
    if (now_ed25519_keypair(pub, priv, seed) != 0) {
        memset(priv, 0, sizeof(priv));
        memset(seed, 0, sizeof(seed));
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "signing key is malformed");
        }
        return -1;
    }
    memset(priv, 0, sizeof(priv));
    memset(seed, 0, sizeof(seed));

    char *pub_b64 = now_b64_encode(pub, sizeof(pub));
    if (!pub_b64) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot encode public key");
        }
        return -1;
    }

    /* Default key id: the first 16 hex digits of the key. Unique per
     * key, stable across re-registration, and recognisable next to the
     * key itself in a listing. */
    char kid[64];
    if (key_id && *key_id) {
        snprintf(kid, sizeof(kid), "%s", key_id);
    } else {
        for (int i = 0; i < 8; i++)
            snprintf(kid + i * 2, 3, "%02x", pub[i]);
        kid[16] = '\0';
    }

    char *host = NULL;
    int   port = 0;
    char *path = NULL;
    int   tls  = 0;

    int rc = pico_http_parse_url_ex(registry_url, &host, &port, &path, &tls);
    if (rc != 0 || !host) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "invalid registry URL: %s", registry_url);
        }
        free(pub_b64); free(host); free(path);
        return -1;
    }

    char *prefix = path ? path : strdup("");
    size_t plen = strlen(prefix);
    while (plen > 0 && prefix[plen - 1] == '/') prefix[--plen] = '\0';

    char *token = auth_for_registry(registry_url, host, port, prefix, tls,
                                    result);

    char keys_path[1024];
    snprintf(keys_path, sizeof(keys_path), "%s/keys", prefix);

    char body[1024];
    int body_len = snprintf(body, sizeof(body),
        "{\"key_id\":\"%s\",\"group_id\":\"%s\",\"public_key\":\"%s\""
        ",\"comment\":\"%s\"}",
        kid, group, pub_b64,
        (comment && *comment) ? comment : "registered by now keys:register");

    PicoHttpHeader headers[2];
    int nhdr = 0;
    char auth_buf[1024];
    if (token && *token) {
        snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", token);
        headers[nhdr].name  = "Authorization";
        headers[nhdr].value = auth_buf;
        nhdr++;
    }

    PicoHttpOptions opts = {0};
    opts.headers = headers;
    opts.header_count = (size_t)nhdr;
    opts.max_redirects = -1;
    opts.tls = tls ? 1 : -1;

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));

    if (verbose)
        fprintf(stderr, "  POST %s:%d%s\n", host, port, keys_path);

    rc = pico_http_post(host, port, keys_path, "application/json",
                        body, (size_t)body_len, &opts, &res);

    free(host); free(prefix); free(token);

    if (rc != PICO_OK) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "POST /keys failed: %s", pico_http_strerror(rc));
        }
        free(pub_b64);
        return -1;
    }

    if (res.status < 200 || res.status >= 300) {
        if (result) {
            result->code = NOW_ERR_IO;
            /* Carry the registry's own words through. It distinguishes
             * an unknown group from a duplicate key id, and guessing
             * between them from the status code alone is what sent
             * people renaming a key that was fine. */
            snprintf(result->message, sizeof(result->message),
                     "registry returned HTTP %d on POST /keys%s%.*s",
                     res.status,
                     res.body_len ? ": " : "",
                     (int)(res.body_len > 200 ? 200 : res.body_len),
                     res.body ? res.body : "");
        }
        pico_http_response_free(&res);
        free(pub_b64);
        return -1;
    }

    pico_http_response_free(&res);

    if (verbose || 1)
        fprintf(stderr, "  registered key %s for %s\n", kid, group);

    free(pub_b64);

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}

NOW_API int now_publish_yank(const char *registry_url,
                              const char *group, const char *artifact,
                              const char *version, const char *reason,
                              int verbose, NowResult *result) {
    if (!registry_url || !group || !artifact || !version) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "yank requires registry URL, group, artifact, and version");
        }
        return -1;
    }

    /* Parse registry URL */
    char *host = NULL;
    int   port = 0;
    char *path = NULL;
    int   tls  = 0;

    int rc = pico_http_parse_url_ex(registry_url, &host, &port, &path, &tls);
    if (rc != 0 || !host) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "invalid registry URL: %s", registry_url);
        }
        free(host); free(path);
        return -1;
    }

    char *prefix = path ? path : strdup("");
    size_t plen = strlen(prefix);
    while (plen > 0 && prefix[plen - 1] == '/') prefix[--plen] = '\0';

    /* Authenticate */
    char *token = auth_for_registry(registry_url, host, port, prefix, tls, result);

    /* Build path: {prefix}/artifact/{group_path}/{artifact}/{version}/{artifact}-{version}.basta/yank */
    char group_path[256];
    strncpy(group_path, group, sizeof(group_path) - 1);
    group_path[sizeof(group_path) - 1] = '\0';
    for (char *p = group_path; *p; p++)
        if (*p == '.') *p = '/';

    char yank_path[1024];
    snprintf(yank_path, sizeof(yank_path),
             "%s/artifact/%s/%s/%s/%s-%s.basta/yank",
             prefix, group_path, artifact, version, artifact, version);

    /* Build body: {"reason":"..."} if reason is given */
    char body[1024] = "";
    size_t body_len = 0;
    if (reason && *reason) {
        body_len = (size_t)snprintf(body, sizeof(body),
                                    "{\"reason\":\"%s\"}", reason);
    }

    /* Build headers */
    PicoHttpHeader headers[2];
    int nhdr = 0;
    char auth_buf[1024];
    if (token && *token) {
        snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", token);
        headers[nhdr].name  = "Authorization";
        headers[nhdr].value = auth_buf;
        nhdr++;
    }

    PicoHttpOptions opts = {0};
    opts.headers = headers;
    opts.header_count = (size_t)nhdr;
    opts.max_redirects = -1;
    opts.tls = tls ? 1 : -1;  /* yank carries a Bearer JWT — use the URL's scheme */

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));

    if (verbose)
        fprintf(stderr, "  POST %s:%d%s\n", host, port, yank_path);

    const char *ct = body_len > 0 ? "application/json" : NULL;
    rc = pico_http_post(host, port, yank_path,
                        ct, body_len > 0 ? body : NULL, body_len,
                        &opts, &res);

    free(host);
    free(prefix);
    free(token);

    if (rc != PICO_OK) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "POST yank failed: %s", pico_http_strerror(rc));
        }
        return -1;
    }

    if (res.status < 200 || res.status >= 300) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "yank failed: registry returned HTTP %d", res.status);
        }
        pico_http_response_free(&res);
        return -1;
    }

    pico_http_response_free(&res);

    if (verbose)
        fprintf(stderr, "  yanked %s:%s:%s\n", group, artifact, version);

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}
