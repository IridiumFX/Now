/*
 * now_layer.c — Cascading configuration layers (§25)
 *
 * Implements layer loading, section merge, and audit trail.
 */
#include "now_layer.h"
#include "now_fs.h"
#include "pasta.h"
#include "alforno_internal.h"  /* alf_value_clone, alf_map_merge */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if !defined(PATH_MAX) || PATH_MAX < 4096
  #undef PATH_MAX
  #define PATH_MAX 4096
#endif

#ifdef _WIN32
  #include <direct.h>
  #define getcwd_compat _getcwd
#else
  #include <unistd.h>
  #define getcwd_compat getcwd
#endif

/* ---- Audit operations ---- */

NOW_API void now_audit_init(NowAuditReport *report) {
    memset(report, 0, sizeof(*report));
}

static void audit_violation_free(NowAuditViolation *v) {
    free(v->section);
    free(v->locked_by);
    free(v->overridden_by);
    free(v->field);
    free(v->override_reason);
    free(v->code);
}

NOW_API void now_audit_free(NowAuditReport *report) {
    for (size_t i = 0; i < report->count; i++)
        audit_violation_free(&report->items[i]);
    free(report->items);
    memset(report, 0, sizeof(*report));
}

static int audit_push(NowAuditReport *report, const char *section,
                       const char *locked_by, const char *overridden_by,
                       const char *field, const char *reason) {
    if (!report) return 0;
    if (report->count >= report->capacity) {
        size_t new_cap = report->capacity ? report->capacity * 2 : 8;
        NowAuditViolation *tmp = realloc(report->items,
                                          new_cap * sizeof(NowAuditViolation));
        if (!tmp) return -1;
        report->items = tmp;
        report->capacity = new_cap;
    }
    NowAuditViolation *v = &report->items[report->count];
    memset(v, 0, sizeof(*v));
    v->section       = section ? strdup(section) : NULL;
    v->locked_by     = locked_by ? strdup(locked_by) : NULL;
    v->overridden_by = overridden_by ? strdup(overridden_by) : NULL;
    v->field         = field ? strdup(field) : NULL;
    v->override_reason = reason ? strdup(reason) : NULL;
    v->code          = strdup("NOW-W0401");
    report->count++;
    return 0;
}

NOW_API char *now_audit_format(const NowAuditReport *report) {
    if (!report || report->count == 0) {
        char *s = strdup("No advisory lock violations.\n");
        return s;
    }

    /* Estimate buffer size */
    size_t bufsize = 256 + report->count * 512;
    char *buf = (char *)malloc(bufsize);
    if (!buf) return NULL;

    int pos = 0;
    pos += snprintf(buf + pos, bufsize - (size_t)pos,
                    "Advisory lock violations in effective configuration:\n\n");

    for (size_t i = 0; i < report->count; i++) {
        const NowAuditViolation *v = &report->items[i];
        pos += snprintf(buf + pos, bufsize - (size_t)pos,
                        "  Section: %s\n"
                        "  Locked by:     %s\n"
                        "  Overridden by: %s\n"
                        "  Field: %s\n",
                        v->section ? v->section : "?",
                        v->locked_by ? v->locked_by : "?",
                        v->overridden_by ? v->overridden_by : "?",
                        v->field ? v->field : "?");
        if (v->override_reason)
            pos += snprintf(buf + pos, bufsize - (size_t)pos,
                            "  Override reason: %s\n", v->override_reason);
        pos += snprintf(buf + pos, bufsize - (size_t)pos,
                        "  Code: %s\n\n", v->code ? v->code : "NOW-W0401");
    }

    pos += snprintf(buf + pos, bufsize - (size_t)pos,
                    "%zu violation(s).\n", report->count);
    return buf;
}

/* ---- Layer section operations ---- */

static void layer_section_free(NowLayerSection *s) {
    free(s->name);
    free(s->description);
    free(s->override_reason);
    /* data is owned by layer's _root — do not free */
}

static int layer_add_section(NowLayer *layer, const char *name,
                              NowSectionPolicy policy, const char *desc,
                              const char *override_reason, void *data) {
    if (layer->section_count >= layer->section_cap) {
        size_t new_cap = layer->section_cap ? layer->section_cap * 2 : 8;
        NowLayerSection *tmp = realloc(layer->sections,
                                        new_cap * sizeof(NowLayerSection));
        if (!tmp) return -1;
        layer->sections = tmp;
        layer->section_cap = new_cap;
    }
    NowLayerSection *s = &layer->sections[layer->section_count];
    memset(s, 0, sizeof(*s));
    s->name = strdup(name);
    s->policy = policy;
    s->description = desc ? strdup(desc) : NULL;
    s->override_reason = override_reason ? strdup(override_reason) : NULL;
    s->data = data;
    layer->section_count++;
    return 0;
}

/* ---- Layer operations ---- */

static void layer_free(NowLayer *layer) {
    free(layer->id);
    free(layer->path);
    for (size_t i = 0; i < layer->section_count; i++)
        layer_section_free(&layer->sections[i]);
    free(layer->sections);
    if (layer->_root)
        pasta_free((PastaValue *)layer->_root);
    memset(layer, 0, sizeof(*layer));
}

static int stack_push_layer(NowLayerStack *stack) {
    if (stack->count >= stack->capacity) {
        size_t new_cap = stack->capacity ? stack->capacity * 2 : 4;
        NowLayer *tmp = realloc(stack->layers, new_cap * sizeof(NowLayer));
        if (!tmp) return -1;
        stack->layers = tmp;
        stack->capacity = new_cap;
    }
    memset(&stack->layers[stack->count], 0, sizeof(NowLayer));
    return (int)stack->count++;
}

