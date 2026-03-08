/*
 * now_resolve.c — Dependency resolution (§6.2-6.13)
 */
#include "now_resolve.h"
#include "now_version.h"
#include "pasta.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Lock file operations ---- */

void now_lock_init(NowLockFile *lf) {
    memset(lf, 0, sizeof(*lf));
}

static void lock_entry_free(NowLockEntry *e) {
    free(e->group);
    free(e->artifact);
    free(e->version);
    free(e->triple);
    free(e->url);
    free(e->sha256);
    free(e->descriptor_sha256);
    free(e->scope);
    for (size_t i = 0; i < e->dep_count; i++)
        free(e->deps[i]);
    free(e->deps);
}

void now_lock_free(NowLockFile *lf) {
    for (size_t i = 0; i < lf->count; i++)
        lock_entry_free(&lf->entries[i]);
    free(lf->entries);
    memset(lf, 0, sizeof(*lf));
}

const NowLockEntry *now_lock_find(const NowLockFile *lf,
                                   const char *group,
                                   const char *artifact) {
    if (!lf || !group || !artifact) return NULL;
    for (size_t i = 0; i < lf->count; i++) {
        if (strcmp(lf->entries[i].group, group) == 0 &&
            strcmp(lf->entries[i].artifact, artifact) == 0)
            return &lf->entries[i];
    }
    return NULL;
}

int now_lock_set(NowLockFile *lf, const NowLockEntry *entry) {
    /* Update if exists */
    for (size_t i = 0; i < lf->count; i++) {
        if (strcmp(lf->entries[i].group, entry->group) == 0 &&
            strcmp(lf->entries[i].artifact, entry->artifact) == 0) {
            lock_entry_free(&lf->entries[i]);
            NowLockEntry *e = &lf->entries[i];
            e->group    = entry->group ? strdup(entry->group) : NULL;
            e->artifact = entry->artifact ? strdup(entry->artifact) : NULL;
            e->version  = entry->version ? strdup(entry->version) : NULL;
            e->triple   = entry->triple ? strdup(entry->triple) : NULL;
            e->url      = entry->url ? strdup(entry->url) : NULL;
            e->sha256   = entry->sha256 ? strdup(entry->sha256) : NULL;
            e->descriptor_sha256 = entry->descriptor_sha256 ? strdup(entry->descriptor_sha256) : NULL;
            e->scope    = entry->scope ? strdup(entry->scope) : NULL;
            e->overridden = entry->overridden;
            e->dep_count = entry->dep_count;
            e->deps = NULL;
            if (entry->dep_count > 0) {
                e->deps = malloc(entry->dep_count * sizeof(char *));
                for (size_t j = 0; j < entry->dep_count; j++)
                    e->deps[j] = strdup(entry->deps[j]);
            }
            return 0;
        }
    }

    /* Add new */
    if (lf->count >= lf->capacity) {
        size_t new_cap = lf->capacity ? lf->capacity * 2 : 8;
        NowLockEntry *tmp = realloc(lf->entries, new_cap * sizeof(NowLockEntry));
        if (!tmp) return -1;
        lf->entries = tmp;
        lf->capacity = new_cap;
    }

    NowLockEntry *e = &lf->entries[lf->count];
    e->group    = entry->group ? strdup(entry->group) : NULL;
    e->artifact = entry->artifact ? strdup(entry->artifact) : NULL;
    e->version  = entry->version ? strdup(entry->version) : NULL;
    e->triple   = entry->triple ? strdup(entry->triple) : NULL;
    e->url      = entry->url ? strdup(entry->url) : NULL;
    e->sha256   = entry->sha256 ? strdup(entry->sha256) : NULL;
    e->descriptor_sha256 = entry->descriptor_sha256 ? strdup(entry->descriptor_sha256) : NULL;
    e->scope    = entry->scope ? strdup(entry->scope) : NULL;
    e->overridden = entry->overridden;
    e->dep_count = entry->dep_count;
    e->deps = NULL;
    if (entry->dep_count > 0) {
        e->deps = malloc(entry->dep_count * sizeof(char *));
        for (size_t j = 0; j < entry->dep_count; j++)
            e->deps[j] = strdup(entry->deps[j]);
    }
    lf->count++;
    return 0;
}

