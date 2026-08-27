/*
 * now_pom.h — Internal Project Object Model structures and loader
 *
 * These structs represent the parsed contents of now.pasta.
 * The public API is in now.h; this header is for internal use.
 */
#ifndef NOW_POM_H
#define NOW_POM_H

#include <stddef.h>
#include "now.h"

/* Dynamic string array */
typedef struct {
    char  **items;
    size_t  count;
    size_t  capacity;
} NowStrArray;

NOW_API void now_strarray_init(NowStrArray *a);
NOW_API int  now_strarray_push(NowStrArray *a, const char *s);
NOW_API void now_strarray_free(NowStrArray *a);

/* Sources configuration (§1.3) */
typedef struct {
    char *dir;             /* source directory */
    /* Set when `dir` was filled in by the loader rather than named in the
     * descriptor. Omitting the key has to mean "there may not be one",
     * not "the default one, and it had better exist" — a module whose
     * whole source list is `include:` has no directory of its own. */
    int   dir_is_default;
    char *headers;         /* public headers directory */
    char *private_headers; /* private headers directory */
    char *pattern;         /* glob pattern */
    NowStrArray include;   /* explicit additions */
    NowStrArray exclude;   /* glob exclusions */
    /* For the 'tests' section only: link mode.
     *   NULL or "single" — every test source compiled and linked into
     *                       one test binary (single main, default).
     *   "each"           — each test source linked into its own binary
     *                       (each file has its own main, all are run). */
    char *mode;
    /* For the 'tests' section only:
     *   defines — extra -DKEY=VAL macros injected at test compile time
     *             (lets tests bake in fixture paths à la
     *             -DVVM_TEST_RESOURCES="src/test/resources").
     *   env     — KEY=VAL pairs set in each test binary's environment
     *             at launch (sibling of defines for runtime lookup). */
    NowStrArray defines;
    NowStrArray env;
} NowSources;

/* Compile configuration (§1.5) */
typedef struct {
    NowStrArray flags;
    NowStrArray warnings;
    NowStrArray defines;
    NowStrArray includes;
    char *std;             /* override std for this block */
    char *opt;             /* none | debug | size | speed | lto */
} NowCompile;

/* Link configuration (§1.5) */
typedef struct {
    NowStrArray flags;
    NowStrArray libs;
    NowStrArray libdirs;
    NowStrArray archives;  /* pre-built static archives (.a/.lib) */
    char *script;          /* linker script path */
    char *script_body;     /* inline linker script (multiline) */
    /* Forward the target/ABI-selecting flags from compile.flags to the
     * link driver. `now` passes compile.flags to the compiler and not to
     * the driver, so a cross-compiled module whose triple lives in
     * compile.flags produced correct objects and then linked with the
     * host's emulation — `ld.lld: <obj> is incompatible with elf_x86_64`,
     * a diagnostic that points at the object rather than the missing
     * flag. Opt-in, so nothing existing changes. */
    int   inherit_target;
} NowLink;

/* Per-triple compile/link overrides (§11.9).
 *
 * One entry per `target_flags:` key. The key is a triple pattern —
 * "os:arch:variant" with `*` accepted in any position — and the value
 * carries `compile:` and `link:` sub-blocks read by the same loaders
 * the top-level blocks use, so every field they understand is
 * understood here.
 *
 * The pattern is kept as written rather than as a parsed NowTriple.
 * Matching re-parses it, which costs nothing at the scale of a
 * descriptor and keeps the string available for diagnostics — a
 * pattern that matched nothing is the thing a user needs to see
 * spelled the way they typed it. */
typedef struct {
    char      *pattern;
    NowCompile compile;
    NowLink    link;
} NowTargetFlags;

typedef struct {
    NowTargetFlags *items;
    size_t          count;
    size_t          cap;
} NowTargetFlagsArray;

/* Assembly — extra files a package carries beyond what it built (§24).
 *
 * DRAFT. An SDK is not a library: it is headers, plus prebuilt link
 * archives, plus startup objects (crt0.o), plus the odd .S, and
 * sometimes sources. `output.type: "header-only"` already covers the
 * headers; this covers the rest.
 *
 *   assembly: {
 *     include: [
 *       { src: "prebuilt/**", dest: "lib/" },
 *       { src: "src/main/asm/**", dest: "asm/", exclude: ["**|*.tmp"] }
 *     ]
 *   }
 *
 * `src` is a glob rooted at the project directory. `dest` is a prefix
 * inside the package; the path stored is `dest` plus the file's path
 * with `src`'s fixed leading directories removed, so
 * "prebuilt/crt0.o" under src "prebuilt/**" and dest "lib/" is stored
 * as "lib/crt0.o" rather than "lib/prebuilt/crt0.o". */