/* ---- Built-in baseline layer ---- */

static void init_baseline(NowLayer *layer) {
    layer->id = strdup("now-baseline");
    layer->source = NOW_LAYER_BUILTIN;

    /* Build baseline sections using Pasta values */
    PastaValue *root = pasta_new_map();
    layer->_root = root;

    /* @compile: open, and deliberately EMPTY.
     *
     * This used to declare `warnings: [Wall, Wextra]` and `opt: debug`,
     * which `now` does not do: now_build.c maps Wall/Wextra only when
     * they are already in compile.warnings, and emits -Og only when opt
     * is set. So the baseline was describing a tool that does not
     * exist -- harmless while layers only fed a report, and three
     * unrequested flags for every project on the machine the moment
     * they fed a build. A baseline that lies is worse than no baseline;
     * the section stays so layers have something to merge onto. */
    PastaValue *compile = pasta_new_map();
    pasta_set(root, "compile", compile);
    layer_add_section(layer, "compile", NOW_POLICY_OPEN,
                      "Compiler settings (no built-in defaults)", NULL, compile);

    /* @repos: open, central registry */
    PastaValue *repos = pasta_new_map();
    PastaValue *registries = pasta_new_array();
    PastaValue *central = pasta_new_map();
    pasta_set(central, "url", pasta_new_string("https://registry.now.build"));
    pasta_set(central, "id", pasta_new_string("central"));
    pasta_set(central, "release", pasta_new_bool(1));
    pasta_set(central, "snapshot", pasta_new_bool(0));
    pasta_push(registries, central);
    pasta_set(repos, "registries", registries);
    pasta_set(root, "repos", repos);
    layer_add_section(layer, "repos", NOW_POLICY_OPEN,
                      "Default package registry", NULL, repos);

    /* @toolchain: open, gcc default */
    PastaValue *tc = pasta_new_map();
    pasta_set(tc, "preset", pasta_new_string("gcc"));
    pasta_set(root, "toolchain", tc);
    layer_add_section(layer, "toolchain", NOW_POLICY_OPEN,
                      "Default toolchain", NULL, tc);

    /* @advisory: locked, default phase guards */
    PastaValue *adv = pasta_new_map();
    PastaValue *guards = pasta_new_map();
    pasta_set(guards, "critical", pasta_new_string("error"));
    pasta_set(guards, "high", pasta_new_string("warn"));
    pasta_set(guards, "medium", pasta_new_string("note"));
    pasta_set(guards, "low", pasta_new_string("note"));
    pasta_set(adv, "phase_guards", guards);
    pasta_set(root, "advisory", adv);
    layer_add_section(layer, "advisory", NOW_POLICY_LOCKED,
                      "Default advisory phase guards", NULL, adv);

    /* @private_groups: open, empty */
    PastaValue *pg = pasta_new_map();
    PastaValue *groups = pasta_new_array();
    pasta_set(pg, "groups", groups);
    pasta_set(root, "private_groups", pg);
    layer_add_section(layer, "private_groups", NOW_POLICY_OPEN,
                      "Private group enforcement", NULL, pg);

    /* @link: open, empty */
    PastaValue *link = pasta_new_map();
    pasta_set(root, "link", link);
    layer_add_section(layer, "link", NOW_POLICY_OPEN, NULL, NULL, link);
}

/* ---- Public API ---- */

NOW_API void now_layer_stack_init(NowLayerStack *stack) {
    memset(stack, 0, sizeof(*stack));
    /* Push baseline as first layer */
    int idx = stack_push_layer(stack);
    if (idx >= 0)
        init_baseline(&stack->layers[idx]);
}

NOW_API void now_layer_stack_free(NowLayerStack *stack) {
    for (size_t i = 0; i < stack->count; i++)
        layer_free(&stack->layers[i]);
    free(stack->layers);
    memset(stack, 0, sizeof(*stack));
}

/* Parse a layer file: a Pasta map where top-level keys are section names.
 * Each section value is a map optionally containing _policy, _description. */
static int parse_layer_document(NowLayer *layer, PastaValue *root) {
    if (!root || pasta_type(root) != PASTA_MAP) return -1;

    layer->_root = root;

    size_t nkeys = pasta_count(root);
    for (size_t i = 0; i < nkeys; i++) {
        const char *key = pasta_map_key(root, i);
        const PastaValue *val = pasta_map_value(root, i);
        if (!key || !val || pasta_type(val) != PASTA_MAP) continue;

        NowSectionPolicy policy = NOW_POLICY_OPEN;
        const char *desc = NULL;
        const char *reason = NULL;

        const PastaValue *pv = pasta_map_get(val, "_policy");
        if (pv && pasta_type(pv) == PASTA_STRING) {
            const char *ps = pasta_get_string(pv);
            if (strcmp(ps, "locked") == 0)
                policy = NOW_POLICY_LOCKED;
        }

        const PastaValue *dv = pasta_map_get(val, "_description");
        if (dv && pasta_type(dv) == PASTA_STRING)
            desc = pasta_get_string(dv);

        const PastaValue *rv = pasta_map_get(val, "_override_reason");
        if (rv && pasta_type(rv) == PASTA_STRING)
            reason = pasta_get_string(rv);

        layer_add_section(layer, key, policy, desc, reason, (void *)val);
    }

    return 0;
}

