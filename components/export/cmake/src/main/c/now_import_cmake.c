/*
 * now_import_cmake.c — Convert CMakeLists.txt to now.pasta
 *
 * Parses common CMake patterns (project, add_library, add_executable,
 * target_sources, target_include_directories, target_compile_definitions,
 * target_link_libraries, add_subdirectory) and generates a now.pasta.
 *
 * Not a full CMake evaluator — handles the 80% case for standard projects.
 */
#include "now_export.h"
#include "now_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- Simple CMake token extraction ---- */

/* Skip whitespace and comments */
static const char *skip_ws(const char *p) {
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        break;
    }
    return p;
}

/* Extract a CMake argument (handles quoted and unquoted) */
static const char *next_arg(const char *p, char *out, size_t cap) {
    p = skip_ws(p);
    if (!*p || *p == ')') return NULL;
    size_t i = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
        if (*p == '"') p++;
    } else {
        while (*p && !isspace((unsigned char)*p) && *p != ')' && *p != '(' && i < cap - 1)
            out[i++] = *p++;
    }
    out[i] = '\0';
    return p;
}

/* Find a CMake command and return pointer after '(' */
static const char *find_cmd(const char *text, const char *cmd) {
    size_t clen = strlen(cmd);
    const char *p = text;
    while ((p = strstr(p, cmd)) != NULL) {
        /* Check it's a standalone command (not part of a longer word) */
        if (p > text && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) { p++; continue; }
        const char *after = p + clen;
        after = skip_ws(after);
        if (*after == '(') return after + 1;
        p++;
    }
    return NULL;
}

/* Find the next occurrence of `cmd` AFTER position `start`.
 * Returns pointer-after-'(' or NULL. Used for iterating all matches. */
static const char *find_cmd_after(const char *start, const char *cmd) {
    return find_cmd(start, cmd);
}

/* Variable substitution table built from `set(VAR value)` lines.
 * Only handles the simple set-to-literal form, not list expansion or
 * sub-commands. Good enough to resolve the `add_subdirectory(${ROOT}/x)`
 * pattern that lab's CMakeLists rely on heavily. */
typedef struct { char name[64]; char value[256]; } CmakeVar;
#define MAX_VARS 64

static int collect_vars(const char *text, CmakeVar *vars) {
    int n = 0;
    const char *p = text;
    char arg[512];
    while ((p = find_cmd_after(p, "set")) != NULL && n < MAX_VARS) {
        const char *next = next_arg(p, arg, sizeof(arg));
        if (!next || !arg[0]) continue;
        /* Skip CMAKE_* internals — too many surprises with cache flags */
        if (strncmp(arg, "CMAKE_", 6) == 0) continue;
        char val[256] = "";
        const char *vp = next_arg(next, val, sizeof(val));
        if (!vp || !val[0]) continue;
        /* Skip "CACHE" / "PATH" / etc. tokens — would consume them as values */
        strncpy(vars[n].name, arg, sizeof(vars[n].name) - 1);
        vars[n].name[sizeof(vars[n].name) - 1] = '\0';
        strncpy(vars[n].value, val, sizeof(vars[n].value) - 1);
        vars[n].value[sizeof(vars[n].value) - 1] = '\0';
        n++;
    }
    return n;
}

/* In-place substitute ${VAR} in `s` with values from `vars`. */
static void resolve_vars(char *s, size_t cap, const CmakeVar *vars, int nvars) {
    char buf[1024];
    int passes = 0;
    while (passes++ < 4 && strchr(s, '$')) {
        size_t len = strlen(s);
        if (len + 1 > sizeof(buf)) return;
        memcpy(buf, s, len + 1);
        const char *src = buf;
        char *dst = s;
        size_t remaining = cap;
        int changed = 0;
        while (*src && remaining > 1) {
            if (src[0] == '$' && src[1] == '{') {
                const char *end = strchr(src + 2, '}');
                if (end) {
                    size_t nl = (size_t)(end - src - 2);
                    int matched = 0;
                    for (int i = 0; i < nvars; i++) {
                        if (strlen(vars[i].name) == nl &&
                            strncmp(vars[i].name, src + 2, nl) == 0) {
                            size_t vl = strlen(vars[i].value);
                            if (vl >= remaining) return;
                            memcpy(dst, vars[i].value, vl);
                            dst += vl; remaining -= vl;
                            src = end + 1;
                            matched = 1;
                            changed = 1;
                            break;
                        }
                    }
                    if (matched) continue;
                }
            }
            *dst++ = *src++; remaining--;
        }
        *dst = '\0';
        if (!changed) break;
    }
}