/* ---- Lock file serialization (Pasta) ---- */

int now_lock_load(NowLockFile *lf, const char *path) {
    now_lock_init(lf);

    FILE *fp = fopen(path, "rb");
    if (!fp) return 0; /* no lock file yet */

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
    if (!root || pr.code != PASTA_OK) return -1;
    if (pasta_type(root) != PASTA_MAP) { pasta_free(root); return -1; }

    const PastaValue *entries = pasta_map_get(root, "entries");
    if (entries && pasta_type(entries) == PASTA_ARRAY) {
        size_t n = pasta_count(entries);
        for (size_t i = 0; i < n; i++) {
            const PastaValue *e = pasta_array_get(entries, i);
            if (!e || pasta_type(e) != PASTA_MAP) continue;

            NowLockEntry entry;
            memset(&entry, 0, sizeof(entry));

            const PastaValue *v;
            v = pasta_map_get(e, "group");
            if (v && pasta_type(v) == PASTA_STRING) entry.group = strdup(pasta_get_string(v));
            v = pasta_map_get(e, "artifact");
            if (v && pasta_type(v) == PASTA_STRING) entry.artifact = strdup(pasta_get_string(v));
            v = pasta_map_get(e, "version");
            if (v && pasta_type(v) == PASTA_STRING) entry.version = strdup(pasta_get_string(v));
            v = pasta_map_get(e, "triple");
            if (v && pasta_type(v) == PASTA_STRING) entry.triple = strdup(pasta_get_string(v));
            v = pasta_map_get(e, "url");
            if (v && pasta_type(v) == PASTA_STRING) entry.url = strdup(pasta_get_string(v));
            v = pasta_map_get(e, "sha256");
            if (v && pasta_type(v) == PASTA_STRING) entry.sha256 = strdup(pasta_get_string(v));
            v = pasta_map_get(e, "descriptor_sha256");
            if (v && pasta_type(v) == PASTA_STRING) entry.descriptor_sha256 = strdup(pasta_get_string(v));
            v = pasta_map_get(e, "scope");
            if (v && pasta_type(v) == PASTA_STRING) entry.scope = strdup(pasta_get_string(v));
            v = pasta_map_get(e, "overridden");
            if (v && pasta_type(v) == PASTA_BOOL) entry.overridden = pasta_get_bool(v);

            v = pasta_map_get(e, "deps");
            if (v && pasta_type(v) == PASTA_ARRAY) {
                size_t dc = pasta_count(v);
                entry.deps = malloc(dc * sizeof(char *));
                entry.dep_count = dc;
                for (size_t j = 0; j < dc; j++) {
                    const PastaValue *dv = pasta_array_get(v, j);
                    entry.deps[j] = (dv && pasta_type(dv) == PASTA_STRING)
                        ? strdup(pasta_get_string(dv)) : strdup("");
                }
            }

            if (entry.group && entry.artifact) {
                now_lock_set(lf, &entry);
            }
            lock_entry_free(&entry);
        }
    }

    pasta_free(root);
    return 0;
}