NOW_API int now_layer_load_file(NowLayerStack *stack, const char *id,
                                 const char *path, NowResult *result) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot open layer: %s", path);
        }
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t nread = fread(buf, 1, (size_t)len, fp);
    buf[nread] = '\0';
    fclose(fp);

    PastaResult pr;
    PastaValue *root = pasta_parse(buf, nread, &pr);
    free(buf);

    if (!root || pr.code != PASTA_OK) {
        if (result) {
            result->code = NOW_ERR_SYNTAX;
            snprintf(result->message, sizeof(result->message),
                     "layer %s: %s (line %d)", path, pr.message, pr.line);
        }
        return -1;
    }

    int idx = stack_push_layer(stack);
    if (idx < 0) { pasta_free(root); return -1; }

    NowLayer *layer = &stack->layers[idx];
    layer->id = strdup(id);
    layer->source = NOW_LAYER_FILE;
    layer->path = strdup(path);

    if (parse_layer_document(layer, root) != 0) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "layer %s: invalid document format", path);
        }
        return -1;
    }

    return 0;
}

NOW_API int now_layer_push_project(NowLayerStack *stack,
                                    const NowProject *project) {
    if (!stack || !project) return -1;

    int idx = stack_push_layer(stack);
    if (idx < 0) return -1;

    NowLayer *layer = &stack->layers[idx];
    layer->id = strdup("project");
    layer->source = NOW_LAYER_FILE;

    /* Build sections from project fields */
    PastaValue *root = pasta_new_map();
    layer->_root = root;

    /* compile section */
    PastaValue *compile = pasta_new_map();
    if (project->compile.warnings.count > 0) {
        PastaValue *w = pasta_new_array();
        for (size_t i = 0; i < project->compile.warnings.count; i++)
            pasta_push(w, pasta_new_string(project->compile.warnings.items[i]));
        pasta_set(compile, "warnings", w);
    }
    if (project->compile.defines.count > 0) {
        PastaValue *d = pasta_new_array();
        for (size_t i = 0; i < project->compile.defines.count; i++)
            pasta_push(d, pasta_new_string(project->compile.defines.items[i]));
        pasta_set(compile, "defines", d);
    }
    if (project->compile.flags.count > 0) {
        PastaValue *f = pasta_new_array();
        for (size_t i = 0; i < project->compile.flags.count; i++)
            pasta_push(f, pasta_new_string(project->compile.flags.items[i]));
        pasta_set(compile, "flags", f);
    }
    if (project->compile.includes.count > 0) {
        PastaValue *inc = pasta_new_array();
        for (size_t i = 0; i < project->compile.includes.count; i++)
            pasta_push(inc, pasta_new_string(project->compile.includes.items[i]));
        pasta_set(compile, "includes", inc);
    }
    if (project->compile.std)
        pasta_set(compile, "std", pasta_new_string(project->compile.std));
    if (project->compile.opt)
        pasta_set(compile, "opt", pasta_new_string(project->compile.opt));
    pasta_set(root, "compile", compile);
    layer_add_section(layer, "compile", NOW_POLICY_OPEN, NULL, NULL, compile);

    /* link section. The baseline declares @link and layers may set it,
     * but the project's own link config was never pushed -- so a layer
     * merged against nothing and a project's libs were invisible to the
     * policy that was supposed to govern them. */
    {
        PastaValue *link = pasta_new_map();
        int any = 0;
        struct { const char *key; const NowStrArray *arr; } lk[] = {
            { "flags",    &project->link.flags    },
            { "libs",     &project->link.libs     },
            { "libdirs",  &project->link.libdirs  },
            { "archives", &project->link.archives },
        };
        for (size_t k = 0; k < sizeof(lk) / sizeof(lk[0]); k++) {
            if (lk[k].arr->count == 0) continue;
            PastaValue *a = pasta_new_array();
            for (size_t i = 0; i < lk[k].arr->count; i++)
                pasta_push(a, pasta_new_string(lk[k].arr->items[i]));
            pasta_set(link, lk[k].key, a);
            any = 1;
        }
        if (any) {
            pasta_set(root, "link", link);
            layer_add_section(layer, "link", NOW_POLICY_OPEN, NULL, NULL, link);
        } else {
            pasta_free(link);
        }
    }

    /* private_groups section */
    if (project->private_groups.count > 0) {
        PastaValue *pg = pasta_new_map();
        PastaValue *groups = pasta_new_array();
        for (size_t i = 0; i < project->private_groups.count; i++)
            pasta_push(groups, pasta_new_string(project->private_groups.items[i]));
        pasta_set(pg, "groups", groups);
        pasta_set(root, "private_groups", pg);
        layer_add_section(layer, "private_groups", NOW_POLICY_OPEN, NULL, NULL, pg);
    }

    return 0;
}

NOW_API int now_layer_discover(NowLayerStack *stack, const char *basedir,
                                NowResult *result) {
    if (!stack || !basedir) return -1;

    /* Collect .now-layer.pasta paths from basedir upward.
     * We need to load them farthest-first (lower priority) then
     * closest-to-project (higher priority), so collect all paths first. */
    char **paths = NULL;
    size_t npaths = 0;
    size_t path_cap = 0;

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", basedir);

    /* Get home directory for stop condition */
    const char *home = now_home_dir();  /* one convention — see now_fs.h */

    for (int depth = 0; depth < 64; depth++) {
        /* Check for VCS root — stop */
        char *git = now_path_join(dir, ".git");
        int is_vcs = git && now_path_exists(git);
        free(git);

        /* Check for .now-layer.pasta */
        char *layer_path = now_path_join(dir, ".now-layer.pasta");
        if (layer_path && now_path_exists(layer_path)) {
            if (npaths >= path_cap) {
                path_cap = path_cap ? path_cap * 2 : 4;
                paths = realloc(paths, path_cap * sizeof(char *));
            }
            paths[npaths++] = layer_path;
        } else {
            free(layer_path);
        }

        if (is_vcs) break;

        /* Check home dir stop */
        if (home && strcmp(dir, home) == 0) break;

        /* Go up one directory */
        char *sep = strrchr(dir, '/');
#ifdef _WIN32
        char *sep2 = strrchr(dir, '\\');
        if (sep2 && (!sep || sep2 > sep)) sep = sep2;
#endif
        if (!sep || sep == dir) break;
        *sep = '\0';
    }

    /* Load in reverse order (farthest first = lowest priority) */
    for (int i = (int)npaths - 1; i >= 0; i--) {
        char id[256];
        snprintf(id, sizeof(id), "fs-layer-%d", (int)(npaths - 1 - (size_t)i));
        now_layer_load_file(stack, id, paths[i], result);
        free(paths[i]);
    }
    free(paths);

    return 0;
}