/* Heuristic: distinguish system/external libs from sibling/project deps.
 *
 * - Names containing "_public_headers" or "_INTERFACE" — CMake INTERFACE
 *   library artifacts, not real libs to link. Drop entirely.
 * - Names looking like sibling project targets (e.g. "iridium_store",
 *   "uanema", "pasta", "alforno", "cookbook", "apennines") → depends.
 * - Short bare names that look like system libs (pthread, m, dl, ws2_32)
 *   → keep as link.libs.
 * The line is fuzzy. A future pass could read sibling pasta files to
 * make this exact. */
static int looks_like_interface_lib(const char *s) {
    return strstr(s, "_public_headers") != NULL ||
           strstr(s, "_INTERFACE") != NULL ||
           strstr(s, "_headers") != NULL;
}

static int looks_like_system_lib(const char *s) {
    /* All-lowercase, no underscores or single segment, length ≤ 12. */
    static const char *known[] = {
        "pthread", "m", "dl", "rt", "c", "stdc++", "z", "ssl", "crypto",
        "ws2_32", "bcrypt", "winmm", "iphlpapi", "advapi32", "user32",
        "kernel32", "shell32", "ole32", "uuid", "secur32", "crypt32",
        "ntdll", "userenv", NULL
    };
    for (int i = 0; known[i]; i++)
        if (strcmp(s, known[i]) == 0) return 1;
    return 0;
}

/* ---- Import ---- */