int now_lock_save(const NowLockFile *lf, const char *path) {
    PastaValue *root = pasta_new_map();
    if (!root) return -1;

    PastaValue *entries = pasta_new_array();
    for (size_t i = 0; i < lf->count; i++) {
        const NowLockEntry *e = &lf->entries[i];
        PastaValue *entry = pasta_new_map();

        if (e->group)    pasta_set(entry, "group", pasta_new_string(e->group));
        if (e->artifact) pasta_set(entry, "artifact", pasta_new_string(e->artifact));
        if (e->version)  pasta_set(entry, "version", pasta_new_string(e->version));
        if (e->triple)   pasta_set(entry, "triple", pasta_new_string(e->triple));
        if (e->url)      pasta_set(entry, "url", pasta_new_string(e->url));
        if (e->sha256)   pasta_set(entry, "sha256", pasta_new_string(e->sha256));
        if (e->descriptor_sha256)
            pasta_set(entry, "descriptor_sha256", pasta_new_string(e->descriptor_sha256));
        if (e->scope)    pasta_set(entry, "scope", pasta_new_string(e->scope));
        if (e->overridden)
            pasta_set(entry, "overridden", pasta_new_bool(1));

        if (e->dep_count > 0) {
            PastaValue *deps = pasta_new_array();
            for (size_t j = 0; j < e->dep_count; j++)
                pasta_push(deps, pasta_new_string(e->deps[j]));
            pasta_set(entry, "deps", deps);
        }

        pasta_push(entries, entry);
    }
    pasta_set(root, "entries", entries);

    char *out = pasta_write(root, PASTA_PRETTY | PASTA_SORTED);
    pasta_free(root);
    if (!out) return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp) { free(out); return -1; }
    fputs(out, fp);
    fclose(fp);
    free(out);
    return 0;
}

/* ---- Resolver ---- */

void now_resolver_init(NowResolver *r, const char *convergence) {
    memset(r, 0, sizeof(*r));
    r->convergence = convergence ? convergence : "lowest";
}

void now_resolver_free(NowResolver *r) {
    for (size_t i = 0; i < r->count; i++) {
        free(r->constraints[i].group);
        free(r->constraints[i].artifact);
        free(r->constraints[i].scope);
        free(r->constraints[i].from);
        now_range_free(&r->constraints[i].range);
    }
    free(r->constraints);
    memset(r, 0, sizeof(*r));
}

int now_resolver_add(NowResolver *r, const char *dep_id,
                      const char *scope, const char *from,
                      int override) {
    /* Parse coordinate: group:artifact:version_range */
    NowCoordinate coord;
    if (now_coord_parse(dep_id, &coord) != 0) return -1;

    /* Parse version range */
    NowVersionRange range;
    if (now_range_parse(coord.version, &range) != 0) {
        now_coord_free(&coord);
        return -1;
    }

    /* Add constraint */
    if (r->count >= r->capacity) {
        size_t new_cap = r->capacity ? r->capacity * 2 : 8;
        NowConstraint *tmp = realloc(r->constraints, new_cap * sizeof(NowConstraint));
        if (!tmp) {
            now_coord_free(&coord);
            now_range_free(&range);
            return -1;
        }
        r->constraints = tmp;
        r->capacity = new_cap;
    }

    NowConstraint *c = &r->constraints[r->count];
    c->group    = coord.group;
    c->artifact = coord.artifact;
    c->range    = range;
    c->scope    = scope ? strdup(scope) : strdup("compile");
    c->from     = from ? strdup(from) : strdup("root");
    c->override = override;

    /* coord.version is consumed by range parse, but we need to free
     * the version string from coord since range took ownership of data */
    free(coord.version);

    r->count++;
    return 0;
}

/* Find all constraints for a given group:artifact */
static size_t find_constraints(const NowResolver *r, const char *group,
                                const char *artifact, size_t *indices, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < r->count && n < max; i++) {
        if (strcmp(r->constraints[i].group, group) == 0 &&
            strcmp(r->constraints[i].artifact, artifact) == 0) {
            indices[n++] = i;
        }
    }
    return n;
}