NOW_API const NowLayerSection *now_layer_find_section(const NowLayer *layer,
                                                       const char *name) {
    if (!layer || !name) return NULL;
    for (size_t i = 0; i < layer->section_count; i++) {
        if (strcmp(layer->sections[i].name, name) == 0)
            return &layer->sections[i];
    }
    return NULL;
}

/* ---- Section merge ---- */

NOW_API void now_layer_merge_strarray(NowStrArray *dst,
                                       const NowStrArray *src,
                                       NowSectionPolicy policy) {
    if (!dst || !src) return;
    for (size_t i = 0; i < src->count; i++) {
        const char *s = src->items[i];
        if (!s) continue;

        /* Handle !exclude: prefix in open sections */
        if (policy == NOW_POLICY_OPEN && strncmp(s, "!exclude:", 9) == 0) {
            const char *to_remove = s + 9;
            /* Remove from dst */
            for (size_t j = 0; j < dst->count; j++) {
                if (dst->items[j] && strcmp(dst->items[j], to_remove) == 0) {
                    free(dst->items[j]);
                    /* Shift remaining items down */
                    for (size_t k = j; k + 1 < dst->count; k++)
                        dst->items[k] = dst->items[k + 1];
                    dst->count--;
                    break;
                }
            }
            continue;
        }

        /* Don't add duplicates */
        int found = 0;
        for (size_t j = 0; j < dst->count; j++) {
            if (dst->items[j] && strcmp(dst->items[j], s) == 0) {
                found = 1;
                break;
            }
        }
        if (!found)
            now_strarray_push(dst, s);
    }
}

/* Deep-clone via alforno's utility (handles all types incl. labels) */
#define pasta_clone alf_value_clone

/* Check if a key exists in a map */
static int pasta_map_has(const PastaValue *map, const char *key) {
    return pasta_map_get(map, key) != NULL;
}

/* Merge overlay map into base, returning a NEW map (no duplicate keys).
 * Caller frees old base and uses returned map instead.
 * For locked policy, track overridden fields in audit. */
static PastaValue *merge_pasta_maps(const PastaValue *base,
                                     const PastaValue *overlay,
                                     NowSectionPolicy policy,
                                     const char *section_name,
                                     const char *base_layer_id,
                                     const char *overlay_layer_id,
                                     const char *override_reason,
                                     NowAuditReport *audit) {
    if (!base && !overlay) return pasta_new_map();
    if (!overlay) return pasta_clone(base);
    if (!base) return pasta_clone(overlay);
    if (pasta_type(base) != PASTA_MAP || pasta_type(overlay) != PASTA_MAP)
        return pasta_clone(base);

    PastaValue *result = pasta_new_map();

    /* First pass: copy base entries, possibly modified by overlay */
    size_t bn = pasta_count(base);
    for (size_t i = 0; i < bn; i++) {
        const char *bk = pasta_map_key(base, i);
        if (!bk) continue;

        const PastaValue *oval = pasta_map_get(overlay, bk);
        if (oval && bk[0] != '_') {
            /* Overlay has this key — it's an override */
            if (policy == NOW_POLICY_LOCKED) {
                audit_push(audit, section_name, base_layer_id,
                           overlay_layer_id, bk, override_reason);
            }

            const PastaValue *bval = pasta_map_value(base, i);

            if (pasta_type(oval) == PASTA_ARRAY) {
                PastaValue *merged = pasta_new_array();

                if (bval && pasta_type(bval) == PASTA_ARRAY &&
                    policy == NOW_POLICY_LOCKED) {
                    /* Locked: accumulate (keep base + add new from overlay) */
                    size_t blen = pasta_count(bval);
                    for (size_t j = 0; j < blen; j++)
                        pasta_push(merged, pasta_clone(pasta_array_get(bval, j)));

                    size_t olen = pasta_count(oval);
                    for (size_t j = 0; j < olen; j++) {
                        const PastaValue *oe = pasta_array_get(oval, j);
                        if (!oe) continue;
                        if (pasta_type(oe) == PASTA_STRING &&
                            strncmp(pasta_get_string(oe), "!exclude:", 9) == 0)
                            continue; /* !exclude: ignored in locked mode */
                        /* Dedup */
                        int dup = 0;
                        size_t mn = pasta_count(merged);
                        for (size_t k = 0; k < mn; k++) {
                            const PastaValue *me = pasta_array_get(merged, k);
                            if (me && oe && pasta_type(me) == pasta_type(oe) &&
                                pasta_type(me) == PASTA_STRING &&
                                strcmp(pasta_get_string(me), pasta_get_string(oe)) == 0) {
                                dup = 1; break;
                            }
                        }
                        if (!dup) pasta_push(merged, pasta_clone(oe));
                    }
                } else {
                    /* Open: base + overlay with !exclude: support */
                    if (bval && pasta_type(bval) == PASTA_ARRAY) {
                        size_t blen = pasta_count(bval);
                        for (size_t j = 0; j < blen; j++)
                            pasta_push(merged, pasta_clone(pasta_array_get(bval, j)));
                    }
                    size_t olen = pasta_count(oval);
                    for (size_t j = 0; j < olen; j++) {
                        const PastaValue *oe = pasta_array_get(oval, j);
                        if (!oe) continue;
                        if (pasta_type(oe) == PASTA_STRING &&
                            strncmp(pasta_get_string(oe), "!exclude:", 9) == 0) {
                            const char *to_rm = pasta_get_string(oe) + 9;
                            /* Remove from merged by rebuilding */
                            PastaValue *rebuilt = pasta_new_array();
                            size_t mn = pasta_count(merged);
                            for (size_t k = 0; k < mn; k++) {
                                const PastaValue *me = pasta_array_get(merged, k);
                                if (me && pasta_type(me) == PASTA_STRING &&
                                    strcmp(pasta_get_string(me), to_rm) == 0)
                                    continue;
                                pasta_push(rebuilt, pasta_clone(me));
                            }
                            pasta_free(merged);
                            merged = rebuilt;
                        } else {
                            /* Dedup */
                            int dup = 0;
                            size_t mn = pasta_count(merged);
                            for (size_t k = 0; k < mn; k++) {
                                const PastaValue *me = pasta_array_get(merged, k);
                                if (me && oe && pasta_type(me) == pasta_type(oe) &&
                                    pasta_type(me) == PASTA_STRING &&
                                    strcmp(pasta_get_string(me), pasta_get_string(oe)) == 0) {
                                    dup = 1; break;
                                }
                            }
                            if (!dup) pasta_push(merged, pasta_clone(oe));
                        }
                    }
                }
                pasta_set(result, bk, merged);
            } else if (pasta_type(oval) == PASTA_MAP) {
                if (policy == NOW_POLICY_OPEN) {
                    /* Delegate to alforno for open-policy map merge */
                    pasta_set(result, bk, alf_map_merge(bval, oval));
                } else {
                    /* Recursive merge for locked policy (audit violations) */
                    PastaValue *sub = merge_pasta_maps(bval, oval, policy,
                        section_name, base_layer_id, overlay_layer_id,
                        override_reason, audit);
                    pasta_set(result, bk, sub);
                }
            } else {
                /* Scalar: overlay wins */
                pasta_set(result, bk, pasta_clone(oval));
            }
        } else {
            /* No overlay for this key — keep base value */
            pasta_set(result, bk, pasta_clone(pasta_map_value(base, i)));
        }
    }

    /* Second pass: add overlay keys that weren't in base */
    size_t on = pasta_count(overlay);
    for (size_t i = 0; i < on; i++) {
        const char *ok = pasta_map_key(overlay, i);
        if (!ok || ok[0] == '_') continue;
        if (pasta_map_has(base, ok)) continue; /* already handled above */
        pasta_set(result, ok, pasta_clone(pasta_map_value(overlay, i)));
    }

    return result;
}