typedef struct {
    char       *src;
    char       *dest;
    NowStrArray exclude;
} NowAssemblyInclude;

typedef struct {
    NowAssemblyInclude *items;
    size_t              count;
    size_t              cap;
} NowAssembly;

/* Output configuration (§1.4) */
typedef struct {
    char *type;            /* executable | static | shared | header-only */
    char *name;
    char *dir;
    /* `outputs:` only. The source whose object supplies main() for this
     * executable, relative to the module. Which object that is gets
     * settled by reading the symbol table, not by matching the
     * filename -- see now_obj_defines_symbol(). NULL for a library, and
     * NULL for a lone executable in a tree with exactly one main(). */
    char *entry;
} NowOutput;

/* `outputs:` -- more than one artifact from one set of objects.
 *
 * A module has always been able to produce one thing. Several
 * executables sharing a directory had to be several modules, which
 * meant several descriptors and several copies of the same compile
 * configuration for sources that were already sitting together.
 *
 * Each executable names its entry source; it links against that
 * object plus every object in the module that defines no entry point.
 * A library output takes the no-entry-point set. Nothing lists files,
 * because the symbol table already knows which objects are which. */
typedef struct {
    NowOutput *items;
    size_t     count;
    size_t     cap;
} NowOutputList;

/* Per-field inheritance policy (§1.11).
 *
 * Every field is tri-state: 1 = inherit, 0 = don't, -1 = unstated (the
 * caller falls back to the next policy level, and finally to the spec
 * default of `true`).
 *
 * Resolution order for a given field, first stated value wins:
 *   module `inherit:` -> root `inherit_defaults:` -> true
 *
 * `all` carries the `inherit: *` / `inherit: null` shorthands, which
 * answer for every field at once.
 *
 * Note: `profiles` and `properties` are parsed so a descriptor that
 * names them keeps its meaning, but neither subsystem exists in the
 * implementation yet, so inheriting them is currently a no-op. */
typedef struct {
    int declared;          /* the block was present at all */
    int all;               /* `*` -> 1, null -> 0, absent -> -1 */
    int version;
    int deps;
    int compile;
    int link;
    int profiles;          /* accepted; profiles are not implemented */
    int properties;        /* accepted; properties are not implemented */
} NowInherit;

/* Dependency entry (§1.6) */
typedef struct {
    char *id;              /* group:artifact:version-or-range */
    char *scope;           /* compile | test | provided | runtime */
    int   optional;
    int   is_volatile;
    int   override;
    int   is_workspace_local;  /* set by workspace_init when this dep
                                 * resolves to a sibling module; tells
                                 * procure to skip its parallel inject
                                 * path so we don't double-add -l flags. */
    NowStrArray exclude;
} NowDep;

/* Dependency list */
typedef struct {
    NowDep *items;
    size_t  count;
    size_t  capacity;
} NowDepArray;

NOW_API void now_deparray_init(NowDepArray *a);
NOW_API int  now_deparray_push(NowDepArray *a);  /* push empty, returns index or -1 */
NOW_API void now_deparray_free(NowDepArray *a);

/* Repository entry (§1.7) */
typedef struct {
    char *url;
    char *id;
    int   release;
    int   snapshot;
    char *auth;
} NowRepo;

typedef struct {
    NowRepo *items;
    size_t   count;
    size_t   capacity;
} NowRepoArray;

NOW_API void now_repoarray_init(NowRepoArray *a);
NOW_API int  now_repoarray_push(NowRepoArray *a);
NOW_API void now_repoarray_free(NowRepoArray *a);

/* Plugin entry (§10) */
typedef struct {
    char *id;              /* group:artifact:version or "now:embed" etc. */
    char *type;            /* "plugin" | "external" (default: "plugin") */
    char *phase;           /* lifecycle hook name */
    char *timeout;         /* e.g. "120s" (default: "30s") */
    char *run;             /* command template for external tools */
    void *config;          /* PastaValue* — plugin-specific config, opaque */
} NowPlugin;

