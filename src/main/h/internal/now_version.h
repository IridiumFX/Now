/*
 * now_version.h — Semantic versioning and range resolution (§6.9-6.10)
 */
#ifndef NOW_VERSION_H
#define NOW_VERSION_H

#include "now.h"

/* Parsed semantic version */
typedef struct {
    int   major;
    int   minor;
    int   patch;
    char *prerelease;  /* e.g. "beta.1" or NULL */
    char *build;       /* e.g. "build.42" or NULL (ignored in comparisons) */
} NowSemVer;

/* Parse a version string. Returns 0 on success. */
NOW_API int now_semver_parse(const char *str, NowSemVer *out);
NOW_API void now_semver_free(NowSemVer *v);

/* Compare two versions. Returns <0, 0, >0 like strcmp.
 * Build metadata is ignored per SemVer spec. */
NOW_API int now_semver_compare(const NowSemVer *a, const NowSemVer *b);

/* Format a version to string. Caller must free. */
NOW_API char *now_semver_to_string(const NowSemVer *v);

/* ---- Version ranges (§6.10) ---- */

typedef enum {
    NOW_RANGE_EXACT,     /* 1.2.3 */
    NOW_RANGE_CARET,     /* ^1.2.3 */
    NOW_RANGE_TILDE,     /* ~1.2.3 */
    NOW_RANGE_GTE,       /* >=1.2.0 */
    NOW_RANGE_COMPOUND,  /* >=1.2.0 <2.0.0 */
    NOW_RANGE_ANY        /* * */
} NowRangeKind;

typedef struct {
    NowRangeKind kind;
    NowSemVer    floor;    /* inclusive lower bound */
    NowSemVer    ceiling;  /* exclusive upper bound (for caret/tilde/compound) */
    int          has_ceiling;
} NowVersionRange;

/* Parse a version range string. Returns 0 on success. */
NOW_API int now_range_parse(const char *str, NowVersionRange *out);
NOW_API void now_range_free(NowVersionRange *r);

/* Check if a version satisfies a range. */
NOW_API int now_range_satisfies(const NowVersionRange *range,
                                 const NowSemVer *version);

/* Intersect two ranges. Returns 0 on success, -1 if disjoint. */
NOW_API int now_range_intersect(const NowVersionRange *a,
                                 const NowVersionRange *b,
                                 NowVersionRange *out);

/* ---- Coordinate parsing ---- */

/* Parse "group:artifact:version" coordinate */
typedef struct {
    char *group;
    char *artifact;
    char *version;  /* version string (may be range) */
} NowCoordinate;

NOW_API int  now_coord_parse(const char *str, NowCoordinate *out);
NOW_API void now_coord_free(NowCoordinate *c);

#endif /* NOW_VERSION_H */