NOW_API void *now_layer_merge_section(const NowLayerStack *stack,
                                       const char *section_name,
                                       NowAuditReport *audit) {
    if (!stack || !section_name) return NULL;

    /* Start with empty map */
    PastaValue *effective = pasta_new_map();

    /* Track which layer locked this section */
    const char *locked_by_id = NULL;
    NowSectionPolicy effective_policy = NOW_POLICY_OPEN;

    /* Merge from lowest (baseline) to highest (project) */
    for (size_t i = 0; i < stack->count; i++) {
        const NowLayerSection *sec =
            now_layer_find_section(&stack->layers[i], section_name);
        if (!sec) continue;

        /* If this layer sets the policy to locked, record it */
        if (sec->policy == NOW_POLICY_LOCKED && !locked_by_id) {
            locked_by_id = stack->layers[i].id;
            effective_policy = NOW_POLICY_LOCKED;
        }

        /* If effective policy is locked and this is NOT the locking layer,
         * any changes are violations */
        NowSectionPolicy merge_policy = NOW_POLICY_OPEN;
        const char *violation_src = NULL;
        if (effective_policy == NOW_POLICY_LOCKED &&
            locked_by_id &&
            strcmp(stack->layers[i].id, locked_by_id) != 0) {
            merge_policy = NOW_POLICY_LOCKED;
            violation_src = locked_by_id;
        }

        PastaValue *merged = merge_pasta_maps(
            effective, (const PastaValue *)sec->data,
            merge_policy, section_name,
            violation_src ? violation_src : stack->layers[i].id,
            stack->layers[i].id,
            sec->override_reason, audit);
        pasta_free(effective);
        effective = merged;
    }

    return effective;
}

/* ---- the environment and the command line --------------------------
 *
 * Three sources feed a build and these are the last two. They are pushed
 * as LAYERS rather than merged by hand, which is most of the reason the
 * layer stack was worth wiring up: precedence is stack order, the merge
 * rules are the ones already tested, and `now tell config-origin` names
 * them without knowing they exist.
 *
 * Order, lowest priority first:
 *
 *     now-baseline                 (claims nothing)
 *     .now-layer.pasta ...         (farthest first)
 *     project                      (the descriptor)
 *     CFLAGS / LDFLAGS             (the POSIX names, if exported)
 *     NOW_CFLAGS / NOW_LDFLAGS
 *     --cflags / --ldflags
 *
 * One layer per VARIABLE, not one per tier: a value that came from
 * LDFLAGS must not be reported as coming from CFLAGS, and the layer id
 * is what `config-origin` prints. Compile and link never interact, so
 * their relative order in the stack does not matter -- only that each
 * NOW_ name outranks its bare name, and the flag outranks both.
 *
 * Reading plain `CFLAGS` is a deliberate compatibility choice and a
 * deliberate risk: a shell that has had it exported for something else
 * will quietly affect a `now` build. That is exactly why every value
 * can name its source -- the mitigation is that the answer is one
 * command away, not that the case cannot arise.
 */

