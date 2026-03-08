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

/* ---- Audit ---- */

NOW_API void now_audit_init(NowAuditReport *report);
NOW_API void now_audit_free(NowAuditReport *report);

/* Format audit report as text. Returns malloc'd string. */
NOW_API char *now_audit_format(const NowAuditReport *report);

#endif /* NOW_LAYER_H */