NOW_API int now_import_cmake(const char *cmake_path, const char *out_path,
                               NowResult *result) {
    if (!cmake_path || !out_path) {
        if (result) { result->code = NOW_ERR_SCHEMA; snprintf(result->message, sizeof(result->message), "NULL path"); }
        return -1;
    }

    /* Read CMakeLists.txt */
    FILE *fp = fopen(cmake_path, "rb");
    if (!fp) {
        if (result) { result->code = NOW_ERR_IO; snprintf(result->message, sizeof(result->message), "cannot open %s", cmake_path); }
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)sz + 1);
    if (!text) { fclose(fp); return -1; }
    fread(text, 1, (size_t)sz, fp);
    text[sz] = '\0';
    fclose(fp);

    /* Extract fields */
    char name[128] = "myproject";
    char version[32] = "1.0.0";
    char output_type[32] = "executable";
    char lang[16] = "c";
    char std[16] = "";        /* CMAKE_C_STANDARD → "c11" etc */

    /* Growable, because a converter that silently drops entries emits a
     * descriptor that looks right and is wrong. These were fixed 2D
     * arrays — 64 defines, 32 includes, 128 sources — where exceeding
     * the bound dropped the rest with no diagnostic and an exit status
     * of 0, and where strncpy also truncated any single entry past the
     * row width. A real CMakeLists hits both. */
    NowStrArray libs, defs, incs, subdirs, sources, depends;
    now_strarray_init(&libs);    now_strarray_init(&defs);
    now_strarray_init(&incs);    now_strarray_init(&subdirs);
    now_strarray_init(&sources); now_strarray_init(&depends);

    const char *p;
    char arg[512];

    /* Collect `set(VAR value)` for substitution in subsequent extractions. */
    CmakeVar vars[MAX_VARS];
    int nvars = collect_vars(text, vars);

    /* Pull CMAKE_C_STANDARD if explicitly set. */
    {
        const char *sp = text;
        while ((sp = find_cmd_after(sp, "set")) != NULL) {
            char k[64], v[32];
            const char *n = next_arg(sp, k, sizeof(k));
            if (!n) continue;
            if (strcmp(k, "CMAKE_C_STANDARD") == 0) {
                if (next_arg(n, v, sizeof(v)) && v[0]) {
                    snprintf(std, sizeof(std), "c%s", v);
                }
                break;
            }
            if (strcmp(k, "CMAKE_CXX_STANDARD") == 0) {
                if (next_arg(n, v, sizeof(v)) && v[0]) {
                    snprintf(std, sizeof(std), "c++%s", v);
                    strcpy(lang, "c++");
                }
                break;
            }
        }
    }

    /* project(NAME VERSION X.Y.Z LANGUAGES C CXX) */
    p = find_cmd(text, "project");
    if (p) {
        p = next_arg(p, name, sizeof(name));
        if (p) {
            while ((p = next_arg(p, arg, sizeof(arg))) != NULL) {
                if (strcmp(arg, "VERSION") == 0)
                    p = next_arg(p, version, sizeof(version));
                else if (strcmp(arg, "CXX") == 0 || strcmp(arg, "C++") == 0)
                    strcpy(lang, "c++");
            }
        }
    }

    /* add_library / add_executable */
    p = find_cmd(text, "add_library");
    if (p) {
        p = next_arg(p, arg, sizeof(arg)); /* skip target name */
        if (p && (p = next_arg(p, arg, sizeof(arg)))) {
            if (strcmp(arg, "STATIC") == 0) strcpy(output_type, "static");
            else if (strcmp(arg, "SHARED") == 0) strcpy(output_type, "shared");
        }
    } else {
        p = find_cmd(text, "add_executable");
        if (p) strcpy(output_type, "executable");
    }

    /* target_link_libraries — iterate every occurrence + classify each
     * argument into either real link-libs or sibling-style depends. */
    {
        const char *s = text;
        while ((p = find_cmd_after(s, "target_link_libraries")) != NULL) {
            next_arg(p, arg, sizeof(arg)); /* skip target name */
            const char *cur = p;
            while ((cur = next_arg(cur, arg, sizeof(arg))) != NULL) {
                if (!arg[0]) break;
                if (strcmp(arg, "PUBLIC") == 0 || strcmp(arg, "PRIVATE") == 0 ||
                    strcmp(arg, "INTERFACE") == 0) continue;
                if (arg[0] == '$') continue; /* unresolved variable */
                if (looks_like_interface_lib(arg)) continue; /* drop INTERFACE-only */
                if (looks_like_system_lib(arg)) {
                    now_strarray_push(&libs, arg);
                } else {
                    /* Sibling-style: emit as a depends entry. Without
                     * group context, use a placeholder the user can edit. */
                    {
                        char dbuf[512];
                        snprintf(dbuf, sizeof(dbuf), "dev.iridium:%s:*", arg);
                        now_strarray_push(&depends, dbuf);
                    }
                }
            }
            s = p;
        }
    }

    /* target_sources(TARGET PRIVATE/PUBLIC file file ...). vulpes
     * flagged this gap — without it the importer drops every source
     * file declared at this granularity (~50 entries per project in
     * lab's tree). */
    {
        const char *s = text;
        while ((p = find_cmd_after(s, "target_sources")) != NULL) {
            next_arg(p, arg, sizeof(arg)); /* skip target name */
            const char *cur = p;
            while ((cur = next_arg(cur, arg, sizeof(arg))) != NULL) {
                if (!arg[0]) break;
                if (strcmp(arg, "PUBLIC") == 0 || strcmp(arg, "PRIVATE") == 0 ||
                    strcmp(arg, "INTERFACE") == 0) continue;
                /* Resolve ${VAR} substitutions */
                resolve_vars(arg, sizeof(arg), vars, nvars);
                if (arg[0] == '$') continue; /* still unresolved */
                now_strarray_push(&sources, arg);
            }
            s = p;
        }
    }

    /* target_compile_definitions */
    p = find_cmd(text, "target_compile_definitions");
    if (p) {
        p = next_arg(p, arg, sizeof(arg)); /* skip target */
        while (p) {
            p = next_arg(p, arg, sizeof(arg));
            if (!p) break;
            if (strcmp(arg, "PUBLIC") == 0 || strcmp(arg, "PRIVATE") == 0) continue;
            now_strarray_push(&defs, arg);
        }
    }

    /* target_include_directories */
    p = find_cmd(text, "target_include_directories");
    if (p) {
        p = next_arg(p, arg, sizeof(arg)); /* skip target */
        while (p) {
            p = next_arg(p, arg, sizeof(arg));
            if (!p) break;
            if (strcmp(arg, "PUBLIC") == 0 || strcmp(arg, "PRIVATE") == 0 ||
                strcmp(arg, "SYSTEM") == 0 || strcmp(arg, "INTERFACE") == 0) continue;
            if (arg[0] == '$') continue;
            now_strarray_push(&incs, arg);
        }
    }

    /* add_subdirectory — resolve ${VAR} references against the
     * collected variable table. Drops still-unresolved entries
     * (caller's CMake would have errored on those too). */
    {
        const char *s = text;
        while ((p = find_cmd(s, "add_subdirectory")) != NULL) {
            if (next_arg(p, arg, sizeof(arg))) {
                resolve_vars(arg, sizeof(arg), vars, nvars);
                if (arg[0] != 0x24)
                    now_strarray_push(&subdirs, arg);
            }
            s = p;
        }
    }

    /* foreach(NAME item1 item2 ...) — recognize the common
     *   foreach(vendor_tree apennines cookbook alforno pasta)
     *       add_subdirectory(... ${vendor_tree} ...)
     *   endforeach()
     * pattern Iridium services use to declare bulk vendored deps.
     * Each item becomes a depends entry. The foreach body's
     * add_subdirectory call is captured by the loop above but
     * with the unsubstituted ${vendor_tree}; here we capture the
     * items themselves as sibling depends. */
    {
        const char *s = text;
        while ((p = find_cmd_after(s, "foreach")) != NULL) {
            char loop_var[64];
            /* Advance past the loop variable name itself before iterating items. */
            const char *cur = next_arg(p, loop_var, sizeof(loop_var));
            if (!cur) { s = p; continue; }
            while ((cur = next_arg(cur, arg, sizeof(arg))) != NULL) {
                if (!arg[0]) break;
                if (arg[0] == '$') continue;
                /* Each item: emit as a sibling-style dep. Skip duplicates
                 * already added via target_link_libraries. */
                int dup = 0;
                for (size_t i = 0; i < depends.count; i++)
                    if (strstr(depends.items[i], arg)) { dup = 1; break; }
                if (!dup) {
                    char dbuf[512];
                    snprintf(dbuf, sizeof(dbuf), "dev.iridium:%s:*", arg);
                    now_strarray_push(&depends, dbuf);
                }
            }
            s = p;
        }
    }

    free(text);

    /* Generate now.pasta */
    FILE *out = fopen(out_path, "w");
    if (!out) {
        if (result) { result->code = NOW_ERR_IO; snprintf(result->message, sizeof(result->message), "cannot write %s", out_path); }
        return -1;
    }

    /* Lowercase the name for artifact */
    char artifact[128];
    strncpy(artifact, name, sizeof(artifact) - 1);
    for (char *c = artifact; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;

    fprintf(out, "/* Imported by `now import:cmake`. Review the group + the\n");
    fprintf(out, " * `depends:` entries' coordinates — group defaults to\n");
    fprintf(out, " * \"com.example\"; adjust to match your registry namespace. */\n");
    fprintf(out, "{\n");
    fprintf(out, "  group:    \"com.example\",\n");
    fprintf(out, "  artifact: \"%s\",\n", artifact);
    fprintf(out, "  version:  \"%s\",\n", version);
    fprintf(out, "  lang:     \"%s\",\n", lang);
    fprintf(out, "  output:   { type: \"%s\", name: \"%s\" }", output_type, artifact);

    /* Sources (from target_sources). When set, `sources.include` lists
     * explicit files; sources.dir stays at the Maven default. */
    if (sources.count > 0) {
        fprintf(out, ",\n\n  sources: {\n    dir: \"src/main/c\",\n    headers: \"src/main/h\",\n    include: [");
        for (size_t i = 0; i < sources.count; i++)
            fprintf(out, "%s\n      \"%s\"", i ? "," : "", sources.items[i]);
        fprintf(out, "\n    ]\n  }");
    }

    /* Compile section */
    if (defs.count > 0 || incs.count > 0 || std[0]) {
        fprintf(out, ",\n\n  compile: {\n");
        int first = 1;
        if (std[0]) {
            fprintf(out, "    std: \"%s\"", std);
            first = 0;
        }
        fprintf(out, "%s    warnings: [\"Wall\", \"Wextra\"]", first ? "" : ",\n");
        if (defs.count > 0) {
            fprintf(out, ",\n    defines: [");
            for (size_t i = 0; i < defs.count; i++)
                fprintf(out, "%s\"%s\"", i ? ", " : "", defs.items[i]);
            fprintf(out, "]");
        }
        if (incs.count > 0) {
            fprintf(out, ",\n    includes: [");
            for (size_t i = 0; i < incs.count; i++)
                fprintf(out, "%s\"%s\"", i ? ", " : "", incs.items[i]);
            fprintf(out, "]");
        }
        fprintf(out, "\n  }");
    }

    /* Link section — system libs only (sibling-style names went to
     * `depends` via the classification pass). */
    if (libs.count > 0) {
        fprintf(out, ",\n\n  link: {\n    libs: [");
        for (size_t i = 0; i < libs.count; i++)
            fprintf(out, "%s\"%s\"", i ? ", " : "", libs.items[i]);
        fprintf(out, "]\n  }");
    }

    /* Depends — sibling-target-style references. Group is a guess;
     * caller should fix the coordinates. */
    if (depends.count > 0) {
        fprintf(out, ",\n\n  depends: [");
        for (size_t i = 0; i < depends.count; i++)
            fprintf(out, "%s\n    { id: \"%s\" }", i ? "," : "", depends.items[i]);
        fprintf(out, "\n  ]");
    }

    /* Vendored from add_subdirectory (resolved against set() vars). */
    if (subdirs.count > 0) {
        fprintf(out, ",\n\n  vendored: [");
        for (size_t i = 0; i < subdirs.count; i++)
            fprintf(out, "%s\n    \"%s\"", i ? "," : "", subdirs.items[i]);
        fprintf(out, "\n  ]");
    }

    fprintf(out, "\n}\n");
    fclose(out);

    now_strarray_free(&libs);    now_strarray_free(&defs);
    now_strarray_free(&incs);    now_strarray_free(&subdirs);
    now_strarray_free(&sources); now_strarray_free(&depends);

    if (result) { result->code = NOW_OK; result->message[0] = '\0'; }
    return 0;
}