/* Split a flag string on whitespace, honouring double quotes.
 *
 * Quotes are not decoration: `-IC:\Program Files\x\include` is an
 * ordinary include path on this machine, and splitting it on spaces
 * yields three flags, none of which is a directory. */
static void split_flags(NowStrArray *dst, const char *s) {
    const char *p = s;
    char buf[4096];
    size_t n = 0;
    int in_quote = 0;

    if (!s) return;
    for (;;) {
        char c = *p;
        if (c == '"') { in_quote = !in_quote; p++; continue; }
        if (c == '\0' || (!in_quote && (c == ' ' || c == '\t'))) {
            if (n > 0) { buf[n] = '\0'; now_strarray_push(dst, buf); n = 0; }
            if (c == '\0') break;
            p++;
            continue;
        }
        if (n + 1 < sizeof(buf)) buf[n++] = c;
        p++;
    }
}

/* One layer carrying one variable's flags into one section.
 * Returns 1 if a layer was pushed, 0 if there was nothing to say. */
static int push_flag_layer(NowLayerStack *stack, const char *id,
                           const char *section, const char *flags) {
    NowStrArray a;
    PastaValue *root, *sec, *arr;
    int idx;
    size_t i;

    if (!flags || !*flags) return 0;

    now_strarray_init(&a);
    split_flags(&a, flags);
    if (a.count == 0) { now_strarray_free(&a); return 0; }

    idx = stack_push_layer(stack);
    if (idx < 0) { now_strarray_free(&a); return 0; }

    arr = pasta_new_array();
    for (i = 0; i < a.count; i++)
        pasta_push(arr, pasta_new_string(a.items[i]));
    now_strarray_free(&a);

    sec  = pasta_new_map();
    pasta_set(sec, "flags", arr);
    root = pasta_new_map();
    pasta_set(root, section, sec);

    stack->layers[idx].id = strdup(id);
    /* Not NOW_LAYER_FILE: these have no path, and the gate asks "was a
     * layer file found" by looking at exactly that. */
    stack->layers[idx].source = NOW_LAYER_BUILTIN;
    stack->layers[idx]._root  = root;
    layer_add_section(&stack->layers[idx], section,
                      NOW_POLICY_OPEN, NULL, NULL, sec);
    return 1;
}

/* Command-line flags, set once by the CLI before anything builds.
 *
 * A static rather than a parameter because a workspace builds its
 * modules through now_workspace.c, which has no argv and should not
 * grow one to carry a flag through. `--cflags` means "this
 * invocation", and an invocation is exactly what a process is. */
static char *g_cli_cflags  = NULL;
static char *g_cli_ldflags = NULL;

NOW_API void now_layer_set_cli_flags(const char *cflags, const char *ldflags) {
    free(g_cli_cflags);
    free(g_cli_ldflags);
    g_cli_cflags  = cflags  ? strdup(cflags)  : NULL;
    g_cli_ldflags = ldflags ? strdup(ldflags) : NULL;
}

/* Push the env and CLI layers. Returns how many were pushed, so the
 * gate can ask whether anything at all wants to change this build. */
static int push_env_and_cli_layers(NowLayerStack *stack) {
    int n = 0;
    n += push_flag_layer(stack, "CFLAGS",      "compile", getenv("CFLAGS"));
    n += push_flag_layer(stack, "LDFLAGS",     "link",    getenv("LDFLAGS"));
    n += push_flag_layer(stack, "NOW_CFLAGS",  "compile", getenv("NOW_CFLAGS"));
    n += push_flag_layer(stack, "NOW_LDFLAGS", "link",    getenv("NOW_LDFLAGS"));
    n += push_flag_layer(stack, "--cflags",    "compile", g_cli_cflags);
    n += push_flag_layer(stack, "--ldflags",   "link",    g_cli_ldflags);
    return n;
}

/* Print locked-section violations, if any, and say how many there were.
 *
 * Returns the violation count so a caller can decide policy: a build
 * warns, `--strict` refuses. Warning by default is the same call
 * `schema:check` makes -- a policy an org set six months ago should not
 * stop someone's build the first time they hit it, but it must never be
 * silent either, because a silently-dropped override is how a locked
 * section becomes decorative. */
NOW_API size_t now_layer_report_violations(const NowAuditReport *report,
                                           const char *who) {
    char *text;
    if (!report || report->count == 0) return 0;
    text = now_audit_format(report);
    if (text) {
        fprintf(stderr, "  layers: %llu locked-section override%s%s%s\n%s",
                (unsigned long long)report->count,
                report->count == 1 ? "" : "s",
                who ? " in " : "", who ? who : "", text);
        free(text);
    }
    return report->count;
}

/* ---- layers reach the build ----------------------------------------
 *
 * Until 2026-08-25 this whole subsystem configured a report and nothing
 * else. `now_layer_merge_section()` had exactly two callers and both
 * were the `layers:*` commands; `now_build.c` never mentioned layers.
 * A `.now-layer.pasta` carrying `compile: { defines: [X] }` showed up
 * in `layers:show --effective` and then the build failed on a source
 * that required X. An org layer was a document about a build, not an
 * input to one.
 *
 * Two things had to be true before wiring it in could be safe.
 *
 * The first is the gate below: a project with no `.now-layer.pasta`
 * anywhere above it must build exactly as it did before. Not
 * "equivalently" -- identically. Same discipline as the workspace
 * inheritance gate, and for the same reason: a machine full of
 * descriptors that never asked for this must not acquire it because a
 * feature landed.
 *
 * The second was that the built-in baseline had to stop claiming
 * defaults `now` does not have. It declared `warnings: [Wall, Wextra]`
 * and `opt: debug`, and `now_build.c` applies neither unless the
 * descriptor asks -- it maps Wall/Wextra only when they are already in
 * `compile.warnings`, and `-Og` only when `opt` is set. So merging the
 * baseline into a build would have handed every project on the machine
 * three flags it never wrote, including an optimisation level. That
 * was not a merge hazard to design around; it was the baseline being
 * wrong about its own tool, and it is fixed rather than worked around.
 */

