#ifndef NOW_H
#define NOW_H

#include <stddef.h>

/* DLL export/import (NOW_STATIC disables for static builds) */
#ifdef NOW_STATIC
  #define NOW_API
#elif defined(_WIN32)
  #ifdef NOW_BUILDING
    #define NOW_API __declspec(dllexport)
  #else
    #define NOW_API __declspec(dllimport)
  #endif
#else
  #define NOW_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Version ---- */

NOW_API const char *now_version(void);

/* ---- Error codes ---- */

typedef enum {
    NOW_OK = 0,
    NOW_ERR_ALLOC,
    NOW_ERR_IO,
    NOW_ERR_SYNTAX,
    NOW_ERR_SCHEMA,
    NOW_ERR_NOT_FOUND,
    NOW_ERR_TOOL,
    NOW_ERR_TEST,
    NOW_ERR_AUTH,
    NOW_ERR_ADVISORY
} NowError;

/* Structured exit codes for CI (§16) */
#define NOW_EXIT_OK          0   /* Success */
#define NOW_EXIT_BUILD       1   /* Build error (compile/link) */
#define NOW_EXIT_TEST        2   /* Test failure */
#define NOW_EXIT_RESOLVE     3   /* Dependency resolution failure */
#define NOW_EXIT_CONFIG      4   /* Configuration error (now.pasta) */
#define NOW_EXIT_IO          5   /* I/O error (disk/network) */
#define NOW_EXIT_AUTH        6   /* Authentication failure */
#define NOW_EXIT_ADVISORY    7   /* Blocked by security advisory */

/* Map NowError to CI exit code */
NOW_API int now_exit_code(NowError err);

typedef struct {
    NowError code;
    int      line;
    int      col;
    char     message[512];
} NowResult;

/* ---- Project Object Model ---- */

/* Opaque project handle — internal structs defined in now_pom.h */
typedef struct NowProject NowProject;

/* Load a project from a now.pasta file path */
NOW_API NowProject *now_project_load(const char *path, NowResult *result);

/* Load a project from a Pasta string in memory */
NOW_API NowProject *now_project_load_string(const char *input, size_t len,
                                             NowResult *result);

/* Free a loaded project */
NOW_API void now_project_free(NowProject *project);

/* ---- Project accessors ---- */

NOW_API const char *now_project_group(const NowProject *p);
NOW_API const char *now_project_artifact(const NowProject *p);
NOW_API const char *now_project_version(const NowProject *p);
NOW_API const char *now_project_name(const NowProject *p);
NOW_API const char *now_project_license(const NowProject *p);
NOW_API const char *now_project_std(const NowProject *p);

/* Language list */
NOW_API size_t      now_project_lang_count(const NowProject *p);
NOW_API const char *now_project_lang(const NowProject *p, size_t index);

/* Output */
NOW_API const char *now_project_output_type(const NowProject *p);
NOW_API const char *now_project_output_name(const NowProject *p);

/* Sources */
NOW_API const char *now_project_source_dir(const NowProject *p);
NOW_API const char *now_project_header_dir(const NowProject *p);
NOW_API const char *now_project_test_dir(const NowProject *p);

/* Dependencies */
NOW_API size_t      now_project_dep_count(const NowProject *p);
NOW_API const char *now_project_dep_id(const NowProject *p, size_t index);
NOW_API const char *now_project_dep_scope(const NowProject *p, size_t index);

/* Compile settings */
NOW_API size_t      now_project_warning_count(const NowProject *p);
NOW_API const char *now_project_warning(const NowProject *p, size_t index);
NOW_API size_t      now_project_define_count(const NowProject *p);
NOW_API const char *now_project_define(const NowProject *p, size_t index);
NOW_API const char *now_project_opt(const NowProject *p);

/* Convergence policy */
NOW_API const char *now_project_convergence(const NowProject *p);

/* ---- Build operations ---- */

/* Run the full build lifecycle: discover sources, compile, link.
 * basedir is the absolute path to the project root.
 * jobs: max parallel compilation jobs (0 = auto-detect CPU count).
 * Returns 0 on success, non-zero on error. */
NOW_API int now_build(const NowProject *project, const char *basedir,
                      int verbose, int jobs, NowResult *result);

/* Run compile only (no link). */
NOW_API int now_compile(const NowProject *project, const char *basedir,
                        int verbose, int jobs, NowResult *result);

/* Run the test phase: compile test sources, link with project objects,
 * execute the test binary. Returns 0 on success (all tests pass). */
NOW_API int now_test(const NowProject *project, const char *basedir,
                     int verbose, int jobs, NowResult *result);

#ifdef __cplusplus
}
#endif

#endif /* NOW_H */