int now_resolver_resolve(NowResolver *r, NowLockFile *lf, NowResult *result) {
    if (!r || !lf) return -1;

    /* Collect unique coordinates */
    typedef struct { char *group; char *artifact; } Coord;
    Coord *coords = NULL;
    size_t ncoords = 0, coord_cap = 0;

    for (size_t i = 0; i < r->count; i++) {
        int found = 0;
        for (size_t j = 0; j < ncoords; j++) {
            if (strcmp(coords[j].group, r->constraints[i].group) == 0 &&
                strcmp(coords[j].artifact, r->constraints[i].artifact) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            if (ncoords >= coord_cap) {
                coord_cap = coord_cap ? coord_cap * 2 : 8;
                coords = realloc(coords, coord_cap * sizeof(Coord));
            }
            coords[ncoords].group    = r->constraints[i].group;
            coords[ncoords].artifact = r->constraints[i].artifact;
            ncoords++;
        }
    }

    /* For each coordinate, intersect all constraints */
    for (size_t ci = 0; ci < ncoords; ci++) {
        size_t indices[64];
        size_t n = find_constraints(r, coords[ci].group, coords[ci].artifact, indices, 64);
        if (n == 0) continue;

        /* Check for override */
        int has_override = 0;
        size_t override_idx = 0;
        for (size_t j = 0; j < n; j++) {
            if (r->constraints[indices[j]].override) {
                has_override = 1;
                override_idx = indices[j];
                break;
            }
        }

        NowVersionRange resolved;
        const char *scope = r->constraints[indices[0]].scope;

        if (has_override) {
            /* Override: use exact version from the override constraint */
            resolved = r->constraints[override_idx].range;
            /* Don't free this one later — we're just borrowing it */
        } else {
            /* Intersect all ranges */
            resolved = r->constraints[indices[0]].range;
            int borrowed = 1; /* first range is borrowed, not owned */

            for (size_t j = 1; j < n; j++) {
                NowVersionRange intersected;
                if (now_range_intersect(&resolved, &r->constraints[indices[j]].range,
                                        &intersected) != 0) {
                    /* Conflict */
                    if (result) {
                        result->code = NOW_ERR_SCHEMA;
                        snprintf(result->message, sizeof(result->message),
                                 "convergence failure for %s:%s — "
                                 "constraint from %s conflicts with %s",
                                 coords[ci].group, coords[ci].artifact,
                                 r->constraints[indices[0]].from,
                                 r->constraints[indices[j]].from);
                    }
                    free(coords);
                    if (!borrowed) now_range_free(&resolved);
                    return -1;
                }
                if (!borrowed) now_range_free(&resolved);
                resolved = intersected;
                borrowed = 0;
            }

            /* Apply convergence policy to select version */
            /* For "lowest": version = floor of resolved range
             * For "highest": version = ceiling - 1 (needs registry; use floor for now)
             * For "exact": range must be exact, or fail */
            if (strcmp(r->convergence, "exact") == 0) {
                if (resolved.kind != NOW_RANGE_EXACT && n > 1) {
                    if (result) {
                        result->code = NOW_ERR_SCHEMA;
                        snprintf(result->message, sizeof(result->message),
                                 "exact convergence requires exact version for %s:%s",
                                 coords[ci].group, coords[ci].artifact);
                    }
                    free(coords);
                    if (!borrowed) now_range_free(&resolved);
                    return -1;
                }
            }

            if (!borrowed) {
                /* We own resolved — will create lock entry from it then free */
            }
            /* If borrowed (single constraint), we just reference it */
        }

        /* Create lock entry with resolved version */
        NowLockEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.group    = coords[ci].group;
        entry.artifact = coords[ci].artifact;
        entry.scope    = (char *)scope;
        entry.overridden = has_override;

        /* Version: use floor as the selected version */
        char *ver_str = now_semver_to_string(&resolved.floor);
        entry.version = ver_str;
        entry.triple  = strdup("noarch");

        now_lock_set(lf, &entry);

        free(ver_str);
        free(entry.triple);

        /* Free resolved if we allocated it (not borrowed) */
        if (!has_override && n > 1) {
            now_range_free(&resolved);
        }
    }

    free(coords);

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}