/* Replace a NowStrArray from a merged Pasta array. Absent key means the
 * layers had nothing to say, and the project keeps what it had. */
static void strarray_from_pasta(NowStrArray *dst, const PastaValue *arr) {
    size_t i, n;
    if (!dst || !arr || pasta_type(arr) != PASTA_ARRAY) return;
    now_strarray_free(dst);
    now_strarray_init(dst);
    n = pasta_count(arr);
    for (i = 0; i < n; i++) {
        const PastaValue *e = pasta_array_get(arr, i);
        if (e && pasta_type(e) == PASTA_STRING)
            now_strarray_push(dst, pasta_get_string(e));
    }
}

static void str_from_pasta(char **dst, const PastaValue *v) {
    if (!dst || !v || pasta_type(v) != PASTA_STRING) return;
    free(*dst);
    *dst = strdup(pasta_get_string(v));
}

NOW_API int now_layer_apply_to_project(NowProject *p, const char *basedir,
                                       NowAuditReport *audit,
                                       NowResult *result) {
    NowLayerStack stack;
    PastaValue *eff;
    size_t i;
    int have_file_layer = 0;
    int have_flag_layer = 0;

    if (!p || !basedir) return -1;

    now_layer_stack_init(&stack);
    now_layer_discover(&stack, basedir, result);

    /* The gate. `now_layer_discover` only ever pushes NOW_LAYER_FILE
     * layers, and the baseline is NOW_LAYER_BUILTIN, so this asks
     * exactly "did we find a .now-layer.pasta". Checked before the
     * project is pushed, because that goes on as a FILE layer too. */
    for (i = 0; i < stack.count; i++) {
        if (stack.layers[i].source == NOW_LAYER_FILE) { have_file_layer = 1; break; }
    }

    now_layer_push_project(&stack, p);

    /* Env and CLI sit above the descriptor. The gate covers all three
     * sources now: with no layer file, no CFLAGS/LDFLAGS in the
     * environment and no --cflags, nothing wants to change this build
     * and it is left exactly as it was. */
    have_flag_layer = push_env_and_cli_layers(&stack);
    if (!have_file_layer && !have_flag_layer) {
        now_layer_stack_free(&stack);
        return 0;
    }

    /* compile: the merged section already contains the project's own
     * values -- it is the top layer -- so this replaces rather than
     * appends. Appending would double every flag the project wrote. */
    eff = (PastaValue *)now_layer_merge_section(&stack, "compile", audit);
    if (eff) {
        strarray_from_pasta(&p->compile.flags,    pasta_map_get(eff, "flags"));
        strarray_from_pasta(&p->compile.warnings, pasta_map_get(eff, "warnings"));
        strarray_from_pasta(&p->compile.defines,  pasta_map_get(eff, "defines"));
        strarray_from_pasta(&p->compile.includes, pasta_map_get(eff, "includes"));
        str_from_pasta(&p->compile.std, pasta_map_get(eff, "std"));
        str_from_pasta(&p->compile.opt, pasta_map_get(eff, "opt"));
        pasta_free(eff);
    }

    eff = (PastaValue *)now_layer_merge_section(&stack, "link", audit);
    if (eff) {
        strarray_from_pasta(&p->link.flags,    pasta_map_get(eff, "flags"));
        strarray_from_pasta(&p->link.libs,     pasta_map_get(eff, "libs"));
        strarray_from_pasta(&p->link.libdirs,  pasta_map_get(eff, "libdirs"));
        strarray_from_pasta(&p->link.archives, pasta_map_get(eff, "archives"));
        pasta_free(eff);
    }

    now_layer_stack_free(&stack);
    return 0;
}

/* ---- where a resolved value came from ------------------------------
 *
 * Once layers feed the build, "why is this flag here" stops having an
 * obvious answer: a define on the compile line might be the project's,
 * a team layer's, or an org layer's three directories up. `now tell
 * config-origin` answers it.
 *
 * This recomputes rather than recording. By the time any phase runs,
 * `now_layer_apply_to_project()` has already merged the layers INTO the
 * project, so asking the live project where its values came from would
 * answer "the project" for every one of them. So the descriptor is
 * re-read from disk in its unmerged state and the stack rebuilt around
 * it. That costs a few file reads on an introspection command nobody
 * runs in a loop, and it keeps the apply path free of bookkeeping that
 * exists only for a report -- which is the mistake this whole subsystem
 * was already making.
 *
 * Two different questions, depending on kind:
 *
 *   - An ARRAY entry is attributed to the LOWEST layer carrying it,
 *     because arrays accumulate and the interesting fact is who
 *     introduced the value.
 *   - A SCALAR is attributed to the HIGHEST layer setting it, because
 *     scalars replace and the interesting fact is who won.
 */

static const char *layer_label(const NowLayer *l) {
    if (!l) return "?";
    /* `fs-layer-0` is what the stack calls a discovered file, which
     * tells a reader nothing. The path is the answer to the question
     * actually being asked. */
    if (l->source == NOW_LAYER_FILE && l->path) return l->path;
    return l->id ? l->id : "?";
}

