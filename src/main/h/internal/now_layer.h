/*
 * now_layer.h — Cascading configuration layers (§25)
 *
 * Layers allow org/team/project configuration to cascade with
 * policy enforcement (open vs locked sections).
 */
#ifndef NOW_LAYER_H
#define NOW_LAYER_H

#include "now.h"
#include "now_pom.h"

/* Section policy */
typedef enum {
    NOW_POLICY_OPEN = 0,    /* lower layers may freely override */
    NOW_POLICY_LOCKED       /* overrides produce audit warnings */
} NowSectionPolicy;

/* A single section in a layer document */
typedef struct {
    char             *name;       /* section name: "compile", "repos", etc. */
    NowSectionPolicy  policy;
    char             *description;
    char             *override_reason;
    void             *data;       /* PastaValue* (map) — owned by layer's _root */
} NowLayerSection;

/* A layer source type */
typedef enum {
    NOW_LAYER_BUILTIN = 0,  /* shipped with now */
    NOW_LAYER_FILE,         /* local file path */
    NOW_LAYER_REPO,         /* published artifact (TODO) */
    NOW_LAYER_URL           /* HTTPS URL (TODO) */
} NowLayerSource;

/* A layer in the stack */
typedef struct {
    char             *id;
    NowLayerSource    source;
    char             *path;       /* file path or coordinate */
    NowLayerSection  *sections;
    size_t            section_count;
    size_t            section_cap;
    void             *_root;      /* PastaValue* — owned, freed on layer_free */
} NowLayer;

/* The complete layer stack */
typedef struct {
    NowLayer *layers;
    size_t    count;
    size_t    capacity;
} NowLayerStack;

/* An audit violation */
typedef struct {
    char *section;          /* which section */
    char *locked_by;        /* layer id that locked it */
    char *overridden_by;    /* layer id that overrode */
    char *field;            /* which field(s) */
    char *override_reason;  /* from _override_reason, may be NULL */
    char *code;             /* e.g. "NOW-W0401" */
} NowAuditViolation;

typedef struct {
    NowAuditViolation *items;
    size_t             count;
    size_t             capacity;
} NowAuditReport;

/* ---- Layer stack operations ---- */

/* Initialize a layer stack with the built-in baseline */
NOW_API void now_layer_stack_init(NowLayerStack *stack);

/* Free a layer stack and all layers */
NOW_API void now_layer_stack_free(NowLayerStack *stack);

/* Load a layer from a file and push it onto the stack.
 * Returns 0 on success. */
NOW_API int now_layer_load_file(NowLayerStack *stack, const char *id,
                                 const char *path, NowResult *result);

/* Push a project's config as the top layer (highest specificity).
 * Reads compile, repos, private_groups sections from the project. */
NOW_API int now_layer_push_project(NowLayerStack *stack,
                                    const NowProject *project);

/* Walk filesystem from basedir upward, loading .now-layer.pasta files.
 * Stops at VCS root or home directory. */
NOW_API int now_layer_discover(NowLayerStack *stack, const char *basedir,
                                NowResult *result);

/* ---- Section query ---- */

/* Find a section by name in a layer. Returns NULL if not found. */
NOW_API const NowLayerSection *now_layer_find_section(const NowLayer *layer,
                                                       const char *name);

/* Get the effective (merged) value for a section across the whole stack.
 * Returns a newly allocated PastaValue* map — caller must pasta_free().
 * Also records audit violations if locked sections are overridden. */
NOW_API void *now_layer_merge_section(const NowLayerStack *stack,
                                       const char *section_name,
                                       NowAuditReport *audit);

/* ---- Merge helpers ---- */

/* Merge a string array from layer section into an existing array.
 * Handles !exclude: entries in open policy. */
NOW_API void now_layer_merge_strarray(NowStrArray *dst,
                                       const NowStrArray *src,
                                       NowSectionPolicy policy);

/* Print locked-section violations and return how many there were.
 * Zero means the layers agreed; a caller under --strict treats any
 * non-zero as fatal. */
NOW_API size_t now_layer_report_violations(const NowAuditReport *report,
                                           const char *who);

/* ---- Apply layers to a project ---- */

/* Merge every discovered `.now-layer.pasta` above `basedir` into the
 * project's compile and link configuration, so the layers actually
 * reach the compile line rather than only `layers:show`.
 *
 * Does nothing at all when no layer file was found -- a project that
 * never asked for layers must build byte-identically to before. Records
 * locked-section overrides in `audit`; the caller decides whether a
 * violation is a warning or a failure.
 *
 * Returns 0 on success (including the nothing-to-do case), -1 on bad
 * arguments. */
NOW_API int now_layer_apply_to_project(NowProject *p, const char *basedir,
                                       NowAuditReport *audit,
                                       NowResult *result);

/* ---- Where a resolved value came from ---- */

/* One resolved configuration value and the layer it came from.
 *
 * `origin` is the layer FILE PATH for a discovered `.now-layer.pasta`,
 * or "project" / "now-baseline" for the two synthetic layers. The
 * stack's own ids ("fs-layer-0") are not used here -- they answer a
 * different question than the one being asked. */
typedef struct {
    char *section;   /* "compile" | "link" */
    char *key;       /* "defines", "flags", "opt", ... */
    char *value;     /* one array entry, or the scalar */
    char *origin;
} NowConfigOrigin;

typedef struct {
    NowConfigOrigin *items;
    size_t           count;
    size_t           cap;
} NowConfigOrigins;

/* Work out where every compile/link value in `basedir`'s project would
 * come from. Re-reads the descriptor unmerged, because by the time
 * anything can ask, the layers have already been merged into the live
 * project and it would answer "project" for everything.
 *
 * Array entries are attributed to the lowest layer carrying them (who
 * introduced it); scalars to the highest layer setting them (who won).
 *
 * Returns 0 on success. Caller frees with now_config_origins_free(). */
NOW_API int now_layer_collect_origins(NowConfigOrigins *dst,
                                      const char *basedir,
                                      NowResult *result);

NOW_API void now_config_origins_free(NowConfigOrigins *o);

/* ---- Audit ---- */

NOW_API void now_audit_init(NowAuditReport *report);
NOW_API void now_audit_free(NowAuditReport *report);

/* Format audit report as text. Returns malloc'd string. */
NOW_API char *now_audit_format(const NowAuditReport *report);

#endif /* NOW_LAYER_H */