typedef struct {
    NowPlugin *items;
    size_t     count;
    size_t     capacity;
} NowPluginArray;

NOW_API void now_pluginarray_init(NowPluginArray *a);
NOW_API int  now_pluginarray_push(NowPluginArray *a);  /* push empty, returns index or -1 */
NOW_API void now_pluginarray_free(NowPluginArray *a);

/* Allocate a zero-initialized NowProject */
NOW_API NowProject *now_project_new(void);

/* Java-specific configuration */
typedef struct {
    char *main_class;        /* entry point for executable JARs */
    char *encoding;          /* source encoding (default: UTF-8) */
    NowStrArray classpath;   /* additional classpath entries */
} NowJava;

/* Platform tag dictionary (§11.x) — path-based platform variants.
 *
 * When set, a directory segment under sources.dir whose name (after
 * alias resolution) appears in `tags` is treated as a *platform gate*:
 * the subtree only compiles if the tag is in the build's active set.
 * Nested tags compose with AND (c/amiga/os4/foo.c needs both).
 * Directories whose name is not in `tags` are ordinary subdirs.
 *
 * Aliases canonicalize synonyms (darwin → macos, win32 → windows) so
 * the dict itself only contains canonical names.
 *
 * Empty dict (no `arch:` section in now.pasta) → no gating, every
 * subdir is ordinary. */
typedef struct {
    NowStrArray tags;          /* canonical platform tokens */
    NowStrArray alias_keys;    /* parallel to alias_values; alias[i] -> values[i] */
    NowStrArray alias_values;
} NowArchDict;

/* Canonicalize `name` through the alias map. Returns `name` itself
 * (no alias) or a pointer into the dict's alias_values (stable for
 * the dict's lifetime). */
NOW_API const char *now_arch_dict_resolve(const NowArchDict *d, const char *name);

/* Returns 1 if `name` (after alias resolution) is a known platform
 * tag — i.e. directories with this name act as platform gates.
 * Returns 0 if the dict is empty or `name` isn't listed. */
NOW_API int now_arch_dict_is_gate(const NowArchDict *d, const char *name);

/* The full Project Object Model (matches forward decl in now.h) */
struct NowProject {
    /* Identity (§1.1) */
    char *group;
    char *artifact;
    char *version;
    char *name;
    char *description;
    char *url;
    char *license;

    /* Language (§1.2) */
    NowStrArray langs;
    char *std;

    /* Source layout (§1.3) */
    NowSources sources;
    NowSources tests;

    /* Output (§1.4) */
    NowOutput output;

    /* Compile & link (§1.5) */
    NowCompile compile;
    NowLink    link;

    /* Per-triple overrides (§11.9). Merged into `compile` and `link`
     * above at load time, once, against the target the invocation is
     * building for — so everything downstream of the loader, including
     * the freshness hash and `--explain`, sees one set of flags and
     * cannot disagree with another. The array is kept after merging
     * for diagnostics only. */
    NowTargetFlagsArray target_flags;

    /* Extra files to carry in the package (§24). Draft. */
    NowAssembly assembly;

    /* Dependencies (§1.6) */
    NowDepArray deps;

    /* Repositories (§1.7) */
    NowRepoArray repos;

    /* Convergence policy (§6.11) */
    char *convergence;     /* lowest | highest | exact */

    /* Dep confusion protection (§8) */
    NowStrArray private_groups;  /* group prefixes that must not resolve from public registries */

    /* Plugins (§10) */
    NowPluginArray plugins;

    /* Java-specific (when langs includes "java") */
    NowJava java;

    /* Platform tag dictionary (§11.x) */
    NowArchDict arch;

    /* Components — your own submodules (full control) */
    NowStrArray components;

    /* Vendored — external deps (read-only, sources discovered) */
    NowStrArray vendored;

    /* Workspace (§1.11) */
    NowStrArray modules;
    NowOutputList outputs;         /* §1.4b -- empty means `output:` alone */
    NowInherit  inherit_defaults;  /* root: policy applied to every module */
    NowInherit  inherit;           /* module: override of the root policy */
    char       *workspace_mode;    /* monorepo | aggregate | inferred */

    /* Raw Pasta tree — kept alive for the project lifetime */
    void *_pasta_root;
};

#endif /* NOW_POM_H */