static int section_array_has(const NowLayerSection *s, const char *key,
                             const char *value) {
    const PastaValue *arr;
    size_t j, n;
    if (!s) return 0;
    arr = pasta_map_get((const PastaValue *)s->data, key);
    if (!arr || pasta_type(arr) != PASTA_ARRAY) return 0;
    n = pasta_count(arr);
    for (j = 0; j < n; j++) {
        const PastaValue *e = pasta_array_get(arr, j);
        if (e && pasta_type(e) == PASTA_STRING &&
            strcmp(pasta_get_string(e), value) == 0) return 1;
    }
    return 0;
}

static const char *section_scalar(const NowLayerSection *s, const char *key) {
    const PastaValue *v;
    if (!s) return NULL;
    v = pasta_map_get((const PastaValue *)s->data, key);
    if (!v || pasta_type(v) != PASTA_STRING) return NULL;
    return pasta_get_string(v);
}

static int origins_push(NowConfigOrigins *dst, const char *section,
                        const char *key, const char *value, const char *origin) {
    NowConfigOrigin *it;
    if (dst->count >= dst->cap) {
        size_t nc = dst->cap ? dst->cap * 2 : 16;
        NowConfigOrigin *tmp = realloc(dst->items, nc * sizeof(*tmp));
        if (!tmp) return -1;
        dst->items = tmp;
        dst->cap = nc;
    }
    it = &dst->items[dst->count++];
    it->section = strdup(section);
    it->key     = strdup(key);
    it->value   = strdup(value ? value : "");
    it->origin  = strdup(origin ? origin : "?");
    return 0;
}

static void collect_section(NowConfigOrigins *dst, const NowLayerStack *st,
                            const char *section, const char *const *arr_keys,
                            const char *const *scalar_keys,
                            NowAuditReport *audit) {
    PastaValue *eff = (PastaValue *)now_layer_merge_section(st, section, audit);
    size_t k;
    if (!eff) return;

    for (k = 0; arr_keys[k]; k++) {
        const PastaValue *a = pasta_map_get(eff, arr_keys[k]);
        size_t i, n;
        if (!a || pasta_type(a) != PASTA_ARRAY) continue;
        n = pasta_count(a);
        for (i = 0; i < n; i++) {
            const PastaValue *e = pasta_array_get(a, i);
            const char *val, *who = "?";
            size_t li;
            if (!e || pasta_type(e) != PASTA_STRING) continue;
            val = pasta_get_string(e);
            /* lowest layer carrying it — who introduced the value */
            for (li = 0; li < st->count; li++) {
                if (section_array_has(
                        now_layer_find_section(&st->layers[li], section),
                        arr_keys[k], val)) {
                    who = layer_label(&st->layers[li]);
                    break;
                }
            }
            origins_push(dst, section, arr_keys[k], val, who);
        }
    }

    for (k = 0; scalar_keys[k]; k++) {
        const PastaValue *v = pasta_map_get(eff, scalar_keys[k]);
        const char *val, *who = "?";
        size_t li;
        if (!v || pasta_type(v) != PASTA_STRING) continue;
        val = pasta_get_string(v);
        /* Highest layer that SETS the key -- who won.
         *
         * Deliberately not "highest layer whose value equals the merged
         * one": with only one layer setting a scalar those are the same
         * answer, and with two they differ only when both chose the
         * same string. Asking who set it at all is both the honest
         * question and the one whose answer changes if this walk is
         * ever reversed, which is what makes it testable. */
        for (li = st->count; li-- > 0; ) {
            if (section_scalar(now_layer_find_section(&st->layers[li], section),
                               scalar_keys[k])) {
                who = layer_label(&st->layers[li]);
                break;
            }
        }
        origins_push(dst, section, scalar_keys[k], val, who);
    }

    pasta_free(eff);
}

NOW_API int now_layer_collect_origins(NowConfigOrigins *dst,
                                      const char *basedir,
                                      NowResult *result) {
    static const char *const compile_arr[] =
        { "flags", "warnings", "defines", "includes", NULL };
    static const char *const compile_sca[] = { "std", "opt", NULL };
    static const char *const link_arr[] =
        { "flags", "libs", "libdirs", "archives", NULL };
    static const char *const link_sca[] = { NULL };

    NowLayerStack stack;
    NowAuditReport audit;
    NowProject *raw;
    NowResult lres;
    char desc[PATH_MAX];

    if (!dst || !basedir) return -1;
    memset(dst, 0, sizeof(*dst));

    snprintf(desc, sizeof(desc), "%s/now.pasta", basedir);
    memset(&lres, 0, sizeof(lres));
    raw = now_project_load(desc, &lres);
    if (!raw) {
        if (result) {
            result->code = lres.code;
            snprintf(result->message, sizeof(result->message),
                     "%s", lres.message);
        }
        return -1;
    }

    now_layer_stack_init(&stack);
    now_layer_discover(&stack, basedir, result);
    now_layer_push_project(&stack, raw);
    /* Same stack the build sees, or this reports on a different build
     * than the one that ran. */
    push_env_and_cli_layers(&stack);

    now_audit_init(&audit);
    collect_section(dst, &stack, "compile", compile_arr, compile_sca, &audit);
    collect_section(dst, &stack, "link", link_arr, link_sca, &audit);
    now_audit_free(&audit);

    now_layer_stack_free(&stack);
    now_project_free(raw);
    return 0;
}

NOW_API void now_config_origins_free(NowConfigOrigins *o) {
    size_t i;
    if (!o) return;
    for (i = 0; i < o->count; i++) {
        free(o->items[i].section);
        free(o->items[i].key);
        free(o->items[i].value);
        free(o->items[i].origin);
    }
    free(o->items);
    memset(o, 0, sizeof(*o));
}
