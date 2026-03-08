/*
 * now_lang.h — Language type system
 *
 * Registry of language definitions with file classification and
 * tool invocation specs. See spec §4.
 */
#ifndef NOW_LANG_H
#define NOW_LANG_H

#include <stddef.h>
#include "now.h"  /* for NOW_API */

/* File role (§4.1) */
typedef enum {
    NOW_ROLE_SOURCE,
    NOW_ROLE_HEADER,
    NOW_ROLE_INTERMEDIATE,
    NOW_ROLE_GENERATED
} NowFileRole;

/* What a tool invocation produces (§4.1) */
typedef enum {
    NOW_PRODUCES_OBJECT,
    NOW_PRODUCES_INTERMEDIATE,
    NOW_PRODUCES_NONE
} NowProduces;

/* A type within a language */
typedef struct {
    const char  *id;
    const char **extensions;   /* NULL-terminated */
    NowFileRole  role;
    const char  *tool_var;
    NowProduces  produces;
    const char  *output_ext;
} NowLangType;

/* A language definition */
typedef struct {
    const char        *id;
    const char        *name;
    const char        *std_flag;
    const NowLangType *types;
    size_t             type_count;
} NowLangDef;

NOW_API void            now_lang_registry_init(void);
NOW_API const NowLangDef *now_lang_find(const char *lang_id);

NOW_API const NowLangType *now_lang_classify(const char *path,
                                              const char *const *active_langs,
                                              size_t lang_count,
                                              const NowLangDef **out_lang);

NOW_API const char **now_lang_source_exts(const char *const *active_langs,
                                           size_t lang_count);

NOW_API const char **now_lang_all_exts(const char *const *active_langs,
                                        size_t lang_count);

#endif /* NOW_LANG_H */
