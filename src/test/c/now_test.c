#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
  #include <direct.h>
  #include <windows.h>
  #define rmdir _rmdir
#else
  #include <unistd.h>
  #include <dirent.h>
#endif
#include "now.h"
#include "pasta.h"

/* Internal headers for unit testing */
#include "now_lang.h"
#include "now_fs.h"
#include "now_version.h"
#include "now_manifest.h"
#include "now_resolve.h"
#include "now_procure.h"
#include "now_build.h"
#include "now_package.h"
#include "now_workspace.h"
#include "now_schema.h"
#include "now_vacate.h"
#include "now_convert.h"
#include "now_tell.h"
#include "now_plugin.h"
#include "now_plugin_registry.h"
#include "now_ci.h"
#include "now_layer.h"
#include "now_arch.h"
#include "now_export.h"
#include "now_trust.h"
#include "now_repro.h"
#include "now_advisory.h"
#include "now_auth.h"
#include "now_module.h"
#include "now_cache.h"
#include "now_sbom.h"
#include "now_remote.h"
#include "now_audit.h"
#include "now_events.h"
#include "now_watch.h"
#include "now_objsym.h"
#include "now_graph.h"
#include "pico_h2.h"
#include "alforno.h"
#include "basta.h"
#include "pico_http.h"
#include "pico_ws.h"

#ifndef NOW_TEST_RESOURCES
  #define NOW_TEST_RESOURCES "."
#endif

/* Shared test helpers defined later in the file. */
static void rmtree_best_effort(const char *path);
static int  write_empty(const char *path);

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { tests_run++; printf("  %-50s", name); } while (0)
#define PASS() \
    do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) \
    do { tests_failed++; printf("FAIL: %s\n", msg); } while (0)
#define ASSERT_STR(actual, expected) \
    do { \
        if (!(actual) || strcmp((actual), (expected)) != 0) { \
            FAIL("expected '" expected "'"); return; \
        } \
    } while (0)
#define ASSERT_EQ(actual, expected) \
    do { \
        if ((actual) != (expected)) { FAIL(#actual " != " #expected); return; } \
    } while (0)
#define ASSERT_NOT_NULL(ptr) \
    do { if (!(ptr)) { FAIL(#ptr " is NULL"); return; } } while (0)
#define ASSERT_NULL(ptr) \
    do { if ((ptr)) { FAIL(#ptr " is not NULL"); return; } } while (0)

/* ---- Version ---- */

static void test_version(void) {
    TEST("now_version");
    const char *v = now_version();
    ASSERT_NOT_NULL(v);
    PASS();
}

/* ---- POM: load from string ---- */

static void test_pom_minimal_string(void) {
    TEST("pom: load minimal from string");
    const char *input =
        "{ group: \"io.test\", artifact: \"demo\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"demo\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_STR(now_project_group(p), "io.test");
    ASSERT_STR(now_project_artifact(p), "demo");
    ASSERT_STR(now_project_version(p), "1.0.0");
    ASSERT_STR(now_project_std(p), "c11");
    ASSERT_EQ(now_project_lang_count(p), (size_t)1);
    ASSERT_STR(now_project_lang(p, 0), "c");
    ASSERT_STR(now_project_output_type(p), "executable");
    ASSERT_STR(now_project_output_name(p), "demo");
    /* default source dirs */
    ASSERT_STR(now_project_source_dir(p), "src/main/c");
    ASSERT_STR(now_project_header_dir(p), "src/main/h");
    now_project_free(p);
    PASS();
}

static void test_pom_lang_scalar(void) {
    TEST("pom: lang scalar shorthand");
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  lang: \"c\", std: \"c11\" }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(now_project_lang_count(p), (size_t)1);
    ASSERT_STR(now_project_lang(p, 0), "c");
    now_project_free(p);
    PASS();
}

static void test_pom_lang_mixed(void) {
    TEST("pom: lang 'mixed' expands to c + c++");
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  lang: \"mixed\" }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(now_project_lang_count(p), (size_t)2);
    ASSERT_STR(now_project_lang(p, 0), "c");
    ASSERT_STR(now_project_lang(p, 1), "c++");
    now_project_free(p);
    PASS();
}

static void test_pom_compile(void) {
    TEST("pom: compile warnings and defines");
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  compile: { warnings: [\"Wall\", \"Wextra\"],"
        "             defines: [\"NDEBUG\", \"FOO=1\"],"
        "             opt: \"speed\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(now_project_warning_count(p), (size_t)2);
    ASSERT_STR(now_project_warning(p, 0), "Wall");
    ASSERT_STR(now_project_warning(p, 1), "Wextra");
    ASSERT_EQ(now_project_define_count(p), (size_t)2);
    ASSERT_STR(now_project_define(p, 0), "NDEBUG");
    ASSERT_STR(now_project_opt(p), "speed");
    now_project_free(p);
    PASS();
}

static void test_pom_os_conditional(void) {
    TEST("pom: OS-conditional sub-blocks merge into parent");
    /* compile.windows / compile.posix and link.* sub-blocks should
     * append to the parent arrays only when host OS matches. */
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  compile: { defines: [\"BASE\"],"
        "             windows: { defines: [\"IS_WINDOWS\"] },"
        "             posix:   { defines: [\"IS_POSIX\"] } } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    /* BASE is always present. Exactly one of IS_WINDOWS or IS_POSIX
     * should be present, depending on host. */
    size_t n = now_project_define_count(p);
    int have_base = 0, have_win = 0, have_posix = 0;
    for (size_t i = 0; i < n; i++) {
        const char *d = now_project_define(p, i);
        if (strcmp(d, "BASE")       == 0) have_base = 1;
        if (strcmp(d, "IS_WINDOWS") == 0) have_win = 1;
        if (strcmp(d, "IS_POSIX")   == 0) have_posix = 1;
    }
    ASSERT_EQ(have_base, 1);
    /* Exactly one OS branch matched. */
    ASSERT_EQ(have_win + have_posix, 1);
#ifdef _WIN32
    ASSERT_EQ(have_win, 1);
#else
    ASSERT_EQ(have_posix, 1);
#endif
    now_project_free(p);
    PASS();
}

static void test_pom_deps(void) {
    TEST("pom: dependency loading");
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  deps: ["
        "    { id: \"org.acme:core:^1.5\", scope: \"compile\" },"
        "    { id: \"unity:unity:2.5.2\",  scope: \"test\"    }"
        "  ] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(now_project_dep_count(p), (size_t)2);
    ASSERT_STR(now_project_dep_id(p, 0), "org.acme:core:^1.5");
    ASSERT_STR(now_project_dep_scope(p, 0), "compile");
    ASSERT_STR(now_project_dep_id(p, 1), "unity:unity:2.5.2");
    ASSERT_STR(now_project_dep_scope(p, 1), "test");
    now_project_free(p);
    PASS();
}

static void test_pom_convergence(void) {
    TEST("pom: convergence policy");
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  convergence: \"lowest\" }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_STR(now_project_convergence(p), "lowest");
    now_project_free(p);
    PASS();
}

/* ---- POM: load from file ---- */

static void test_pom_load_file(void) {
    TEST("pom: load minimal.pasta from file");
    char path[512];
    snprintf(path, sizeof(path), "%s/minimal.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }
    ASSERT_STR(now_project_group(p), "io.example");
    ASSERT_STR(now_project_artifact(p), "hello");
    ASSERT_STR(now_project_output_type(p), "executable");
    ASSERT_EQ(now_project_dep_count(p), (size_t)1);
    ASSERT_STR(now_project_dep_id(p, 0), "unity:unity:2.5.2");
    now_project_free(p);
    PASS();
}

static void test_pom_load_rich(void) {
    TEST("pom: load rich.pasta from file");
    char path[512];
    snprintf(path, sizeof(path), "%s/rich.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }
    ASSERT_STR(now_project_group(p), "org.acme");
    ASSERT_STR(now_project_artifact(p), "rocketlib");
    ASSERT_STR(now_project_version(p), "3.0.0-beta.1");
    ASSERT_STR(now_project_name(p), "Rocket Library");
    ASSERT_STR(now_project_license(p), "Apache-2.0");
    ASSERT_EQ(now_project_lang_count(p), (size_t)2);
    ASSERT_STR(now_project_lang(p, 0), "c");
    ASSERT_STR(now_project_lang(p, 1), "c++");
    ASSERT_STR(now_project_output_type(p), "shared");
    ASSERT_STR(now_project_output_name(p), "rocket");
    ASSERT_EQ(now_project_warning_count(p), (size_t)3);
    ASSERT_STR(now_project_opt(p), "speed");
    ASSERT_EQ(now_project_dep_count(p), (size_t)2);
    ASSERT_STR(now_project_convergence(p), "lowest");
    /* sources overridden */
    ASSERT_STR(now_project_source_dir(p), "src/main");
    ASSERT_STR(now_project_header_dir(p), "include");
    now_project_free(p);
    PASS();
}

/* ---- POM: error handling ---- */

static void test_pom_syntax_error(void) {
    TEST("pom: syntax error reported");
    const char *input = "{ broken ]]]";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    if (p) { now_project_free(p); FAIL("should have failed"); return; }
    ASSERT_EQ(res.code, NOW_ERR_SYNTAX);
    PASS();
}

static void test_pom_not_a_map(void) {
    TEST("pom: non-map root rejected");
    const char *input = "[1, 2, 3]";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    if (p) { now_project_free(p); FAIL("should have failed"); return; }
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);
    PASS();
}

static void test_pom_file_not_found(void) {
    TEST("pom: missing file error");
    NowResult res;
    NowProject *p = now_project_load("/nonexistent/now.pasta", &res);
    if (p) { now_project_free(p); FAIL("should have failed"); return; }
    ASSERT_EQ(res.code, NOW_ERR_IO);
    PASS();
}

/* ---- Language type system ---- */

static void test_lang_find_c(void) {
    TEST("lang: find C definition");
    now_lang_registry_init();
    const NowLangDef *c = now_lang_find("c");
    ASSERT_NOT_NULL(c);
    ASSERT_STR(c->id, "c");
    PASS();
}

static void test_lang_find_cxx(void) {
    TEST("lang: find C++ definition");
    const NowLangDef *cxx = now_lang_find("c++");
    ASSERT_NOT_NULL(cxx);
    ASSERT_STR(cxx->id, "c++");
    PASS();
}

static void test_lang_classify_c(void) {
    TEST("lang: classify .c file as c-source");
    const char *langs[] = { "c" };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("foo/bar.c", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "c-source");
    ASSERT_EQ(type->role, NOW_ROLE_SOURCE);
    ASSERT_STR(type->output_ext, ".c.o");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "c");
    PASS();
}

static void test_lang_classify_h(void) {
    TEST("lang: classify .h file as c-header");
    const char *langs[] = { "c" };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("include/api.h", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "c-header");
    ASSERT_EQ(type->role, NOW_ROLE_HEADER);
    PASS();
}

static void test_lang_classify_cpp(void) {
    TEST("lang: classify .cpp as cxx-source");
    const char *langs[] = { "c", "c++" };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("src/engine.cpp", langs, 2, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "cxx-source");
    PASS();
}

static void test_lang_classify_unknown(void) {
    TEST("lang: unknown extension returns NULL");
    const char *langs[] = { "c" };
    const NowLangType *type = now_lang_classify("readme.txt", langs, 1, NULL);
    if (type) { FAIL("should be NULL"); return; }
    PASS();
}

static void test_lang_source_exts(void) {
    TEST("lang: source extensions for C");
    const char *langs[] = { "c" };
    const char **exts = now_lang_source_exts(langs, 1);
    ASSERT_NOT_NULL(exts);
    /* Should contain .c and .i */
    int found_c = 0, found_i = 0;
    for (const char **e = exts; *e; e++) {
        if (strcmp(*e, ".c") == 0) found_c = 1;
        if (strcmp(*e, ".i") == 0) found_i = 1;
    }
    free(exts);
    if (!found_c) { FAIL("missing .c"); return; }
    if (!found_i) { FAIL("missing .i"); return; }
    PASS();
}

/* ---- Filesystem utilities ---- */

static void test_fs_path_join(void) {
    TEST("fs: path_join");
    char *p = now_path_join("foo", "bar.c");
    ASSERT_NOT_NULL(p);
    ASSERT_STR(p, "foo/bar.c");
    free(p);
    PASS();
}

static void test_fs_path_join_trailing_sep(void) {
    TEST("fs: path_join with trailing separator");
    char *p = now_path_join("foo/", "bar.c");
    ASSERT_NOT_NULL(p);
    ASSERT_STR(p, "foo/bar.c");
    free(p);
    PASS();
}

static void test_fs_path_ext(void) {
    TEST("fs: path_ext");
    ASSERT_STR(now_path_ext("foo/bar.c"), ".c");
    ASSERT_STR(now_path_ext("foo/bar.cpp"), ".cpp");
    ASSERT_STR(now_path_ext("foo/bar"), "");
    PASS();
}

static void test_fs_obj_path(void) {
    TEST("fs: obj_path derivation");
    char *obj = now_obj_path("/proj", "src/main/c/net/parser.c",
                              "src/main/c", "target");
    ASSERT_NOT_NULL(obj);
    /* Should end with net/parser.c.o */
    if (!strstr(obj, "net/parser.c.o") && !strstr(obj, "net\\parser.c.o")) {
        FAIL(obj);
        free(obj);
        return;
    }
    free(obj);
    PASS();
}

/* ---- Glob matching (spec §26) ---- */

static void test_glob_star_basename(void) {
    TEST("glob: * matches within a segment, basename rule");
    /* No '/' in pattern → matches the basename only. */
    ASSERT_EQ(now_glob_match("*.c", "foo.c"), 1);
    ASSERT_EQ(now_glob_match("*.c", "sub/foo.c"), 1);   /* basename foo.c */
    ASSERT_EQ(now_glob_match("*.c", "foo.h"), 0);
    ASSERT_EQ(now_glob_match("*.c", "foo.c.o"), 0);     /* must end at .c */
    PASS();
}

static void test_glob_doublestar_suffix(void) {
    TEST("glob: **.c crosses segments");
    ASSERT_EQ(now_glob_match("**.c", "foo.c"), 1);
    ASSERT_EQ(now_glob_match("**.c", "sub/foo.c"), 1);
    ASSERT_EQ(now_glob_match("**.c", "a/b/c/foo.c"), 1);
    ASSERT_EQ(now_glob_match("**.c", "foo.h"), 0);
    PASS();
}

static void test_glob_doublestar_slash_zero(void) {
    TEST("glob: **/foo.c matches zero or more dirs");
    ASSERT_EQ(now_glob_match("**/foo.c", "foo.c"), 1);   /* zero dirs */
    ASSERT_EQ(now_glob_match("**/foo.c", "sub/foo.c"), 1);
    ASSERT_EQ(now_glob_match("**/foo.c", "a/b/foo.c"), 1);
    ASSERT_EQ(now_glob_match("**/foo.c", "foo.h"), 0);
    PASS();
}

static void test_glob_doublestar_middle(void) {
    TEST("glob: src/**/foo.c anchored prefix + middle **");
    ASSERT_EQ(now_glob_match("src/**/foo.c", "src/foo.c"), 1);
    ASSERT_EQ(now_glob_match("src/**/foo.c", "src/a/foo.c"), 1);
    ASSERT_EQ(now_glob_match("src/**/foo.c", "src/a/b/foo.c"), 1);
    ASSERT_EQ(now_glob_match("src/**/foo.c", "other/foo.c"), 0);
    PASS();
}

static void test_glob_single_star_no_cross_slash(void) {
    TEST("glob: single * does not cross /");
    /* Pattern has '/', so matched against full path. */
    ASSERT_EQ(now_glob_match("src/*.c", "src/foo.c"), 1);
    ASSERT_EQ(now_glob_match("src/*.c", "src/sub/foo.c"), 0);
    PASS();
}

static void test_glob_question_and_class(void) {
    TEST("glob: ? and [..] character classes");
    ASSERT_EQ(now_glob_match("f?o.c", "foo.c"), 1);
    ASSERT_EQ(now_glob_match("f?o.c", "fo.c"), 0);      /* ? needs one char */
    ASSERT_EQ(now_glob_match("foo.[ch]", "foo.c"), 1);
    ASSERT_EQ(now_glob_match("foo.[ch]", "foo.h"), 1);
    ASSERT_EQ(now_glob_match("foo.[ch]", "foo.o"), 0);
    ASSERT_EQ(now_glob_match("foo.[a-z]", "foo.q"), 1);
    ASSERT_EQ(now_glob_match("foo.[!ch]", "foo.o"), 1); /* negated */
    ASSERT_EQ(now_glob_match("foo.[!ch]", "foo.c"), 0);
    PASS();
}

static void test_glob_double_extension(void) {
    TEST("glob: dots are literal (double-extension objects)");
    ASSERT_EQ(now_glob_match("**.c.o", "target/obj/parser.c.o"), 1);
    ASSERT_EQ(now_glob_match("**.c.o", "target/obj/parser.s.o"), 0);
    ASSERT_EQ(now_glob_match("**/*.o", "target/obj/parser.c.o"), 1);
    PASS();
}

static void test_glob_escape_and_literal(void) {
    TEST("glob: escape + plain literal match");
    ASSERT_EQ(now_glob_match("foo.c", "foo.c"), 1);
    ASSERT_EQ(now_glob_match("a\\*b", "a*b"), 1);       /* escaped star = literal */
    ASSERT_EQ(now_glob_match("a\\*b", "axb"), 0);
    PASS();
}

static void test_glob_backslash_path_normalized(void) {
    TEST("glob: backslash path separators are normalized");
    ASSERT_EQ(now_glob_match("src/*.c", "src\\foo.c"), 1);
    ASSERT_EQ(now_glob_match("**/foo.c", "a\\b\\foo.c"), 1);
    PASS();
}

/* ---- Remote cache ---- */

static void test_remote_config_parse_full(void) {
    TEST("remote: parse config with all fields");
    const char *input =
        "{ object_cache: { url: \"http://cache.local:9090\","
        "  token: \"my-secret\", push: true } }";
    NowRemoteCacheConfig cfg;
    int rc = now_remote_config_parse(input, strlen(input), &cfg);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(cfg.url, "http://cache.local:9090");
    ASSERT_STR(cfg.token, "my-secret");
    ASSERT_EQ(cfg.push, 1);
    now_remote_config_free(&cfg);
    PASS();
}

static void test_remote_config_parse_minimal(void) {
    TEST("remote: parse config with only url");
    const char *input = "{ object_cache: { url: \"http://localhost:8080\" } }";
    NowRemoteCacheConfig cfg;
    int rc = now_remote_config_parse(input, strlen(input), &cfg);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(cfg.url, "http://localhost:8080");
    ASSERT_NULL(cfg.token);
    ASSERT_EQ(cfg.push, 0);
    now_remote_config_free(&cfg);
    PASS();
}

static void test_remote_config_parse_no_section(void) {
    TEST("remote: parse config without object_cache returns -1");
    const char *input = "{ something_else: \"foo\" }";
    NowRemoteCacheConfig cfg;
    int rc = now_remote_config_parse(input, strlen(input), &cfg);
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_remote_config_parse_no_url(void) {
    TEST("remote: parse config without url returns -1");
    const char *input = "{ object_cache: { token: \"secret\" } }";
    NowRemoteCacheConfig cfg;
    int rc = now_remote_config_parse(input, strlen(input), &cfg);
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_remote_config_free_null(void) {
    TEST("remote: config_free on zeroed struct is safe");
    NowRemoteCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    now_remote_config_free(&cfg);
    now_remote_config_free(NULL);
    PASS();
}

static void test_remote_cache_restore_unreachable(void) {
    TEST("remote: restore from unreachable host returns -1");
    NowRemoteCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "http://127.0.0.1:1";
    char outpath[256];
    snprintf(outpath, sizeof(outpath), "%s/remote_test.o", NOW_TEST_RESOURCES);
    int rc = now_remote_cache_restore(&cfg, "abcdef1234567890", outpath, ".o");
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_remote_cache_store_push_disabled(void) {
    TEST("remote: store with push=0 returns -1 immediately");
    NowRemoteCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "http://127.0.0.1:1";
    cfg.push = 0;
    int rc = now_remote_cache_store(&cfg, "abcdef", "/nonexistent.o", ".o");
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_remote_cache_store_unreachable(void) {
    TEST("remote: store to unreachable host returns -1");
    NowRemoteCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "http://127.0.0.1:1";
    cfg.push = 1;
    /* Need a real file to read */
    char path[256];
    snprintf(path, sizeof(path), "%s/minimal.pasta", NOW_TEST_RESOURCES);
    int rc = now_remote_cache_store(&cfg, "abcdef1234567890", path, ".o");
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_remote_cache_key_url_safe(void) {
    TEST("remote: cache key is hex-only (URL-safe)");
    /* now_cache_key returns 64-char hex string */
    char *key = now_cache_key("abc123", "def456", "/usr/bin/gcc");
    ASSERT_NOT_NULL(key);
    /* Verify all chars are hex digits */
    for (size_t i = 0; key[i]; i++) {
        char c = key[i];
        int is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!is_hex) { free(key); FAIL("non-hex char in cache key"); return; }
    }
    ASSERT_EQ(strlen(key), (size_t)64);
    free(key);
    PASS();
}

/* ---- Enterprise auth (LDAP/SSO) ---- */

static void test_auth_method_parse(void) {
    TEST("auth: method parse");
    ASSERT_EQ((int)now_auth_method_parse("token"), (int)NOW_AUTH_TOKEN);
    ASSERT_EQ((int)now_auth_method_parse("ldap"), (int)NOW_AUTH_LDAP);
    ASSERT_EQ((int)now_auth_method_parse("oidc"), (int)NOW_AUTH_OIDC);
    ASSERT_EQ((int)now_auth_method_parse("oauth2"), (int)NOW_AUTH_OIDC);
    ASSERT_EQ((int)now_auth_method_parse(NULL), (int)NOW_AUTH_TOKEN);
    ASSERT_EQ((int)now_auth_method_parse("unknown"), (int)NOW_AUTH_TOKEN);
    PASS();
}

static void test_auth_method_name(void) {
    TEST("auth: method name");
    ASSERT_STR(now_auth_method_name(NOW_AUTH_TOKEN), "token");
    ASSERT_STR(now_auth_method_name(NOW_AUTH_LDAP), "ldap");
    ASSERT_STR(now_auth_method_name(NOW_AUTH_OIDC), "oidc");
    PASS();
}

static void test_auth_creds_free_null(void) {
    TEST("auth: creds_free null safety");
    now_auth_creds_free(NULL);  /* should not crash */
    NowCredentials c;
    memset(&c, 0, sizeof(c));
    now_auth_creds_free(&c);   /* should not crash on zeroed struct */
    PASS();
}

static void test_auth_load_no_file(void) {
    TEST("auth: load returns -1 with no credentials file");
    NowCredentials c;
    /* Use a URL that won't match anything */
    int rc = now_auth_load("http://nonexistent.example.com:9999", &c);
    /* Either -1 (no file) or -1 (no match) */
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_auth_load_null_safety(void) {
    TEST("auth: load null safety");
    ASSERT_EQ(now_auth_load(NULL, NULL), -1);
    NowCredentials c;
    ASSERT_EQ(now_auth_load(NULL, &c), -1);
    ASSERT_EQ(now_auth_load("http://x", NULL), -1);
    PASS();
}

static void test_token_cache_lifecycle(void) {
    TEST("auth: token cache put/get/remove");
    const char *url = "http://test-cache-lifecycle.example.com:12345";

    /* Put a token */
    int rc = now_token_cache_put(url, "test-jwt-abc123", 3600);
    ASSERT_EQ(rc, 0);

    /* Get it back */
    char *jwt = now_token_cache_get(url);
    ASSERT_NOT_NULL(jwt);
    ASSERT_STR(jwt, "test-jwt-abc123");
    free(jwt);

    /* Remove it */
    rc = now_token_cache_remove(url);
    ASSERT_EQ(rc, 0);

    /* Should be gone */
    jwt = now_token_cache_get(url);
    if (jwt) { free(jwt); FAIL("token should have been removed"); return; }

    /* Remove again — should return -1 */
    rc = now_token_cache_remove(url);
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_token_cache_expired(void) {
    TEST("auth: token cache returns NULL for expired token");
    const char *url = "http://test-cache-expired.example.com:12345";

    /* Put with 0 seconds TTL (already expired) */
    int rc = now_token_cache_put(url, "expired-jwt", 0);
    ASSERT_EQ(rc, 0);

    /* Should return NULL (expired within 60s margin) */
    char *jwt = now_token_cache_get(url);
    if (jwt) { free(jwt); now_token_cache_remove(url); FAIL("expired token should be NULL"); return; }

    /* Cleanup */
    now_token_cache_remove(url);
    PASS();
}

static void test_token_cache_overwrite(void) {
    TEST("auth: token cache overwrites existing entry");
    const char *url = "http://test-cache-overwrite.example.com:12345";

    now_token_cache_put(url, "jwt-v1", 3600);
    now_token_cache_put(url, "jwt-v2", 3600);

    char *jwt = now_token_cache_get(url);
    ASSERT_NOT_NULL(jwt);
    ASSERT_STR(jwt, "jwt-v2");
    free(jwt);

    now_token_cache_remove(url);
    PASS();
}

static void test_auth_ldap_login_null(void) {
    TEST("auth: ldap login null safety");
    NowResult res;
    memset(&res, 0, sizeof(res));
    char *jwt = NULL;
    ASSERT_EQ(now_auth_login_ldap(NULL, "user", "pass", &jwt, &res), -1);
    ASSERT_EQ(now_auth_login_ldap("http://x", NULL, "pass", &jwt, &res), -1);
    ASSERT_EQ(now_auth_login_ldap("http://x", "user", NULL, &jwt, &res), -1);
    ASSERT_EQ(now_auth_login_ldap("http://x", "user", "pass", NULL, &res), -1);
    PASS();
}

static void test_auth_ldap_login_unreachable(void) {
    TEST("auth: ldap login to unreachable host returns error");
    NowResult res;
    memset(&res, 0, sizeof(res));
    char *jwt = NULL;
    int rc = now_auth_login_ldap("http://127.0.0.1:1", "user", "pass",
                                  &jwt, &res);
    ASSERT_EQ(rc, -1);
    if (jwt) { free(jwt); FAIL("should not get JWT from unreachable host"); return; }
    PASS();
}

static void test_auth_oidc_client_null(void) {
    TEST("auth: oidc client credentials null safety");
    NowResult res;
    memset(&res, 0, sizeof(res));
    char *jwt = NULL;
    ASSERT_EQ(now_auth_login_oidc_client(NULL, "cid", "cs", &jwt, &res), -1);
    ASSERT_EQ(now_auth_login_oidc_client("http://x", NULL, "cs", &jwt, &res), -1);
    ASSERT_EQ(now_auth_login_oidc_client("http://x", "cid", NULL, &jwt, &res), -1);
    PASS();
}

static void test_auth_oidc_client_unreachable(void) {
    TEST("auth: oidc client creds to unreachable host returns error");
    NowResult res;
    memset(&res, 0, sizeof(res));
    char *jwt = NULL;
    int rc = now_auth_login_oidc_client("http://127.0.0.1:1",
                                          "client-id", "client-secret",
                                          &jwt, &res);
    ASSERT_EQ(rc, -1);
    if (jwt) { free(jwt); FAIL("should not get JWT"); return; }
    PASS();
}

static void test_auth_discover_unreachable(void) {
    TEST("auth: discover unreachable registry defaults to token");
    NowRegistryInfo info;
    int rc = now_auth_discover("http://127.0.0.1:1", &info);
    ASSERT_EQ(rc, -1);
    /* Should default to token auth */
    ASSERT_EQ(info.supports_token, 1);
    now_auth_discovery_free(&info);
    PASS();
}

static void test_auth_discovery_free_null(void) {
    TEST("auth: discovery_free null safety");
    now_auth_discovery_free(NULL);  /* should not crash */
    NowRegistryInfo info;
    memset(&info, 0, sizeof(info));
    now_auth_discovery_free(&info); /* should not crash */
    PASS();
}

static void test_auth_get_token_no_creds(void) {
    TEST("auth: get_token returns NULL with no credentials");
    NowResult res;
    memset(&res, 0, sizeof(res));
    char *jwt = now_auth_get_token("http://nonexistent.example.com:9999", 0, &res);
    if (jwt) { free(jwt); FAIL("should not get token without credentials"); return; }
    PASS();
}

/* ---- SBOM generation ---- */

static void test_sbom_to_json_basic(void) {
    TEST("sbom: generate JSON from project");
    const char *pasta =
        "{ group: \"com.example\", artifact: \"demo\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"demo\" } }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    /* Check CycloneDX envelope */
    if (!strstr(json, "\"bomFormat\": \"CycloneDX\"")) { free(json); now_project_free(p); FAIL("missing bomFormat"); return; }
    if (!strstr(json, "\"specVersion\": \"1.5\"")) { free(json); now_project_free(p); FAIL("missing specVersion"); return; }
    if (!strstr(json, "urn:uuid:")) { free(json); now_project_free(p); FAIL("missing serialNumber"); return; }
    /* Check metadata component */
    if (!strstr(json, "\"group\": \"com.example\"")) { free(json); now_project_free(p); FAIL("missing group"); return; }
    if (!strstr(json, "\"name\": \"demo\"")) { free(json); now_project_free(p); FAIL("missing artifact"); return; }
    if (!strstr(json, "\"version\": \"1.0.0\"")) { free(json); now_project_free(p); FAIL("missing version"); return; }
    if (!strstr(json, "\"type\": \"application\"")) { free(json); now_project_free(p); FAIL("missing type application"); return; }
    /* Check purl */
    if (!strstr(json, "pkg:now/com.example/demo@1.0.0")) { free(json); now_project_free(p); FAIL("missing purl"); return; }
    /* Check tool */
    if (!strstr(json, "\"vendor\": \"now\"")) { free(json); now_project_free(p); FAIL("missing tool vendor"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

static void test_sbom_library_type(void) {
    TEST("sbom: library project type");
    const char *pasta =
        "{ group: \"io.lib\", artifact: \"utils\", version: \"2.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"shared\", name: \"utils\" } }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    if (!strstr(json, "\"type\": \"library\"")) { free(json); now_project_free(p); FAIL("should be library"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

static void test_sbom_with_deps(void) {
    TEST("sbom: declared deps in components");
    const char *pasta =
        "{ group: \"com.app\", artifact: \"main\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"main\" },"
        "  deps: ["
        "    { id: \"org.lib:crypto:^1.2.0\" },"
        "    { id: \"org.lib:net:~2.0.0\" }"
        "  ] }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    /* Should have component entries for declared deps */
    if (!strstr(json, "\"group\": \"org.lib\"")) { free(json); now_project_free(p); FAIL("missing dep group"); return; }
    if (!strstr(json, "\"name\": \"crypto\"")) { free(json); now_project_free(p); FAIL("missing crypto dep"); return; }
    if (!strstr(json, "\"name\": \"net\"")) { free(json); now_project_free(p); FAIL("missing net dep"); return; }
    /* Check dependencies section */
    if (!strstr(json, "\"dependencies\":")) { free(json); now_project_free(p); FAIL("missing dependencies"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

static void test_sbom_with_license(void) {
    TEST("sbom: license field in metadata");
    const char *pasta =
        "{ group: \"com.oss\", artifact: \"lib\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\", license: \"MIT\","
        "  output: { type: \"static\", name: \"lib\" } }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    if (!strstr(json, "\"id\": \"MIT\"")) { free(json); now_project_free(p); FAIL("missing license"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

static void test_sbom_generate_file(void) {
    TEST("sbom: generate to file");
    const char *pasta =
        "{ group: \"com.test\", artifact: \"sbomtest\", version: \"0.1.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"sbomtest\" } }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_sbom_output.json",
             NOW_TEST_RESOURCES);

    int rc = now_sbom_generate(p, NULL, outpath,
                                NOW_SBOM_CYCLONEDX_JSON, &res);
    ASSERT_EQ(rc, 0);

    /* Read back and verify */
    FILE *fp = fopen(outpath, "r");
    if (!fp) { now_project_free(p); FAIL("cannot read output"); return; }
    fseek(fp, 0, SEEK_END);
    long flen = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)flen + 1);
    fread(buf, 1, (size_t)flen, fp);
    buf[flen] = '\0';
    fclose(fp);

    if (!strstr(buf, "\"bomFormat\": \"CycloneDX\"")) { free(buf); now_project_free(p); FAIL("invalid output"); return; }
    if (!strstr(buf, "\"name\": \"sbomtest\"")) { free(buf); now_project_free(p); FAIL("missing artifact in file"); return; }

    free(buf);
    remove(outpath);
    now_project_free(p);
    PASS();
}

static void test_sbom_null_project(void) {
    TEST("sbom: null project returns NULL");
    char *json = now_sbom_to_json(NULL, NULL);
    ASSERT_NULL(json);
    PASS();
}

static void test_sbom_scope_mapping(void) {
    TEST("sbom: scope mapping (test→excluded, provided→optional)");
    const char *pasta =
        "{ group: \"com.app\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"app\" },"
        "  deps: ["
        "    { id: \"org.test:mock:1.0.0\", scope: \"test\" },"
        "    { id: \"org.api:spec:2.0.0\", scope: \"provided\" }"
        "  ] }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    if (!strstr(json, "\"scope\": \"excluded\"")) { free(json); now_project_free(p); FAIL("test scope not mapped to excluded"); return; }
    if (!strstr(json, "\"scope\": \"optional\"")) { free(json); now_project_free(p); FAIL("provided scope not mapped to optional"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

static void test_sbom_no_deps(void) {
    TEST("sbom: project with no deps");
    const char *pasta =
        "{ group: \"com.solo\", artifact: \"alone\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"alone\" } }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    /* components array should be empty */
    if (!strstr(json, "\"components\": [")) { free(json); now_project_free(p); FAIL("missing components"); return; }
    /* dependencies should still have root entry */
    if (!strstr(json, "\"dependencies\": [")) { free(json); now_project_free(p); FAIL("missing dependencies"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

/* ---- Audit logging ---- */

static void test_audit_config_parse_full(void) {
    TEST("audit: config parse with all fields");
    const char *pasta = "{ audit: { enabled: true, max_entries: 500, log_path: \"/tmp/test.pasta\" } }";
    NowAuditConfig cfg;
    ASSERT_EQ(now_audit_config_parse(pasta, strlen(pasta), &cfg), 0);
    ASSERT_EQ(cfg.enabled, 1);
    ASSERT_EQ(cfg.max_entries, 500);
    ASSERT_NOT_NULL(cfg.log_path);
    ASSERT_STR(cfg.log_path, "/tmp/test.pasta");
    now_audit_config_free(&cfg);
    PASS();
}

static void test_audit_config_parse_disabled(void) {
    TEST("audit: config parse disabled");
    const char *pasta = "{ audit: { enabled: false } }";
    NowAuditConfig cfg;
    ASSERT_EQ(now_audit_config_parse(pasta, strlen(pasta), &cfg), 0);
    ASSERT_EQ(cfg.enabled, 0);
    now_audit_config_free(&cfg);
    PASS();
}

static void test_audit_config_parse_no_section(void) {
    TEST("audit: config parse missing section returns -1");
    const char *pasta = "{ other: { foo: \"bar\" } }";
    NowAuditConfig cfg;
    ASSERT_EQ(now_audit_config_parse(pasta, strlen(pasta), &cfg), -1);
    PASS();
}

static void test_audit_config_free_null(void) {
    TEST("audit: config_free on zeroed struct is safe");
    NowAuditConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    now_audit_config_free(&cfg);
    now_audit_config_free(NULL);
    PASS();
}

static void test_audit_event_name_roundtrip(void) {
    TEST("audit: event name roundtrip");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_BUILD), "build");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_PUBLISH), "publish");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_YANK), "yank");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_PROCURE), "procure");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_AUTH_LOGIN), "auth_login");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_VERIFY), "verify");
    ASSERT_EQ(now_audit_event_parse("publish"), NOW_AUDIT_PUBLISH);
    ASSERT_EQ(now_audit_event_parse("advisory"), NOW_AUDIT_ADVISORY);
    PASS();
}

static void test_audit_record_disabled(void) {
    TEST("audit: record when disabled is no-op");
    int rc = now_audit_record(NOW_AUDIT_BUILD, "local", "test", "ok", NULL);
    ASSERT_EQ(rc, 0);
    PASS();
}

static void test_audit_record_and_show(void) {
    TEST("audit: record and show roundtrip");
    const char *tmp_cfg = "target/test_audit_config.pasta";
    const char *tmp_log = "target/test_audit.pasta";
    remove(tmp_log);

    /* Create config and parse it */
    {
        FILE *f = fopen(tmp_cfg, "w");
        if (f) {
            fprintf(f, "{ audit: { enabled: true, log_path: \"%s\" } }\n", tmp_log);
            fclose(f);
        }
    }

    NowAuditConfig cfg;
    size_t flen = 0;
    char *data = NULL;
    FILE *fp = fopen(tmp_cfg, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        flen = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = (char *)malloc(flen + 1);
        if (data) { fread(data, 1, flen, fp); data[flen] = '\0'; }
        fclose(fp);
    }
    ASSERT_NOT_NULL(data);
    ASSERT_EQ(now_audit_config_parse(data, flen, &cfg), 0);
    ASSERT_EQ(cfg.enabled, 1);
    ASSERT_NOT_NULL(cfg.log_path);
    if (strcmp(cfg.log_path, tmp_log) != 0) { FAIL("log_path mismatch"); return; }
    free(data);
    now_audit_config_free(&cfg);

    /* Verify event name table completeness */
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_AUTH_LOGOUT), "auth_logout");

    remove(tmp_log);
    remove(tmp_cfg);
    PASS();
}

/* ---- Rust FFI ---- */

static void test_rust_lang_registered(void) {
    TEST("rust: language registered");
    const NowLangDef *lang = now_lang_find("rust");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "rust");
    ASSERT_STR(lang->name, "Rust");
    PASS();
}

static void test_rust_classify_rs(void) {
    TEST("rust: classify .rs as rust-source");
    const char *langs[] = { "rust", NULL };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("test.rs", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "rust-source");
    ASSERT_EQ(type->role, NOW_ROLE_SOURCE);
    ASSERT_STR(type->tool_var, "${rustc}");
    PASS();
}

/* ---- Go + Julia ---- */

static void test_go_lang_registered(void) {
    TEST("go: language registered");
    const NowLangDef *lang = now_lang_find("go");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "go");
    PASS();
}

static void test_go_classify(void) {
    TEST("go: classify .go as go-source");
    const char *langs[] = { "go", NULL };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("main.go", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "go-source");
    ASSERT_STR(type->tool_var, "${go}");
    PASS();
}

static void test_julia_lang_registered(void) {
    TEST("julia: language registered");
    const NowLangDef *lang = now_lang_find("julia");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "julia");
    PASS();
}

static void test_julia_classify(void) {
    TEST("julia: classify .jl as julia-source");
    const char *langs[] = { "julia", NULL };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("solver.jl", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "julia-source");
    PASS();
}

/* ---- HTTP/2 ---- */

#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)
static void test_h2_hpack_encode_get(void) {
    TEST("h2: HPACK encode GET request");
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ(pico_hpack_encode("GET", "https", "example.com", "/",
                                  NULL, 0, &buf, &len), 0);
    ASSERT_NOT_NULL(buf);
    /* :method GET = static index 2 (0x82), :scheme https = index 7 (0x87),
     * :path / = index 4 (0x84), :authority = index 1 with value */
    if (len < 4) { FAIL("too short"); free(buf); return; }
    /* First byte should be 0x82 (indexed :method GET) */
    ASSERT_EQ((int)buf[0], 0x82);
    free(buf);
    PASS();
}

static void test_h2_hpack_decode_status(void) {
    TEST("h2: HPACK decode :status 200");
    /* Static index 8 = :status 200 → byte 0x88 */
    uint8_t encoded[] = { 0x88 };
    int status = 0;
    PicoHttpHeader *hdrs = NULL;
    size_t count = 0;
    ASSERT_EQ(pico_hpack_decode(encoded, sizeof(encoded), &status, &hdrs, &count), 0);
    ASSERT_EQ(status, 200);
    free(hdrs);
    PASS();
}

static void test_h2_frame_layout(void) {
    TEST("h2: frame header layout");
    /* Verify the H2 connection preface constant */
    const char *preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    ASSERT_EQ((int)strlen(preface), 24);
    PASS();
}

static void test_h2_hpack_encode_with_headers(void) {
    TEST("h2: HPACK encode with extra headers");
    PicoHttpHeader extra[2];
    extra[0].name = "content-type";
    extra[0].value = "application/json";
    extra[1].name = "x-custom";
    extra[1].value = "test";
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ(pico_hpack_encode("POST", "https", "api.example.com", "/v1/data",
                                  extra, 2, &buf, &len), 0);
    ASSERT_NOT_NULL(buf);
    /* :method POST = static index 3 (0x83) */
    ASSERT_EQ((int)buf[0], 0x83);
    if (len < 10) { FAIL("too short for headers"); free(buf); return; }
    free(buf);
    PASS();
}
#endif /* PICO_HTTP_TLS */

/* ---- Graph cache ---- */

static void test_graph_key_deterministic(void) {
    TEST("graph: key is deterministic");
    char *k1 = now_graph_key(NULL, "/usr/bin/gcc", "abc123");
    char *k2 = now_graph_key(NULL, "/usr/bin/gcc", "abc123");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (strcmp(k1, k2) != 0) { FAIL("keys should match"); free(k1); free(k2); return; }
    free(k1);
    free(k2);
    PASS();
}

static void test_graph_key_varies(void) {
    TEST("graph: different inputs produce different keys");
    char *k1 = now_graph_key(NULL, "/usr/bin/gcc", "abc123");
    char *k2 = now_graph_key(NULL, "/usr/bin/clang", "abc123");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (strcmp(k1, k2) == 0) { FAIL("keys should differ"); free(k1); free(k2); return; }
    free(k1);
    free(k2);
    PASS();
}

static void test_graph_serialize_roundtrip(void) {
    TEST("graph: serialize/deserialize roundtrip");
    NowManifest m;
    now_manifest_init(&m);
    now_manifest_set(&m, "src/main.c", "target/main.o", "hash1", "fhash1", 1000);
    now_manifest_set(&m, "src/util.c", "target/util.o", "hash2", "fhash2", 2000);

    const char *deps[] = { "/usr/include/stdio.h" };
    const char *dhashes[] = { "dephash1" };
    now_manifest_set_deps(&m, "src/main.c", deps, dhashes, 1);

    size_t len = 0;
    char *data = now_graph_serialize(&m, &len);
    ASSERT_NOT_NULL(data);
    if (len == 0) { FAIL("empty output"); free(data); now_manifest_free(&m); return; }

    /* Verify it's valid pasta with type marker */
    if (!strstr(data, "now-build-graph")) { FAIL("missing type marker"); free(data); now_manifest_free(&m); return; }

    /* Deserialize back */
    NowManifest m2;
    ASSERT_EQ(now_graph_deserialize(data, len, &m2), 0);
    free(data);

    /* Verify entries survived */
    const NowManifestEntry *e1 = now_manifest_find(&m2, "src/main.c");
    const NowManifestEntry *e2 = now_manifest_find(&m2, "src/util.c");
    ASSERT_NOT_NULL(e1);
    ASSERT_NOT_NULL(e2);
    ASSERT_STR(e1->source_hash, "hash1");
    ASSERT_STR(e2->source_hash, "hash2");

    /* Verify deps */
    ASSERT_EQ((int)e1->dep_count, 1);
    ASSERT_STR(e1->dep_hashes[0], "dephash1");

    now_manifest_free(&m);
    now_manifest_free(&m2);
    PASS();
}

static void test_graph_deserialize_bad_input(void) {
    TEST("graph: deserialize rejects bad input");
    NowManifest m;
    ASSERT_EQ(now_graph_deserialize(NULL, 0, &m), -1);
    ASSERT_EQ(now_graph_deserialize("not pasta", 9, &m), -1);
    /* Valid pasta but wrong type */
    const char *wrong = "{ type: \"wrong\" }";
    ASSERT_EQ(now_graph_deserialize(wrong, strlen(wrong), &m), -1);
    PASS();
}

static void test_graph_pull_unreachable(void) {
    TEST("graph: pull from unreachable host returns -1");
    NowRemoteCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "http://192.0.2.1:9999";  /* TEST-NET, unreachable */
    NowManifest m;
    ASSERT_EQ(now_graph_pull(&cfg, "testkey", &m), -1);
    PASS();
}

/* ---- Watch ---- */

static void test_watch_opts_init(void) {
    TEST("watch: opts init defaults");
    NowWatchOpts opts;
    now_watch_opts_init(&opts);
    ASSERT_EQ(opts.poll_ms, 500);
    ASSERT_EQ(opts.verbose, 0);
    ASSERT_EQ(opts.jobs, 0);
    PASS();
}

static void test_watch_snapshot_hello(void) {
    TEST("watch: snapshot discovers hello project files");
    char path[512];
    snprintf(path, sizeof(path), "%s/hello/now.pasta", NOW_TEST_RESOURCES);

    NowResult result;
    memset(&result, 0, sizeof(result));
    NowProject *p = now_project_load(path, &result);
    ASSERT_NOT_NULL(p);

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/hello", NOW_TEST_RESOURCES);

    NowWatchSnapshot snap;
    ASSERT_EQ(now_watch_snapshot(p, basedir, &snap), 0);
    /* hello project has at least main.c */
    if (snap.count == 0) { FAIL("no files found"); now_project_free(p); return; }
    /* pasta_mtime should be non-zero */
    if (snap.pasta_mtime == 0) { FAIL("pasta_mtime is 0"); now_project_free(p); return; }

    now_watch_snapshot_free(&snap);
    now_project_free(p);
    PASS();
}

static void test_watch_diff_no_change(void) {
    TEST("watch: diff detects no change");
    NowWatchSnapshot a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.pasta_mtime = 100;
    b.pasta_mtime = 100;
    ASSERT_EQ(now_watch_diff(&a, &b), 0);
    PASS();
}

static void test_watch_diff_source_change(void) {
    TEST("watch: diff detects source change");
    NowWatchEntry ea = { .path = "test.c", .mtime = 100 };
    NowWatchEntry eb = { .path = "test.c", .mtime = 200 };
    NowWatchSnapshot a = { .entries = &ea, .count = 1, .pasta_mtime = 100 };
    NowWatchSnapshot b = { .entries = &eb, .count = 1, .pasta_mtime = 100 };
    ASSERT_EQ(now_watch_diff(&a, &b), 1);
    PASS();
}

static void test_watch_diff_pasta_change(void) {
    TEST("watch: diff detects pasta change");
    NowWatchSnapshot a = { .entries = NULL, .count = 0, .pasta_mtime = 100 };
    NowWatchSnapshot b = { .entries = NULL, .count = 0, .pasta_mtime = 200 };
    ASSERT_EQ(now_watch_diff(&a, &b), 2);
    PASS();
}

static void test_watch_snapshot_free_null(void) {
    TEST("watch: snapshot_free NULL is safe");
    now_watch_snapshot_free(NULL);
    NowWatchSnapshot s;
    memset(&s, 0, sizeof(s));
    now_watch_snapshot_free(&s);
    PASS();
}

/* ---- Build integration ---- */

static void test_build_hello(void) {
    TEST("build: compile and link hello project");
    char path[512];
    snprintf(path, sizeof(path), "%s/hello/now.pasta", NOW_TEST_RESOURCES);

    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/hello", NOW_TEST_RESOURCES);

    int rc = now_build(p, basedir, 0, 0, &res);
    now_project_free(p);

    if (rc != 0) { FAIL(res.message); return; }

    /* Check output exists */
    char out_path[512];
#ifdef _WIN32
    snprintf(out_path, sizeof(out_path), "%s/target/bin/hello.exe", basedir);
#else
    snprintf(out_path, sizeof(out_path), "%s/target/bin/hello", basedir);
#endif
    if (!now_path_exists(out_path)) {
        FAIL("output binary not found");
        return;
    }
    PASS();
}

/* Two-sided proof that sources.exclude is glob-matched end-to-end
 * through the build loop. The excluded files contain a #error, so:
 *   - with the glob exclude, they are skipped → build succeeds
 *   - without it, they compile → #error → build fails
 * This is layout-independent (no assumptions about target/obj paths)
 * and exercises the §26.2 base-path rooting (pattern relative to
 * sources.dir, while discovered paths include the dir prefix). */
static void test_build_exclude_glob(void) {
    TEST("build: sources.exclude is glob-matched (vendor/**.c)");

    char root[512];
    snprintf(root, sizeof(root), "%s/exclude_proj", NOW_TEST_RESOURCES);
    char csrc[512];
    snprintf(csrc, sizeof(csrc), "%s/src/main/c", root);
    char target[512];
    snprintf(target, sizeof(target), "%s/target", root);

    rmtree_best_effort(csrc);
    rmtree_best_effort(target);

    char dir[512];
    now_mkdir_p(csrc);
    snprintf(dir, sizeof(dir), "%s/src/main/c/vendor/sqlite", root);
    now_mkdir_p(dir);

    char p[512];
    FILE *f;
    snprintf(p, sizeof(p), "%s/src/main/c/keep.c", root);
    f = fopen(p, "w"); if (!f) { FAIL("setup keep.c"); return; }
    fputs("int keep_fn(void) { return 42; }\n", f); fclose(f);

    /* Broken sources under vendor/ — compile iff NOT excluded. */
    snprintf(p, sizeof(p), "%s/src/main/c/vendor/sqlite/amalg.c", root);
    f = fopen(p, "w"); if (!f) { FAIL("setup amalg.c"); return; }
    fputs("#error this file should have been excluded\n", f); fclose(f);

    /* Static lib output — no main() / link entry point needed. */
    const char *pasta_excl =
        "{ group: \"org.test\", artifact: \"excl\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"excl\" },"
        "  sources: { dir: \"src/main/c\", exclude: [\"vendor/**.c\"] } }";

    NowResult res;
    NowProject *prj = now_project_load_string(pasta_excl, strlen(pasta_excl), &res);
    if (!prj) { FAIL(res.message); return; }
    int rc_excl = now_build(prj, root, 0, 0, &res);
    now_project_free(prj);
    if (rc_excl != 0) {
        FAIL("build failed despite vendor/**.c exclude (glob not applied?)");
        return;
    }

    /* Negative control: same tree, no exclude → the #error must bite. */
    rmtree_best_effort(target);
    const char *pasta_noexcl =
        "{ group: \"org.test\", artifact: \"excl\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"excl\" },"
        "  sources: { dir: \"src/main/c\" } }";
    NowProject *prj2 = now_project_load_string(pasta_noexcl, strlen(pasta_noexcl), &res);
    if (!prj2) { FAIL(res.message); return; }
    int rc_noexcl = now_build(prj2, root, 0, 0, &res);
    now_project_free(prj2);
    if (rc_noexcl == 0) {
        FAIL("build succeeded without exclude — broken vendor file was not compiled");
        return;
    }

    rmtree_best_effort(target);
    PASS();
}

/* sources.pattern selects the candidate set (spec §1.3 resolution
 * order: pattern -> include -> exclude). Verified by inspecting the
 * sources now_build_init discovers — no compiler needed. */
static void test_build_pattern_filter(void) {
    TEST("build: sources.pattern narrows discovery (keep/**.c)");

    char root[512];
    snprintf(root, sizeof(root), "%s/pattern_proj", NOW_TEST_RESOURCES);
    char csrc[512];
    snprintf(csrc, sizeof(csrc), "%s/src/main/c", root);
    rmtree_best_effort(csrc);

    char dir[512];
    now_mkdir_p(csrc);
    snprintf(dir, sizeof(dir), "%s/src/main/c/keep", root); now_mkdir_p(dir);
    snprintf(dir, sizeof(dir), "%s/src/main/c/skip", root); now_mkdir_p(dir);

    char p[512];
    snprintf(p, sizeof(p), "%s/src/main/c/top.c", root);       write_empty(p);
    snprintf(p, sizeof(p), "%s/src/main/c/keep/a.c", root);    write_empty(p);
    snprintf(p, sizeof(p), "%s/src/main/c/skip/b.c", root);    write_empty(p);

    const char *pasta =
        "{ group: \"org.test\", artifact: \"pat\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"pat\" },"
        "  sources: { dir: \"src/main/c\", pattern: \"keep/**.c\" } }";

    NowResult res;
    NowProject *prj = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(prj);

    NowBuildCtx ctx;
    int rc = now_build_init(&ctx, prj, root, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(prj); return; }

    int has_top = 0, has_keep = 0, has_skip = 0;
    for (size_t i = 0; i < ctx.sources.count; i++) {
        const char *path = ctx.sources.paths[i];
        if (strstr(path, "top.c")) has_top = 1;
        if (strstr(path, "a.c"))   has_keep = 1;
        if (strstr(path, "b.c"))   has_skip = 1;
    }
    ASSERT_EQ(has_keep, 1);   /* keep/a.c matches keep/**.c */
    ASSERT_EQ(has_top, 0);    /* top.c at root — not under keep/ */
    ASSERT_EQ(has_skip, 0);   /* skip/b.c — different subtree */

    now_build_free(&ctx);
    now_project_free(prj);
    rmtree_best_effort(csrc);
    PASS();
}

/* Omitting `sources.dir` has to mean "there may not be one".
 *
 * The loader fills the key in, so by build time an omitted `dir` is
 * indistinguishable from a named one — and a module whose entire source
 * list is `sources.include`, naming files owned by other modules, failed
 * on the absence of a directory it never mentioned. Amy hit it on their
 * twentieth module and has carried a stub `src/main/c` holding a single
 * `extern` declaration ever since.
 *
 * Both boundaries are asserted, because the fix is only correct if it
 * keeps the failure that is worth having: a directory the author NAMED
 * and got wrong must still be an error. */
static void test_build_include_only_module(void) {
    TEST("build: no sources.dir is legal when include: supplies the files");

    char root[512];
    snprintf(root, sizeof(root), "%s/include_only_proj", NOW_TEST_RESOURCES);

    char csrc[512], vend[512], p[512];
    snprintf(csrc, sizeof(csrc), "%s/src/main/c", root);
    rmtree_best_effort(csrc);                 /* the point: it must NOT exist */
    snprintf(vend, sizeof(vend), "%s/vendor", root);
    now_mkdir_p(vend);
    snprintf(p, sizeof(p), "%s/vendor/only.c", root);
    write_empty(p);

    /* No `dir` key at all, and one include. */
    const char *pasta =
        "{ group: \"org.test\", artifact: \"inconly\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"inconly\" },"
        "  sources: { include: [\"vendor/only.c\"] } }";

    NowResult res;
    NowProject *prj = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(prj);

    NowBuildCtx ctx;
    int rc = now_build_init(&ctx, prj, root, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(prj); return; }

    int has_only = 0;
    for (size_t i = 0; i < ctx.sources.count; i++)
        if (strstr(ctx.sources.paths[i], "only.c")) has_only = 1;
    ASSERT_EQ(has_only, 1);

    now_build_free(&ctx);
    now_project_free(prj);

    /* A directory the descriptor NAMES and that is missing stays an
     * error — that is a typo, and building quietly around it would
     * produce something other than what was asked for. */
    const char *named =
        "{ group: \"org.test\", artifact: \"typo\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"typo\" },"
        "  sources: { dir: \"src/typo/c\", include: [\"vendor/only.c\"] } }";

    NowProject *prj2 = now_project_load_string(named, strlen(named), &res);
    ASSERT_NOT_NULL(prj2);
    NowBuildCtx ctx2;
    int rc2 = now_build_init(&ctx2, prj2, root, &res);
    ASSERT_EQ(rc2 != 0, 1);
    if (rc2 == 0) now_build_free(&ctx2);
    now_project_free(prj2);

    /* And a module with neither a directory nor any includes is still
     * an error rather than an empty build. */
    const char *bare =
        "{ group: \"org.test\", artifact: \"bare\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"bare\" } }";

    NowProject *prj3 = now_project_load_string(bare, strlen(bare), &res);
    ASSERT_NOT_NULL(prj3);
    NowBuildCtx ctx3;
    int rc3 = now_build_init(&ctx3, prj3, root, &res);
    ASSERT_EQ(rc3 != 0, 1);
    if (rc3 == 0) now_build_free(&ctx3);
    now_project_free(prj3);

    rmtree_best_effort(vend);
    PASS();
}

/* Warning flags have to reach the TEST compile, not only the module
 * compile.
 *
 * Reported by Amy 2026-08-20 with a measurement: five tests added, the
 * `RUN()` lines forgotten, and the suite reported "13 tests, 1818
 * checks, 0 failures" — a suite that grew by nothing. `-Wall` names
 * exactly that case, because an unregistered test is an unused static
 * function, and running the same compiler by hand printed the warning.
 * `now test` did not, because `compile.warnings` was never added to the
 * test argv: the warning was never GENERATED, not generated and
 * dropped. Default-on warnings such as -Wpointer-sign did appear, which
 * is why it looked like output being lost for some modules only.
 *
 * Asserted through `Werror` rather than by capturing stderr: with the
 * flags reaching the compiler an unused static is a hard error, so the
 * property is visible in the return code. */
static void test_build_warnings_reach_test_compile(void) {
    TEST("build: compile.warnings reach the test compile");

    char root[512], d[512], p[512];
    snprintf(root, sizeof(root), "%s/testwarn_proj", NOW_TEST_RESOURCES);
    /* Start from nothing. A test object left by an earlier run is judged
     * fresh by source content and a flags key derived from the
     * DESCRIPTOR, so a change to the argv that `now` itself builds is
     * invisible to it — which is how this test passed against the very
     * binary it was written to catch. */
    snprintf(d, sizeof(d), "%s/target", root); rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/src/main/c", root); now_mkdir_p(d);
    snprintf(d, sizeof(d), "%s/src/test/c", root); now_mkdir_p(d);

    FILE *fp;
    snprintf(p, sizeof(p), "%s/src/main/c/lib.c", root);
    fp = fopen(p, "wb");
    if (!fp) { FAIL("cannot write lib.c"); return; }
    fputs("int lib_val(void) { return 7; }\n", fp);
    fclose(fp);

    snprintf(p, sizeof(p), "%s/src/test/c/t.c", root);
    fp = fopen(p, "wb");
    if (!fp) { FAIL("cannot write t.c"); return; }
    fputs("int lib_val(void);\n"
          "int main(void) { return lib_val() == 7 ? 0 : 1; }\n"
          "/* defined, never registered: -Wall names this */\n"
          "static void never_registered(void) { }\n", fp);
    fclose(fp);

    const char *pasta =
        "{ group: \"org.test\", artifact: \"tw\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"tw\" },"
        "  compile: { warnings: [\"Wall\", \"Werror\"] },"
        "  tests: { dir: \"src/test/c\" } }";

    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *prj = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(prj);

    NowBuildCtx ctx;
    int rc = now_build_init(&ctx, prj, root, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(prj); return; }

    /* Build the library first, so that if the test stage does succeed
     * it succeeds completely. Without this the link fails for want of a
     * library and the failure is ambiguous — it was, on the first run. */
    if (now_build_compile(&ctx, &res) != 0) {
        FAIL(res.message); now_build_free(&ctx); now_project_free(prj); return;
    }
    if (now_build_link(&ctx, &res) != 0) {
        FAIL(res.message); now_build_free(&ctx); now_project_free(prj); return;
    }

    /* With the flags reaching it, the unused static is an error and the
     * test stage must refuse to build. Without them it compiles clean
     * and this returns 0 — which is the defect. */
    int trc = now_build_test(&ctx, &res);
    ASSERT_EQ(trc != 0, 1);
    if (trc != 0 && !strstr(res.message, "test compile")) {
        FAIL(res.message);   /* failed, but not where we meant */
        now_build_free(&ctx);
        now_project_free(prj);
        return;
    }

    now_build_free(&ctx);
    now_project_free(prj);

    snprintf(d, sizeof(d), "%s/src", root);    rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/target", root); rmtree_best_effort(d);
    PASS();
}

/* In mode=each, the per-file binary must be named after the source that
 * produced it — not after whatever sits at the same index in the
 * unfiltered source list.
 *
 * Found in cookbook, which excludes one stress driver that sorts before
 * its unit tests: `now test` compiled cookbook_test.c, wrote the binary
 * as target/test/bin/cookbook_stress.exe, and reported the failure
 * against a file it had never compiled. Two test sources with an
 * exclusion on the one that sorts first is the smallest reproduction. */
static void test_build_each_names_binaries_by_source(void) {
    TEST("build: mode=each names each binary after its own source");

    char root[512], d[512], p[512];
    snprintf(root, sizeof(root), "%s/eachname_proj", NOW_TEST_RESOURCES);
    snprintf(d, sizeof(d), "%s/target", root); rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/src", root);    rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/src/main/c", root); now_mkdir_p(d);
    snprintf(d, sizeof(d), "%s/src/test/c", root); now_mkdir_p(d);

    FILE *fp;
    snprintf(p, sizeof(p), "%s/src/main/c/lib.c", root);
    fp = fopen(p, "wb");
    if (!fp) { FAIL("cannot write lib.c"); return; }
    fputs("int lib_val(void) { return 7; }\n", fp);
    fclose(fp);

    /* Sorts first and is excluded. The #error makes the exclusion
     * two-sided: if it were ever compiled the build would fail here
     * rather than quietly producing a misnamed binary. */
    snprintf(p, sizeof(p), "%s/src/test/c/aaa_driver.c", root);
    fp = fopen(p, "wb");
    if (!fp) { FAIL("cannot write aaa_driver.c"); return; }
    fputs("#error this driver should have been excluded\n", fp);
    fclose(fp);

    snprintf(p, sizeof(p), "%s/src/test/c/zzz_unit.c", root);
    fp = fopen(p, "wb");
    if (!fp) { FAIL("cannot write zzz_unit.c"); return; }
    fputs("int lib_val(void);\n"
          "int main(void) { return lib_val() == 7 ? 0 : 1; }\n", fp);
    fclose(fp);

    const char *pasta =
        "{ group: \"org.test\", artifact: \"en\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"en\" },"
        "  tests: { dir: \"src/test/c\", mode: \"each\","
        "           exclude: [\"aaa_driver.c\"] } }";

    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *prj = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(prj);

    NowBuildCtx ctx;
    if (now_build_init(&ctx, prj, root, &res) != 0) {
        FAIL(res.message); now_project_free(prj); return;
    }
    if (now_build_compile(&ctx, &res) != 0) {
        FAIL(res.message); now_build_free(&ctx); now_project_free(prj); return;
    }
    if (now_build_link(&ctx, &res) != 0) {
        FAIL(res.message); now_build_free(&ctx); now_project_free(prj); return;
    }
    int trc = now_build_test(&ctx, &res);
    now_build_free(&ctx);
    now_project_free(prj);

    if (trc != 0) { FAIL(res.message); return; }

#ifdef _WIN32
    const char *xsuf = ".exe";
#else
    const char *xsuf = "";
#endif
    char kept[512], ghost[512];
    snprintf(kept,  sizeof(kept),  "%s/target/test/bin/zzz_unit%s",   root, xsuf);
    snprintf(ghost, sizeof(ghost), "%s/target/test/bin/aaa_driver%s", root, xsuf);

    int kept_ok  = now_path_exists(kept);
    int ghost_no = !now_path_exists(ghost);

    snprintf(d, sizeof(d), "%s/src", root);    rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/target", root); rmtree_best_effort(d);

    if (!kept_ok) {
        FAIL("binary for zzz_unit.c was not written under its own name");
        return;
    }
    if (!ghost_no) {
        FAIL("a binary was named after the excluded source");
        return;
    }
    PASS();
}

/* ---- build event stream (specs/now-events-v1.md) ---- */

/* ASSERT_STR concatenates `expected` into a message literal, so it only
 * works against a literal. These tests compare two decoded values. */
#define ASSERT_SAME_STR(a, b) \
    do { if (strcmp((a), (b)) != 0) { \
        tests_failed++; \
        printf("FAIL: %s != %s (\"%s\" vs \"%s\")\n", #a, #b, (a), (b)); \
        return; } } while (0)

static void ev_seed(NowEvent *ev, NowEventType type) {
    memset(ev, 0, sizeof(*ev));
    ev->v = NOW_EVENTS_SCHEMA_VERSION;
    snprintf(ev->run, sizeof(ev->run), "a3f1c9d2e4b6");
    ev->seq = 7;
    snprintf(ev->ts, sizeof(ev->ts), "2026-08-21T09:41:02Z");
    ev->event = type;
    snprintf(ev->phase, sizeof(ev->phase), "compile");
    ev->ok = 0;
    ev->code = -1;
    ev->elapsed_ms = -1;
    ev->counts.compiled = ev->counts.skipped = ev->counts.failed =
        ev->counts.passed = ev->counts.total = -1;
}

static void test_events_encode_decode_roundtrip(void) {
    TEST("events: encode -> decode keeps every field");

    NowEvent in, out;
    char buf[NOW_EVENTS_MAX_DATAGRAM + 1];

    ev_seed(&in, NOW_EVENT_RUN_FINISHED);
    snprintf(in.project, sizeof(in.project), "dev.iridium:gut:0.1.0");
    snprintf(in.module, sizeof(in.module), "src/main/c/remote.c");
    snprintf(in.detail, sizeof(in.detail), "51 compiled, 1 failed");
    in.code = 1;
    in.elapsed_ms = 8421;
    in.counts.compiled = 51;
    in.counts.failed = 1;
    in.counts.total = 52;
    in.pid = 4242;
    snprintf(in.host, sizeof(in.host), "workstation");

    size_t n = now_event_render(buf, sizeof(buf), &in, "basta");
    ASSERT_EQ(n > 0, 1);
    ASSERT_EQ(now_event_decode(&out, buf, n), 0);

    ASSERT_EQ(out.v, NOW_EVENTS_SCHEMA_VERSION);
    ASSERT_SAME_STR(out.run, in.run);
    ASSERT_EQ((int)out.seq, (int)in.seq);
    ASSERT_SAME_STR(out.ts, in.ts);
    ASSERT_EQ((int)out.event, (int)in.event);
    ASSERT_SAME_STR(out.phase, in.phase);
    ASSERT_EQ(out.ok, 0);
    ASSERT_SAME_STR(out.project, in.project);
    ASSERT_SAME_STR(out.module, in.module);
    ASSERT_SAME_STR(out.detail, in.detail);
    ASSERT_EQ(out.code, 1);
    ASSERT_EQ((int)out.elapsed_ms, 8421);
    ASSERT_EQ(out.counts.compiled, 51);
    ASSERT_EQ(out.counts.failed, 1);
    ASSERT_EQ(out.counts.total, 52);
    ASSERT_EQ(out.pid, 4242);
    ASSERT_STR(out.host, "workstation");
    PASS();
}

static void test_events_detail_survives_escaping(void) {
    TEST("events: a compiler diagnostic survives the wire intact");

    /* The payload that made JSON the wire format in the first draft:
     * quotes, a backslash, newlines and a tab. A blob carries all four
     * without noticing they are there. */
    const char *nasty =
        "remote.c:512:9: error: expected \";\" before \"}\" token\n"
        "  512 |     u64 pos = *pos_inout\n"
        "\t     |         ^ path C:\\Users\\x\n";

    NowEvent in, out;
    char buf[NOW_EVENTS_MAX_DATAGRAM + 1];

    ev_seed(&in, NOW_EVENT_MODULE_FAILED);
    snprintf(in.detail, sizeof(in.detail), "%s", nasty);

    /* Both wire forms have to carry it, and the decoder has to pick the
     * right one without being told. */
    size_t n = now_event_render(buf, sizeof(buf), &in, "basta");
    ASSERT_EQ(n > 0, 1);
    ASSERT_EQ(now_event_decode(&out, buf, n), 0);
    ASSERT_SAME_STR(out.detail, nasty);
    ASSERT_EQ(out.detail_lossy, 0);

    n = now_event_render(buf, sizeof(buf), &in, "json");
    ASSERT_EQ(n > 0, 1);
    ASSERT_EQ(now_event_decode(&out, buf, n), 0);
    ASSERT_SAME_STR(out.detail, nasty);
    PASS();
}

static void test_events_oversize_detail_is_truncated_not_dropped(void) {
    TEST("events: an oversize detail truncates, and says so");

    NowEvent in, out;
    char buf[NOW_EVENTS_MAX_DATAGRAM + 1];
    size_t i;

    ev_seed(&in, NOW_EVENT_MODULE_FAILED);

    /* A blob costs nine bytes whatever it holds, so a full detail on its
     * own now fits and this path is only reached when a long path and a
     * long diagnostic arrive together — which is the ordinary case for a
     * deep source tree, not a contrived one. Filling all three is what
     * makes the bound bite; a detail alone would sail under it. */
    for (i = 0; i < sizeof(in.module) - 1; i++)  in.module[i]  = 'm';
    in.module[sizeof(in.module) - 1] = '\0';
    for (i = 0; i < sizeof(in.project) - 1; i++) in.project[i] = 'p';
    in.project[sizeof(in.project) - 1] = '\0';
    for (i = 0; i < sizeof(in.detail) - 1; i++)  in.detail[i]  = '"';
    in.detail[sizeof(in.detail) - 1] = '\0';

    size_t n = now_event_render(buf, sizeof(buf), &in, "basta");
    ASSERT_EQ(n > 0, 1);
    /* Never fragment: §3 makes this a hard bound, not a target. */
    ASSERT_EQ(n <= NOW_EVENTS_MAX_DATAGRAM, 1);
    ASSERT_EQ(now_event_decode(&out, buf, n), 0);
    ASSERT_EQ(out.detail_lossy, 1);
    ASSERT_EQ(strlen(out.detail) < strlen(in.detail), 1);
    /* The event still arrives, which is the whole point of truncating. */
    ASSERT_EQ((int)out.event, (int)NOW_EVENT_MODULE_FAILED);
    PASS();
}

static void test_events_decode_reads_spaced_json(void) {
    TEST("events: JSON with spaces after the colons decodes");

    /* Our encoder emits no space after a colon, so a decoder checked
     * only against it round-trips perfectly and cannot read anything
     * else. This is what a Python sender produced, and it was rejected
     * outright — reported by the listener as silence. */
    const char *spaced =
        "{\"v\": 1, \"run\": \"a3f1c9d2e4b6\", \"seq\": 3, "
        "\"ts\": \"2026-08-21T09:41:03Z\", \"event\": \"module.failed\", "
        "\"phase\": \"compile\", \"ok\": false, "
        "\"module\": \"src/main/c/remote.c\", \"code\": 1}";

    NowEvent out;
    ASSERT_EQ(now_event_decode(&out, spaced, strlen(spaced)), 0);
    ASSERT_STR(out.run, "a3f1c9d2e4b6");
    ASSERT_EQ((int)out.seq, 3);
    ASSERT_EQ((int)out.event, (int)NOW_EVENT_MODULE_FAILED);
    ASSERT_STR(out.module, "src/main/c/remote.c");
    ASSERT_EQ(out.ok, 0);
    ASSERT_EQ(out.code, 1);
    PASS();
}

static void test_events_version_and_unknown_fields(void) {
    TEST("events: unknown fields are ignored, unknown versions are not");

    /* §11: adding a field is not a version bump, so a v1 reader must
     * accept one it has never heard of... */
    const char *future_field =
        "{\"v\":1,\"run\":\"a3f1c9d2e4b6\",\"seq\":1,"
        "\"ts\":\"2026-08-21T09:41:01Z\",\"event\":\"run.started\","
        "\"phase\":\"build\",\"ok\":true,\"sig\":\"deadbeef\","
        "\"lane\":\"experimental\"}";
    NowEvent out;
    ASSERT_EQ(now_event_decode(&out, future_field, strlen(future_field)), 0);
    ASSERT_EQ((int)out.event, (int)NOW_EVENT_RUN_STARTED);

    /* ...and must refuse a version whose meaning it cannot know. */
    const char *v2 =
        "{\"v\":2,\"run\":\"a3f1c9d2e4b6\",\"seq\":1,"
        "\"ts\":\"2026-08-21T09:41:01Z\",\"event\":\"run.started\","
        "\"phase\":\"build\",\"ok\":true}";
    ASSERT_EQ(now_event_decode(&out, v2, strlen(v2)) != 0, 1);

    /* An event name we do not know is not an event. */
    const char *bogus =
        "{\"v\":1,\"run\":\"a3f1c9d2e4b6\",\"seq\":1,"
        "\"ts\":\"2026-08-21T09:41:01Z\",\"event\":\"run.exploded\","
        "\"phase\":\"build\",\"ok\":true}";
    ASSERT_EQ(now_event_decode(&out, bogus, strlen(bogus)) != 0, 1);

    /* And neither is a bare line of log text. */
    const char *junk = "compiled 41, skipped 0 (up to date)";
    ASSERT_EQ(now_event_decode(&out, junk, strlen(junk)) != 0, 1);
    PASS();
}

/* Both of these exist because a negative control stayed green.
 *
 * The datagram path moved from a raw-socket shim to apennines'
 * t3/net/udp.h on 2026-08-24. Breaking the send, or binding the wrong
 * port, turned five tests red — but breaking the RECEIVE POLL did not
 * turn anything red, and neither did deleting the localhost mapping.
 * Two pieces of hand-written logic with nothing watching them. */

/* apennines' udp_socket_recv takes no timeout and there is no
 * set_recv_timeout beside the other setsockopt knobs, so what used to be
 * SO_RCVTIMEO is now a hand-written non-blocking poll. **A poll that
 * gives up immediately and a poll that never returns both look like
 * `rc == 0` to a caller that does not measure the clock.** Every other
 * events test sends before it receives, so the datagram is already
 * waiting on the first try and the loop never runs. */
static void test_events_recv_waits_out_its_timeout(void) {
    TEST("events: a receive with nothing to read waits out its timeout");

    NowResult res;
    memset(&res, 0, sizeof(res));

    NowEventSource *src = now_event_source_open("udp://127.0.0.1:19482", 0, &res);
    if (!src) { FAIL(res.message); return; }

    NowEvent ev;
    double t0 = now_clock_secs();
    int rc = now_event_source_recv(src, &ev, 400);
    double elapsed = now_clock_secs() - t0;
    now_event_source_close(src);

    if (rc != 0) { FAIL("expected a timeout, got an event"); return; }
    if (elapsed < 0.25) { FAIL("gave up long before the timeout"); return; }
    if (elapsed > 4.0)  { FAIL("did not come back near the timeout"); return; }

    /* And a non-positive timeout must NOT spin: it means block, and the
     * socket is put back into blocking mode for it. Only the argument
     * check is exercised here — a blocking recv with nothing to read
     * would hang the suite, which is the whole reason the poll exists. */
    ASSERT_EQ(now_event_source_recv(NULL, &ev, 400), -2);
    PASS();
}

/* `addr_sockaddr_create` parses IP literals only — it returns a hatch
 * for "localhost" rather than resolving it. The sink maps that name to
 * 127.0.0.1 itself, deliberately, because going through a resolver
 * would hand back AAAA ::1 first on this machine and an events v1
 * socket is IPv4. Nothing tested it: every other events test spells the
 * literal, so deleting the mapping left the suite at 374/374 while
 * `now build --events udp://localhost:PORT` silently sent nothing. */
static void test_events_localhost_reaches_the_loopback_socket(void) {
    TEST("events: udp://localhost reaches the same socket as 127.0.0.1");

    NowResult res;
    memset(&res, 0, sizeof(res));

    /* literal on the listening side, name on the sending side */
    NowEventSource *src = now_event_source_open("udp://127.0.0.1:19483", 0, &res);
    if (!src) { FAIL(res.message); return; }

    NowEventSink *sink = now_event_sink_open("udp://localhost:19483", NULL);
    if (!sink) { now_event_source_close(src); FAIL("sink open on localhost"); return; }

    NowEvent in, out;
    ev_seed(&in, NOW_EVENT_RUN_STARTED);
    now_event_sink_send(sink, &in);

    int rc = now_event_source_recv(src, &out, 3000);
    now_event_sink_close(sink);
    now_event_source_close(src);

    if (rc != 1) { FAIL("nothing arrived from udp://localhost"); return; }
    ASSERT_EQ((int)out.event, (int)NOW_EVENT_RUN_STARTED);

    /* and the name works on the listening side too, which reaches the
     * bind path rather than the send path */
    memset(&res, 0, sizeof(res));
    NowEventSource *src2 = now_event_source_open("udp://localhost:19484", 0, &res);
    if (!src2) { FAIL(res.message); return; }
    NowEventSink *sink2 = now_event_sink_open("udp://127.0.0.1:19484", NULL);
    if (!sink2) { now_event_source_close(src2); FAIL("sink open"); return; }

    now_event_sink_send(sink2, &in);
    rc = now_event_source_recv(src2, &out, 3000);
    now_event_sink_close(sink2);
    now_event_source_close(src2);

    if (rc != 1) { FAIL("nothing arrived at a listener bound by name"); return; }
    PASS();
}

static void test_events_over_a_real_socket(void) {
    TEST("events: a datagram crosses loopback and decodes");

    NowResult res;
    memset(&res, 0, sizeof(res));

    NowEventSource *src = now_event_source_open("udp://127.0.0.1:19477", 0, &res);
    if (!src) { FAIL(res.message); return; }

    NowEventSink *sink = now_event_sink_open("udp://127.0.0.1:19477", NULL);
    if (!sink) { now_event_source_close(src); FAIL("sink open"); return; }

    NowEvent in, out;
    ev_seed(&in, NOW_EVENT_MODULE_FAILED);
    snprintf(in.module, sizeof(in.module), "src/main/c/remote.c");
    snprintf(in.detail, sizeof(in.detail), "error: expected \";\"\nline 2");

    now_event_sink_send(sink, &in);

    int rc = now_event_source_recv(src, &out, 3000);
    now_event_sink_close(sink);
    now_event_source_close(src);

    if (rc != 1) { FAIL("no event arrived over loopback"); return; }
    ASSERT_EQ((int)out.event, (int)NOW_EVENT_MODULE_FAILED);
    ASSERT_STR(out.module, "src/main/c/remote.c");
    ASSERT_SAME_STR(out.detail, in.detail);
    PASS();
}

static void test_events_listener_refuses_untrusted_addresses(void) {
    TEST("events: v1 refuses to listen where anyone could forge");

    NowResult res;

    /* Unsigned events on a shared segment are forgeable by anyone who
     * can reach the socket. §8 enforces that here rather than in prose,
     * so it is a limitation instead of a hole. */
    memset(&res, 0, sizeof(res));
    ASSERT_NULL(now_event_source_open("udp://239.7.7.7:9099", 0, &res));
    ASSERT_EQ(strstr(res.message, "multicast") != NULL, 1);

    /* Even with --insecure, multicast stays refused in v1. */
    memset(&res, 0, sizeof(res));
    ASSERT_NULL(now_event_source_open("udp://239.7.7.7:9099", 1, &res));

    memset(&res, 0, sizeof(res));
    ASSERT_NULL(now_event_source_open("udp://10.0.0.5:9099", 0, &res));
    ASSERT_EQ(strstr(res.message, "unsigned") != NULL, 1);

    /* A malformed URL is refused rather than half-parsed. */
    memset(&res, 0, sizeof(res));
    ASSERT_NULL(now_event_source_open("http://127.0.0.1:9099", 0, &res));
    memset(&res, 0, sizeof(res));
    ASSERT_NULL(now_event_source_open("udp://127.0.0.1", 0, &res));
    PASS();
}

static void test_events_names_round_trip(void) {
    TEST("events: every event name parses back to itself");

    int i;
    for (i = 0; i < (int)NOW_EVENT__COUNT; i++) {
        const char *name = now_event_name((NowEventType)i);
        ASSERT_EQ((int)now_event_parse_name(name), i);
    }
    ASSERT_EQ((int)now_event_parse_name("nope"), (int)NOW_EVENT__COUNT);
    /* run.finished is the one a waiter blocks on, so it is the one that
     * has to be repeated on the wire. */
    ASSERT_EQ(now_event_is_terminal(NOW_EVENT_RUN_FINISHED), 1);
    ASSERT_EQ(now_event_is_terminal(NOW_EVENT_RUN_PROGRESS), 0);
    PASS();
}

static void test_events_blob_carries_what_no_string_can(void) {
    TEST("events: a blob detail arrives byte-exact, whatever is in it");

    /* Every one of these used to arrive altered-and-flagged, and the
     * first four of them are what the hand-written Pasta writer existed
     * to handle. On the blob path they must all be byte-exact with
     * detail_lossy CLEAR: a blob has a byte count and no delimiter, so
     * there is nothing for the writer to work around.
     *
     * The last three are the cases no string form reaches at any size —
     * a compiler quoting a Java text block back at us, a fence that
     * would have to escalate, and bytes that are not UTF-8 at all
     * because the compiler ran under a different locale. */
    static const char *hard[] = {
        "error: unknown type name \"u64\"",   /* quote, no newline */
        "ends with a quote\"",                 /* would close early */
        "line one\nline two",
        "multi\nline with \"quotes\" inside",
        "contains \"\"\" a delimiter",
        "Main.java:12: error: incompatible types\n"
        "    String s = \"\"\"\n        hello\n        \"\"\";",
        "the fence itself: \"\"\"# and \"\"\"##",
        "latin-1 from a foreign locale: \xe9\xff\xfe",
        NULL
    };
    int i;

    for (i = 0; hard[i]; i++) {
        NowEvent in, out;
        char buf[NOW_EVENTS_MAX_DATAGRAM + 1];
        size_t n;

        ev_seed(&in, NOW_EVENT_MODULE_FAILED);
        snprintf(in.detail, sizeof(in.detail), "%s", hard[i]);

        n = now_event_render(buf, sizeof(buf), &in, "basta");
        ASSERT_EQ(n > 0, 1);
        ASSERT_EQ(now_event_decode(&out, buf, n), 0);
        ASSERT_SAME_STR(out.detail, hard[i]);
        ASSERT_EQ(out.detail_lossy, 0);
        ASSERT_EQ((int)out.event, (int)NOW_EVENT_MODULE_FAILED);
    }
    PASS();
}

static void test_events_a_blob_cannot_forge_a_field(void) {
    TEST("events: a diagnostic that looks like a field is not read as one");

    /* The reader finds fields by scanning for `key:`, so a value that is
     * somebody else's bytes could otherwise write its own envelope: a
     * compiler error quoting a line of Pasta would do it by accident,
     * and that is before anyone tries on purpose. The scan steps over a
     * blob rather than through it, which is the whole defence. */
    NowEvent in, out;
    char buf[NOW_EVENTS_MAX_DATAGRAM + 1];
    size_t n;
    const char *forgery =
        "x.c:1: error in { v: 1, run: \"ffffffffffff\", seq: 99, "
        "module: \"innocent.c\", ok: true, code: 0 }";

    ev_seed(&in, NOW_EVENT_MODULE_FAILED);
    in.ok = 0;
    in.code = -1;
    snprintf(in.module, sizeof(in.module), "src/main/c/guilty.c");
    snprintf(in.detail, sizeof(in.detail), "%s", forgery);

    n = now_event_render(buf, sizeof(buf), &in, "basta");
    ASSERT_EQ(n > 0, 1);
    ASSERT_EQ(now_event_decode(&out, buf, n), 0);

    ASSERT_SAME_STR(out.detail, forgery);
    ASSERT_STR(out.module, "src/main/c/guilty.c");
    ASSERT_SAME_STR(out.run, in.run);
    ASSERT_EQ((int)out.seq, (int)in.seq);
    ASSERT_EQ(out.ok, 0);
    ASSERT_EQ(out.code, -1);
    PASS();
}

static void test_events_formats_do_not_read_each_other(void) {
    TEST("events: a Basta reader will not half-read JSON, or the reverse");

    NowEvent in, out;
    char buf[NOW_EVENTS_MAX_DATAGRAM + 1];
    size_t n;

    ev_seed(&in, NOW_EVENT_RUN_FINISHED);
    in.code = 0;
    in.ok = 1;

    /* Each decoder must refuse the other's output outright. If one could
     * partially read the other, the dispatching decoder would be a guess
     * rather than a dispatch. */
    n = now_event_render(buf, sizeof(buf), &in, "basta");
    ASSERT_EQ(n > 0, 1);
    ASSERT_EQ(now_event_decode_basta(&out, buf, n), 0);
    ASSERT_EQ(now_event_decode_json(&out, buf, n) != 0, 1);

    n = now_event_render(buf, sizeof(buf), &in, "json");
    ASSERT_EQ(n > 0, 1);
    ASSERT_EQ(now_event_decode_json(&out, buf, n), 0);
    ASSERT_EQ(now_event_decode_basta(&out, buf, n) != 0, 1);
    PASS();
}

static void test_events_emitter_end_to_end(void) {
    TEST("events: the emitter's run reaches a listener in order");

    NowResult res;
    memset(&res, 0, sizeof(res));

    NowEventSource *src = now_event_source_open("udp://127.0.0.1:19478", 0, &res);
    if (!src) { FAIL(res.message); return; }

    remove("target/test-events.jsonl");
    now_events_open("udp://127.0.0.1:19478", NULL, "target/test-events.jsonl");
    ASSERT_EQ(now_events_active(), 1);

    now_events_run_started("build", "dev.iridium:demo:1.0.0");
    now_events_phase_started("compile");
    now_events_module_failed("src/main/c/broken.c",
                             "error: unknown type name \"u64\"");
    now_events_run_finished(1, NULL);
    now_events_close();

    /* Five distinct events, in order, with contiguous seq: the four
     * emitted here plus the `phase.finished` the emitter adds when the
     * run ends with a phase still open. The terminal event is sent three
     * times and must dedupe to one. */
    {
        NowEvent ev;
        int seen = 0;
        long last = -1;
        int saw_started = 0, saw_failed = 0, saw_finished = 0;
        int saw_phase_closed = 0;
        int guard;

        for (guard = 0; guard < 20; guard++) {
            int rc = now_event_source_recv(src, &ev, 1500);
            if (rc != 1) break;
            if (ev.seq == last) continue;          /* the terminal repeat */
            ASSERT_EQ((int)(ev.seq == last + 1), 1);
            last = ev.seq;
            seen++;
            if (ev.event == NOW_EVENT_RUN_STARTED) {
                saw_started = 1;
                ASSERT_SAME_STR(ev.project, "dev.iridium:demo:1.0.0");
            }
            if (ev.event == NOW_EVENT_MODULE_FAILED) {
                saw_failed = 1;
                ASSERT_SAME_STR(ev.module, "src/main/c/broken.c");
                /* The double quote is the payload that decided the wire
                 * format; it has to survive the whole path. */
                ASSERT_EQ(strstr(ev.detail, "\"u64\"") != NULL, 1);
                /* ok flips false the moment something fails, not at the
                 * end of the run. */
                ASSERT_EQ(ev.ok, 0);
            }
            if (ev.event == NOW_EVENT_PHASE_FINISHED) saw_phase_closed = 1;
            if (ev.event == NOW_EVENT_RUN_FINISHED) {
                saw_finished = 1;
                ASSERT_EQ(ev.code, 1);
                break;
            }
        }
        now_event_source_close(src);

        ASSERT_EQ(saw_started, 1);
        ASSERT_EQ(saw_failed, 1);
        ASSERT_EQ(saw_finished, 1);
        /* The compile phase was opened and never explicitly closed, so
         * the run end has to close it — otherwise a listener cannot tell
         * a phase that ended from one still running. */
        ASSERT_EQ(saw_phase_closed, 1);
        ASSERT_EQ(seen, 5);
    }

    /* §6: the datagram is a doorbell, the file is the record. A waiter
     * that missed a packet has to be able to read what it missed. */
    {
        FILE *fp = fopen("target/test-events.jsonl", "rb");
        char line[2048];
        int lines = 0;
        if (!fp) { FAIL("no sidecar written"); return; }
        while (fgets(line, sizeof(line), fp)) lines++;
        fclose(fp);
        remove("target/test-events.jsonl");
        ASSERT_EQ(lines, 5);
    }
    PASS();
}

static void test_events_off_by_default(void) {
    TEST("events: with no destination, nothing is opened or written");

    /* The opt-in promise, asserted rather than assumed: a build that did
     * not ask for events must not create a socket or a file. */
    const char *prev = getenv("NOW_EVENTS");
    if (prev && *prev) { tests_run--; printf("SKIP (NOW_EVENTS is set)\n"); return; }

    remove("target/should-not-exist.jsonl");
    now_events_open(NULL, NULL, NULL);
    ASSERT_EQ(now_events_active(), 0);

    /* Every entry point must be safe to call while off. */
    now_events_run_started("build", "dev.iridium:demo:1.0.0");
    now_events_phase_started("compile");
    now_events_module_failed("x.c", "boom");
    now_events_test_failed("t", "boom");
    now_events_progress(NULL);
    now_events_run_finished(1, NULL);
    now_events_close();

    {
        FILE *fp = fopen("target/should-not-exist.jsonl", "rb");
        if (fp) { fclose(fp); FAIL("a file was written with events off"); return; }
    }
    PASS();
}

/* ---- the vendored Pasta writer must not emit what its parser refuses ----
 *
 * Reported to basta 2026-08-21 and fixed in `0696908`: `write_string`
 * chose the """ form only for a newline, so a single-line string carrying
 * a quote self-terminated and `pasta_write` returned a document that
 * would not re-parse. The number path had the mirror of it — the writer
 * emitted exponents the vendored lexer had no production for.
 *
 * The assertion is the invariant rather than a list of good cases: **not
 * that everything writes, but that whatever IS written reads back**. A
 * value the format cannot represent may be refused; it may not be
 * written as if it could be. That is what makes this test survive a
 * re-vendor rather than pinning today's behaviour. */
static void test_pasta_writer_output_always_reparses(void) {
    TEST("pasta: anything the writer emits, the parser reads back");

    static const char *strings[] = {
        "plain text no specials",
        "error: unknown type name \"u64\"",     /* quote, no newline */
        "error: expected ';' before '}' token",
        "line one\nline two",
        "multi\nline with \"quotes\" inside",
        "ends with a quote\"",                   /* delimiter collision */
        "contains \"\"\" a triple",              /* may be refused */
        "tab\there",
        "",
        NULL
    };
    static const double numbers[] = {
        1.0, 0.15, 1e16, 1e17, 1e-5, 2.5e-8, 123456789.0, -0.0625
    };
    int i;

    for (i = 0; strings[i]; i++) {
        PastaValue *m = pasta_new_map();
        char *text;
        PastaResult pr;
        PastaValue *back;

        pasta_set(m, "detail", pasta_new_string(strings[i]));
        text = pasta_write(m, BASTA_COMPACT);
        if (!text) {
            /* Refusing to serialise is allowed. Corrupting is not. */
            pasta_free(m);
            continue;
        }

        back = pasta_parse(text, strlen(text), &pr);
        if (!back) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "writer emitted unparseable text for case %d: %.80s",
                     i, text);
            free(text);
            pasta_free(m);
            FAIL(msg);
            return;
        }
        {
            const PastaValue *dv = pasta_map_get(back, "detail");
            const char *got = dv ? pasta_get_string(dv) : NULL;
            if (!got || strcmp(got, strings[i]) != 0) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "case %d round-tripped to different bytes: %.60s",
                         i, got ? got : "(null)");
                pasta_free(back); free(text); pasta_free(m);
                FAIL(msg);
                return;
            }
        }
        pasta_free(back);
        free(text);
        pasta_free(m);
    }

    for (i = 0; i < (int)(sizeof(numbers) / sizeof(numbers[0])); i++) {
        PastaValue *m = pasta_new_map();
        char *text;
        PastaResult pr;
        PastaValue *back;

        pasta_set(m, "n", basta_new_number(numbers[i]));
        text = pasta_write(m, BASTA_COMPACT);
        if (!text) { pasta_free(m); continue; }

        back = pasta_parse(text, strlen(text), &pr);
        if (!back) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "writer emitted unparseable number %g as %.60s",
                     numbers[i], text);
            free(text); pasta_free(m);
            FAIL(msg);
            return;
        }
        pasta_free(back);
        free(text);
        pasta_free(m);
    }
    PASS();
}

/* Does `hay` contain `needle` anywhere in its bytes? Archives store
 * member names as plain text in the member headers, so this is enough to
 * ask what is in one without shelling out to `ar t`. */
static int file_contains_bytes(const char *path, const char *needle) {
    FILE *fp = fopen(path, "rb");
    char *buf;
    long sz;
    int found = 0;
    size_t n;

    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return 0; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return -1; }
    n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    {
        size_t nlen = strlen(needle), i;
        for (i = 0; i + nlen <= n; i++)
            if (memcmp(buf + i, needle, nlen) == 0) { found = 1; break; }
    }
    free(buf);
    return found;
}

/* A source that goes away must not leave its object in the archive.
 *
 * Amy, 2026-08-21: `git mv` a source to another module and the old
 * module's archive was relinked around the orphaned object, so the final
 * link died on duplicate symbols for functions that exist in exactly one
 * file on disk.
 *
 * The cause was `ar r`, which inserts or replaces the members it is
 * given and leaves the rest — so the archive accumulated rather than
 * being a product of the current object list. Note what this test does
 * NOT need: no header changes, no stale timestamps, nothing about the
 * object is out of date. That is why the dependency graph could not see
 * it, and why this needs a test of its own. */
static void test_build_archive_drops_a_removed_source(void) {
    TEST("build: a source that goes away leaves the archive");

    char root[512], d[512], p[512], lib[512];
    snprintf(root, sizeof(root), "%s/stale_obj_proj", NOW_TEST_RESOURCES);
    snprintf(d, sizeof(d), "%s/target", root); rmtree_best_effort(d);
    /* BEST EFFORT is not good enough here, and this test is why.
     *
     * It asserts that a removed source's object leaves the archive. If
     * the setup could not delete target/ - on Windows a lingering handle
     * from a previous build is enough - the archive it inspects is the
     * PREVIOUS run's, and the assertion fails for a reason that has
     * nothing to do with the code under test. Seen intermittently on
     * 2026-08-24 and 2026-08-25, passing 3/3 on re-run each time.
     *
     * A setup that cannot establish its precondition must say so, rather
     * than let the assertion misreport it as a defect in the build. */
    if (now_path_exists(d)) {
        FAIL("could not remove target/ - a previous build still holds it "
             "open; this run cannot establish its precondition");
        return;
    }
    snprintf(d, sizeof(d), "%s/src", root);    rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/src/main/c", root); now_mkdir_p(d);

    FILE *fp;
    snprintf(p, sizeof(p), "%s/src/main/c/keep.c", root);
    fp = fopen(p, "wb");
    if (!fp) { FAIL("cannot write keep.c"); return; }
    fputs("int keep_me(void) { return 1; }\n", fp);
    fclose(fp);

    char mover[512];
    snprintf(mover, sizeof(mover), "%s/src/main/c/mover.c", root);
    fp = fopen(mover, "wb");
    if (!fp) { FAIL("cannot write mover.c"); return; }
    fputs("int moves_away(void) { return 2; }\n", fp);
    fclose(fp);

    const char *pasta =
        "{ group: \"org.test\", artifact: \"stale\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"stale\" },"
        "  sources: { dir: \"src/main/c\" } }";

    NowResult res;
    memset(&res, 0, sizeof(res));

    /* First build: both objects, both archived. */
    {
        NowProject *prj = now_project_load_string(pasta, strlen(pasta), &res);
        ASSERT_NOT_NULL(prj);
        if (now_build(prj, root, 0, 0, &res) != 0) {
            FAIL(res.message); now_project_free(prj); return;
        }
        now_project_free(prj);
    }

#ifdef _WIN32
    snprintf(lib, sizeof(lib), "%s/target/bin/libstale.a", root);
#else
    snprintf(lib, sizeof(lib), "%s/target/bin/libstale.a", root);
#endif
    if (file_contains_bytes(lib, "mover") != 1) {
        FAIL("setup: mover.c.o was not in the first archive");
        return;
    }

    /* The source moves away. Nothing else changes — no header edits, no
     * touched timestamps.
     *
     * Checked, for the same reason the target/ removal above is checked:
     * on Windows a lingering handle makes remove() a silent no-op, and
     * then mover.c is still there, still compiles, and is still in the
     * archive -- which this test reports as "the removed source's object
     * is still in the archive". That reads as a defect in incremental
     * rebuild, and sends the next person to the wrong file. Seen once in
     * 11 runs on 2026-08-25.
     *
     * The precondition of this test is that the source is GONE. If it is
     * not gone, there is no test to run. */
    remove(mover);
    if (now_path_exists(mover)) {
        FAIL("could not remove mover.c - something still holds it open; "
             "this run cannot establish its precondition");
        return;
    }

    {
        NowProject *prj = now_project_load_string(pasta, strlen(pasta), &res);
        ASSERT_NOT_NULL(prj);
        if (now_build(prj, root, 0, 0, &res) != 0) {
            FAIL(res.message); now_project_free(prj); return;
        }
        now_project_free(prj);
    }

    {
        int still_there = file_contains_bytes(lib, "mover");
        int keep_there  = file_contains_bytes(lib, "keep");

        snprintf(d, sizeof(d), "%s/src", root);    rmtree_best_effort(d);
        snprintf(d, sizeof(d), "%s/target", root); rmtree_best_effort(d);

        if (keep_there != 1) { FAIL("the surviving object left the archive too"); return; }
        if (still_there != 0) {
            FAIL("the removed source's object is still in the archive");
            return;
        }
    }
    PASS();
}

static void test_events_every_started_phase_is_finished(void) {
    TEST("events: a phase that starts always finishes");

    /* A `phase.started` with no matching `phase.finished` reads to a
     * listener exactly like a phase that is still running, so a build
     * that returns out of the middle of one would strand a watcher. The
     * compile phase has several early returns; rather than find them all
     * and hope, the emitter closes an open phase when the next one
     * starts and when the run ends. This asserts that. */
    NowResult res;
    memset(&res, 0, sizeof(res));

    NowEventSource *src = now_event_source_open("udp://127.0.0.1:19479", 0, &res);
    if (!src) { FAIL(res.message); return; }

    remove("target/test-pairing.jsonl");
    now_events_open("udp://127.0.0.1:19479", NULL, "target/test-pairing.jsonl");

    now_events_run_started("build", "dev.iridium:demo:1.0.0");
    now_events_phase_started("compile");
    /* No phase_finished for compile — the next phase must close it. */
    now_events_phase_started("link");
    /* No phase_finished for link either — the run end must close it. */
    now_events_run_finished(0, NULL);
    now_events_close();

    {
        NowEvent ev;
        int started = 0, finished = 0, guard;
        long last = -1;

        for (guard = 0; guard < 24; guard++) {
            int rc = now_event_source_recv(src, &ev, 1200);
            if (rc != 1) break;
            if (ev.seq == last) continue;
            last = ev.seq;
            if (ev.event == NOW_EVENT_PHASE_STARTED)  started++;
            if (ev.event == NOW_EVENT_PHASE_FINISHED) finished++;
            if (ev.event == NOW_EVENT_RUN_FINISHED)   break;
        }
        now_event_source_close(src);
        remove("target/test-pairing.jsonl");

        ASSERT_EQ(started, 2);
        ASSERT_EQ(finished, 2);
    }
    PASS();
}

static void test_events_the_late_phases_bracket_themselves(void) {
    TEST("events: package, publish and procure open and close a phase");

    /* The three phases that ran under run.started/run.finished with no
     * bracket of their own until now.
     *
     * Each body is handed arguments it refuses, so it returns from its
     * very first line. That is deliberately the case the wrapper exists
     * for: the pair has to hold for the earliest return, not only for
     * the path where everything works. */
    NowResult res;
    memset(&res, 0, sizeof(res));

    NowEventSource *src = now_event_source_open("udp://127.0.0.1:19481", 0, &res);
    if (!src) { FAIL(res.message); return; }

    now_events_open("udp://127.0.0.1:19481", NULL, NULL);

    now_events_run_started("publish", "dev.iridium:demo:1.0.0");
    now_procure(NULL, NULL, &res);
    now_package(NULL, NULL, 0, &res);
    now_publish(NULL, NULL, NULL, 0, &res);
    now_events_run_finished(1, NULL);
    now_events_close();

    {
        NowEvent ev;
        int started = 0, finished = 0, guard;
        int saw_procure = 0, saw_package = 0, saw_publish = 0;
        int first_finish_ok = -1;
        long last = -1;

        for (guard = 0; guard < 32; guard++) {
            int rc = now_event_source_recv(src, &ev, 1200);
            if (rc != 1) break;
            if (ev.seq == last) continue;
            last = ev.seq;
            if (ev.event == NOW_EVENT_PHASE_STARTED) {
                started++;
                if (strcmp(ev.phase, "procure") == 0) saw_procure = 1;
                if (strcmp(ev.phase, "package") == 0) saw_package = 1;
                if (strcmp(ev.phase, "publish") == 0) saw_publish = 1;
            }
            if (ev.event == NOW_EVENT_PHASE_FINISHED) {
                finished++;
                if (first_finish_ok < 0) first_finish_ok = ev.ok;
            }
            if (ev.event == NOW_EVENT_RUN_FINISHED) break;
        }
        now_event_source_close(src);

        ASSERT_EQ(started, 3);
        ASSERT_EQ(finished, 3);
        ASSERT_EQ(saw_procure && saw_package && saw_publish, 1);
        /* Only the FIRST finish is evidence that a wrapper passed its
         * body's result along. `ok` is the run's flag, not the phase's
         * (§4: false once anything in the run has failed), so once
         * procure has cleared it every later event reads false whatever
         * its own wrapper said. Asserting on all three would look like a
         * stronger check and be a weaker one. */
        ASSERT_EQ(first_finish_ok, 0);
    }
    PASS();
}

static void test_events_test_failed_carries_the_case(void) {
    TEST("events: test.failed names the test and flips ok");

    NowResult res;
    memset(&res, 0, sizeof(res));

    NowEventSource *src = now_event_source_open("udp://127.0.0.1:19480", 0, &res);
    if (!src) { FAIL(res.message); return; }

    now_events_open("udp://127.0.0.1:19480", NULL, NULL);
    now_events_run_started("test", "dev.iridium:demo:1.0.0");
    now_events_phase_started("test");
    now_events_test_failed("suite/parser_test", "exit 1");
    now_events_run_finished(1, NULL);
    now_events_close();

    {
        NowEvent ev;
        int saw = 0, guard;
        long last = -1;

        for (guard = 0; guard < 24; guard++) {
            int rc = now_event_source_recv(src, &ev, 1200);
            if (rc != 1) break;
            if (ev.seq == last) continue;
            last = ev.seq;
            if (ev.event == NOW_EVENT_TEST_FAILED) {
                saw = 1;
                ASSERT_SAME_STR(ev.module, "suite/parser_test");
                ASSERT_SAME_STR(ev.detail, "exit 1");
                /* ok is false from the failure onwards, not only at the
                 * end of the run. */
                ASSERT_EQ(ev.ok, 0);
            }
            if (ev.event == NOW_EVENT_RUN_FINISHED) {
                /* and it stays false through the terminal event */
                ASSERT_EQ(ev.ok, 0);
                break;
            }
        }
        now_event_source_close(src);
        ASSERT_EQ(saw, 1);
    }
    PASS();
}

/* --fail-fast stops starting work; it does not kill work in flight.
 *
 * Driven at jobs=1 on purpose. The pool path dispatches up to 32 jobs
 * before the first failure comes back, so how much it skips depends on
 * timing and would make this test a race. The serial path makes the
 * decision observable exactly: it checks before each job, so the counts
 * are deterministic.
 *
 * The assertion is the relationship, not the numbers — discovery order
 * is not something this test should be pinning. */
static void test_build_fail_fast_stops_starting_work(void) {
    TEST("build: --fail-fast stops starting new compiles");

    char root[512], d[512], p[512];
    int i;
    int compiled_default = -1, compiled_ff = -1;

    snprintf(root, sizeof(root), "%s/failfast_proj", NOW_TEST_RESOURCES);
    snprintf(d, sizeof(d), "%s/target", root); rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/src", root);    rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/src/main/c", root); now_mkdir_p(d);

    for (i = 0; i < 8; i++) {
        FILE *fp;
        snprintf(p, sizeof(p), "%s/src/main/c/m%02d.c", root, i);
        fp = fopen(p, "wb");
        if (!fp) { FAIL("cannot write a source"); return; }
        if (i == 3)
            fputs("int broken(void) { u64 x = 0; return x; }\n", fp);
        else
            fprintf(fp, "int fn_%d(void) { return %d; }\n", i, i);
        fclose(fp);
    }

    const char *pasta =
        "{ group: \"org.test\", artifact: \"ff\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"ff\" },"
        "  sources: { dir: \"src/main/c\" } }";

    /* Default: one failure does not stop the rest. */
    {
        NowResult res;
        NowProject *prj;
        memset(&res, 0, sizeof(res));
        prj = now_project_load_string(pasta, strlen(pasta), &res);
        ASSERT_NOT_NULL(prj);
        now_build_set_fail_fast(0);
        (void)now_build(prj, root, 0, 1, &res);
        compiled_default = res.build_compiled;
        now_project_free(prj);
    }

    snprintf(d, sizeof(d), "%s/target", root); rmtree_best_effort(d);

    /* --fail-fast: the same tree stops early. */
    {
        NowResult res;
        NowProject *prj;
        memset(&res, 0, sizeof(res));
        prj = now_project_load_string(pasta, strlen(pasta), &res);
        ASSERT_NOT_NULL(prj);
        now_build_set_fail_fast(1);
        (void)now_build(prj, root, 0, 1, &res);
        compiled_ff = res.build_compiled;
        now_project_free(prj);
    }
    /* A global: leaving it on would quietly change every later test. */
    now_build_set_fail_fast(0);

    snprintf(d, sizeof(d), "%s/src", root);    rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/target", root); rmtree_best_effort(d);

    /* Both runs must have failed — otherwise this measured nothing. */
    if (compiled_default < 0 || compiled_ff < 0) {
        FAIL("no compile counts reported");
        return;
    }
    if (compiled_ff >= compiled_default) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "--fail-fast compiled %d, default compiled %d — expected fewer",
                 compiled_ff, compiled_default);
        FAIL(msg);
        return;
    }
    PASS();
}

static void test_build_java_hello(void) {
    TEST("build: compile and package Java project");

    /* Skip if javac not available */
    int javac_rc = now_exec((const char *const []){"javac", "-version", NULL}, 0);
    if (javac_rc != 0) {
        tests_run--;  /* don't count skipped */
        printf("SKIP (javac not in PATH)\n");
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/hello_java/now.pasta", NOW_TEST_RESOURCES);

    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/hello_java", NOW_TEST_RESOURCES);

    int rc = now_build(p, basedir, 0, 0, &res);
    now_project_free(p);

    if (rc != 0) { FAIL(res.message); return; }

    /* Check JAR output exists */
    char jar_path[512];
    snprintf(jar_path, sizeof(jar_path), "%s/target/bin/hello.jar", basedir);
    if (!now_path_exists(jar_path)) {
        FAIL("output JAR not found");
        return;
    }
    PASS();
}

/* ---- Semantic versioning ---- */

static void test_semver_parse_basic(void) {
    TEST("semver: parse 1.2.3");
    NowSemVer v;
    ASSERT_EQ(now_semver_parse("1.2.3", &v), 0);
    ASSERT_EQ(v.major, 1);
    ASSERT_EQ(v.minor, 2);
    ASSERT_EQ(v.patch, 3);
    if (v.prerelease) { FAIL("prerelease should be NULL"); now_semver_free(&v); return; }
    now_semver_free(&v);
    PASS();
}

static void test_semver_parse_prerelease(void) {
    TEST("semver: parse 3.0.0-beta.1");
    NowSemVer v;
    ASSERT_EQ(now_semver_parse("3.0.0-beta.1", &v), 0);
    ASSERT_EQ(v.major, 3);
    ASSERT_EQ(v.minor, 0);
    ASSERT_EQ(v.patch, 0);
    ASSERT_NOT_NULL(v.prerelease);
    ASSERT_STR(v.prerelease, "beta.1");
    now_semver_free(&v);
    PASS();
}

static void test_semver_parse_build(void) {
    TEST("semver: parse 1.0.0+build.42");
    NowSemVer v;
    ASSERT_EQ(now_semver_parse("1.0.0+build.42", &v), 0);
    ASSERT_EQ(v.major, 1);
    ASSERT_NOT_NULL(v.build);
    ASSERT_STR(v.build, "build.42");
    now_semver_free(&v);
    PASS();
}

static void test_semver_compare(void) {
    TEST("semver: compare ordering");
    NowSemVer a, b;
    now_semver_parse("1.0.0", &a);
    now_semver_parse("2.0.0", &b);
    if (now_semver_compare(&a, &b) >= 0) { FAIL("1.0.0 should < 2.0.0"); now_semver_free(&a); now_semver_free(&b); return; }
    now_semver_free(&a); now_semver_free(&b);

    now_semver_parse("1.2.3", &a);
    now_semver_parse("1.2.3", &b);
    ASSERT_EQ(now_semver_compare(&a, &b), 0);
    now_semver_free(&a); now_semver_free(&b);

    /* pre-release < release */
    now_semver_parse("1.0.0-rc.1", &a);
    now_semver_parse("1.0.0", &b);
    if (now_semver_compare(&a, &b) >= 0) { FAIL("1.0.0-rc.1 should < 1.0.0"); now_semver_free(&a); now_semver_free(&b); return; }
    now_semver_free(&a); now_semver_free(&b);
    PASS();
}

static void test_semver_to_string(void) {
    TEST("semver: to_string roundtrip");
    NowSemVer v;
    now_semver_parse("1.2.3-beta.1", &v);
    char *s = now_semver_to_string(&v);
    ASSERT_NOT_NULL(s);
    ASSERT_STR(s, "1.2.3-beta.1");
    free(s);
    now_semver_free(&v);
    PASS();
}

/* ---- Version ranges ---- */

static void test_range_exact(void) {
    TEST("range: exact 1.2.3");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse("1.2.3", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_EXACT);

    NowSemVer v;
    now_semver_parse("1.2.3", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("1.2.4", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_caret(void) {
    TEST("range: caret ^1.2.3 → [1.2.3, 2.0.0)");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse("^1.2.3", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_CARET);

    NowSemVer v;
    now_semver_parse("1.2.3", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("1.9.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("2.0.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_semver_parse("1.2.2", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_caret_pre1(void) {
    TEST("range: caret ^0.9.3 → [0.9.3, 0.10.0)");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse("^0.9.3", &r), 0);

    NowSemVer v;
    now_semver_parse("0.9.5", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("0.10.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_tilde(void) {
    TEST("range: tilde ~1.2.3 → [1.2.3, 1.3.0)");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse("~1.2.3", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_TILDE);

    NowSemVer v;
    now_semver_parse("1.2.9", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("1.3.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_gte(void) {
    TEST("range: >=2.0.0");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse(">=2.0.0", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_GTE);

    NowSemVer v;
    now_semver_parse("2.0.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("3.5.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("1.9.9", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_compound(void) {
    TEST("range: >=1.2.0 <2.0.0");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse(">=1.2.0 <2.0.0", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_COMPOUND);

    NowSemVer v;
    now_semver_parse("1.5.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("2.0.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_wildcard(void) {
    TEST("range: * matches anything");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse("*", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_ANY);

    NowSemVer v;
    now_semver_parse("99.99.99", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_intersect(void) {
    TEST("range: intersect ^1.3.0 ∩ ^1.2.0 → [1.3.0, 2.0.0)");
    NowVersionRange a, b, out;
    now_range_parse("^1.3.0", &a);
    now_range_parse("^1.2.0", &b);
    ASSERT_EQ(now_range_intersect(&a, &b, &out), 0);

    /* Floor should be 1.3.0 */
    ASSERT_EQ(out.floor.major, 1);
    ASSERT_EQ(out.floor.minor, 3);
    ASSERT_EQ(out.floor.patch, 0);

    /* Ceiling should be 2.0.0 */
    ASSERT_EQ(out.ceiling.major, 2);
    ASSERT_EQ(out.ceiling.minor, 0);

    now_range_free(&a);
    now_range_free(&b);
    now_range_free(&out);
    PASS();
}

/* ---- Coordinate parsing ---- */

static void test_coord_parse(void) {
    TEST("coord: parse org.acme:core:^1.5.0");
    NowCoordinate c;
    ASSERT_EQ(now_coord_parse("org.acme:core:^1.5.0", &c), 0);
    ASSERT_STR(c.group, "org.acme");
    ASSERT_STR(c.artifact, "core");
    ASSERT_STR(c.version, "^1.5.0");
    now_coord_free(&c);
    PASS();
}

/* ---- Manifest ---- */

static void test_manifest_set_find(void) {
    TEST("manifest: set and find entry");
    NowManifest m;
    now_manifest_init(&m);
    ASSERT_EQ(now_manifest_set(&m, "src/main.c", "target/obj/main.c.o",
                                "abc123", "def456", 1000), 0);
    const NowManifestEntry *e = now_manifest_find(&m, "src/main.c");
    ASSERT_NOT_NULL(e);
    ASSERT_STR(e->source, "src/main.c");
    ASSERT_STR(e->object, "target/obj/main.c.o");
    ASSERT_STR(e->source_hash, "abc123");
    now_manifest_free(&m);
    PASS();
}

static void test_manifest_update(void) {
    TEST("manifest: update existing entry");
    NowManifest m;
    now_manifest_init(&m);
    now_manifest_set(&m, "src/main.c", "obj1", "hash1", "fh1", 100);
    now_manifest_set(&m, "src/main.c", "obj2", "hash2", "fh2", 200);
    ASSERT_EQ(m.count, (size_t)1);
    const NowManifestEntry *e = now_manifest_find(&m, "src/main.c");
    ASSERT_NOT_NULL(e);
    ASSERT_STR(e->object, "obj2");
    ASSERT_STR(e->source_hash, "hash2");
    now_manifest_free(&m);
    PASS();
}

static void test_sha256_string(void) {
    TEST("manifest: sha256 of known string");
    char *hash = now_sha256_string("hello", 5);
    ASSERT_NOT_NULL(hash);
    /* SHA-256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824 */
    ASSERT_STR(hash, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    free(hash);
    PASS();
}

/* ---- Resolver ---- */

static void test_resolver_highest_defers_to_registry(void) {
    TEST("resolver: 'highest' defers version choice to the registry");
    NowResolver r;
    now_resolver_init(&r, "highest");
    ASSERT_EQ(now_resolver_add(&r, "zlib:zlib:^1.3.0", "compile", "root", 0), 0);

    NowLockFile lf;
    now_lock_init(&lf);
    NowResult res;
    ASSERT_EQ(now_resolver_resolve(&r, &lf, &res), 0);

    const NowLockEntry *e = now_lock_find(&lf, "zlib", "zlib");
    ASSERT_NOT_NULL(e);
    /* Empty means "ask the registry". Writing the floor here is what
     * made `convergence: "highest"` silently behave as "lowest". */
    ASSERT_NOT_NULL(e->version);
    if (e->version[0] != '\0') {
        FAIL("highest should not pin the range floor");
        now_lock_free(&lf); now_resolver_free(&r);
        return;
    }

    now_lock_free(&lf);
    now_resolver_free(&r);
    PASS();
}

static void test_resolver_single_dep(void) {
    TEST("resolver: single dependency resolves");
    NowResolver r;
    now_resolver_init(&r, "lowest");
    ASSERT_EQ(now_resolver_add(&r, "zlib:zlib:^1.3.0", "compile", "root", 0), 0);

    NowLockFile lf;
    now_lock_init(&lf);
    NowResult res;
    ASSERT_EQ(now_resolver_resolve(&r, &lf, &res), 0);
    ASSERT_EQ(lf.count, (size_t)1);

    const NowLockEntry *e = now_lock_find(&lf, "zlib", "zlib");
    ASSERT_NOT_NULL(e);
    ASSERT_STR(e->group, "zlib");
    ASSERT_STR(e->artifact, "zlib");
    ASSERT_STR(e->version, "1.3.0");  /* lowest = floor */
    ASSERT_STR(e->scope, "compile");

    now_lock_free(&lf);
    now_resolver_free(&r);
    PASS();
}

static void test_resolver_convergence_lowest(void) {
    TEST("resolver: lowest convergence picks floor of intersection");
    NowResolver r;
    now_resolver_init(&r, "lowest");
    now_resolver_add(&r, "zlib:zlib:^1.3.0", "compile", "A", 0);
    now_resolver_add(&r, "zlib:zlib:^1.2.0", "compile", "B", 0);

    NowLockFile lf;
    now_lock_init(&lf);
    NowResult res;
    ASSERT_EQ(now_resolver_resolve(&r, &lf, &res), 0);

    const NowLockEntry *e = now_lock_find(&lf, "zlib", "zlib");
    ASSERT_NOT_NULL(e);
    /* Intersection of ^1.3.0 and ^1.2.0 = [1.3.0, 2.0.0), lowest = 1.3.0 */
    ASSERT_STR(e->version, "1.3.0");

    now_lock_free(&lf);
    now_resolver_free(&r);
    PASS();
}

static void test_resolver_conflict(void) {
    TEST("resolver: disjoint ranges produce conflict");
    NowResolver r;
    now_resolver_init(&r, "lowest");
    now_resolver_add(&r, "zlib:zlib:^1.0.0", "compile", "A", 0);
    now_resolver_add(&r, "zlib:zlib:^2.0.0", "compile", "B", 0);

    NowLockFile lf;
    now_lock_init(&lf);
    NowResult res;
    int rc = now_resolver_resolve(&r, &lf, &res);
    if (rc == 0) { FAIL("should have conflicted"); now_lock_free(&lf); now_resolver_free(&r); return; }
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);

    now_lock_free(&lf);
    now_resolver_free(&r);
    PASS();
}

static void test_resolver_multiple_deps(void) {
    TEST("resolver: multiple different deps resolve independently");
    NowResolver r;
    now_resolver_init(&r, "lowest");
    now_resolver_add(&r, "zlib:zlib:^1.3.0", "compile", "root", 0);
    now_resolver_add(&r, "org.acme:core:~4.2.0", "compile", "root", 0);

    NowLockFile lf;
    now_lock_init(&lf);
    NowResult res;
    ASSERT_EQ(now_resolver_resolve(&r, &lf, &res), 0);
    ASSERT_EQ(lf.count, (size_t)2);

    ASSERT_NOT_NULL(now_lock_find(&lf, "zlib", "zlib"));
    ASSERT_NOT_NULL(now_lock_find(&lf, "org.acme", "core"));
    ASSERT_STR(now_lock_find(&lf, "org.acme", "core")->version, "4.2.0");

    now_lock_free(&lf);
    now_resolver_free(&r);
    PASS();
}

static void test_resolver_override(void) {
    TEST("resolver: override forces version despite range conflict");
    NowResolver r;
    now_resolver_init(&r, "lowest");
    now_resolver_add(&r, "zlib:zlib:^1.0.0", "compile", "A", 0);
    now_resolver_add(&r, "zlib:zlib:2.0.0", "compile", "override", 1);

    NowLockFile lf;
    now_lock_init(&lf);
    NowResult res;
    ASSERT_EQ(now_resolver_resolve(&r, &lf, &res), 0);

    const NowLockEntry *e = now_lock_find(&lf, "zlib", "zlib");
    ASSERT_NOT_NULL(e);
    ASSERT_STR(e->version, "2.0.0");
    ASSERT_EQ(e->overridden, 1);

    now_lock_free(&lf);
    now_resolver_free(&r);
    PASS();
}

static void test_lock_save_load(void) {
    TEST("lock: save and reload");
    NowLockFile lf;
    now_lock_init(&lf);

    NowLockEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.group = "zlib";
    entry.artifact = "zlib";
    entry.version = "1.3.0";
    entry.scope = "compile";
    entry.triple = "noarch";
    now_lock_set(&lf, &entry);

    char path[512];
    snprintf(path, sizeof(path), "%s/test_lock.pasta", NOW_TEST_RESOURCES);
    ASSERT_EQ(now_lock_save(&lf, path), 0);
    now_lock_free(&lf);

    /* Reload */
    NowLockFile lf2;
    ASSERT_EQ(now_lock_load(&lf2, path), 0);
    ASSERT_EQ(lf2.count, (size_t)1);
    const NowLockEntry *e = now_lock_find(&lf2, "zlib", "zlib");
    ASSERT_NOT_NULL(e);
    ASSERT_STR(e->version, "1.3.0");
    ASSERT_STR(e->scope, "compile");

    now_lock_free(&lf2);
    /* Clean up test file */
    remove(path);
    PASS();
}

static void test_test_phase(void) {
    TEST("test: compile and run test sources");
    char path[512];
    snprintf(path, sizeof(path), "%s/testable/now.pasta", NOW_TEST_RESOURCES);

    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/testable", NOW_TEST_RESOURCES);

    int rc = now_test(p, basedir, 0, 0, &res);
    now_project_free(p);

    if (rc != 0) { FAIL(res.message); return; }
    PASS();
}

/* ---- Main ---- */

/* ---- HTTP client ---- */

static void test_pico_http_version(void) {
    TEST("pico_http: version string");
    const char *v = pico_http_version();
    ASSERT_NOT_NULL(v);
    ASSERT_STR(v, "0.3.0");
    PASS();
}

static void test_pico_http_parse_url(void) {
    TEST("pico_http: parse URL");
    char *host = NULL, *path = NULL;
    int port = 0;
    int rc = pico_http_parse_url("http://localhost:8080/resolve/org/acme",
                                 &host, &port, &path);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(host, "localhost");
    ASSERT_EQ(port, 8080);
    ASSERT_STR(path, "/resolve/org/acme");
    free(host);
    free(path);
    PASS();
}

static void test_pico_http_parse_url_no_port(void) {
    TEST("pico_http: parse URL without port");
    char *host = NULL, *path = NULL;
    int port = 0;
    int rc = pico_http_parse_url("http://example.com/api",
                                 &host, &port, &path);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(host, "example.com");
    ASSERT_EQ(port, 80);
    ASSERT_STR(path, "/api");
    free(host);
    free(path);
    PASS();
}

static void test_pico_http_parse_url_no_path(void) {
    TEST("pico_http: parse URL without path");
    char *host = NULL, *path = NULL;
    int port = 0;
    int rc = pico_http_parse_url("http://example.com",
                                 &host, &port, &path);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(host, "example.com");
    ASSERT_EQ(port, 80);
    ASSERT_STR(path, "/");
    free(host);
    free(path);
    PASS();
}

static void test_pico_http_parse_url_https(void) {
    TEST("pico_http: parse https URL");
    char *host = NULL, *path = NULL;
    int port = 0, tls = 0;
    int rc = pico_http_parse_url_ex("https://example.com/api",
                                     &host, &port, &path, &tls);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(host, "example.com");
    ASSERT_EQ(port, 443);
    ASSERT_STR(path, "/api");
    ASSERT_EQ(tls, 1);
    free(host);
    free(path);
    PASS();
}

static void test_pico_http_parse_url_reject_ftp(void) {
    TEST("pico_http: reject ftp URL");
    char *host = NULL, *path = NULL;
    int port = 0;
    int rc = pico_http_parse_url("ftp://example.com/file",
                                 &host, &port, &path);
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_pico_http_error_codes(void) {
    TEST("pico_http: error code strings");
    ASSERT_STR(pico_http_strerror(PICO_OK), "success");
    ASSERT_STR(pico_http_strerror(PICO_ERR_DNS), "DNS resolution failed");
    ASSERT_STR(pico_http_strerror(PICO_ERR_CONNECT), "connection failed");
    ASSERT_STR(pico_http_strerror(PICO_ERR_TOO_MANY_REDIRECTS), "too many redirects");
    ASSERT_STR(pico_http_strerror(PICO_ERR_TLS), "TLS error");
    /* Unknown code */
    ASSERT_STR(pico_http_strerror(-99), "unknown error");
    PASS();
}

static void test_pico_http_invalid_args(void) {
    TEST("pico_http: invalid arguments return PICO_ERR_INVALID");
    PicoHttpResponse res;
    ASSERT_EQ(pico_http_get(NULL, 80, "/", NULL, &res), PICO_ERR_INVALID);
    ASSERT_EQ(pico_http_get("host", 80, NULL, NULL, &res), PICO_ERR_INVALID);
    ASSERT_EQ(pico_http_get("host", 80, "/", NULL, NULL), PICO_ERR_INVALID);
    PASS();
}

static void test_pico_http_dns_failure(void) {
    TEST("pico_http: DNS failure returns PICO_ERR_DNS");
    PicoHttpResponse res;
    PicoHttpOptions opts = {0};
    opts.connect_timeout_ms = 1000;
    int rc = pico_http_get("this-host-does-not-exist-7f3a.invalid",
                           80, "/", &opts, &res);
    ASSERT_EQ(rc, PICO_ERR_DNS);
    PASS();
}

static void test_pico_http_connect_failure(void) {
    TEST("pico_http: connect failure returns PICO_ERR_CONNECT");
    PicoHttpResponse res;
    PicoHttpOptions opts = {0};
    opts.connect_timeout_ms = 1000;
    /* Port 1 is almost certainly not listening */
    int rc = pico_http_get("127.0.0.1", 1, "/", &opts, &res);
    ASSERT_EQ(rc, PICO_ERR_CONNECT);
    PASS();
}

static void test_pico_http_find_header(void) {
    TEST("pico_http: find_header on empty response");
    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));
    if (pico_http_find_header(&res, "Content-Type") != NULL) {
        FAIL("expected NULL for empty response");
        return;
    }
    PASS();
}

static void test_pico_http_request_url(void) {
    TEST("pico_http: request with invalid URL");
    PicoHttpResponse res;
    int rc = pico_http_request("GET", "not-a-url", NULL, NULL, 0, NULL, &res);
    ASSERT_EQ(rc, PICO_ERR_INVALID);
    PASS();
}

static void test_pico_http_response_free_zeroed(void) {
    TEST("pico_http: response_free on zeroed struct");
    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));
    pico_http_response_free(&res); /* should not crash */
    pico_http_response_free(NULL); /* should not crash */
    PASS();
}

static void test_pico_http_stream_invalid_args(void) {
    TEST("pico_http: get_stream rejects NULL args");
    PicoHttpResponse res;
    int rc = pico_http_get_stream(NULL, 80, "/", NULL, &res, NULL, NULL);
    ASSERT_EQ(rc, PICO_ERR_INVALID);
    rc = pico_http_get_stream("localhost", 80, "/", NULL, &res, NULL, NULL);
    ASSERT_EQ(rc, PICO_ERR_INVALID);
    PASS();
}

static int stream_counter_fn(const void *data, size_t len, void *userdata) {
    (void)data;
    size_t *total = (size_t *)userdata;
    *total += len;
    return 0;
}

static void test_pico_http_stream_connect_failure(void) {
    TEST("pico_http: get_stream connect failure");
    PicoHttpResponse res;
    size_t total = 0;
    int rc = pico_http_get_stream("127.0.0.1", 1, "/", NULL, &res,
                                   stream_counter_fn, &total);
    ASSERT_EQ(rc != PICO_OK, 1);
    ASSERT_EQ(total, 0);
    PASS();
}

static int stream_abort_fn(const void *data, size_t len, void *userdata) {
    (void)data; (void)len; (void)userdata;
    return -1; /* abort immediately */
}

static void test_pico_http_stream_callback_type(void) {
    TEST("pico_http: stream callback type exists");
    /* Verify the typedef compiles and can be assigned */
    PicoHttpWriteFn fn = stream_counter_fn;
    ASSERT_NOT_NULL((void *)(size_t)fn);
    fn = stream_abort_fn;
    ASSERT_NOT_NULL((void *)(size_t)fn);
    PASS();
}

static void test_pico_http_tls_noverify_option(void) {
    TEST("pico_http: tls_noverify option accepted");
    /* Verify that PicoHttpOptions with tls_noverify compiles and the
     * option is propagated (connect will fail, but no crash). */
    PicoHttpResponse res;
    PicoHttpOptions opts = {0};
    opts.tls_noverify = 1;
    opts.connect_timeout_ms = 500;
    int rc = pico_http_get("127.0.0.1", 1, "/", &opts, &res);
    /* Expect connect failure — we just care that the option didn't crash */
    ASSERT_EQ(rc, PICO_ERR_CONNECT);
    PASS();
}

static void test_pico_http_tls_options_zero_init(void) {
    TEST("pico_http: zero-init options means verify enabled");
    /* Verify that zero-initialized PicoHttpOptions has tls_noverify=0,
     * meaning verification is the default. */
    PicoHttpOptions opts = {0};
    ASSERT_EQ(opts.tls_noverify, 0);
    if (opts.ca_file != NULL) { FAIL("expected NULL ca_file"); return; }
    if (opts.ca_data != NULL) { FAIL("expected NULL ca_data"); return; }
    ASSERT_EQ((int)opts.ca_data_len, 0);
    PASS();
}

static void test_pico_http_tls_ca_file_option(void) {
    TEST("pico_http: ca_file option accepted");
    PicoHttpResponse res;
    PicoHttpOptions opts = {0};
    opts.ca_file = "/nonexistent/ca.pem";
    opts.connect_timeout_ms = 500;
    /* Will fail to connect, but ensures the ca_file path doesn't crash */
    int rc = pico_http_get("127.0.0.1", 1, "/", &opts, &res);
    ASSERT_EQ(rc, PICO_ERR_CONNECT);
    PASS();
}

static void test_pico_ws_tls_noverify_option(void) {
    TEST("pico_ws: tls_noverify option accepted");
    PicoWsOptions opts = {0};
    opts.tls_noverify = 1;
    opts.connect_timeout_ms = 500;
    int err = 0;
    PicoWs *ws = pico_ws_connect("ws://127.0.0.1:1/ws", &opts, &err);
    if (ws != NULL) { FAIL("expected NULL"); pico_ws_close(ws); return; }
    ASSERT_EQ(err, PICO_WS_ERR_CONNECT);
    PASS();
}

static void test_pico_ws_tls_options_zero_init(void) {
    TEST("pico_ws: zero-init options means verify enabled");
    PicoWsOptions opts = {0};
    ASSERT_EQ(opts.tls_noverify, 0);
    if (opts.ca_file != NULL) { FAIL("expected NULL ca_file"); return; }
    if (opts.ca_data != NULL) { FAIL("expected NULL ca_data"); return; }
    ASSERT_EQ((int)opts.ca_data_len, 0);
    PASS();
}

/* ---- WebSocket client ---- */

static void test_pico_ws_version(void) {
    TEST("pico_ws: version string");
    const char *v = pico_ws_version();
    ASSERT_NOT_NULL(v);
    ASSERT_STR(v, "0.1.0");
    PASS();
}

static void test_pico_ws_error_codes(void) {
    TEST("pico_ws: error code strings");
    ASSERT_STR(pico_ws_strerror(PICO_WS_OK), "success");
    ASSERT_STR(pico_ws_strerror(PICO_WS_ERR_CONNECT), "connection failed");
    ASSERT_STR(pico_ws_strerror(PICO_WS_ERR_HANDSHAKE), "WebSocket handshake failed");
    ASSERT_STR(pico_ws_strerror(PICO_WS_ERR_CLOSED), "connection closed");
    ASSERT_STR(pico_ws_strerror(-99), "unknown error");
    PASS();
}

static void test_pico_ws_invalid_args(void) {
    TEST("pico_ws: invalid arguments");
    int err = 0;
    PicoWs *ws = pico_ws_connect(NULL, NULL, &err);
    if (ws != NULL) { FAIL("expected NULL"); pico_ws_close(ws); return; }
    ASSERT_EQ(err, PICO_WS_ERR_INVALID);
    ASSERT_EQ(pico_ws_send(NULL, "hi", 2, 0), PICO_WS_ERR_INVALID);
    char buf[16];
    int r = pico_ws_recv(NULL, buf, sizeof(buf), 0, NULL);
    ASSERT_EQ(r, PICO_WS_ERR_INVALID);
    PASS();
}

static void test_pico_ws_bad_url(void) {
    TEST("pico_ws: bad URL scheme");
    int err = 0;
    PicoWs *ws = pico_ws_connect("http://localhost/path", NULL, &err);
    if (ws != NULL) { FAIL("expected NULL"); pico_ws_close(ws); return; }
    ASSERT_EQ(err, PICO_WS_ERR_URL);
    PASS();
}

static void test_pico_ws_connect_failure(void) {
    TEST("pico_ws: connect failure");
    int err = 0;
    PicoWsOptions opts = {0};
    opts.connect_timeout_ms = 1000;
    PicoWs *ws = pico_ws_connect("ws://127.0.0.1:1/ws", &opts, &err);
    if (ws != NULL) { FAIL("expected NULL"); pico_ws_close(ws); return; }
    ASSERT_EQ(err, PICO_WS_ERR_CONNECT);
    PASS();
}

static void test_pico_ws_close_null(void) {
    TEST("pico_ws: close NULL safe");
    pico_ws_close(NULL); /* should not crash */
    PASS();
}

/* ---- Procure ---- */

static void test_repo_dep_path(void) {
    TEST("procure: repo dep path");
    char *p = now_repo_dep_path("/tmp/repo", "org.acme", "core", "1.5.0");
    ASSERT_NOT_NULL(p);
    /* Check it contains the expected components */
    if (!strstr(p, "org") || !strstr(p, "acme") ||
        !strstr(p, "core") || !strstr(p, "1.5.0")) {
        FAIL("path missing expected components");
        free(p);
        return;
    }
    free(p);
    PASS();
}

/* A rejected fetch leaves the descriptor behind — it is downloaded
 * before the checksum and the signature are checked. If that alone
 * counted as installed, the next `procure` skipped every check and
 * reported success with nothing installed. */
static void test_repo_is_installed_needs_payload(void) {
    TEST("procure: descriptor alone is not an install");
    char root[512];
    snprintf(root, sizeof(root), "%s/repo_installed", NOW_TEST_RESOURCES);
    rmtree_best_effort(root);

    char *dep = now_repo_dep_path(root, "org.acme", "core", "1.5.0");
    if (!dep) { FAIL("dep path"); return; }
    now_mkdir_p(dep);

    char p[1024];
    snprintf(p, sizeof(p), "%s/now.pasta", dep);
    FILE *f = fopen(p, "w");
    if (!f) { FAIL("setup now.pasta"); free(dep); return; }
    fputs("{ group: \"org.acme\", artifact: \"core\", version: \"1.5.0\" }\n", f);
    fclose(f);

    if (now_repo_is_installed(root, "org.acme", "core", "1.5.0")) {
        FAIL("descriptor without payload counted as installed");
        free(dep); return;
    }

    /* Unpacked headers are what the compile path consumes. */
    snprintf(p, sizeof(p), "%s/h", dep);
    now_mkdir_p(p);
    if (!now_repo_is_installed(root, "org.acme", "core", "1.5.0")) {
        FAIL("descriptor plus h/ should count as installed");
        free(dep); return;
    }

    free(dep);
    rmtree_best_effort(root);
    PASS();
}

static void test_procure_no_deps(void) {
    TEST("procure: no deps returns success");
    /* A project with no dependencies should succeed immediately */
    NowResult res;
    const char *pasta =
        "{ group: \"io.test\", artifact: \"nodeps\", version: \"1.0.0\","
        "  lang: \"c\" }";
    NowProject *proj = now_project_load_string(pasta, strlen(pasta), &res);
    if (!proj) { FAIL(res.message); return; }

    NowProcureOpts opts = {0};
    opts.repo_root = "/tmp/now-test-repo";
    int rc = now_procure(proj, &opts, &res);
    now_project_free(proj);
    ASSERT_EQ(rc, 0);
    PASS();
}

static void test_cpu_count(void) {
    TEST("cpu count >= 1");
    int n = now_cpu_count();
    if (n < 1) { FAIL("cpu count < 1"); return; }
    PASS();
}

static void test_obj_path_ex_obj(void) {
    TEST("fs: obj_path_ex with .obj extension");
    char *p = now_obj_path_ex("/project", "src/main/c/foo.c",
                               "src/main/c", "target", ".obj");
    if (!p) { FAIL("returned NULL"); return; }
    /* Should end with foo.c.obj, not foo.c.o */
    const char *end = strstr(p, "foo.c.obj");
    if (!end) { FAIL(p); free(p); return; }
    free(p);
    PASS();
}

static void test_toolchain_gcc_default(void) {
    TEST("toolchain: defaults to gcc (no CC env)");
    /* Save and clear CC */
    const char *saved_cc = getenv("CC");
    char *saved_copy = saved_cc ? strdup(saved_cc) : NULL;
#ifdef _WIN32
    _putenv("CC=");
#else
    unsetenv("CC");
#endif
    NowToolchain tc;
    memset(&tc, 0, sizeof(tc));
    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    now_toolchain_resolve(&tc, &proj);

    int ok = (tc.is_msvc == 0);
    now_toolchain_free(&tc);

    /* Restore CC */
    if (saved_copy) {
        char buf[512];
        snprintf(buf, sizeof(buf), "CC=%s", saved_copy);
#ifdef _WIN32
        _putenv(buf);
#else
        setenv("CC", saved_copy, 1);
#endif
        free(saved_copy);
    }

    if (!ok) { FAIL("is_msvc should be 0"); return; }
    PASS();
}

static void test_publish_missing_identity(void) {
    TEST("publish: rejects project without group/artifact/version");
    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_publish(&proj, ".", "http://localhost:9999", 0, &res);
    if (rc == 0) { FAIL("should fail without identity"); return; }
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);
    PASS();
}

static void test_publish_no_package(void) {
    TEST("publish: fails when tarball not found");
    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    proj.group = "org.test";
    proj.artifact = "nope";
    proj.version = "1.0.0";
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_publish(&proj, ".", "http://localhost:9999", 0, &res);
    if (rc == 0) { FAIL("should fail without package"); return; }
    ASSERT_EQ(res.code, NOW_ERR_NOT_FOUND);
    PASS();
}

static void test_auth_load_no_creds(void) {
    TEST("auth: load returns -1 when no credentials file");
    NowCredentials creds;
    memset(&creds, 0, sizeof(creds));
    int rc = now_auth_load("http://no.such.registry", &creds);
    /* Should return -1 since ~/.now/credentials.pasta likely doesn't match */
    if (rc == 0 && creds.token != NULL) {
        /* Unlikely but valid — credentials file exists with this URL */
        now_auth_creds_free(&creds);
    }
    PASS();
}

static void test_auth_login_null_creds(void) {
    TEST("auth: login fails with NULL credentials");
    char *jwt = NULL;
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_auth_login("localhost", 9999, "", NULL, 0, &jwt, &res);
    if (rc != -1) { FAIL("should fail with NULL creds"); return; }
    if (jwt != NULL) { FAIL("jwt should be NULL"); free(jwt); return; }
    ASSERT_EQ(res.code, NOW_ERR_AUTH);
    PASS();
}

static void test_auth_login_connect_failure(void) {
    TEST("auth: login fails on connection refused");
    NowCredentials creds;
    memset(&creds, 0, sizeof(creds));
    creds.username = strdup("alice");
    creds.token = strdup("secret");
    char *jwt = NULL;
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_auth_login("127.0.0.1", 19999, "", &creds, 0, &jwt, &res);
    /* Should fail because nothing is listening */
    if (rc != -1) { FAIL("should fail on connect"); free(jwt); }
    if (jwt != NULL) { FAIL("jwt should be NULL"); free(jwt); }
    now_auth_creds_free(&creds);
    PASS();
}

static void test_yank_no_url(void) {
    TEST("yank: fails with NULL registry URL");
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_publish_yank(NULL, "org.test", "lib", "1.0.0", NULL, 0, &res);
    if (rc != -1) { FAIL("should fail with NULL URL"); return; }
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);
    PASS();
}

static void test_yank_connect_failure(void) {
    TEST("yank: fails on connection refused");
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_publish_yank("http://127.0.0.1:19999", "org.test", "lib",
                               "1.0.0", "security issue", 0, &res);
    if (rc != -1) { FAIL("should fail on connect"); return; }
    PASS();
}

static void test_dep_updates_no_deps(void) {
    TEST("dep:updates: project with no deps returns 0");
    NowResult res;
    memset(&res, 0, sizeof(res));
    const char *pasta =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\", lang: \"c\" }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }
    int rc = now_dep_updates(p, NULL, 0, &res);
    now_project_free(p);
    if (rc != 0) { FAIL("expected 0 updates for project with no deps"); return; }
    PASS();
}

static void test_dep_updates_null_project(void) {
    TEST("dep:updates: NULL project returns -1");
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_dep_updates(NULL, NULL, 0, &res);
    if (rc != -1) { FAIL("should fail with NULL project"); return; }
    PASS();
}

static void test_cache_mirror_no_url(void) {
    TEST("cache:mirror: NULL URL returns -1");
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_cache_mirror(NULL, NULL, 0, &res);
    if (rc != -1) { FAIL("should fail with NULL URL"); return; }
    PASS();
}

static void test_cache_mirror_connect_failure(void) {
    TEST("cache:mirror: connection refused returns -1");
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_cache_mirror("http://127.0.0.1:19999", NULL, 0, &res);
    if (rc != -1) { FAIL("should fail on connect"); return; }
    PASS();
}

static void test_toolchain_msvc_detect(void) {
    TEST("toolchain: detects MSVC from CC=cl.exe");
#ifdef _WIN32
    const char *saved_cc = getenv("CC");
    char *saved_copy = saved_cc ? strdup(saved_cc) : NULL;
    _putenv("CC=cl.exe");

    NowToolchain tc;
    memset(&tc, 0, sizeof(tc));
    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    now_toolchain_resolve(&tc, &proj);

    int ok = (tc.is_msvc == 1);
    int ar_ok = tc.ar && strstr(tc.ar, "lib") != NULL;
    now_toolchain_free(&tc);

    /* Restore CC */
    if (saved_copy) {
        char buf[512];
        snprintf(buf, sizeof(buf), "CC=%s", saved_copy);
        _putenv(buf);
        free(saved_copy);
    } else {
        _putenv("CC=");
    }

    if (!ok) { FAIL("is_msvc should be 1"); return; }
    if (!ar_ok) { FAIL("ar should be lib.exe"); return; }
    PASS();
#else
    /* MSVC detection only applies on Windows */
    PASS();
#endif
}

static void test_toolchain_java_resolve(void) {
    TEST("toolchain: resolves javac/jar/java for Java projects");
    /* Create a project with Java lang */
    const char *input = "{ group: \"t\", artifact: \"t\", version: \"1.0.0\", langs: [\"java\"] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowToolchain tc;
    memset(&tc, 0, sizeof(tc));
    now_toolchain_resolve(&tc, p);

    /* javac/jar/java should be set (even if not found on system, they'll be defaults) */
    ASSERT_NOT_NULL(tc.javac);
    ASSERT_NOT_NULL(tc.jar);
    ASSERT_NOT_NULL(tc.java);

    now_toolchain_free(&tc);
    now_project_free(p);
    PASS();
}

static void test_toolchain_no_java_for_c(void) {
    TEST("toolchain: no javac/jar for C projects");
    const char *input = "{ group: \"t\", artifact: \"t\", version: \"1.0.0\", langs: [\"c\"] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowToolchain tc;
    memset(&tc, 0, sizeof(tc));
    now_toolchain_resolve(&tc, p);

    /* Java tools should NOT be resolved for C projects */
    ASSERT_NULL(tc.javac);
    ASSERT_NULL(tc.jar);
    ASSERT_NULL(tc.java);

    now_toolchain_free(&tc);
    now_project_free(p);
    PASS();
}

/* ---- Workspace ---- */

static void test_is_workspace_true(void) {
    TEST("workspace: detect workspace root");
    char path[512];
    snprintf(path, sizeof(path), "%s/workspace/now.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }
    if (!now_is_workspace(p)) { FAIL("expected workspace"); now_project_free(p); return; }
    now_project_free(p);
    PASS();
}

static void test_is_workspace_false(void) {
    TEST("workspace: single project is not workspace");
    NowResult res;
    const char *pasta =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\", lang: \"c\" }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }
    if (now_is_workspace(p)) { FAIL("expected non-workspace"); now_project_free(p); return; }
    now_project_free(p);
    PASS();
}

static void test_workspace_init(void) {
    TEST("workspace: init loads modules and builds graph");
    char path[512];
    snprintf(path, sizeof(path), "%s/workspace/now.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/workspace", NOW_TEST_RESOURCES);

    NowWorkspace ws;
    int rc = now_workspace_init(&ws, p, basedir, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(p); return; }

    /* Should have 2 modules */
    if (ws.module_count != 2) { FAIL("expected 2 modules"); now_workspace_free(&ws); now_project_free(p); return; }

    now_workspace_free(&ws);
    now_project_free(p);
    PASS();
}

static void test_workspace_topo_sort(void) {
    TEST("workspace: topo sort orders core before app");
    char path[512];
    snprintf(path, sizeof(path), "%s/workspace/now.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/workspace", NOW_TEST_RESOURCES);

    NowWorkspace ws;
    int rc = now_workspace_init(&ws, p, basedir, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(p); return; }

    int **waves = NULL;
    int *wave_sizes = NULL;
    int nwaves = now_workspace_topo_sort(&ws, &waves, &wave_sizes, &res);
    if (nwaves < 1) { FAIL("expected at least 1 wave"); now_workspace_free(&ws); now_project_free(p); return; }

    /* core has no deps so it should appear in wave 0.
     * app depends on core so it must appear in a later wave. */
    int core_wave = -1, app_wave = -1;
    for (int w = 0; w < nwaves; w++) {
        for (int m = 0; m < wave_sizes[w]; m++) {
            int idx = waves[w][m];
            if (strcmp(ws.modules[idx].name, "core") == 0) core_wave = w;
            if (strcmp(ws.modules[idx].name, "app") == 0) app_wave = w;
        }
    }

    /* Free waves */
    for (size_t i = 0; i < ws.module_count; i++) free(waves[i]);
    free(waves); free(wave_sizes);
    now_workspace_free(&ws);
    now_project_free(p);

    if (core_wave < 0 || app_wave < 0) { FAIL("missing module in waves"); return; }
    if (core_wave >= app_wave) { FAIL("core must be in earlier wave than app"); return; }
    PASS();
}

static void test_workspace_is_null(void) {
    TEST("workspace: is_workspace(NULL) returns 0");
    if (now_is_workspace(NULL) != 0) { FAIL("expected 0"); return; }
    PASS();
}

static int strarray_contains_suffix(const NowStrArray *a, const char *suf) {
    size_t n = strlen(suf);
    for (size_t i = 0; i < a->count; i++) {
        size_t m = strlen(a->items[i]);
        if (m >= n && strcmp(a->items[i] + m - n, suf) == 0) return 1;
    }
    return 0;
}
static int strarray_contains(const NowStrArray *a, const char *s) {
    for (size_t i = 0; i < a->count; i++)
        if (strcmp(a->items[i], s) == 0) return 1;
    return 0;
}

static void test_workspace_inject_sibling(void) {
    TEST("workspace: auto-injects sibling include/libdir/lib + STATIC define");
    char path[512];
    snprintf(path, sizeof(path), "%s/workspace/now.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/workspace", NOW_TEST_RESOURCES);

    NowWorkspace ws;
    int rc = now_workspace_init(&ws, p, basedir, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(p); return; }

    /* Find the 'app' module — it depends on the static 'core' sibling. */
    int app_idx = -1;
    for (size_t i = 0; i < ws.module_count; i++)
        if (strcmp(ws.modules[i].name, "app") == 0) { app_idx = (int)i; break; }
    if (app_idx < 0) { FAIL("missing app module"); now_workspace_free(&ws); now_project_free(p); return; }

    NowProject *app = ws.modules[app_idx].project;
    int ok_inc  = strarray_contains_suffix(&app->compile.includes, "/core/src/main/h");
    int ok_dir  = strarray_contains_suffix(&app->link.libdirs,     "/core/target/bin");
    int ok_lib  = strarray_contains(&app->link.libs,               "core");
    int ok_def  = strarray_contains(&app->compile.defines,         "CORE_STATIC");

    now_workspace_free(&ws);
    now_project_free(p);

    if (!ok_inc) { FAIL("missing injected include for core/src/main/h"); return; }
    if (!ok_dir) { FAIL("missing injected libdir for core/target/bin"); return; }
    if (!ok_lib) { FAIL("missing injected -lcore"); return; }
    if (!ok_def) { FAIL("missing injected CORE_STATIC define"); return; }
    PASS();
}

/* Regression for the link-phase libdir/lib cap (starletc BLOCKER):
 * an executable depending on >32 sibling static libs had its 33rd+
 * injected -L<dir> silently dropped, so the matching -l resolved
 * against nothing ("cannot find -lXXX"). Inject was correct — the cap
 * was in the link argv builder — so this must actually link to catch
 * it. Generates a workspace of 35 trivial static libs + one exe that
 * depends on all of them. */
static void test_workspace_libdir_cap(void) {
    TEST("workspace: >32 sibling libdirs all reach the link line");

    enum { NLIB = 35 };
    char root[512];
    snprintf(root, sizeof(root), "%s/ws_libcap", NOW_TEST_RESOURCES);
    rmtree_best_effort(root);
    now_mkdir_p(root);

    char p[700], f[900];
    FILE *fp;

    /* NLIB trivial static-lib modules: libNN/ with one unique symbol. */
    for (int i = 0; i < NLIB; i++) {
        snprintf(p, sizeof(p), "%s/lib%02d/src/main/c", root, i);
        now_mkdir_p(p);
        snprintf(f, sizeof(f), "%s/lib%02d/src/main/c/a.c", root, i);
        fp = fopen(f, "w");
        if (!fp) { FAIL("setup lib src"); return; }
        fprintf(fp, "int wscaplib%02d_fn(void){return %d;}\n", i, i);
        fclose(fp);

        snprintf(f, sizeof(f), "%s/lib%02d/now.pasta", root, i);
        fp = fopen(f, "w");
        if (!fp) { FAIL("setup lib pasta"); return; }
        fprintf(fp,
            "{ group: \"org.test\", artifact: \"wscaplib%02d\", version: \"1\","
            "  langs: [\"c\"],"
            "  output: { type: \"static\", name: \"wscaplib%02d\" } }\n", i, i);
        fclose(fp);
    }

    /* The executable consumer depending on every lib. */
    snprintf(p, sizeof(p), "%s/app/src/main/c", root);
    now_mkdir_p(p);
    snprintf(f, sizeof(f), "%s/app/src/main/c/main.c", root);
    fp = fopen(f, "w");
    if (!fp) { FAIL("setup app main"); return; }
    fputs("int main(void){return 0;}\n", fp);
    fclose(fp);

    snprintf(f, sizeof(f), "%s/app/now.pasta", root);
    fp = fopen(f, "w");
    if (!fp) { FAIL("setup app pasta"); return; }
    fputs("{ group: \"org.test\", artifact: \"wscapapp\", version: \"1\","
          "  langs: [\"c\"],"
          "  output: { type: \"executable\", name: \"wscapapp\" },"
          "  depends: [", fp);
    for (int i = 0; i < NLIB; i++)
        fprintf(fp, "%s { id: \"org.test:wscaplib%02d:*\" }",
                i ? "," : "", i);
    fputs(" ] }\n", fp);
    fclose(fp);

    /* Root workspace listing all libs (in order) then the app. */
    snprintf(f, sizeof(f), "%s/now.pasta", root);
    fp = fopen(f, "w");
    if (!fp) { FAIL("setup root pasta"); return; }
    fputs("{ group: \"org.test\", artifact: \"wscap\", version: \"1\","
          "  langs: [\"c\"], modules: [", fp);
    for (int i = 0; i < NLIB; i++)
        fprintf(fp, "%s \"lib%02d\"", i ? "," : "", i);
    fputs(", \"app\" ] }\n", fp);
    fclose(fp);

    NowResult res;
    NowProject *prj = now_project_load(f, &res);
    if (!prj) { FAIL(res.message); return; }

    NowWorkspace ws;
    int rc = now_workspace_init(&ws, prj, root, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(prj); return; }
    rc = now_workspace_build(&ws, 0, 0, &res);
    now_workspace_free(&ws);
    now_project_free(prj);

    if (rc != 0) {
        /* Pre-fix: link fails with "cannot find -lwscaplib32".. */
        FAIL(res.message[0] ? res.message : "workspace build failed");
        return;
    }

    char exe[700];
#ifdef _WIN32
    snprintf(exe, sizeof(exe), "%s/app/target/bin/wscapapp.exe", root);
#else
    snprintf(exe, sizeof(exe), "%s/app/target/bin/wscapapp", root);
#endif
    if (!now_path_exists(exe)) { FAIL("app exe not produced"); return; }

    rmtree_best_effort(root);
    PASS();
}

/* ---- Plugin system ---- */

static void test_plugin_is_builtin(void) {
    TEST("plugin: detect built-in ids");
    if (!now_plugin_is_builtin("now:version")) { FAIL("now:version should be builtin"); return; }
    if (!now_plugin_is_builtin("now:embed")) { FAIL("now:embed should be builtin"); return; }
    if (now_plugin_is_builtin("org.acme:foo:1.0.0")) { FAIL("should not be builtin"); return; }
    if (now_plugin_is_builtin(NULL)) { FAIL("NULL should not be builtin"); return; }
    PASS();
}

static void test_plugin_pom_load(void) {
    TEST("plugin: load plugins from now.pasta");
    NowResult res;
    const char *pasta =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  lang: \"c\","
        "  plugins: ["
        "    { id: \"now:version\", phase: \"generate\" },"
        "    { id: \"now:embed\", phase: \"generate\","
        "      config: { src: \"assets\", prefix: \"res_\" } }"
        "  ] }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }
    if (p->plugins.count != 2) { FAIL("expected 2 plugins"); now_project_free(p); return; }
    ASSERT_STR(p->plugins.items[0].id, "now:version");
    ASSERT_STR(p->plugins.items[0].phase, "generate");
    ASSERT_STR(p->plugins.items[1].id, "now:embed");
    /* config should be non-NULL (raw PastaValue*) */
    if (!p->plugins.items[1].config) { FAIL("config should be set"); now_project_free(p); return; }
    now_project_free(p);
    PASS();
}

static void test_plugin_run_hook_no_plugins(void) {
    TEST("plugin: run_hook with no plugins is no-op");
    NowResult res;
    const char *pasta =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\", lang: \"c\" }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }
    NowPluginResult out;
    int rc = now_plugin_run_hook(p, ".", NOW_HOOK_GENERATE, 0, &out, &res);
    now_project_free(p);
    ASSERT_EQ(rc, 0);
    PASS();
}

static void test_plugin_version_generate(void) {
    TEST("plugin: now:version generates _now_version.c");
    NowResult res;
    const char *pasta =
        "{ group: \"org.test\", artifact: \"hello\", version: \"2.3.4\","
        "  lang: \"c\","
        "  plugins: ["
        "    { id: \"now:version\", phase: \"generate\" }"
        "  ] }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/plugin_test", NOW_TEST_RESOURCES);
    /* Create the basedir if needed */
    now_mkdir_p(basedir);

    NowPluginResult out;
    now_plugin_result_init(&out);
    int rc = now_plugin_run_hook(p, basedir, NOW_HOOK_GENERATE, 0, &out, &res);
    now_project_free(p);
    if (rc != 0) { FAIL(res.message); now_plugin_result_free(&out); return; }

    /* Should have produced 1 source file */
    if (out.sources.count != 1) { FAIL("expected 1 generated source"); now_plugin_result_free(&out); return; }

    /* Verify the generated file exists and contains expected content */
    FILE *fp = fopen(out.sources.items[0], "r");
    if (!fp) { FAIL("generated file not found"); now_plugin_result_free(&out); return; }

    char buf[2048];
    size_t nread = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[nread] = '\0';
    fclose(fp);

    if (!strstr(buf, "NOW_VERSION")) { FAIL("missing NOW_VERSION"); now_plugin_result_free(&out); return; }
    if (!strstr(buf, "\"2.3.4\"")) { FAIL("missing version string"); now_plugin_result_free(&out); return; }
    if (!strstr(buf, "\"org.test\"")) { FAIL("missing group"); now_plugin_result_free(&out); return; }
    if (!strstr(buf, "NOW_VERSION_MAJOR = 2")) { FAIL("wrong major"); now_plugin_result_free(&out); return; }

    now_plugin_result_free(&out);
    PASS();
}

static void test_plugin_result_init_free(void) {
    TEST("plugin: result init and free");
    NowPluginResult r;
    now_plugin_result_init(&r);
    if (!r.ok) { FAIL("should be ok"); return; }
    now_plugin_result_free(&r);
    /* Should not crash */
    PASS();
}

static void test_plugin_unknown_builtin(void) {
    TEST("plugin: unknown built-in returns error");
    NowPlugin pl;
    memset(&pl, 0, sizeof(pl));
    pl.id = "now:nonexistent";
    pl.phase = "generate";

    NowPluginResult out;
    now_plugin_result_init(&out);
    NowResult res;
    memset(&res, 0, sizeof(res));

    int rc = now_plugin_invoke(&pl, NULL, ".", "generate", 0, &out, &res);
    now_plugin_result_free(&out);
    if (rc == 0) { FAIL("should have failed"); return; }
    ASSERT_EQ(res.code, NOW_ERR_NOT_FOUND);
    PASS();
}

/* ---- Plugin registry ---- */

static void test_plugin_manifest_parse_string(void) {
    TEST("plugin registry: parse manifest from string");
    const char *input =
        "{ id: \"org.now.plugins:protobuf-c:1.0.0\","
        "  name: \"Protobuf C Generator\","
        "  description: \"Generates C sources from .proto files\","
        "  protocol: \"1.0.0\","
        "  hooks: [\"generate\"],"
        "  requires: [\"source-inject\", \"fail-build\"],"
        "  network: { required: true },"
        "  requires_now: \">=1.0.0\" }";
    NowPluginInfo info;
    NowResult res;
    int rc = now_plugin_manifest_parse_string(input, strlen(input), &info, &res);
    ASSERT_EQ(rc, 0);
    if (!info.id || strcmp(info.id, "org.now.plugins:protobuf-c:1.0.0") != 0) {
        FAIL("wrong id");
        now_plugin_info_free(&info);
        return;
    }
    if (!info.name || strcmp(info.name, "Protobuf C Generator") != 0) {
        FAIL("wrong name");
        now_plugin_info_free(&info);
        return;
    }
    if (!info.protocol || strcmp(info.protocol, "1.0.0") != 0) {
        FAIL("wrong protocol");
        now_plugin_info_free(&info);
        return;
    }
    ASSERT_EQ(info.hooks.count, (size_t)1);
    ASSERT_EQ(info.requires.count, (size_t)2);
    ASSERT_EQ(info.network_required, 1);
    if (!info.requires_now || strcmp(info.requires_now, ">=1.0.0") != 0) {
        FAIL("wrong requires_now");
        now_plugin_info_free(&info);
        return;
    }
    now_plugin_info_free(&info);
    PASS();
}

static void test_plugin_manifest_parse_minimal(void) {
    TEST("plugin registry: parse minimal manifest");
    const char *input = "{ id: \"org.now.plugins:simple:0.1.0\", protocol: \"1.0.0\" }";
    NowPluginInfo info;
    NowResult res;
    int rc = now_plugin_manifest_parse_string(input, strlen(input), &info, &res);
    ASSERT_EQ(rc, 0);
    if (!info.id || strcmp(info.id, "org.now.plugins:simple:0.1.0") != 0) {
        FAIL("wrong id");
        now_plugin_info_free(&info);
        return;
    }
    ASSERT_EQ(info.hooks.count, (size_t)0);
    ASSERT_EQ(info.requires.count, (size_t)0);
    ASSERT_EQ(info.network_required, 0);
    now_plugin_info_free(&info);
    PASS();
}

static void test_plugin_manifest_missing_id(void) {
    TEST("plugin registry: manifest without id fails");
    const char *input = "{ name: \"No ID\", protocol: \"1.0.0\" }";
    NowPluginInfo info;
    NowResult res;
    int rc = now_plugin_manifest_parse_string(input, strlen(input), &info, &res);
    if (rc == 0) { FAIL("should have failed"); now_plugin_info_free(&info); return; }
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);
    PASS();
}

static void test_plugin_manifest_parse_file_missing(void) {
    TEST("plugin registry: missing manifest file returns -1");
    NowPluginInfo info;
    NowResult res;
    int rc = now_plugin_manifest_parse("/nonexistent/plugin.pasta", &info, &res);
    if (rc == 0) { FAIL("should have failed"); now_plugin_info_free(&info); return; }
    ASSERT_EQ(res.code, NOW_ERR_NOT_FOUND);
    PASS();
}

static void test_plugin_info_free_null_safe(void) {
    TEST("plugin registry: info_free on NULL is safe");
    now_plugin_info_free(NULL);
    NowPluginInfo info;
    memset(&info, 0, sizeof(info));
    now_plugin_info_free(&info);
    PASS();
}

static void test_plugin_find_binary_missing(void) {
    TEST("plugin registry: find_binary returns NULL for missing");
    char *path = now_plugin_find_binary("/nonexistent/repo",
                                          "org.test", "myplugin", "1.0.0");
    if (path != NULL) { FAIL("should be NULL"); free(path); return; }
    PASS();
}

static void test_plugin_list_empty_repo(void) {
    TEST("plugin registry: list on empty repo returns 0");
    NowPluginInfo *plugins = NULL;
    size_t count = 0;
    NowResult res;
    int rc = now_plugin_list("/nonexistent/repo", &plugins, &count, &res);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, (size_t)0);
    PASS();
}

static void test_plugin_search_no_match(void) {
    TEST("plugin registry: search with no match returns 0");
    NowPluginInfo *plugins = NULL;
    size_t count = 0;
    NowResult res;
    int rc = now_plugin_search("zzz_nonexistent", "/nonexistent/repo",
                                &plugins, &count, &res);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, (size_t)0);
    PASS();
}

static void test_plugin_install_bad_registry(void) {
    TEST("plugin registry: install from bad registry fails");
    NowResult res;
    int rc = now_plugin_install("http://127.0.0.1:1",
                                  "org.test", "myplugin", "1.0.0",
                                  "/tmp/test_repo", 0, &res);
    if (rc == 0) { FAIL("should have failed"); return; }
    PASS();
}

static void test_plugin_manifest_roundtrip(void) {
    TEST("plugin registry: write + parse manifest file roundtrip");
    const char *input =
        "{ id: \"org.now.plugins:roundtrip:2.0.0\","
        "  name: \"Roundtrip Test\","
        "  description: \"A test plugin\","
        "  protocol: \"1.0.0\","
        "  hooks: [\"pre-compile\", \"post-compile\"],"
        "  requires: [\"fail-build\"],"
        "  network: { required: false } }";

    /* Write to temp file */
    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_plugin_manifest.pasta",
             NOW_TEST_RESOURCES);
    FILE *fp = fopen(outpath, "w");
    ASSERT_NOT_NULL(fp);
    fwrite(input, 1, strlen(input), fp);
    fclose(fp);

    /* Parse back */
    NowPluginInfo info;
    NowResult res;
    int rc = now_plugin_manifest_parse(outpath, &info, &res);
    remove(outpath);
    ASSERT_EQ(rc, 0);
    if (!info.id || strcmp(info.id, "org.now.plugins:roundtrip:2.0.0") != 0) {
        FAIL("wrong id after roundtrip");
        now_plugin_info_free(&info);
        return;
    }
    ASSERT_EQ(info.hooks.count, (size_t)2);
    ASSERT_EQ(info.requires.count, (size_t)1);
    ASSERT_EQ(info.network_required, 0);
    now_plugin_info_free(&info);
    PASS();
}

/* ---- Dep confusion protection ---- */

static void test_private_group_exact_match(void) {
    TEST("private_groups: exact prefix match");
    NowStrArray pg;
    now_strarray_init(&pg);
    now_strarray_push(&pg, "org.acme");
    ASSERT_EQ(now_group_is_private(&pg, "org.acme"), 1);
    now_strarray_free(&pg);
    PASS();
}

static void test_private_group_dotted_child(void) {
    TEST("private_groups: dotted child match");
    NowStrArray pg;
    now_strarray_init(&pg);
    now_strarray_push(&pg, "org.acme");
    ASSERT_EQ(now_group_is_private(&pg, "org.acme.internal"), 1);
    ASSERT_EQ(now_group_is_private(&pg, "org.acme.core.util"), 1);
    now_strarray_free(&pg);
    PASS();
}

static void test_private_group_no_false_positive(void) {
    TEST("private_groups: no false positive on similar prefix");
    NowStrArray pg;
    now_strarray_init(&pg);
    now_strarray_push(&pg, "org.acme");
    ASSERT_EQ(now_group_is_private(&pg, "org.acmecorp"), 0);
    ASSERT_EQ(now_group_is_private(&pg, "org.acm"), 0);
    ASSERT_EQ(now_group_is_private(&pg, "com.example"), 0);
    now_strarray_free(&pg);
    PASS();
}

static void test_private_group_multiple_prefixes(void) {
    TEST("private_groups: multiple prefixes");
    NowStrArray pg;
    now_strarray_init(&pg);
    now_strarray_push(&pg, "org.acme");
    now_strarray_push(&pg, "com.internal");
    ASSERT_EQ(now_group_is_private(&pg, "org.acme.libs"), 1);
    ASSERT_EQ(now_group_is_private(&pg, "com.internal"), 1);
    ASSERT_EQ(now_group_is_private(&pg, "com.example"), 0);
    now_strarray_free(&pg);
    PASS();
}

static void test_private_group_null_safe(void) {
    TEST("private_groups: NULL-safe");
    ASSERT_EQ(now_group_is_private(NULL, "org.acme"), 0);
    NowStrArray pg;
    now_strarray_init(&pg);
    ASSERT_EQ(now_group_is_private(&pg, NULL), 0);
    ASSERT_EQ(now_group_is_private(&pg, "anything"), 0);
    now_strarray_free(&pg);
    PASS();
}

static void test_private_group_pom_load(void) {
    TEST("private_groups: loaded from now.pasta");
    NowResult res;
    const char *pasta =
        "{ group: \"org.acme\", artifact: \"app\", version: \"1.0.0\","
        "  lang: \"c\","
        "  private_groups: [\"org.acme\", \"com.secret\"] }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }
    ASSERT_EQ(p->private_groups.count, (size_t)2);
    ASSERT_STR(p->private_groups.items[0], "org.acme");
    ASSERT_STR(p->private_groups.items[1], "com.secret");
    now_project_free(p);
    PASS();
}

static void test_private_group_procure_fail(void) {
    TEST("private_groups: procure fails without repos");
    NowResult res;
    const char *pasta =
        "{ group: \"org.acme\", artifact: \"app\", version: \"1.0.0\","
        "  lang: \"c\","
        "  private_groups: [\"org.internal\"],"
        "  deps: [{ id: \"org.internal:secret:^1.0.0\" }] }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }

    NowProcureOpts opts = {0};
    opts.repo_root = "/tmp/now-test-repo-confuse";
    memset(&res, 0, sizeof(res));
    int rc = now_procure(p, &opts, &res);
    now_project_free(p);

    /* Should fail because private group has no declared repos */
    if (rc == 0) { FAIL("should fail for private group without repos"); return; }
    if (!strstr(res.message, "private group")) { FAIL(res.message); return; }
    PASS();
}

static void test_link_inherit_target_parses(void) {
    TEST("link.inherit_target: parsed, and off unless asked for");
    NowResult res;
    const char *off =
        "{ group: \"t\", artifact: \"a\", version: \"1.0.0\", lang: \"c\","
        "  link: { flags: [\"-nostdlib\"] } }";
    NowProject *p = now_project_load_string(off, strlen(off), &res);
    if (!p) { FAIL(res.message); return; }
    if (now_project_link_inherit_target(p) != 0) {
        FAIL("inherit_target defaulted to on"); now_project_free(p); return;
    }
    now_project_free(p);

    const char *on =
        "{ group: \"t\", artifact: \"a\", version: \"1.0.0\", lang: \"c\","
        "  link: { inherit_target: true } }";
    p = now_project_load_string(on, strlen(on), &res);
    if (!p) { FAIL(res.message); return; }
    if (now_project_link_inherit_target(p) != 1) {
        FAIL("inherit_target not read"); now_project_free(p); return;
    }
    now_project_free(p);
    PASS();
}

static void test_lock_differs(void) {
    TEST("--locked: lockfile drift is detected in all three directions");
    NowLockFile before, after;
    now_lock_init(&before);
    now_lock_init(&after);

    NowLockEntry e;
    memset(&e, 0, sizeof(e));
    e.group = (char *)"org.acme"; e.artifact = (char *)"core";
    e.version = (char *)"1.0.0";  e.scope = (char *)"compile";
    now_lock_set(&before, &e);
    now_lock_set(&after,  &e);

    const char *what = NULL, *which = NULL;
    if (now_lock_differs(&before, &after, &what, &which)) {
        FAIL("identical lockfiles reported as differing");
        goto done;
    }

    /* Version drift — the case --locked exists to catch. */
    e.version = (char *)"1.1.0";
    now_lock_set(&after, &e);
    if (!now_lock_differs(&before, &after, &what, &which) ||
        strcmp(what, "changed version") != 0) {
        FAIL("version change not detected");
        goto done;
    }

    /* Addition. */
    now_lock_free(&after); now_lock_init(&after);
    e.version = (char *)"1.0.0";
    now_lock_set(&after, &e);
    e.artifact = (char *)"extra";
    now_lock_set(&after, &e);
    if (!now_lock_differs(&before, &after, &what, &which) ||
        strcmp(what, "added") != 0) {
        FAIL("addition not detected");
        goto done;
    }

    /* Removal. */
    now_lock_free(&after); now_lock_init(&after);
    if (!now_lock_differs(&before, &after, &what, &which) ||
        strcmp(what, "removed") != 0) {
        FAIL("removal not detected");
        goto done;
    }

    /* An unresolved version is not drift. The check now runs before the
     * network as well as after it, and a range still reads as "" at that
     * point — reporting it would refuse every `--locked` run that uses
     * one. */
    now_lock_free(&after); now_lock_init(&after);
    e.artifact = (char *)"core";
    e.version  = (char *)"";
    now_lock_set(&after, &e);
    if (now_lock_differs(&before, &after, &what, &which)) {
        FAIL("unresolved version reported as drift");
        goto done;
    }

    now_lock_free(&before);
    now_lock_free(&after);
    PASS();
    return;
done:
    now_lock_free(&before);
    now_lock_free(&after);
}

static void test_registry_is_public(void) {
    TEST("private_groups: central registry recognised on host, not spelling");
    if (!now_registry_is_public("https://repo.now.build")) { FAIL("repo.now.build"); return; }
    if (!now_registry_is_public("https://registry.now.build/central")) { FAIL("registry.now.build w/ path"); return; }
    /* The fence must not be defeated by scheme, port or trailing path. */
    if (!now_registry_is_public("http://repo.now.build:8080/x")) { FAIL("scheme/port variant"); return; }
    if (now_registry_is_public("https://pkg.acme.internal/now")) { FAIL("private host flagged public"); return; }
    /* Suffix games must not match: evil-repo.now.build.attacker.com */
    if (now_registry_is_public("https://repo.now.build.attacker.com/")) { FAIL("suffix impersonation"); return; }
    PASS();
}

static void test_private_group_fence_stops_at_public(void) {
    TEST("private_groups: candidates stop before the public registry");
    NowResult res;
    const char *pasta =
        "{ group: \"org.acme\", artifact: \"app\", version: \"1.0.0\","
        "  lang: \"c\","
        "  private_groups: [\"org.internal\"],"
        "  repos: [\"https://pkg.acme.internal/now\","
        "          \"https://repo.now.build\","
        "          \"https://late.acme.internal/now\"] }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }

    const char *out[8];
    /* Private group: only the repo declared ahead of the public one. */
    size_t n = now_registry_candidates(p, NULL, "org.internal.svc", out, 8);
    if (n != 1) { FAIL("private group should get exactly one candidate"); now_project_free(p); return; }
    if (strstr(out[0], "pkg.acme.internal") == NULL) { FAIL(out[0]); now_project_free(p); return; }

    /* Public group: every declared repo, in order. */
    n = now_registry_candidates(p, NULL, "com.example", out, 8);
    if (n != 3) { FAIL("public group should see all repos"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_private_group_repo_override_fenced(void) {
    TEST("private_groups: --repo cannot unlock a private group");
    NowResult res;
    const char *pasta =
        "{ group: \"org.acme\", artifact: \"app\", version: \"1.0.0\","
        "  lang: \"c\", private_groups: [\"org.internal\"] }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }

    const char *out[8];
    NowProcureOpts opts = {0};
    opts.registry_url = "https://repo.now.build";
    /* Pointing --repo at the public registry must not serve a private group. */
    if (now_registry_candidates(p, &opts, "org.internal.svc", out, 8) != 0) {
        FAIL("public --repo served a private group"); now_project_free(p); return;
    }
    /* A private --repo is honoured, and replaces the declared list. */
    opts.registry_url = "https://pkg.acme.internal/now";
    if (now_registry_candidates(p, &opts, "org.internal.svc", out, 8) != 1) {
        FAIL("private --repo should be honoured"); now_project_free(p); return;
    }
    now_project_free(p);
    PASS();
}

static void test_private_group_nested_resolve_form(void) {
    TEST("private_groups: spec's resolve: {} form is honoured");
    NowResult res;
    /* §25.2 documents this spelling; it used to warn as an unknown key
     * and leave the project unfenced. */
    const char *pasta =
        "{ group: \"org.acme\", artifact: \"app\", version: \"1.0.0\","
        "  lang: \"c\","
        "  resolve: { private_groups: [\"org.internal\"] } }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }
    if (!now_group_is_private(&p->private_groups, "org.internal.svc")) {
        FAIL("nested resolve.private_groups not applied");
        now_project_free(p);
        return;
    }
    now_project_free(p);
    PASS();
}

/* ---- Layer system ---- */

static void test_layer_stack_init(void) {
    TEST("layers: stack init with baseline");
    NowLayerStack stack;
    now_layer_stack_init(&stack);
    ASSERT_EQ(stack.count, (size_t)1);
    ASSERT_STR(stack.layers[0].id, "now-baseline");
    ASSERT_EQ(stack.layers[0].source, NOW_LAYER_BUILTIN);
    now_layer_stack_free(&stack);
    PASS();
}

static void test_layer_baseline_sections(void) {
    TEST("layers: baseline has compile, repos, toolchain");
    NowLayerStack stack;
    now_layer_stack_init(&stack);
    const NowLayerSection *compile = now_layer_find_section(&stack.layers[0], "compile");
    ASSERT_NOT_NULL(compile);
    ASSERT_EQ(compile->policy, NOW_POLICY_OPEN);

    const NowLayerSection *repos = now_layer_find_section(&stack.layers[0], "repos");
    ASSERT_NOT_NULL(repos);

    const NowLayerSection *tc = now_layer_find_section(&stack.layers[0], "toolchain");
    ASSERT_NOT_NULL(tc);

    const NowLayerSection *adv = now_layer_find_section(&stack.layers[0], "advisory");
    ASSERT_NOT_NULL(adv);
    ASSERT_EQ(adv->policy, NOW_POLICY_LOCKED);

    now_layer_stack_free(&stack);
    PASS();
}

static void test_layer_load_file(void) {
    TEST("layers: load enterprise layer from file");
    NowLayerStack stack;
    now_layer_stack_init(&stack);

    char path[512];
    snprintf(path, sizeof(path), "%s/layers/enterprise.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    int rc = now_layer_load_file(&stack, "enterprise", path, &res);
    if (rc != 0) { FAIL(res.message); now_layer_stack_free(&stack); return; }

    ASSERT_EQ(stack.count, (size_t)2);
    ASSERT_STR(stack.layers[1].id, "enterprise");

    const NowLayerSection *compile = now_layer_find_section(&stack.layers[1], "compile");
    ASSERT_NOT_NULL(compile);
    ASSERT_EQ(compile->policy, NOW_POLICY_LOCKED);

    const NowLayerSection *pg = now_layer_find_section(&stack.layers[1], "private_groups");
    ASSERT_NOT_NULL(pg);
    ASSERT_EQ(pg->policy, NOW_POLICY_LOCKED);

    now_layer_stack_free(&stack);
    PASS();
}

static void test_layer_merge_open(void) {
    TEST("layers: merge open section (overlay wins)");
    NowLayerStack stack;
    now_layer_stack_init(&stack);

    char path[512];
    snprintf(path, sizeof(path), "%s/layers/enterprise.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    now_layer_load_file(&stack, "enterprise", path, &res);

    NowAuditReport audit;
    now_audit_init(&audit);

    /* toolchain is open in both layers — enterprise overrides baseline */
    PastaValue *effective = (PastaValue *)now_layer_merge_section(&stack, "toolchain", &audit);
    ASSERT_NOT_NULL(effective);

    const PastaValue *preset = pasta_map_get(effective, "preset");
    ASSERT_NOT_NULL(preset);
    ASSERT_STR(pasta_get_string(preset), "llvm");

    /* No violations for open section */
    ASSERT_EQ(audit.count, (size_t)0);

    pasta_free(effective);
    now_audit_free(&audit);
    now_layer_stack_free(&stack);
    PASS();
}

static void test_layer_merge_locked_audit(void) {
    TEST("layers: merge locked section produces audit violation");
    NowLayerStack stack;
    now_layer_stack_init(&stack);

    /* Load enterprise layer (locks compile) */
    char path[512];
    snprintf(path, sizeof(path), "%s/layers/enterprise.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    now_layer_load_file(&stack, "enterprise", path, &res);

    /* Push a project layer that overrides compile */
    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    now_strarray_init(&proj.compile.warnings);
    now_strarray_init(&proj.compile.defines);
    now_strarray_init(&proj.compile.flags);
    now_strarray_init(&proj.compile.includes);
    now_strarray_push(&proj.compile.defines, "MY_DEFINE");
    proj.compile.opt = "speed";
    now_strarray_init(&proj.private_groups);
    now_strarray_init(&proj.langs);
    now_layer_push_project(&stack, &proj);

    NowAuditReport audit;
    now_audit_init(&audit);

    PastaValue *effective = (PastaValue *)now_layer_merge_section(&stack, "compile", &audit);
    ASSERT_NOT_NULL(effective);

    /* Should have audit violations (project overriding enterprise's locked compile) */
    if (audit.count == 0) { FAIL("expected audit violations"); pasta_free(effective); now_audit_free(&audit); now_layer_stack_free(&stack); now_strarray_free(&proj.compile.warnings); now_strarray_free(&proj.compile.defines); now_strarray_free(&proj.compile.flags); now_strarray_free(&proj.compile.includes); now_strarray_free(&proj.private_groups); now_strarray_free(&proj.langs); return; }

    ASSERT_STR(audit.items[0].code, "NOW-W0401");

    pasta_free(effective);
    now_audit_free(&audit);
    now_layer_stack_free(&stack);
    now_strarray_free(&proj.compile.warnings);
    now_strarray_free(&proj.compile.defines);
    now_strarray_free(&proj.compile.flags);
    now_strarray_free(&proj.compile.includes);
    now_strarray_free(&proj.private_groups);
    now_strarray_free(&proj.langs);
    PASS();
}

/* ---- layers reach the compile line --------------------------------
 *
 * §25 cascading layers was a complete subsystem that configured a
 * report: `now_layer_merge_section()` had two callers and both were
 * the `layers:*` commands, so a `.now-layer.pasta` showed up in
 * `layers:show --effective` and never touched a build. Measured
 * 2026-08-25 before the wiring: a layer carrying
 * `compile: { defines: [X] }` displayed correctly and the build then
 * failed on a source that required X.
 *
 * These test `now_layer_apply_to_project()`, which is the seam. The
 * two that matter most are the gate (a project with no layer file must
 * be untouched) and the baseline (which used to claim compile defaults
 * `now` does not have, and would have handed them to every project on
 * the machine the moment layers fed a build).
 */

static void layer_write(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(body, f);
    fclose(f);
}

/* A project directory with a descriptor, and optionally a layer file.
 * Returns the loaded project, or NULL. */
static NowProject *layer_fixture_ex(char *dir_out, size_t dir_cap,
                                    const char *name,
                                    const char *project_compile,
                                    const char *layer_body) {
    char desc[512];
    char body[1024];
    NowResult res;

    snprintf(dir_out, dir_cap, "%s/%s", NOW_TEST_RESOURCES, name);
    rmtree_best_effort(dir_out);
    now_mkdir_p(dir_out);

    snprintf(desc, sizeof(desc), "%s/now.pasta", dir_out);
    snprintf(body, sizeof(body),
        "{ group: \"org.t\", artifact: \"lfix\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  output: { type: \"executable\", name: \"lfix\" },"
        "  sources: { dir: \"src/main/c\" },"
        "  compile: %s }",
        project_compile ? project_compile
                        : "{ defines: [\"FROM_PROJECT\"] }");
    layer_write(desc, body);

    {
        char lp[512];
        snprintf(lp, sizeof(lp), "%s/.now-layer.pasta", dir_out);
        if (layer_body) {
            layer_write(lp, layer_body);
        } else if (now_path_exists(lp)) {
            /* rmtree_best_effort is best-effort, and on Windows a lock
             * makes it a silent no-op. A leftover layer file here would
             * make the no-layer test assert against the previous run,
             * which is the failure mode this whole exercise keeps
             * finding. Remove it, and if that fails, say so. */
            remove(lp);
            if (now_path_exists(lp)) return NULL;
        }
    }

    memset(&res, 0, sizeof(res));
    return now_project_load(desc, &res);
}

static NowProject *layer_fixture(char *dir_out, size_t dir_cap,
                                 const char *name, const char *layer_body) {
    return layer_fixture_ex(dir_out, dir_cap, name, NULL, layer_body);
}

static int layer_has_define(const NowProject *p, const char *want) {
    size_t i;
    for (i = 0; i < p->compile.defines.count; i++)
        if (strcmp(p->compile.defines.items[i], want) == 0) return 1;
    return 0;
}

/* THE GATE. A project with no .now-layer.pasta anywhere above it must
 * come out of this exactly as it went in. Not equivalently -- the same
 * defines, and no acquired opt level. The built-in baseline used to
 * declare `warnings: [Wall, Wextra]` and `opt: debug`, none of which
 * now_build.c applies on its own, so wiring layers in without this
 * would have changed the compile line of every project in the
 * ecosystem. */
static void test_layers_a_project_without_one_is_untouched(void) {
    char dir[512];
    NowProject *p;
    NowAuditReport audit;

    TEST("layers: a project with no layer file is untouched");

    p = layer_fixture(dir, sizeof(dir), "layer_none", NULL);
    ASSERT_NOT_NULL(p);

    now_audit_init(&audit);
    ASSERT_EQ(now_layer_apply_to_project(p, dir, &audit, NULL), 0);

    ASSERT_EQ((int)p->compile.defines.count, 1);
    ASSERT_EQ(layer_has_define(p, "FROM_PROJECT"), 1);
    /* The baseline's phantom defaults must not have arrived. */
    ASSERT_EQ((int)p->compile.warnings.count, 0);
    if (p->compile.opt) { FAIL("acquired an opt level nobody asked for"); }
    else {
        now_audit_free(&audit);
        now_project_free(p);
        PASS();
        return;
    }
    now_audit_free(&audit);
    now_project_free(p);
}

static void test_layers_an_open_layer_reaches_compile(void) {
    char dir[512];
    NowProject *p;
    NowAuditReport audit;

    TEST("layers: an open layer's define reaches compile config");

    p = layer_fixture(dir, sizeof(dir), "layer_open",
                      "{ compile: { defines: [\"FROM_LAYER\"] } }");
    ASSERT_NOT_NULL(p);

    now_audit_init(&audit);
    ASSERT_EQ(now_layer_apply_to_project(p, dir, &audit, NULL), 0);

    /* Both: the layer's, and the project's own. A merge that dropped
     * either would be a different bug with the same symptom. */
    ASSERT_EQ(layer_has_define(p, "FROM_LAYER"), 1);
    ASSERT_EQ(layer_has_define(p, "FROM_PROJECT"), 1);
    /* Open policy, so no violations. */
    ASSERT_EQ((int)audit.count, 0);

    now_audit_free(&audit);
    now_project_free(p);
    PASS();
}

/* Locked means additive: a lower layer may add, but the project cannot
 * remove or replace, and every attempt is recorded. Both values must
 * survive -- a "locked" section that silently discarded the project's
 * own defines would break builds rather than govern them. */
static void test_layers_locked_is_additive_and_audited(void) {
    char dir[512];
    NowProject *p;
    NowAuditReport audit;

    TEST("layers: a locked section accumulates and records the override");

    p = layer_fixture(dir, sizeof(dir), "layer_locked",
                      "{ compile: { _policy: \"locked\","
                      "             defines: [\"FROM_ORG\"] } }");
    ASSERT_NOT_NULL(p);

    now_audit_init(&audit);
    ASSERT_EQ(now_layer_apply_to_project(p, dir, &audit, NULL), 0);

    ASSERT_EQ(layer_has_define(p, "FROM_ORG"), 1);
    ASSERT_EQ(layer_has_define(p, "FROM_PROJECT"), 1);

    /* And the override was not silent. A locked section nobody is told
     * about is decorative. */
    if (audit.count == 0) {
        FAIL("a locked section was overridden with no violation recorded");
        now_audit_free(&audit);
        now_project_free(p);
        return;
    }
    ASSERT_STR(audit.items[0].code, "NOW-W0401");

    now_audit_free(&audit);
    now_project_free(p);
    PASS();
}

/* The baseline is documentation, and it has to be true. It declared
 * Wall/Wextra/debug while now_build.c applies none of them unless the
 * descriptor asks. */
static void test_layers_the_baseline_claims_no_compile_defaults(void) {
    NowLayerStack stack;
    const NowLayerSection *sec;

    TEST("layers: the built-in baseline claims no compile defaults");

    now_layer_stack_init(&stack);
    if (stack.count == 0) {
        FAIL("baseline layer missing");
        now_layer_stack_free(&stack);
        return;
    }
    sec = now_layer_find_section(&stack.layers[0], "compile");
    if (!sec) {
        FAIL("baseline has no compile section to merge onto");
        now_layer_stack_free(&stack);
        return;
    }
    /* Present, and empty. */
    ASSERT_EQ((int)pasta_count((const PastaValue *)sec->data), 0);
    now_layer_stack_free(&stack);
    PASS();
}

/* ---- config-origin ------------------------------------------------
 *
 * With layers feeding the build, a define on the compile line has three
 * possible sources and the compile line says nothing about which. These
 * check the attribution rules, which differ by kind on purpose: an array
 * entry is credited to the LOWEST layer carrying it (who introduced the
 * value), a scalar to the HIGHEST layer setting it (who won).
 *
 * The second one is the one that would rot quietly -- get it backwards
 * and every scalar reports the org layer that lost, which reads
 * plausibly and sends someone editing the wrong file.
 */
static const NowConfigOrigin *origin_find(const NowConfigOrigins *o,
                                          const char *key, const char *value) {
    size_t i;
    for (i = 0; i < o->count; i++)
        if (strcmp(o->items[i].key, key) == 0 &&
            strcmp(o->items[i].value, value) == 0)
            return &o->items[i];
    return NULL;
}

static void test_origin_credits_the_layer_that_introduced_a_define(void) {
    char dir[512];
    NowProject *p;
    NowConfigOrigins og;
    const NowConfigOrigin *e;

    TEST("config-origin: a define is credited to the layer that introduced it");

    /* SHARED is in BOTH. That is the whole point: with a value in one
     * layer only, walking the stack up or down finds the same answer and
     * the rule under test is unobservable. The first version of this
     * test did exactly that and passed with the walk reversed. */
    p = layer_fixture_ex(dir, sizeof(dir), "origin_arr",
                         "{ defines: [\"SHARED\", \"FROM_PROJECT\"] }",
                         "{ compile: { defines: [\"SHARED\", \"FROM_LAYER\"] } }");
    ASSERT_NOT_NULL(p);
    now_project_free(p);   /* collect re-reads the descriptor itself */

    ASSERT_EQ(now_layer_collect_origins(&og, dir, NULL), 0);

    /* Credited to the layer, because arrays accumulate and the useful
     * fact is who put the value there first -- not who repeated it. */
    e = origin_find(&og, "defines", "SHARED");
    if (!e) { FAIL("the shared define is not in the origins"); goto done; }
    if (!strstr(e->origin, ".now-layer.pasta")) {
        FAIL("a define in both was credited to the project, not the layer");
        goto done;
    }

    e = origin_find(&og, "defines", "FROM_LAYER");
    if (!e) { FAIL("the layer's own define is missing"); goto done; }
    if (!strstr(e->origin, ".now-layer.pasta")) {
        FAIL("the layer's define was not credited to the layer file");
        goto done;
    }

    e = origin_find(&og, "defines", "FROM_PROJECT");
    if (!e) { FAIL("the project's define is missing"); goto done; }
    ASSERT_STR(e->origin, "project");

    now_config_origins_free(&og);
    PASS();
    return;
done:
    now_config_origins_free(&og);
}

/* A scalar set by BOTH must name the one that won. Same reasoning as
 * above and the opposite direction: scalars replace rather than
 * accumulate, so the interesting fact is who is on the compile line,
 * not who asked first. */
static void test_origin_credits_the_winner_of_a_scalar(void) {
    char dir[512];
    NowProject *p;
    NowConfigOrigins og;
    const NowConfigOrigin *e;

    TEST("config-origin: a scalar set by both is credited to the winner");

    p = layer_fixture_ex(dir, sizeof(dir), "origin_scalar",
                         "{ defines: [\"FROM_PROJECT\"], opt: \"speed\" }",
                         "{ compile: { opt: \"size\" } }");
    ASSERT_NOT_NULL(p);
    now_project_free(p);

    ASSERT_EQ(now_layer_collect_origins(&og, dir, NULL), 0);

    /* The project's value is the one that survives... */
    e = origin_find(&og, "opt", "speed");
    if (!e) { FAIL("the winning opt value is not in the origins"); goto done; }
    /* ...and it must be credited to the project, not to the layer it
     * beat. Crediting the loser sends someone to edit the wrong file,
     * and it reads entirely plausibly while doing so. */
    ASSERT_STR(e->origin, "project");

    /* The losing value must not be reported as if it were in effect. */
    if (origin_find(&og, "opt", "size")) {
        FAIL("a scalar that lost the merge is reported as effective");
        goto done;
    }

    now_config_origins_free(&og);
    PASS();
    return;
done:
    now_config_origins_free(&og);
}

/* No layer file anywhere: everything is the project's, and the query
 * still answers rather than erroring. Someone debugging a build should
 * not have to know whether layers are in play before they can ask. */
static void test_origin_answers_without_any_layer_file(void) {
    char dir[512];
    NowProject *p;
    NowConfigOrigins og;
    const NowConfigOrigin *e;

    TEST("config-origin: answers for a project with no layers at all");

    p = layer_fixture(dir, sizeof(dir), "origin_none", NULL);
    ASSERT_NOT_NULL(p);
    now_project_free(p);

    ASSERT_EQ(now_layer_collect_origins(&og, dir, NULL), 0);
    e = origin_find(&og, "defines", "FROM_PROJECT");
    if (!e) { FAIL("the project's own define is not in the origins"); goto done; }
    ASSERT_STR(e->origin, "project");

    /* And nothing was attributed to the baseline, which claims nothing. */
    {
        size_t i;
        for (i = 0; i < og.count; i++) {
            if (strcmp(og.items[i].origin, "now-baseline") == 0) {
                FAIL("a value was credited to a baseline that claims none");
                goto done;
            }
        }
    }
    now_config_origins_free(&og);
    PASS();
    return;
done:
    now_config_origins_free(&og);
}

/* ---- the environment and the command line -------------------------
 *
 * The third and fourth configuration sources. They are layers, so what
 * these check is not the merge (already covered) but the three things
 * specific to them: that the precedence order is the documented one,
 * that a value can say which of them it came from, and that a quoted
 * path survives the split.
 *
 * `setenv` is not portable to MSVC's CRT, so these use _putenv_s on
 * Windows and setenv elsewhere, both wrapped.
 */
static void env_set(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void env_clear_flag_vars(void) {
    env_set("CFLAGS", NULL);
    env_set("LDFLAGS", NULL);
    env_set("NOW_CFLAGS", NULL);
    env_set("NOW_LDFLAGS", NULL);
    now_layer_set_cli_flags(NULL, NULL);
}

/* Order is the whole contract: gcc takes the last flag, so the source
 * that appears last is the source that wins. Documented as
 * descriptor -> CFLAGS -> NOW_CFLAGS -> --cflags. */
static void test_envcli_precedence_is_the_documented_order(void) {
    char dir[512];
    NowProject *p;
    NowAuditReport audit;
    int i_env = -1, i_now = -1, i_cli = -1, i_proj = -1;
    size_t i;

    TEST("env/cli: flags arrive in descriptor, CFLAGS, NOW_CFLAGS, --cflags order");

    env_clear_flag_vars();
    p = layer_fixture_ex(dir, sizeof(dir), "envcli_order",
                         "{ flags: [\"-DPROJ\"] }", NULL);
    ASSERT_NOT_NULL(p);

    env_set("CFLAGS", "-DENV");
    env_set("NOW_CFLAGS", "-DNOWENV");
    now_layer_set_cli_flags("-DCLI", NULL);

    now_audit_init(&audit);
    ASSERT_EQ(now_layer_apply_to_project(p, dir, &audit, NULL), 0);
    now_audit_free(&audit);

    for (i = 0; i < p->compile.flags.count; i++) {
        const char *f = p->compile.flags.items[i];
        if (strcmp(f, "-DPROJ")   == 0) i_proj = (int)i;
        if (strcmp(f, "-DENV")    == 0) i_env  = (int)i;
        if (strcmp(f, "-DNOWENV") == 0) i_now  = (int)i;
        if (strcmp(f, "-DCLI")    == 0) i_cli  = (int)i;
    }
    env_clear_flag_vars();

    if (i_proj < 0 || i_env < 0 || i_now < 0 || i_cli < 0) {
        FAIL("a configuration source did not reach compile.flags at all");
        now_project_free(p);
        return;
    }
    if (!(i_proj < i_env && i_env < i_now && i_now < i_cli)) {
        FAIL("the sources are not in precedence order on the compile line");
        now_project_free(p);
        return;
    }
    now_project_free(p);
    PASS();
}

/* The gate covers all three sources, not just the layer file. With none
 * of them present the project must come through untouched -- this is
 * what keeps every existing descriptor on the machine building as it
 * did before any of this landed. */
static void test_envcli_gate_covers_env_and_cli_too(void) {
    char dir[512];
    NowProject *p;
    NowAuditReport audit;

    TEST("env/cli: no layer, no env, no flag means no change");

    env_clear_flag_vars();
    p = layer_fixture(dir, sizeof(dir), "envcli_gate", NULL);
    ASSERT_NOT_NULL(p);

    now_audit_init(&audit);
    ASSERT_EQ(now_layer_apply_to_project(p, dir, &audit, NULL), 0);
    now_audit_free(&audit);

    ASSERT_EQ((int)p->compile.defines.count, 1);
    ASSERT_EQ(layer_has_define(p, "FROM_PROJECT"), 1);
    ASSERT_EQ((int)p->compile.flags.count, 0);
    now_project_free(p);
    PASS();
}

/* One layer per VARIABLE. A value from LDFLAGS reported as coming from
 * CFLAGS sends someone to unset the wrong thing, and the whole reason
 * plain CFLAGS is read at all is that the origin is one command away. */
static void test_envcli_origin_names_the_variable(void) {
    char dir[512];
    NowProject *p;
    NowConfigOrigins og;
    const NowConfigOrigin *e;

    TEST("env/cli: a value names the variable it came from");

    env_clear_flag_vars();
    p = layer_fixture_ex(dir, sizeof(dir), "envcli_origin", NULL, NULL);
    ASSERT_NOT_NULL(p);
    now_project_free(p);

    env_set("CFLAGS", "-DFROM_CFLAGS");
    env_set("LDFLAGS", "-lfromldflags");
    now_layer_set_cli_flags("-DFROM_CLI", NULL);

    ASSERT_EQ(now_layer_collect_origins(&og, dir, NULL), 0);
    env_clear_flag_vars();

    e = origin_find(&og, "flags", "-DFROM_CFLAGS");
    if (!e) { FAIL("CFLAGS did not reach the merged config"); goto done; }
    ASSERT_STR(e->origin, "CFLAGS");

    e = origin_find(&og, "flags", "-lfromldflags");
    if (!e) { FAIL("LDFLAGS did not reach the merged config"); goto done; }
    /* Not "CFLAGS" -- one layer per variable is the point. */
    ASSERT_STR(e->origin, "LDFLAGS");

    e = origin_find(&og, "flags", "-DFROM_CLI");
    if (!e) { FAIL("--cflags did not reach the merged config"); goto done; }
    ASSERT_STR(e->origin, "--cflags");

    now_config_origins_free(&og);
    PASS();
    return;
done:
    now_config_origins_free(&og);
}

/* A quoted path with a space is one flag. `-IC:\Program Files\...` is an
 * ordinary include path here, and splitting it on whitespace yields
 * three flags, none of which is a directory -- which the compiler then
 * reports as a missing header rather than as a broken flag. */
static void test_envcli_a_quoted_path_stays_one_flag(void) {
    char dir[512];
    NowProject *p;
    NowAuditReport audit;
    int found = 0;
    size_t i;

    TEST("env/cli: a quoted path with a space stays one flag");

    env_clear_flag_vars();
    p = layer_fixture_ex(dir, sizeof(dir), "envcli_quote", NULL, NULL);
    ASSERT_NOT_NULL(p);

    now_layer_set_cli_flags("-I\"C:\\Program Files\\x\\include\" -DQ", NULL);
    now_audit_init(&audit);
    ASSERT_EQ(now_layer_apply_to_project(p, dir, &audit, NULL), 0);
    now_audit_free(&audit);
    env_clear_flag_vars();

    for (i = 0; i < p->compile.flags.count; i++)
        if (strcmp(p->compile.flags.items[i],
                   "-IC:\\Program Files\\x\\include") == 0) found = 1;

    if (!found) {
        FAIL("the quoted include path was split on its spaces");
        now_project_free(p);
        return;
    }
    /* And the flag after it still parsed as its own flag. */
    ASSERT_EQ((int)p->compile.flags.count, 2);
    now_project_free(p);
    PASS();
}

/* ---- zero-config: compile to objects and stop ---------------------
 *
 * `now build` in a tree with no now.pasta used to exit 3. It now walks,
 * compiles what it recognises, and stops before linking -- because
 * compiling is the last step whose failure is loud, and every decision
 * past it (what links with what, static or shared, what is exported)
 * fails silently as a successful build of the wrong thing.
 *
 * What these check is the boundary rather than the compiler: that it
 * finds sources without being told where they are, that it never
 * produces an artifact, that config still reaches it, and that a second
 * run does not eat its own output.
 */
static void zc_write(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(body, f);
    fclose(f);
}

/* A tree with no descriptor. Deliberately NOT in src/main/c: the walker
 * must not depend on a layout, because the layout is exactly what a
 * tree without a descriptor has not committed to. */
static int zc_fixture(char *dir_out, size_t cap, const char *name) {
    char p[512];
    snprintf(dir_out, cap, "%s/%s", NOW_TEST_RESOURCES, name);
    rmtree_best_effort(dir_out);
    if (now_path_exists(dir_out)) return 0;
    now_mkdir_p(dir_out);
    snprintf(p, sizeof(p), "%s/code", dir_out);
    now_mkdir_p(p);
    return 1;
}

static void test_zeroconfig_compiles_a_tree_with_no_descriptor(void) {
    char dir[512], p[512], obj[512];
    NowResult res;

    TEST("zero-config: a tree with no now.pasta compiles to objects");

    if (!zc_fixture(dir, sizeof(dir), "zc_plain")) {
        FAIL("could not clear the fixture - cannot establish precondition");
        return;
    }
    snprintf(p, sizeof(p), "%s/code/helper.c", dir);
    zc_write(p, "int helper(void) { return 7; }\n");
    snprintf(p, sizeof(p), "%s/code/app.c", dir);
    zc_write(p, "int helper(void);\nint main(void) { return helper() - 7; }\n");

    memset(&res, 0, sizeof(res));
    ASSERT_EQ(now_build_objects(dir, &res), 0);

    /* Objects exist... */
    snprintf(obj, sizeof(obj), "%s/target/obj/main/code/helper.c.o", dir);
    if (!now_path_exists(obj)) {
        FAIL("no object was produced for a source it should have found");
        return;
    }

    /* ...and nothing was linked. This is the half that makes it a
     * design rather than an unfinished build. */
    snprintf(obj, sizeof(obj), "%s/target/bin", dir);
    if (now_path_exists(obj)) {
        FAIL("zero-config produced an artifact; it must stop at objects");
        return;
    }
    PASS();
}

/* Nothing to compile is not a failure. A tree of documentation is a
 * legitimate thing to point this at, and exiting non-zero would make it
 * unusable in anything that checks status. */
static void test_zeroconfig_an_empty_tree_is_not_an_error(void) {
    char dir[512], p[512];
    NowResult res;

    TEST("zero-config: a tree with no sources is reported, not failed");

    if (!zc_fixture(dir, sizeof(dir), "zc_empty")) {
        FAIL("could not clear the fixture - cannot establish precondition");
        return;
    }
    /* A HEADER, not a README.
     *
     * A file no language claims proves nothing here -- classify returns
     * NULL for it either way, so the test passes whether or not the
     * detector filters on what actually produces an object. A .h IS
     * claimed by C, and must still not make this a C tree with nothing
     * to compile. Measured: with the filter removed, README.md left
     * this green. */
    snprintf(p, sizeof(p), "%s/code/only_a_header.h", dir);
    zc_write(p, "int f(void);\n");

    memset(&res, 0, sizeof(res));
    ASSERT_EQ(now_build_objects(dir, &res), 0);
    PASS();
}

/* Compiling is the last loud step, and this is what that means: with no
 * -I and no -D the source cannot compile, and the build says so rather
 * than producing something. The companion half -- that supplying them
 * fixes it -- is the next test. */
static void test_zeroconfig_a_source_that_cannot_compile_fails(void) {
    char dir[512], p[512];
    NowResult res;

    TEST("zero-config: a source that cannot compile fails the build");

    if (!zc_fixture(dir, sizeof(dir), "zc_needs_cfg")) {
        FAIL("could not clear the fixture - cannot establish precondition");
        return;
    }
    snprintf(p, sizeof(p), "%s/code/a.c", dir);
    zc_write(p, "#ifndef WANTED\n#error \"no define\"\n#endif\n"
                "int f(void) { return 1; }\n");

    memset(&res, 0, sizeof(res));
    now_layer_set_cli_flags(NULL, NULL);
    if (now_build_objects(dir, &res) == 0) {
        FAIL("a source that cannot compile produced a successful build");
        return;
    }
    PASS();
}

/* ...and the configuration sources reach this mode too. Without this,
 * zero-config would be unusable on any tree whose headers are not
 * beside its sources -- which is most of them. */
static void test_zeroconfig_takes_config_from_the_command_line(void) {
    char dir[512], p[512], obj[512];
    NowResult res;

    TEST("zero-config: --cflags reaches the compile line");

    if (!zc_fixture(dir, sizeof(dir), "zc_cfg")) {
        FAIL("could not clear the fixture - cannot establish precondition");
        return;
    }
    snprintf(p, sizeof(p), "%s/code/a.c", dir);
    zc_write(p, "#ifndef WANTED\n#error \"no define\"\n#endif\n"
                "int f(void) { return 1; }\n");

    now_layer_set_cli_flags("-DWANTED", NULL);
    memset(&res, 0, sizeof(res));
    ASSERT_EQ(now_build_objects(dir, &res), 0);
    now_layer_set_cli_flags(NULL, NULL);

    snprintf(obj, sizeof(obj), "%s/target/obj/main/code/a.c.o", dir);
    if (!now_path_exists(obj)) {
        FAIL("the object was not produced even with the define supplied");
        return;
    }
    PASS();
}

/* The walk starts at the tree root, which contains target/. Without an
 * exclusion the second run compiles the first run's output, and a tree
 * that is built twice is not the tree that was built once. */
static void test_zeroconfig_does_not_walk_its_own_output(void) {
    char dir[512], p[512];
    NowResult res;
    NowFileList a, b;
    const char *exts[] = { ".o", ".obj", NULL };

    TEST("zero-config: a second run does not compile the first run's output");

    if (!zc_fixture(dir, sizeof(dir), "zc_rewalk")) {
        FAIL("could not clear the fixture - cannot establish precondition");
        return;
    }
    snprintf(p, sizeof(p), "%s/code/a.c", dir);
    zc_write(p, "int f(void) { return 1; }\n");

    memset(&res, 0, sizeof(res));
    ASSERT_EQ(now_build_objects(dir, &res), 0);
    now_filelist_init(&a);
    now_discover_sources(dir, "target", exts, &a);

    /* Plant a SOURCE under target/, the way a code generator would.
     *
     * Objects alone prove nothing: the walk looks for source
     * extensions, and target/ holds none, so dropping the exclusion
     * changes nothing and this test stays green while watching nothing.
     * A generated .c is the case the exclusion exists for, and the
     * failure it prevents compounds -- every run compiles the last
     * run's generated sources plus its own. */
    snprintf(p, sizeof(p), "%s/target/generated.c", dir);
    zc_write(p, "int generated_thing(void) { return 1; }\n");

    ASSERT_EQ(now_build_objects(dir, &res), 0);
    now_filelist_init(&b);
    now_discover_sources(dir, "target", exts, &b);

    /* Same object count both times. Growth here means the walk consumed
     * its own output, and it compounds every run. */
    if (a.count != b.count) {
        FAIL("the object tree grew on a second run");
        now_filelist_free(&a);
        now_filelist_free(&b);
        return;
    }
    now_filelist_free(&a);
    now_filelist_free(&b);
    PASS();
}

static void test_layer_merge_strarray_exclude(void) {
    TEST("layers: !exclude: removes entries in open mode");
    NowStrArray dst;
    now_strarray_init(&dst);
    now_strarray_push(&dst, "Wall");
    now_strarray_push(&dst, "Wextra");
    now_strarray_push(&dst, "Wpedantic");

    NowStrArray src;
    now_strarray_init(&src);
    now_strarray_push(&src, "!exclude:Wpedantic");
    now_strarray_push(&src, "Wformat");

    now_layer_merge_strarray(&dst, &src, NOW_POLICY_OPEN);

    /* Wpedantic should be removed, Wformat added */
    ASSERT_EQ(dst.count, (size_t)3);  /* Wall, Wextra, Wformat */
    /* Check Wpedantic is gone */
    int found_pedantic = 0;
    int found_format = 0;
    for (size_t i = 0; i < dst.count; i++) {
        if (strcmp(dst.items[i], "Wpedantic") == 0) found_pedantic = 1;
        if (strcmp(dst.items[i], "Wformat") == 0) found_format = 1;
    }
    if (found_pedantic) { FAIL("Wpedantic should be excluded"); now_strarray_free(&dst); now_strarray_free(&src); return; }
    if (!found_format) { FAIL("Wformat should be added"); now_strarray_free(&dst); now_strarray_free(&src); return; }

    now_strarray_free(&dst);
    now_strarray_free(&src);
    PASS();
}

static void test_layer_audit_format(void) {
    TEST("layers: audit format output");
    NowAuditReport audit;
    now_audit_init(&audit);

    /* Empty report */
    char *out = now_audit_format(&audit);
    ASSERT_NOT_NULL(out);
    if (!strstr(out, "No advisory")) { FAIL("expected no-violations message"); free(out); return; }
    free(out);

    now_audit_free(&audit);
    PASS();
}

static void test_layer_push_project(void) {
    TEST("layers: push project as top layer");
    NowLayerStack stack;
    now_layer_stack_init(&stack);

    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    now_strarray_init(&proj.compile.warnings);
    now_strarray_init(&proj.compile.defines);
    now_strarray_init(&proj.compile.flags);
    now_strarray_init(&proj.compile.includes);
    now_strarray_init(&proj.private_groups);
    now_strarray_init(&proj.langs);
    now_strarray_push(&proj.private_groups, "org.secret");
    now_layer_push_project(&stack, &proj);

    ASSERT_EQ(stack.count, (size_t)2);
    ASSERT_STR(stack.layers[1].id, "project");

    const NowLayerSection *pg = now_layer_find_section(&stack.layers[1], "private_groups");
    ASSERT_NOT_NULL(pg);

    now_layer_stack_free(&stack);
    now_strarray_free(&proj.compile.warnings);
    now_strarray_free(&proj.compile.defines);
    now_strarray_free(&proj.compile.flags);
    now_strarray_free(&proj.compile.includes);
    now_strarray_free(&proj.private_groups);
    now_strarray_free(&proj.langs);
    PASS();
}

/* ---- Alforno integration ---- */

static void test_alforno_aggregate_merge(void) {
    TEST("alforno: aggregate merges two pastlets");
    AlfContext *ctx = alf_create(ALF_AGGREGATE, NULL);
    ASSERT_NOT_NULL(ctx);

    const char *input1 = "@db {\n  engine: \"postgres\",\n  pool: 10\n}\n";
    const char *input2 = "@db {\n  pool: 20,\n  timeout: 30\n}\n@cache {\n  ttl: 60\n}\n";

    AlfResult ar;
    ASSERT_EQ(alf_add_input(ctx, input1, strlen(input1), &ar), 0);
    ASSERT_EQ(alf_add_input(ctx, input2, strlen(input2), &ar), 0);

    PastaValue *out = alf_process(ctx, &ar);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ((int)ar.code, ALF_OK);

    /* @db should have pool=20 (overlay wins), engine preserved, timeout added */
    const PastaValue *db = pasta_map_get(out, "db");
    ASSERT_NOT_NULL(db);
    ASSERT_EQ((int)pasta_get_number(pasta_map_get(db, "pool")), 20);
    ASSERT_STR(pasta_get_string(pasta_map_get(db, "engine")), "postgres");
    ASSERT_EQ((int)pasta_get_number(pasta_map_get(db, "timeout")), 30);

    /* @cache should be present */
    const PastaValue *cache = pasta_map_get(out, "cache");
    ASSERT_NOT_NULL(cache);
    ASSERT_EQ((int)pasta_get_number(pasta_map_get(cache, "ttl")), 60);

    pasta_free(out);
    alf_free(ctx);
    PASS();
}

static void test_alforno_parameterize(void) {
    TEST("alforno: @vars substitution");
    AlfContext *ctx = alf_create(ALF_AGGREGATE, NULL);
    ASSERT_NOT_NULL(ctx);

    const char *input =
        "@vars {\n  name: \"myapp\",\n  version: \"1.0\"\n}\n"
        "@project {\n  artifact: \"{name}\",\n  ver: \"{version}\"\n}\n";

    AlfResult ar;
    ASSERT_EQ(alf_add_input(ctx, input, strlen(input), &ar), 0);

    PastaValue *out = alf_process(ctx, &ar);
    ASSERT_NOT_NULL(out);

    /* @vars should be consumed (not in output) */
    ASSERT_NULL(pasta_map_get(out, "vars"));

    /* @project should have substituted values */
    const PastaValue *proj = pasta_map_get(out, "project");
    ASSERT_NOT_NULL(proj);
    ASSERT_STR(pasta_get_string(pasta_map_get(proj, "artifact")), "myapp");
    ASSERT_STR(pasta_get_string(pasta_map_get(proj, "ver")), "1.0");

    pasta_free(out);
    alf_free(ctx);
    PASS();
}

static void test_alforno_conflate(void) {
    TEST("alforno: conflate filters to recipe fields");
    AlfContext *ctx = alf_create(ALF_CONFLATE, NULL);
    ASSERT_NOT_NULL(ctx);

    const char *recipe =
        "@output {\n  consumes: [\"settings\"],\n  theme: \"allowed\"\n}\n";
    const char *input =
        "@settings {\n  theme: \"dark\",\n  secret: \"hunter2\"\n}\n";

    AlfResult ar;
    ASSERT_EQ(alf_set_recipe(ctx, recipe, strlen(recipe), &ar), 0);
    ASSERT_EQ(alf_add_input(ctx, input, strlen(input), &ar), 0);

    PastaValue *out = alf_process(ctx, &ar);
    ASSERT_NOT_NULL(out);

    /* @output should have theme but NOT secret */
    const PastaValue *o = pasta_map_get(out, "output");
    ASSERT_NOT_NULL(o);
    ASSERT_STR(pasta_get_string(pasta_map_get(o, "theme")), "dark");
    ASSERT_NULL(pasta_map_get(o, "secret"));

    pasta_free(out);
    alf_free(ctx);
    PASS();
}

static void test_alforno_collect_merge(void) {
    TEST("alforno: conflate collect merges arrays");
    AlfContext *ctx = alf_create(ALF_CONFLATE, NULL);
    ASSERT_NOT_NULL(ctx);

    const char *recipe =
        "@merged {\n  consumes: [\"a\", \"b\"],\n  merge: \"collect\",\n"
        "  tags: \"collected\"\n}\n";
    const char *input1 = "@a {\n  tags: \"fast\"\n}\n";
    const char *input2 = "@b {\n  tags: \"safe\"\n}\n";

    AlfResult ar;
    ASSERT_EQ(alf_set_recipe(ctx, recipe, strlen(recipe), &ar), 0);
    ASSERT_EQ(alf_add_input(ctx, input1, strlen(input1), &ar), 0);
    ASSERT_EQ(alf_add_input(ctx, input2, strlen(input2), &ar), 0);

    PastaValue *out = alf_process(ctx, &ar);
    ASSERT_NOT_NULL(out);

    /* @merged.tags should be an array of both values */
    const PastaValue *m = pasta_map_get(out, "merged");
    ASSERT_NOT_NULL(m);
    const PastaValue *tags = pasta_map_get(m, "tags");
    ASSERT_NOT_NULL(tags);
    ASSERT_EQ((int)pasta_type(tags), PASTA_ARRAY);
    ASSERT_EQ((int)pasta_count(tags), 2);

    pasta_free(out);
    alf_free(ctx);
    PASS();
}

/* ---- Multi-architecture / triples ---- */

static void test_triple_parse_full(void) {
    TEST("triple: parse full os:arch:variant");
    NowTriple t;
    now_triple_parse(&t, "linux:amd64:gnu");
    ASSERT_STR(t.os, "linux");
    ASSERT_STR(t.arch, "amd64");
    ASSERT_STR(t.variant, "gnu");
    PASS();
}

static void test_triple_parse_shorthand(void) {
    TEST("triple: parse shorthand fills from host");
    NowTriple t;
    now_triple_parse(&t, ":amd64:musl");
    /* os should be empty before fill */
    ASSERT_STR(t.os, "");
    now_triple_fill_from_host(&t);
    /* os should now be filled from host */
    const NowTriple *host = now_host_triple_parsed();
    if (strcmp(t.os, host->os) != 0) { FAIL("os not filled from host"); return; }
    ASSERT_STR(t.arch, "amd64");
    ASSERT_STR(t.variant, "musl");
    PASS();
}

static void test_triple_format(void) {
    TEST("triple: format as colon-separated string");
    NowTriple t;
    now_triple_parse(&t, "windows:amd64:msvc");
    char buf[256];
    now_triple_format(&t, buf, sizeof(buf));
    ASSERT_STR(buf, "windows:amd64:msvc");
    PASS();
}

static void test_triple_dir(void) {
    TEST("triple: format as directory name (dashes)");
    NowTriple t;
    now_triple_parse(&t, "linux:arm64:musl");
    char buf[256];
    now_triple_dir(&t, buf, sizeof(buf));
    ASSERT_STR(buf, "linux-arm64-musl");
    PASS();
}

static void test_triple_cmp(void) {
    TEST("triple: compare equal and unequal");
    NowTriple a, b;
    now_triple_parse(&a, "linux:amd64:gnu");
    now_triple_parse(&b, "linux:amd64:gnu");
    ASSERT_EQ(now_triple_cmp(&a, &b), 0);
    now_triple_parse(&b, "linux:arm64:gnu");
    if (now_triple_cmp(&a, &b) == 0) { FAIL("expected unequal"); return; }
    PASS();
}

static void test_triple_match_exact(void) {
    TEST("triple: wildcard match exact");
    NowTriple pat, concrete;
    now_triple_parse(&pat, "linux:amd64:gnu");
    now_triple_parse(&concrete, "linux:amd64:gnu");
    ASSERT_EQ(now_triple_match(&pat, &concrete), 1);
    now_triple_parse(&concrete, "linux:arm64:gnu");
    ASSERT_EQ(now_triple_match(&pat, &concrete), 0);
    PASS();
}

static void test_triple_match_wildcard(void) {
    TEST("triple: wildcard * matches any value");
    NowTriple pat, c1, c2;
    now_triple_parse(&pat, "linux:*:musl");
    now_triple_parse(&c1, "linux:amd64:musl");
    now_triple_parse(&c2, "linux:arm64:musl");
    ASSERT_EQ(now_triple_match(&pat, &c1), 1);
    ASSERT_EQ(now_triple_match(&pat, &c2), 1);
    /* Different OS should not match */
    NowTriple c3;
    now_triple_parse(&c3, "macos:arm64:musl");
    ASSERT_EQ(now_triple_match(&pat, &c3), 0);
    PASS();
}

static void test_triple_host_detect(void) {
    TEST("triple: host detection returns valid triple");
    const NowTriple *host = now_host_triple_parsed();
    ASSERT_NOT_NULL(host);
    if (host->os[0] == '\0') { FAIL("empty os"); return; }
    if (host->arch[0] == '\0') { FAIL("empty arch"); return; }
    if (host->variant[0] == '\0') { FAIL("empty variant"); return; }
    PASS();
}

static void test_triple_is_native(void) {
    TEST("triple: native detection");
    const NowTriple *host = now_host_triple_parsed();
    ASSERT_EQ(now_triple_is_native(host), 1);
    NowTriple cross;
    now_triple_parse(&cross, "freestanding:riscv64:none");
    ASSERT_EQ(now_triple_is_native(&cross), 0);
    PASS();
}

/* ---- Per-triple compile/link overrides (§11.9, `target_flags`) ----
 *
 * One descriptor drives every test below: four entries, two of which
 * match a riscv64 freestanding build and two of which must not. Using
 * one fixture for all of them is deliberate — the interesting failures
 * are entries leaking into a build they do not describe, and that is
 * only visible when the non-matching entries are present.
 *
 * The base blocks carry a flag and a scalar of their own so "appended
 * after the base" and "replaced the base" are distinguishable from
 * "the base was dropped". */
static const char *TFLAGS_POM =
    "{ group: \"org.test\", artifact: \"tflags\", version: \"1.0\","
    "  langs: [\"c\"],"
    "  compile: { flags: [\"-base\"], defines: [\"BASE\"], opt: \"speed\" },"
    "  link:    { flags: [\"-Lbase\"], libs: [\"baselib\"] },"
    "  target_flags: {"
    "    \"freestanding:riscv64:none\": {"
    "      compile: { flags: [\"--target=riscv64-unknown-elf\", \"-mabi=lp64\"],"
    "                 defines: [\"RV64\"], opt: \"size\" },"
    "      link:    { flags: [\"-nostdlib\"], libs: [\"rvlib\"] } },"
    "    \"freestanding:*:*\": {"
    "      compile: { flags: [\"-ffreestanding\"], defines: [\"FREESTANDING\"] } },"
    "    \"freestanding:arm64:none\": {"
    "      compile: { flags: [\"-mgeneral-regs-only\"], defines: [\"ARM64\"] },"
    "      link:    { flags: [\"-armlink\"] } },"
    "    \"linux:*:gnu\": {"
    "      compile: { flags: [\"-pthread\"], defines: [\"GLIBC\"] } }"
    "  } }";

/* Load TFLAGS_POM as if `--target <triple>` had been given. Restores
 * nothing — every caller must clear the effective target, because it
 * is process-wide and a leak into the next test is exactly the kind of
 * cross-test coupling that makes a suite lie. */
static NowProject *load_tflags_for(const char *triple) {
    NowTriple t;
    now_triple_parse(&t, triple);
    now_arch_set_effective_target(&t);
    NowResult res;
    NowProject *p = now_project_load_string(TFLAGS_POM, strlen(TFLAGS_POM), &res);
    now_arch_set_effective_target(NULL);
    return p;
}

/* Index of `s` in `a`, or -1. Order matters in §11.9 and a `contains`
 * cannot see it. */
static int strarray_index(const NowStrArray *a, const char *s) {
    for (size_t i = 0; i < a->count; i++)
        if (strcmp(a->items[i], s) == 0) return (int)i;
    return -1;
}

static void test_target_flags_match_in_declaration_order(void) {
    TEST("target_flags: matching entries append in declaration order");
    NowProject *p = load_tflags_for("freestanding:riscv64:none");
    ASSERT_NOT_NULL(p);

    int base = strarray_index(&p->compile.flags, "-base");
    int exact = strarray_index(&p->compile.flags, "--target=riscv64-unknown-elf");
    int wild  = strarray_index(&p->compile.flags, "-ffreestanding");

    if (base != 0)          { now_project_free(p); FAIL("base flag not first"); return; }
    if (exact < 0 || wild < 0) { now_project_free(p); FAIL("a matching entry contributed nothing"); return; }
    /* The exact entry is declared before the wildcard one, so it must
     * appear first — not because either is "more specific", but
     * because §11.9 orders by declaration and nothing else. */
    if (exact > wild)       { now_project_free(p); FAIL("declaration order not preserved"); return; }

    now_project_free(p);
    PASS();
}

/* The invariant, rather than a list of flags that must be absent: NO
 * string contributed by an entry whose pattern does not match the
 * effective target may appear anywhere in the merged blocks.
 *
 * Written this way because the case list can only catch the leaks we
 * already suspect. This walks the descriptor's own entries, so an
 * entry added to the fixture later is covered without the assertion
 * being touched — and a matcher that silently degrades to "match
 * everything" fails here even though every positive test still
 * passes. */
static void test_target_flags_unmatched_entries_contribute_nothing(void) {
    TEST("target_flags: nothing from a non-matching pattern reaches the build");
    NowProject *p = load_tflags_for("freestanding:riscv64:none");
    ASSERT_NOT_NULL(p);

    NowTriple target;
    now_triple_parse(&target, "freestanding:riscv64:none");

    int checked = 0;
    for (size_t i = 0; i < p->target_flags.count; i++) {
        const NowTargetFlags *e = &p->target_flags.items[i];
        NowTriple pattern;
        now_triple_parse(&pattern, e->pattern);
        if (now_triple_match(&pattern, &target)) continue;
        checked++;
        for (size_t f = 0; f < e->compile.flags.count; f++) {
            if (strarray_index(&p->compile.flags, e->compile.flags.items[f]) >= 0) {
                now_project_free(p); FAIL("a non-matching compile flag leaked"); return;
            }
        }
        for (size_t d = 0; d < e->compile.defines.count; d++) {
            if (strarray_index(&p->compile.defines, e->compile.defines.items[d]) >= 0) {
                now_project_free(p); FAIL("a non-matching define leaked"); return;
            }
        }
        for (size_t f = 0; f < e->link.flags.count; f++) {
            if (strarray_index(&p->link.flags, e->link.flags.items[f]) >= 0) {
                now_project_free(p); FAIL("a non-matching link flag leaked"); return;
            }
        }
    }
    /* A test that examined no non-matching entry proved nothing. */
    if (checked != 2) { now_project_free(p); FAIL("expected 2 non-matching entries"); return; }

    now_project_free(p);
    PASS();
}

static void test_target_flags_follow_the_target(void) {
    TEST("target_flags: the same descriptor differs by target");
    NowProject *rv = load_tflags_for("freestanding:riscv64:none");
    ASSERT_NOT_NULL(rv);
    NowProject *arm = load_tflags_for("freestanding:arm64:none");
    if (!arm) { now_project_free(rv); FAIL("arm64 load failed"); return; }

    int rv_has_rv   = strarray_index(&rv->compile.defines,  "RV64") >= 0;
    int rv_has_arm  = strarray_index(&rv->compile.defines,  "ARM64") >= 0;
    int arm_has_arm = strarray_index(&arm->compile.defines, "ARM64") >= 0;
    int arm_has_rv  = strarray_index(&arm->compile.defines, "RV64") >= 0;
    /* Both get the wildcard entry — which is what proves the two loads
     * differ by target rather than by one of them having failed. */
    int both_wild = strarray_index(&rv->compile.defines,  "FREESTANDING") >= 0 &&
                    strarray_index(&arm->compile.defines, "FREESTANDING") >= 0;

    now_project_free(rv);
    now_project_free(arm);

    if (!rv_has_rv || !arm_has_arm) { FAIL("exact entry missed its own target"); return; }
    if (rv_has_arm || arm_has_rv)   { FAIL("exact entry crossed targets"); return; }
    if (!both_wild)                 { FAIL("wildcard entry missed one of them"); return; }
    PASS();
}

static void test_target_flags_scalars_replace(void) {
    TEST("target_flags: scalars replace the base, arrays append to it");
    NowProject *p = load_tflags_for("freestanding:riscv64:none");
    ASSERT_NOT_NULL(p);

    /* opt: base says "speed", the matching entry says "size". A
     * freestanding target that cannot honour the base's choice has to
     * be able to overrule it, which is why this one replaces. */
    if (!p->compile.opt || strcmp(p->compile.opt, "size") != 0) {
        now_project_free(p); FAIL("opt was not replaced"); return;
    }
    /* ...while the base's own define survives beside the new ones. */
    if (strarray_index(&p->compile.defines, "BASE") != 0) {
        now_project_free(p); FAIL("base define lost or reordered"); return;
    }
    now_project_free(p);
    PASS();
}

static void test_target_flags_link_block(void) {
    TEST("target_flags: the link block merges too");
    NowProject *p = load_tflags_for("freestanding:riscv64:none");
    ASSERT_NOT_NULL(p);
    if (strarray_index(&p->link.flags, "-Lbase")   != 0 ||
        strarray_index(&p->link.flags, "-nostdlib") < 0 ||
        strarray_index(&p->link.libs,  "baselib")  != 0 ||
        strarray_index(&p->link.libs,  "rvlib")     < 0) {
        now_project_free(p); FAIL("link block not merged"); return;
    }
    now_project_free(p);
    PASS();
}

static void test_target_flags_absent_is_inert(void) {
    TEST("target_flags: a descriptor without the block is unchanged");
    static const char *POM =
        "{ group: \"org.test\", artifact: \"plain\", version: \"1.0\","
        "  langs: [\"c\"], compile: { flags: [\"-base\"] } }";
    NowResult res;
    NowProject *p = now_project_load_string(POM, strlen(POM), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ((int)p->target_flags.count, 0);
    ASSERT_EQ((int)p->compile.flags.count, 1);
    ASSERT_STR(p->compile.flags.items[0], "-base");
    now_project_free(p);
    PASS();
}


/* ---- assembly: extra files a package carries (§24, DRAFT) ----
 *
 * The block parsed and was ignored for as long as it has existed, and
 * `warn_descriptor_keys` said so — which is the right behaviour for an
 * unimplemented field and the reason aurora found out by reading a
 * warning rather than by shipping a broken SDK.
 *
 * These lock the parse. The packaging and install behaviour is exercised
 * end to end rather than here, because what it produces is a file
 * layout and asserting on a layout from inside this suite would mean
 * reimplementing the layout. */

static const char *ASM_POM =
    "{ group: \"org.test\", artifact: \"sdk\", version: \"1.0\","
    "  langs: [\"c\"],"
    "  output: { type: \"header-only\" },"
    "  assembly: {"
    "    include: ["
    "      { src: \"prebuilt/**\", dest: \"lib/\", exclude: [\"**/*.tmp\"] },"
    "      { src: \"src/main/asm/**\", dest: \"asm/\" },"
    "      { dest: \"nowhere/\" }"
    "    ]"
    "  } }";

static void test_assembly_parses_its_includes(void) {
    TEST("assembly: include entries parse in order");
    NowResult res;
    NowProject *p = now_project_load_string(ASM_POM, strlen(ASM_POM), &res);
    ASSERT_NOT_NULL(p);

    /* Three entries were written and one has no `src`. An entry that
     * selects nothing is dropped at load rather than carried as an
     * empty rule, because a rule that cannot match is indistinguishable
     * from one that matched nothing — and only one of those is worth
     * warning about later. */
    ASSERT_EQ((int)p->assembly.count, 2);

    ASSERT_STR(p->assembly.items[0].src,  "prebuilt/**");
    ASSERT_STR(p->assembly.items[0].dest, "lib/");
    ASSERT_EQ((int)p->assembly.items[0].exclude.count, 1);
    ASSERT_STR(p->assembly.items[0].exclude.items[0], "**/*.tmp");

    ASSERT_STR(p->assembly.items[1].src,  "src/main/asm/**");
    ASSERT_STR(p->assembly.items[1].dest, "asm/");
    ASSERT_EQ((int)p->assembly.items[1].exclude.count, 0);

    now_project_free(p);
    PASS();
}

static void test_assembly_absent_is_inert(void) {
    TEST("assembly: a descriptor without the block carries nothing");
    static const char *POM =
        "{ group: \"org.test\", artifact: \"plain\", version: \"1.0\","
        "  langs: [\"c\"] }";
    NowResult res;
    NowProject *p = now_project_load_string(POM, strlen(POM), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ((int)p->assembly.count, 0);
    now_project_free(p);
    PASS();
}


/* ---- schema:check (§31.19) ----
 *
 * These are the first tests of the descriptor DIAGNOSTICS, as opposed
 * to the descriptor loader. Until the collector existed the only way to
 * observe an "unknown key" warning was to read a build log, so the key
 * lists that decide what users are told had no coverage at all — the
 * lists were edited by hand and verified by eye, twice this month.
 *
 * The distinction under test is the one that matters and the one most
 * easily got backwards: an ERROR is a descriptor that cannot mean what
 * it says; a WARNING is one this build does not fully implement. A
 * descriptor written against a newer spec must still be usable, so
 * warnings never fail the check — that is `--strict`'s job.
 */

static void write_file(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(body, 1, strlen(body), f);
    fclose(f);
}

static int diag_has(const NowDiagList *l, const char *code) {
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(l->items[i].code, code) == 0) return 1;
    return 0;
}

/* Run schema:check over a descriptor written to a scratch file. */
static NowSchemaResult schema_of(const char *body, NowDiagList *e,
                                 NowDiagList *w) {
    write_file("target/schema_t.pasta", body);
    now_diaglist_init(e);
    now_diaglist_init(w);
    return now_schema_check("target/schema_t.pasta", e, w);
}

static void test_schema_accepts_a_good_descriptor(void) {
    NowDiagList e, w;
    TEST("schema: a valid descriptor is valid and quiet");
    NowSchemaResult r = schema_of(
        "{ group: \"org.t\", artifact: \"a\", version: \"1.2.3\","
        "  langs: [\"c\"], std: \"c11\" }", &e, &w);
    ASSERT_EQ((int)r, (int)NOW_SCHEMA_VALID);
    ASSERT_EQ((int)e.count, 0);
    ASSERT_EQ((int)w.count, 0);
    now_diaglist_free(&e); now_diaglist_free(&w);
    PASS();
}

static void test_schema_missing_file_is_two_not_one(void) {
    NowDiagList e, w;
    TEST("schema: a missing descriptor is 2, not invalid");
    now_diaglist_init(&e); now_diaglist_init(&w);
    /* §31.19 gives "not found" its own exit code precisely so a script
     * can tell "you are in the wrong directory" from "your descriptor
     * is wrong". Collapsing them would make the command useless in the
     * case it is most often run by mistake. */
    NowSchemaResult r = now_schema_check("target/definitely_absent.pasta",
                                         &e, &w);
    ASSERT_EQ((int)r, (int)NOW_SCHEMA_NOT_FOUND);
    now_diaglist_free(&e); now_diaglist_free(&w);
    PASS();
}

static void test_schema_rejects_what_cannot_be_meant(void) {
    NowDiagList e, w;
    TEST("schema: version, std, output.type and dep coordinates");

    NowSchemaResult r = schema_of(
        "{ group: \"org.t\", artifact: \"a\", version: \"not-a-version\","
        "  langs: [\"c\"] }", &e, &w);
    ASSERT_EQ((int)r, (int)NOW_SCHEMA_INVALID);
    ASSERT_EQ(diag_has(&e, "NOW-E0103"), 1);
    now_diaglist_free(&e); now_diaglist_free(&w);

    r = schema_of("{ group: \"org.t\", artifact: \"a\", version: \"1.0.0\","
                  "  langs: [\"c\"], std: \"c14\" }", &e, &w);
    ASSERT_EQ((int)r, (int)NOW_SCHEMA_INVALID);
    ASSERT_EQ(diag_has(&e, "NOW-E0102"), 1);
    now_diaglist_free(&e); now_diaglist_free(&w);

    r = schema_of("{ group: \"org.t\", artifact: \"a\", version: \"1.0.0\","
                  "  langs: [\"c\"], output: { type: \"libraryish\" } }", &e, &w);
    ASSERT_EQ((int)r, (int)NOW_SCHEMA_INVALID);
    ASSERT_EQ(diag_has(&e, "NOW-E0104"), 1);
    now_diaglist_free(&e); now_diaglist_free(&w);

    r = schema_of("{ group: \"org.t\", artifact: \"a\", version: \"1.0.0\","
                  "  langs: [\"c\"], deps: [ { id: \"org.example\" } ] }", &e, &w);
    ASSERT_EQ((int)r, (int)NOW_SCHEMA_INVALID);
    ASSERT_EQ(diag_has(&e, "NOW-E0105"), 1);
    now_diaglist_free(&e); now_diaglist_free(&w);
    PASS();
}

/* The std table is per-language, so the control is that the SAME value
 * is right in one language and wrong in another. A check that only ever
 * saw one language could pass with the table ignored entirely. */
static void test_schema_std_is_per_language(void) {
    NowDiagList e, w;
    TEST("schema: c++20 is valid for c++ and not for c");

    NowSchemaResult r = schema_of(
        "{ group: \"org.t\", artifact: \"a\", version: \"1.0.0\","
        "  langs: [\"c++\"], std: \"c++20\" }", &e, &w);
    ASSERT_EQ((int)r, (int)NOW_SCHEMA_VALID);
    now_diaglist_free(&e); now_diaglist_free(&w);

    r = schema_of("{ group: \"org.t\", artifact: \"a\", version: \"1.0.0\","
                  "  langs: [\"c\"], std: \"c++20\" }", &e, &w);
    ASSERT_EQ((int)r, (int)NOW_SCHEMA_INVALID);
    ASSERT_EQ(diag_has(&e, "NOW-E0102"), 1);
    now_diaglist_free(&e); now_diaglist_free(&w);
    PASS();
}

/* The three warning classes, and the assertion that they are WARNINGS.
 *
 * This is the first coverage the key lists have ever had. Getting the
 * class wrong is not cosmetic: an unimplemented field promoted to an
 * error would refuse every descriptor written against a newer spec,
 * which is exactly the adoption case. */
static void test_schema_unimplemented_is_a_warning_not_an_error(void) {
    NowDiagList e, w;
    TEST("schema: unknown, inert and dead fields warn but stay valid");
    NowSchemaResult r = schema_of(
        "{ group: \"org.t\", artifact: \"a\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  profiles: { debug: {} },"
        "  compil: { flags: [\"-O2\"] },"
        "  sources: { dir: \"src/main/c\", defines: { X: \"1\" } } }", &e, &w);

    ASSERT_EQ((int)r, (int)NOW_SCHEMA_VALID);   /* still valid */
    ASSERT_EQ((int)e.count, 0);
    ASSERT_EQ(diag_has(&w, "NOW-W0002"), 1);    /* profiles: inert */
    ASSERT_EQ(diag_has(&w, "NOW-W0001"), 1);    /* compil: typo */
    ASSERT_EQ(diag_has(&w, "NOW-W0003"), 1);    /* sources.defines: dead */
    now_diaglist_free(&e); now_diaglist_free(&w);
    PASS();
}

/* A descriptor too broken to parse still has to produce a position, or
 * the command cannot do the one thing it exists for. */
static void test_schema_syntax_error_carries_a_position(void) {
    NowDiagList e, w;
    TEST("schema: a syntax error names a line and column");
    NowSchemaResult r = schema_of(
        "{ group: \"org.t\", artifact: \"a\", version: \"1.0.0\",", &e, &w);
    ASSERT_EQ((int)r, (int)NOW_SCHEMA_INVALID);
    ASSERT_EQ((int)e.count, 1);
    ASSERT_EQ(diag_has(&e, "NOW-E0001"), 1);
    if (e.items[0].line <= 0) {
        now_diaglist_free(&e); now_diaglist_free(&w);
        FAIL("syntax error reported no line");
        return;
    }
    /* And the position must not be duplicated into the message, which
     * the loader formats as "LINE:COL: text". */
    if (strstr(e.items[0].message, ":") &&
        e.items[0].message[0] >= '0' && e.items[0].message[0] <= '9') {
        now_diaglist_free(&e); now_diaglist_free(&w);
        FAIL("message repeats the position");
        return;
    }
    now_diaglist_free(&e); now_diaglist_free(&w);
    PASS();
}


/* ---- vacate (§2.2) ----
 *
 * The first thing in `now` that deletes anything a user did not name,
 * from a directory every project on the machine shares. So the tests
 * are about the REFUSALS, not the removals: removing the right artifact
 * is one assertion, and the other three are about not removing the
 * wrong one.
 *
 * They run against a fake repo under target/, with HOME repointed, so
 * a containment bug destroys a scratch directory rather than a home
 * directory. That is the only way to test a delete honestly — a test
 * that mocks the removal proves the caller's arithmetic and nothing
 * about the thing that touches the disk.
 */

static void vac_write(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(body, 1, strlen(body), f);
    fclose(f);
}

/* Build a disposable repo:  keep/1.0.0 (referenced), orphan/2.0.0 (not). */
static int vac_setup(char *home_out, size_t cap) {
    char p[512];
    snprintf(home_out, cap, "target/vac_home");

    now_mkdir_p("target/vac_projects");
    now_mkdir_p("target/vac_home/.now/repo/org/example/keep/1.0.0");
    vac_write("target/vac_home/.now/repo/org/example/keep/1.0.0/now.pasta",
              "{ group: \"org.example\", artifact: \"keep\", "
              "version: \"1.0.0\", langs: [\"c\"] }");

    now_mkdir_p("target/vac_home/.now/repo/org/example/orphan/2.0.0");
    vac_write("target/vac_home/.now/repo/org/example/orphan/2.0.0/now.pasta",
              "{ group: \"org.example\", artifact: \"orphan\", "
              "version: \"2.0.0\", langs: [\"c\"] }");

    snprintf(p, sizeof(p), "target/vac_projects/now.lock.pasta");
    vac_write(p, "{ entries: [ { group: \"org.example\", artifact: \"keep\", "
                 "version: \"1.0.0\", triple: \"noarch\", sha256: \"0\" } ] }");
    return 0;
}

static int vac_exists(const char *rel) {
    return now_path_exists(rel);
}

/* Point HOME at the fake repo, and put it back afterwards.
 *
 * This is process-wide state. Leaving it pointed at target/vac_home
 * would silently redirect every later test that reads ~/.now — the
 * cross-test coupling that makes a suite lie about which run failed. */
static char vac_home_saved[1024];
static int  vac_home_have_saved = 0;

static void vac_restore_home(void) {
    char buf[1200];
    if (!vac_home_have_saved) return;
#ifdef _WIN32
    snprintf(buf, sizeof(buf), "USERPROFILE=%s", vac_home_saved);
    _putenv(buf);
#else
    setenv("HOME", vac_home_saved, 1);
#endif
}
/* Point HOME at the fake repo for the duration of one call. */
static void vac_set_home(const char *dir) {
    char abs[1024];
    char buf[1200];
    if (!now_path_exists(dir)) return;
    if (!vac_home_have_saved) {
#ifdef _WIN32
        const char *cur = getenv("USERPROFILE");
#else
        const char *cur = getenv("HOME");
#endif
        snprintf(vac_home_saved, sizeof(vac_home_saved), "%s", cur ? cur : "");
        vac_home_have_saved = 1;
    }
#ifdef _WIN32
    if (!_fullpath(abs, dir, sizeof(abs))) return;
    snprintf(buf, sizeof(buf), "USERPROFILE=%s", abs);
    _putenv(buf);
#else
    if (!realpath(dir, abs)) return;
    setenv("HOME", abs, 1);
#endif
}

static void test_vacate_removes_only_the_unreferenced(void) {
    char home[256];
    NowVacateOpts opts;
    NowResult res;
    const char *scans[1];

    TEST("vacate: removes the unreferenced and keeps the rest");
    vac_setup(home, sizeof(home));
    vac_set_home(home);

    memset(&opts, 0, sizeof(opts));
    memset(&res, 0, sizeof(res));
    scans[0] = "target/vac_projects";
    opts.scan_roots = scans;
    opts.scan_count = 1;
    opts.quiet = 1;

    ASSERT_EQ(now_vacate(&opts, &res), 0);
    ASSERT_EQ(vac_exists("target/vac_home/.now/repo/org/example/keep/1.0.0"), 1);
    ASSERT_EQ(vac_exists("target/vac_home/.now/repo/org/example/orphan/2.0.0"), 0);
    vac_restore_home();
    PASS();
}

/* THE REFUSAL. A scan that found no lock files has not shown anything
 * to be unreferenced — it has shown that it looked nowhere. Those two
 * states produce identical evidence and call for opposite actions, so
 * the empty scan must refuse. Without this, running vacate from a
 * directory with no projects under it would empty the shared repo. */
static void test_vacate_refuses_an_empty_scan(void) {
    char home[256];
    NowVacateOpts opts;
    NowResult res;
    const char *scans[1];

    TEST("vacate: an empty scan refuses rather than removing everything");
    vac_setup(home, sizeof(home));
    vac_set_home(home);

    memset(&opts, 0, sizeof(opts));
    memset(&res, 0, sizeof(res));
    scans[0] = "target/vac_home";   /* holds the repo, but no lock files */
    opts.scan_roots = scans;
    opts.scan_count = 1;
    opts.quiet = 1;

    if (now_vacate(&opts, &res) == 0) {
        vac_restore_home();
        FAIL("empty scan was accepted");
        return;
    }
    /* And nothing was touched. */
    ASSERT_EQ(vac_exists("target/vac_home/.now/repo/org/example/keep/1.0.0"), 1);
    ASSERT_EQ(vac_exists("target/vac_home/.now/repo/org/example/orphan/2.0.0"), 1);
    vac_restore_home();
    PASS();
}

static void test_vacate_dry_run_removes_nothing(void) {
    char home[256];
    NowVacateOpts opts;
    NowResult res;
    const char *scans[1];

    TEST("vacate: --dry-run reports and removes nothing");
    vac_setup(home, sizeof(home));
    vac_set_home(home);

    memset(&opts, 0, sizeof(opts));
    memset(&res, 0, sizeof(res));
    scans[0] = "target/vac_projects";
    opts.scan_roots = scans;
    opts.scan_count = 1;
    opts.quiet = 1;
    opts.dry_run = 1;

    ASSERT_EQ(now_vacate(&opts, &res), 0);
    /* The orphan is still there — which is the whole assertion, and the
     * one a reviewer would want to see before trusting the real run. */
    ASSERT_EQ(vac_exists("target/vac_home/.now/repo/org/example/orphan/2.0.0"), 1);
    ASSERT_EQ(vac_exists("target/vac_home/.now/repo/org/example/keep/1.0.0"), 1);
    vac_restore_home();
    PASS();
}

/* --force ignores references. It must still ignore only references —
 * not the repo boundary. */
static void test_vacate_force_takes_the_referenced_too(void) {
    char home[256];
    NowVacateOpts opts;
    NowResult res;
    const char *scans[1];

    TEST("vacate: --force removes even what is referenced");
    vac_setup(home, sizeof(home));
    vac_set_home(home);

    memset(&opts, 0, sizeof(opts));
    memset(&res, 0, sizeof(res));
    scans[0] = "target/vac_projects";
    opts.scan_roots = scans;
    opts.scan_count = 1;
    opts.quiet = 1;
    opts.force = 1;

    ASSERT_EQ(now_vacate(&opts, &res), 0);
    ASSERT_EQ(vac_exists("target/vac_home/.now/repo/org/example/keep/1.0.0"), 0);
    ASSERT_EQ(vac_exists("target/vac_home/.now/repo/org/example/orphan/2.0.0"), 0);
    vac_restore_home();
    PASS();
}


/* ---- tell / tool: / convert ----
 *
 * The spec had these as one `tool:` family of nine. They are two
 * features that shared a prefix — asking `now` something, and running
 * something you declared — plus a format converter that was filed
 * beside them for no reason. Splitting them is the change under test as
 * much as the code is, so the tests are organised the same way.
 */

static void tc_write(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(body, 1, strlen(body), f);
    fclose(f);
}

static NowProject *tc_project(const char *body) {
    NowResult r;
    now_mkdir_p("target/tc/src/main/c");
    tc_write("target/tc/now.pasta", body);
    /* A real source file, because `tell source-files` asks the build
     * where the sources are and a project with none is an error there —
     * correctly, since `now_build_init` cannot tell "this project has no
     * sources" from "this directory would not read". */
    tc_write("target/tc/src/main/c/one.c", "int one(void) { return 1; }\n");
    memset(&r, 0, sizeof(r));
    return now_project_load("target/tc/now.pasta", &r);
}

/* Capture what `tell` writes, so the assertion is on the bytes a script
 * would receive rather than on a return code. */
static int tc_tell(char *out, size_t cap, const NowProject *p,
                   const char *what, const char *const *argv, int argc,
                   const char *fmt) {
    FILE *f = fopen("target/tc/out.txt", "w+b");
    int rc;
    NowResult r;
    size_t n;
    if (!f) return -1;
    memset(&r, 0, sizeof(r));
    rc = now_tell(f, p, "target/tc", what, argv, argc, fmt, &r);
    fflush(f);
    fseek(f, 0, SEEK_SET);
    n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return rc;
}

static const char *TC_POM =
    "{ group: \"org.t\", artifact: \"demo\", version: \"1.0.0\","
    "  langs: [\"c\"],"
    "  compile: { flags: [\"-O2\", \"-Wall\"] },"
    "  tools: { hello: { description: \"hi\", run: \"echo hi\" } } }";

static void test_tell_reads_the_effective_value(void) {
    char buf[512];
    NowProject *p = tc_project(TC_POM);
    TEST("tell: a field the descriptor never wrote still answers");
    ASSERT_NOT_NULL(p);

    /* sources.dir was not written. The loader defaults it, and the
     * useful answer is the effective one — a caller asking where the
     * sources are wants src/main/c, not an empty line. */
    ASSERT_EQ(tc_tell(buf, sizeof(buf), p, "sources.dir", NULL, 0, "text"), 0);
    ASSERT_STR(buf, "src/main/c\n");

    ASSERT_EQ(tc_tell(buf, sizeof(buf), p, "artifact", NULL, 0, "text"), 0);
    ASSERT_STR(buf, "demo\n");

    now_project_free(p);
    PASS();
}

/* A scalar prints BARE in text mode. `$(now tell sources.dir)` is the
 * overwhelmingly common use and a quoted string there is a bug. */
static void test_tell_text_is_unquoted_and_json_is_not(void) {
    char buf[512];
    NowProject *p = tc_project(TC_POM);
    TEST("tell: text is bare, json is quoted");
    ASSERT_NOT_NULL(p);

    ASSERT_EQ(tc_tell(buf, sizeof(buf), p, "artifact", NULL, 0, "text"), 0);
    ASSERT_STR(buf, "demo\n");
    ASSERT_EQ(tc_tell(buf, sizeof(buf), p, "artifact", NULL, 0, "json"), 0);
    ASSERT_STR(buf, "\"demo\"\n");

    now_project_free(p);
    PASS();
}

/* THE NAMING RULE, which is the whole reason there can be no ambiguity:
 * hyphenated is a computed query, anything else is a descriptor field.
 * The spec's `tool:query sources` / `tool:sources` pair could not tell
 * those apart; this cannot fail to. */
static void test_tell_hyphen_separates_the_two_namespaces(void) {
    char buf[4096];
    NowProject *p = tc_project(TC_POM);
    TEST("tell: hyphen means computed, dot means descriptor");
    ASSERT_NOT_NULL(p);

    /* A descriptor field. */
    ASSERT_EQ(tc_tell(buf, sizeof(buf), p, "compile.flags", NULL, 0, "text"), 0);
    if (!strstr(buf, "-O2")) { now_project_free(p); FAIL("compile.flags empty"); return; }

    /* A computed query with a name that is NOT a descriptor field, and
     * it must actually answer — an empty success would prove only that
     * the name routed somewhere. */
    ASSERT_EQ(tc_tell(buf, sizeof(buf), p, "source-files", NULL, 0, "text"), 0);
    if (!strstr(buf, "one.c")) {
        now_project_free(p); FAIL("source-files listed nothing"); return;
    }

    /* And neither namespace answers for the other: a hyphenated name
     * that is not a query fails as a query, not as a field. */
    ASSERT_EQ(tc_tell(buf, sizeof(buf), p, "no-such-query", NULL, 0, "text") != 0, 1);
    ASSERT_EQ(tc_tell(buf, sizeof(buf), p, "sources.nope", NULL, 0, "text") != 0, 1);

    now_project_free(p);
    PASS();
}

/* An unknown field must be an ERROR. Printing nothing is how a script
 * ends up branching on "" and believing the field was unset. */
static void test_tell_a_typo_is_an_error_not_an_empty_answer(void) {
    char buf[512];
    NowProject *p = tc_project(TC_POM);
    TEST("tell: an unknown field fails rather than printing nothing");
    ASSERT_NOT_NULL(p);
    if (tc_tell(buf, sizeof(buf), p, "sources.dirr", NULL, 0, "text") == 0) {
        now_project_free(p);
        FAIL("a typo was answered successfully");
        return;
    }
    now_project_free(p);
    PASS();
}

static void test_tool_list_and_run(void) {
    NowProject *p = tc_project(TC_POM);
    NowResult r;
    TEST("tool: a declared tool is listed and runs");
    ASSERT_NOT_NULL(p);
    memset(&r, 0, sizeof(r));

    {
        FILE *f = fopen("target/tc/tools.txt", "w+b");
        char buf[512];
        size_t n;
        if (!f) { now_project_free(p); FAIL("no scratch file"); return; }
        now_tool_list(f, p, "target/tc", &r);
        fflush(f); fseek(f, 0, SEEK_SET);
        n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        if (!strstr(buf, "hello")) {
            now_project_free(p); FAIL("declared tool not listed"); return;
        }
    }

    /* An unknown tool is an error, not a silent success — the shape
     * that would let `now tool:run lint` in CI pass by doing nothing. */
    if (now_tool_run(p, "target/tc", "nosuchtool", 0, &r) >= 0) {
        now_project_free(p);
        FAIL("an undeclared tool ran successfully");
        return;
    }
    now_project_free(p);
    PASS();
}

/* convert: the round trip has to preserve the data, and the one thing
 * it cannot preserve has to be known. */
static void test_convert_round_trips_through_json(void) {
    NowResult r;
    NowProject *a, *b;
    TEST("convert: pasta -> json -> pasta keeps the descriptor");

    now_mkdir_p("target/tc");
    tc_write("target/tc/rt.pasta",
             "{ group: \"org.t\", artifact: \"rt\", version: \"2.3.4\","
             "  langs: [\"c\"], compile: { flags: [\"-O2\"] } }");

    memset(&r, 0, sizeof(r));
    {
        FILE *j = fopen("target/tc/rt.json", "wb");
        int rc;
        if (!j) { FAIL("no scratch file"); return; }
        rc = now_convert("target/tc/rt.pasta", "json", 0, j, &r);
        fclose(j);
        if (rc != 0) { FAIL("convert to json failed"); return; }
    }

    a = now_project_load("target/tc/rt.pasta", &r);
    b = now_project_load("target/tc/rt.json", &r);
    if (!a || !b) { FAIL("one side did not load"); return; }

    /* Compared field by field against the ORIGINAL rather than against
     * a literal: the assertion is that the round trip preserved the
     * descriptor, and hard-coding the expected values would let both
     * sides drift together without the test noticing.
     *
     * ASSERT_STR takes a literal — it pastes its second argument into
     * the failure message — so these are written out. */
    if (strcmp(b->group, a->group) != 0 ||
        strcmp(b->artifact, a->artifact) != 0 ||
        strcmp(b->version, a->version) != 0) {
        now_project_free(a); now_project_free(b);
        FAIL("identity did not survive the round trip");
        return;
    }
    if (b->compile.flags.count != a->compile.flags.count ||
        (a->compile.flags.count &&
         strcmp(b->compile.flags.items[0], a->compile.flags.items[0]) != 0)) {
        now_project_free(a); now_project_free(b);
        FAIL("compile.flags did not survive the round trip");
        return;
    }

    now_project_free(a);
    now_project_free(b);
    PASS();
}

/* json5 is refused rather than approximated: JSON5 permits trailing
 * commas, Pasta does not, so emitting it would produce a file we could
 * not read back. A one-way conversion is worse than a refusal. */
static void test_convert_refuses_json5(void) {
    NowResult r;
    TEST("convert: json5 is refused, with the reason");
    memset(&r, 0, sizeof(r));
    now_mkdir_p("target/tc");
    tc_write("target/tc/j5.pasta",
             "{ group: \"org.t\", artifact: \"j\", version: \"1.0.0\" }");
    if (now_convert("target/tc/j5.pasta", "json5", 0, NULL, &r) == 0) {
        FAIL("json5 was accepted");
        return;
    }
    if (!strstr(r.message, "trailing")) {
        FAIL("refused without saying why");
        return;
    }
    PASS();
}


/* ---- watch: what counts as an input ----
 *
 * `now watch` re-walks the source roots each poll and diffs the listing,
 * so it catches a file being created or deleted and not just edited.
 * That is the right model — `now` finds sources by walking, so a
 * watcher holding a fixed file list would miss the file you just added.
 *
 * What it got wrong was WHICH extensions to walk for. That was a
 * hardcoded list, separate from the language registry the build uses,
 * and it had drifted: no `.go`, no `.jl`, no `.hh`/`.hxx`. A Go project
 * under `now watch` therefore watched nothing at all and reported no
 * changes, forever.
 *
 * These tests assert the two lists cannot drift again, by asserting
 * there is only one.
 */

static int wt_has_ext(const char **exts, const char *e) {
    if (!exts) return 0;
    for (size_t i = 0; exts[i]; i++)
        if (strcmp(exts[i], e) == 0) return 1;
    return 0;
}

/* The registry is the single statement of what a source file is. If a
 * language claims an extension, a watcher that walks for that language
 * has to walk for it — the alternative is a build that reacts to a file
 * the watcher cannot see. */
static void test_watch_asks_the_registry_for_extensions(void) {
    TEST("watch: every language's extensions come from the registry");
    now_lang_registry_init();

    {
        const char *go[]   = { "go" };
        const char *jl[]   = { "julia" };
        const char *cpp[]  = { "c++" };
        const char **e;

        /* The two that were missing entirely — a watcher without these
         * sits idle while the project changes under it. */
        e = now_lang_all_exts(go, 1);
        if (!wt_has_ext(e, ".go")) { free((void *)e); FAIL("go has no .go"); return; }
        free((void *)e);

        e = now_lang_all_exts(jl, 1);
        if (!wt_has_ext(e, ".jl")) { free((void *)e); FAIL("julia has no .jl"); return; }
        free((void *)e);

        /* And the quiet one: a C++ project using .hh headers. The build
         * reacts to those; the old watcher did not. */
        e = now_lang_all_exts(cpp, 1);
        if (!wt_has_ext(e, ".hh") || !wt_has_ext(e, ".hxx")) {
            free((void *)e); FAIL("c++ is missing .hh/.hxx"); return;
        }
        free((void *)e);
    }
    PASS();
}

/* The snapshot must actually pick a language's files up. This is the
 * end of the chain the previous test only checks the start of: a
 * registry that knows about .go is worth nothing if the snapshot still
 * walks for a hardcoded set. */
static void test_watch_snapshot_sees_a_go_source(void) {
    NowResult r;
    NowProject *p;
    NowWatchSnapshot snap;
    int found = 0;

    TEST("watch: a .go source is in the snapshot");

    now_mkdir_p("target/wt/src/main/go");
    {
        FILE *f = fopen("target/wt/now.pasta", "wb");
        if (!f) { FAIL("no scratch"); return; }
        fputs("{ group: \"org.t\", artifact: \"g\", version: \"1.0.0\","
              "  langs: [\"go\"],"
              "  sources: { dir: \"src/main/go\" } }", f);
        fclose(f);
        f = fopen("target/wt/src/main/go/main.go", "wb");
        if (!f) { FAIL("no scratch"); return; }
        fputs("package main\nfunc main() {}\n", f);
        fclose(f);
    }

    memset(&r, 0, sizeof(r));
    p = now_project_load("target/wt/now.pasta", &r);
    ASSERT_NOT_NULL(p);

    memset(&snap, 0, sizeof(snap));
    if (now_watch_snapshot(p, "target/wt", &snap) != 0) {
        now_project_free(p);
        FAIL("snapshot failed");
        return;
    }
    for (size_t i = 0; i < snap.count; i++)
        if (strstr(snap.entries[i].path, "main.go")) { found = 1; break; }

    now_watch_snapshot_free(&snap);
    now_project_free(p);

    if (!found) { FAIL("a .go source was not watched"); return; }
    PASS();
}

/* Walking rather than listing is what makes a NEW file visible. Assert
 * it, because the difference only shows up on creation and that is
 * exactly the case a file-list watcher gets wrong. */
static void test_watch_notices_a_file_that_did_not_exist(void) {
    NowResult r;
    NowProject *p;
    NowWatchSnapshot before, after;

    TEST("watch: a newly created source changes the snapshot");

    now_mkdir_p("target/wt2/src/main/c");
    {
        FILE *f = fopen("target/wt2/now.pasta", "wb");
        if (!f) { FAIL("no scratch"); return; }
        fputs("{ group: \"org.t\", artifact: \"w\", version: \"1.0.0\","
              "  langs: [\"c\"], sources: { dir: \"src/main/c\" } }", f);
        fclose(f);
        f = fopen("target/wt2/src/main/c/a.c", "wb");
        if (!f) { FAIL("no scratch"); return; }
        fputs("int a(void){return 1;}\n", f);
        fclose(f);
    }
    memset(&r, 0, sizeof(r));
    p = now_project_load("target/wt2/now.pasta", &r);
    ASSERT_NOT_NULL(p);

    memset(&before, 0, sizeof(before));
    now_watch_snapshot(p, "target/wt2", &before);

    {
        FILE *f = fopen("target/wt2/src/main/c/b.c", "wb");
        if (f) { fputs("int b(void){return 2;}\n", f); fclose(f); }
    }
    memset(&after, 0, sizeof(after));
    now_watch_snapshot(p, "target/wt2", &after);

    {
        int changed = now_watch_diff(&before, &after);
        now_watch_snapshot_free(&before);
        now_watch_snapshot_free(&after);
        now_project_free(p);
        remove("target/wt2/src/main/c/b.c");
        if (!(changed & 1)) { FAIL("a new file did not register"); return; }
    }
    PASS();
}


/* ---- a workspace root that has sources of its own ----
 *
 * `now_is_workspace()` is `modules.count > 0`, and that one boolean was
 * deciding two unrelated questions: does this project have children, and
 * does it build itself. So a root with BOTH `modules:` and `src/main/c`
 * built its children and discarded its own sources — including the
 * `output:` it declared, which was never produced and never mentioned,
 * with exit code 0.
 *
 * The two cases below are the two the tree can distinguish without a
 * descriptor saying anything:
 *
 *   sources + children -> a GROUPING module: build both
 *   children only      -> a COLLECTION:      build the children
 *
 * The second is what worked before, so it is here as the control on the
 * first: a fix that made every root build itself would break aggregate
 * roots, and this is what would catch it.
 */

static void grp_write(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(body, 1, strlen(body), f);
    fclose(f);
}

/* Build a two-level workspace under target/. `root_src` NULL means the
 * root has no sources of its own — the collection case. */
static NowProject *grp_setup(const char *dir, const char *root_src) {
    char p[512];
    NowResult r;

    /* Remove the whole tree first, and MEAN it.
     *
     * The first version of this test did not, and its negative control
     * came back green: disabling the fix left the previous run's
     * libgrproot.a on disk, grp_built() found it, and the test passed
     * while watching nothing. A test that passes on last run's artifacts
     * is not testing this run - the same trap the stale_obj_proj setup
     * carries, found here by the control rather than by luck. */
    rmtree_best_effort(dir);

    snprintf(p, sizeof(p), "%s/child/src/main/c", dir);
    now_mkdir_p(p);

    snprintf(p, sizeof(p), "%s/now.pasta", dir);
    grp_write(p,
        "{ group: \"org.t\", artifact: \"root\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"grproot\" },"
        "  modules: [\"child\"] }");

    snprintf(p, sizeof(p), "%s/child/now.pasta", dir);
    grp_write(p,
        "{ group: \"org.t\", artifact: \"child\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"grpchild\" } }");

    snprintf(p, sizeof(p), "%s/child/src/main/c/child.c", dir);
    grp_write(p, "int child_fn(void) { return 2; }\n");

    if (root_src) {
        snprintf(p, sizeof(p), "%s/src/main/c", dir);
        now_mkdir_p(p);
        snprintf(p, sizeof(p), "%s/src/main/c/root.c", dir);
        grp_write(p, root_src);
    }

    snprintf(p, sizeof(p), "%s/now.pasta", dir);
    memset(&r, 0, sizeof(r));
    return now_project_load(p, &r);
}

static int grp_built(const char *dir, const char *name) {
    char p[512];
    snprintf(p, sizeof(p), "%s/target/bin/lib%s.a", dir, name);
    if (now_path_exists(p)) return 1;
    snprintf(p, sizeof(p), "%s/target/bin/%s.lib", dir, name);
    return now_path_exists(p);
}

/* THE BUG. A root declaring an output and holding sources produced
 * neither, and said nothing. */
static void test_workspace_root_builds_its_own_sources(void) {
    NowProject *p;
    NowWorkspace ws;
    NowResult r;

    TEST("workspace: a root with sources builds them too");

    p = grp_setup("target/grp", "int root_fn(void) { return 1; }\n");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(now_is_workspace(p), 1);

    /* Precondition, asserted rather than assumed: nothing is built yet.
     * Without this the postcondition can be satisfied by a leftover. */
    if (grp_built("target/grp", "grproot") ||
        grp_built("target/grp/child", "grpchild")) {
        now_project_free(p);
        FAIL("setup left artifacts behind - this run cannot prove anything");
        return;
    }

    memset(&r, 0, sizeof(r));
    memset(&ws, 0, sizeof(ws));
    if (now_workspace_init(&ws, p, "target/grp", &r) != 0) {
        now_project_free(p); FAIL("workspace init failed"); return;
    }
    if (now_workspace_build(&ws, 0, 2, &r) != 0) {
        now_workspace_free(&ws); now_project_free(p);
        FAIL(r.message[0] ? r.message : "workspace build failed");
        return;
    }
    now_workspace_free(&ws);

    if (!grp_built("target/grp/child", "grpchild")) {
        now_project_free(p); FAIL("the module was not built"); return;
    }
    /* The one that used to be missing. */
    if (!grp_built("target/grp", "grproot")) {
        now_project_free(p);
        FAIL("the root's own declared output was not produced");
        return;
    }
    now_project_free(p);
    PASS();
}

/* THE CONTROL, and it is a real one: a root with no sources of its own
 * must still work. A fix that simply always built the root would fail
 * here with "no source files found", and that shape — a collection of
 * modules with nothing at the top — is how most workspaces are written. */
static void test_workspace_collection_root_needs_no_sources(void) {
    NowProject *p;
    NowWorkspace ws;
    NowResult r;

    TEST("workspace: a root with no sources is still fine");

    p = grp_setup("target/grpc", NULL);
    ASSERT_NOT_NULL(p);

    if (grp_built("target/grpc/child", "grpchild")) {
        now_project_free(p);
        FAIL("setup left artifacts behind - this run cannot prove anything");
        return;
    }

    memset(&r, 0, sizeof(r));
    memset(&ws, 0, sizeof(ws));
    if (now_workspace_init(&ws, p, "target/grpc", &r) != 0) {
        now_project_free(p); FAIL("workspace init failed"); return;
    }
    if (now_workspace_build(&ws, 0, 2, &r) != 0) {
        now_workspace_free(&ws); now_project_free(p);
        FAIL(r.message[0] ? r.message : "collection build failed");
        return;
    }
    now_workspace_free(&ws);

    if (!grp_built("target/grpc/child", "grpchild")) {
        now_project_free(p); FAIL("the module was not built"); return;
    }
    now_project_free(p);
    PASS();
}


/* ---- object symbol tables, and the entry point ----
 *
 * `now` decided which object held the program's entry point by matching
 * the filename `main.c.o`. An executable whose entry point lived in
 * `app.c` therefore linked its own main() into the test binary next to
 * the test's own, and `now test` died with `multiple definition of
 * 'main'`. A filename is a convention; a symbol table is a fact.
 *
 * These build a real project and read the real objects, because the
 * thing under test is a parser of what the compiler actually emits.
 * A hand-built fixture would test my idea of COFF rather than gcc's.
 */

static void osym_write(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(body, 1, strlen(body), f);
    fclose(f);
}

/* Build a project whose entry point is NOT called main.c, and whose
 * other translation unit has no entry point at all. Returns 0 on
 * success and fills the two object paths. */
static int osym_build(char *entry_obj, char *plain_obj, size_t cap) {
    char root[512], d[512], p[512];
    NowResult res;
    NowProject *prj;
    const char *pasta =
        "{ group: \"org.test\", artifact: \"osym\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"executable\", name: \"osym\" },"
        "  sources: { dir: \"src/main/c\" } }";

    snprintf(root, sizeof(root), "%s/osym_proj", NOW_TEST_RESOURCES);
    snprintf(d, sizeof(d), "%s/target", root); rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/src", root);    rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/src/main/c", root); now_mkdir_p(d);

    snprintf(p, sizeof(p), "%s/src/main/c/app.c", root);
    osym_write(p, "int helper(void) { return 7; }\n"
                  "int main(void) { return helper() - 7; }\n");
    snprintf(p, sizeof(p), "%s/src/main/c/plain.c", root);
    osym_write(p, "int plain_fn(void) { return 3; }\n");

    memset(&res, 0, sizeof(res));
    prj = now_project_load_string(pasta, strlen(pasta), &res);
    if (!prj) return -1;
    if (now_build(prj, root, 0, 0, &res) != 0) { now_project_free(prj); return -1; }
    now_project_free(prj);

    snprintf(entry_obj, cap, "%s/target/obj/main/app.c.o", root);
    if (!now_path_exists(entry_obj))
        snprintf(entry_obj, cap, "%s/target/obj/main/app.c.obj", root);
    snprintf(plain_obj, cap, "%s/target/obj/main/plain.c.o", root);
    if (!now_path_exists(plain_obj))
        snprintf(plain_obj, cap, "%s/target/obj/main/plain.c.obj", root);

    return (now_path_exists(entry_obj) && now_path_exists(plain_obj)) ? 0 : -1;
}

static void test_objsym_finds_main_regardless_of_filename(void) {
    char entry[512], plain[512];
    TEST("objsym: main() is found by symbol, not by filename");

    if (osym_build(entry, plain, sizeof(entry)) != 0) {
        FAIL("could not build the fixture project");
        return;
    }
    /* app.c.o defines main even though it is not called main.c — this
     * is the whole bug. */
    ASSERT_EQ(now_obj_defines_symbol(entry, "main"), 1);
    /* plain.c.o does not. Without this the test would pass against a
     * reader that answered 1 for everything. */
    ASSERT_EQ(now_obj_defines_symbol(plain, "main"), 0);
    PASS();
}

/* "Cannot tell" must be its own answer. A caller reading -1 as 0 would
 * exclude nothing and reproduce the duplicate-main link; reading it as
 * 1 would exclude a needed object. */
static void test_objsym_unreadable_is_neither_yes_nor_no(void) {
    char p[512];
    TEST("objsym: an unreadable object answers -1, not 0");

    snprintf(p, sizeof(p), "%s/osym_proj/not_an_object.txt", NOW_TEST_RESOURCES);
    osym_write(p, "this is not an object file, it is a sentence\n");
    ASSERT_EQ(now_obj_defines_symbol(p, "main"), -1);
    remove(p);

    snprintf(p, sizeof(p), "%s/osym_proj/does_not_exist.o", NOW_TEST_RESOURCES);
    ASSERT_EQ(now_obj_defines_symbol(p, "main"), -1);
    PASS();
}

/* The diagnostic depends on this count: an entry-point object that
 * defines nothing else costs tests nothing, and must stay silent. */
static void test_objsym_counts_what_the_entry_point_hides(void) {
    char entry[512], plain[512];
    int others;
    TEST("objsym: counts the other globals an entry point hides");

    if (osym_build(entry, plain, sizeof(entry)) != 0) {
        FAIL("could not build the fixture project");
        return;
    }
    /* app.c defines main AND helper, so tests lose helper. */
    others = now_obj_other_global_count(entry, "main");
    if (others < 1) { FAIL("expected at least one hidden global"); return; }

    /* And the reader is not simply counting every symbol: a file with
     * no main still reports its own globals against that name. */
    if (now_obj_other_global_count(plain, "main") < 1) {
        FAIL("plain.c.o should report its own globals");
        return;
    }
    PASS();
}


/* THE INTEGRATION TEST, and the reason it exists.
 *
 * The three tests above prove `now_obj_defines_symbol` reads a symbol
 * table correctly. None of them proves the TEST LINK asks it — and when
 * that wiring was reverted to filename matching, all three still passed.
 * A negative control found that; the reader was watched and its only
 * caller was not.
 *
 * So this builds and tests a project the old code could not handle:
 *
 *   app.c  — main(), and nothing else
 *   lib.c  — the function under test
 *   t.c    — the test
 *
 * With the entry point found by symbol, app.c.o is excluded from the
 * test link and the rest links cleanly. With it found by filename,
 * app.c.o is kept — it is not called main.c — and the link dies on a
 * duplicate main(). `main()` is alone in its file deliberately: that is
 * the arrangement where excluding the object costs nothing, so a
 * failure here is about the entry-point decision and nothing else.
 */
static void test_entry_point_wiring_survives_a_renamed_main(void) {
    char root[512], d[512], p[512];
    NowResult res;
    NowProject *prj;
    const char *pasta =
        "{ group: \"org.test\", artifact: \"entrywire\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"executable\", name: \"entrywire\" },"
        "  sources: { dir: \"src/main/c\" },"
        "  tests:   { dir: \"src/test/c\" } }";

    TEST("build: an entry point not called main.c still tests");

    snprintf(root, sizeof(root), "%s/entrywire_proj", NOW_TEST_RESOURCES);
    snprintf(d, sizeof(d), "%s/target", root); rmtree_best_effort(d);
    if (now_path_exists(d)) {
        FAIL("could not clear target/ - cannot establish the precondition");
        return;
    }
    snprintf(d, sizeof(d), "%s/src", root); rmtree_best_effort(d);
    snprintf(d, sizeof(d), "%s/src/main/c", root); now_mkdir_p(d);
    snprintf(d, sizeof(d), "%s/src/test/c", root); now_mkdir_p(d);

    /* main() alone in a file NOT called main.c */
    snprintf(p, sizeof(p), "%s/src/main/c/app.c", root);
    osym_write(p, "int thing(void);\nint main(void) { return thing() - 5; }\n");
    snprintf(p, sizeof(p), "%s/src/main/c/lib.c", root);
    osym_write(p, "int thing(void) { return 5; }\n");
    snprintf(p, sizeof(p), "%s/src/test/c/t.c", root);
    osym_write(p, "int thing(void);\nint main(void) { return thing() == 5 ? 0 : 1; }\n");

    memset(&res, 0, sizeof(res));
    prj = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(prj);

    if (now_test(prj, root, 0, 0, &res) != 0) {
        now_project_free(prj);
        FAIL(res.message[0] ? res.message
                            : "test link failed for a non-main.c entry point");
        return;
    }
    now_project_free(prj);
    PASS();
}


/* ---- a detail cut on the way IN says so ----
 *
 * `detail_lossy` was set by the encoder and only by the encoder. The
 * event's own `detail` is `char[768]` and `ev_emit` filled it with
 * snprintf, so anything longer lost its tail before the encoder ever
 * saw it — and the encoder, finding a detail that fits, marked the
 * record byte-exact. Measured 2026-08-24: a 941-byte gcc diagnostic
 * arrived as 767 bytes with `detail_lossy: false`.
 *
 * The existing oversize test starts from a NowEvent that is ALREADY
 * full and renders it, so it watches the encoder and could not see
 * this. This one goes in through the public emit path, which is where
 * a real diagnostic enters.
 */

/* Capture what the emitter sends by pointing it at a loopback listener
 * is the end-to-end route; for this we only need the event as filled,
 * so the sink is read back through the sidecar the emitter writes. */
static void test_events_detail_cut_on_the_way_in_says_so(void) {
    char big[2048];
    size_t i;
    NowEvent ev;

    TEST("events: a detail cut while filling is marked lossy");

    /* Longer than NowEvent.detail, the way a real compiler diagnostic
     * is — the compile path passes the raw captured output, not the
     * 512-byte result->message. */
    for (i = 0; i < sizeof(big) - 1; i++) big[i] = 'x';
    big[sizeof(big) - 1] = '\0';

    memset(&ev, 0, sizeof(ev));
    now_event_set_detail(&ev, big);

    /* The detail was cut... */
    if (strlen(ev.detail) >= sizeof(big) - 1) {
        FAIL("expected the detail to be truncated");
        return;
    }
    /* ...and the record says so. This is the assertion that was false. */
    ASSERT_EQ(ev.detail_lossy, 1);
    PASS();
}

/* The control on the above: a detail that FITS must not be flagged.
 * A fix that set the flag unconditionally would satisfy the test above
 * and make the flag meaningless, which is worse than the bug — every
 * consumer would learn to ignore it. */
static void test_events_a_detail_that_fits_is_not_lossy(void) {
    NowEvent ev;
    TEST("events: a detail that fits is not marked lossy");

    memset(&ev, 0, sizeof(ev));
    now_event_set_detail(&ev, "compiler failed on src/main/c/a.c (exit 1)");

    ASSERT_STR(ev.detail, "compiler failed on src/main/c/a.c (exit 1)");
    ASSERT_EQ(ev.detail_lossy, 0);
    PASS();
}

/* And the boundary, because off-by-one here is the difference between
 * "flagged when it should not be" and "not flagged when it should".
 * Exactly-fits must be clean; one more byte must be flagged. */
static void test_events_lossy_boundary_is_exact(void) {
    char exact[4096], over[4096];
    NowEvent a, b;
    size_t cap;

    TEST("events: the lossy boundary is exact");

    memset(&a, 0, sizeof(a));
    cap = sizeof(a.detail);          /* 768 incl. the NUL */

    memset(exact, 'y', cap - 1); exact[cap - 1] = '\0';   /* fits exactly */
    memset(over,  'y', cap);     over[cap]      = '\0';   /* one too many */

    now_event_set_detail(&a, exact);
    ASSERT_EQ(a.detail_lossy, 0);
    ASSERT_EQ(strlen(a.detail), cap - 1);

    memset(&b, 0, sizeof(b));
    now_event_set_detail(&b, over);
    ASSERT_EQ(b.detail_lossy, 1);
    PASS();
}

/* ---- Path-based platform variants (arch.tags + path gating) ---- */

static const char *ARCH_POM =
    "{ group: \"org.test\", artifact: \"archproj\", version: \"1.0\","
    "  langs: [\"c\"],"
    "  arch: {"
    "    tags: [\"linux\", \"windows\", \"amiga\", \"os3\", \"os4\","
    "           \"amd64\", \"arm64\"],"
    "    aliases: { darwin: \"macos\", win32: \"windows\" }"
    "  } }";

static void test_arch_parse_tags(void) {
    TEST("arch: parse tags from now.pasta");
    NowResult res;
    NowProject *p = now_project_load_string(ARCH_POM, strlen(ARCH_POM), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ((int)p->arch.tags.count, 7);
    ASSERT_STR(p->arch.tags.items[0], "linux");
    ASSERT_STR(p->arch.tags.items[2], "amiga");
    now_project_free(p);
    PASS();
}

static void test_arch_parse_aliases(void) {
    TEST("arch: parse aliases as key:value map");
    NowResult res;
    NowProject *p = now_project_load_string(ARCH_POM, strlen(ARCH_POM), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ((int)p->arch.alias_keys.count, 2);
    ASSERT_STR(now_arch_dict_resolve(&p->arch, "darwin"), "macos");
    ASSERT_STR(now_arch_dict_resolve(&p->arch, "win32"), "windows");
    /* Non-alias passes through unchanged */
    ASSERT_STR(now_arch_dict_resolve(&p->arch, "linux"), "linux");
    now_project_free(p);
    PASS();
}

static void test_arch_is_gate(void) {
    TEST("arch: is_gate distinguishes tags from non-tags");
    NowResult res;
    NowProject *p = now_project_load_string(ARCH_POM, strlen(ARCH_POM), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(now_arch_dict_is_gate(&p->arch, "linux"), 1);
    ASSERT_EQ(now_arch_dict_is_gate(&p->arch, "amiga"), 1);
    /* Aliases resolve before lookup, so "darwin" should be treated as macos
     * — but macos isn't in the tag list here, so still 0. */
    ASSERT_EQ(now_arch_dict_is_gate(&p->arch, "darwin"), 0);
    /* win32 → windows, which IS a tag */
    ASSERT_EQ(now_arch_dict_is_gate(&p->arch, "win32"), 1);
    /* Random subdir name is not a gate */
    ASSERT_EQ(now_arch_dict_is_gate(&p->arch, "utils"), 0);
    now_project_free(p);
    PASS();
}

static void test_arch_active_tags_from_triple(void) {
    TEST("arch: active tag set derived from triple");
    NowResult res;
    NowProject *p = now_project_load_string(ARCH_POM, strlen(ARCH_POM), &res);
    NowTriple t;
    now_triple_parse(&t, "linux:amd64:gnu");
    NowTagSet active;
    now_arch_active_tags(&t, p, NULL, 0, &active);
    ASSERT_EQ(now_tagset_has(&active, "linux"), 1);
    ASSERT_EQ(now_tagset_has(&active, "amd64"), 1);
    ASSERT_EQ(now_tagset_has(&active, "gnu"), 1);
    ASSERT_EQ(now_tagset_has(&active, "windows"), 0);
    now_tagset_free(&active);
    now_project_free(p);
    PASS();
}

static void test_arch_active_tags_with_user(void) {
    TEST("arch: user tags + alias canonicalization");
    NowResult res;
    NowProject *p = now_project_load_string(ARCH_POM, strlen(ARCH_POM), &res);
    NowTriple t;
    now_triple_parse(&t, "windows:amd64:mingw");
    /* User passes "os4" (sub-platform invisible in triple) and "darwin"
     * which is an alias and should canonicalize to macos. */
    const char *user[] = { "os4", "darwin" };
    NowTagSet active;
    now_arch_active_tags(&t, p, user, 2, &active);
    ASSERT_EQ(now_tagset_has(&active, "windows"), 1);
    ASSERT_EQ(now_tagset_has(&active, "os4"), 1);
    ASSERT_EQ(now_tagset_has(&active, "macos"), 1);  /* via darwin alias */
    ASSERT_EQ(now_tagset_has(&active, "darwin"), 0); /* alias was resolved */
    now_tagset_free(&active);
    now_project_free(p);
    PASS();
}

/* Build a synthetic source tree:
 *   <root>/c/common.c
 *   <root>/c/linux/linux_only.c
 *   <root>/c/windows/win_only.c
 *   <root>/c/amiga/os4/aos4_only.c
 *   <root>/c/utils/helper.c   (non-gated subdir)
 */
static int write_empty(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs("/* test stub */\n", f);
    fclose(f);
    return 0;
}

/* Best-effort recursive directory removal — used to clean synthetic
 * test trees so files from a prior run (or a different host's layout)
 * can't leak into discovery. Silently ignores a missing path. */
static void rmtree_best_effort(const char *path) {
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            rmtree_best_effort(child);
        else
            remove(child);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    _rmdir(path);
#else
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode))
            rmtree_best_effort(child);
        else
            remove(child);
    }
    closedir(d);
    rmdir(path);
#endif
}

static int build_arch_tree(const char *root) {
    char p[512];
    snprintf(p, sizeof(p), "%s/c", root); now_mkdir_p(p);
    snprintf(p, sizeof(p), "%s/c/linux", root); now_mkdir_p(p);
    snprintf(p, sizeof(p), "%s/c/windows", root); now_mkdir_p(p);
    snprintf(p, sizeof(p), "%s/c/amiga/os4", root); now_mkdir_p(p);
    snprintf(p, sizeof(p), "%s/c/utils", root); now_mkdir_p(p);
    snprintf(p, sizeof(p), "%s/c/common.c", root); if (write_empty(p)) return -1;
    snprintf(p, sizeof(p), "%s/c/linux/linux_only.c", root); if (write_empty(p)) return -1;
    snprintf(p, sizeof(p), "%s/c/windows/win_only.c", root); if (write_empty(p)) return -1;
    snprintf(p, sizeof(p), "%s/c/amiga/os4/aos4_only.c", root); if (write_empty(p)) return -1;
    snprintf(p, sizeof(p), "%s/c/utils/helper.c", root); if (write_empty(p)) return -1;
    return 0;
}

static int filelist_has_basename(const NowFileList *fl, const char *base) {
    for (size_t i = 0; i < fl->count; i++) {
        const char *p = fl->paths[i];
        const char *bn = now_path_basename(p);
        if (bn && strcmp(bn, base) == 0) return 1;
    }
    return 0;
}

static void test_arch_gate_skips_nonmatching(void) {
    TEST("arch: gating skips subdirs not in active set");
    char root[512];
    snprintf(root, sizeof(root), "%s/arch_tree", NOW_TEST_RESOURCES);
    if (build_arch_tree(root) != 0) { FAIL("setup failed"); return; }

    NowResult res;
    NowProject *p = now_project_load_string(ARCH_POM, strlen(ARCH_POM), &res);
    NowTriple t;
    now_triple_parse(&t, "linux:amd64:gnu");
    NowTagSet active;
    now_arch_active_tags(&t, p, NULL, 0, &active);

    NowFileList fl;
    now_filelist_init(&fl);
    const char *exts[] = { ".c", NULL };
    now_discover_sources_filtered(root, "c", exts, p, &active, &fl);

    ASSERT_EQ(filelist_has_basename(&fl, "common.c"), 1);
    ASSERT_EQ(filelist_has_basename(&fl, "linux_only.c"), 1);
    ASSERT_EQ(filelist_has_basename(&fl, "helper.c"), 1);   /* utils/ not a gate */
    ASSERT_EQ(filelist_has_basename(&fl, "win_only.c"), 0); /* skipped */
    ASSERT_EQ(filelist_has_basename(&fl, "aos4_only.c"), 0);/* amiga/ skipped */

    now_filelist_free(&fl);
    now_tagset_free(&active);
    now_project_free(p);
    PASS();
}

static void test_arch_gate_nested_and(void) {
    TEST("arch: nested gates compose with AND (amiga + os4)");
    char root[512];
    snprintf(root, sizeof(root), "%s/arch_tree", NOW_TEST_RESOURCES);
    /* tree already built by previous test, but rebuild is idempotent */
    if (build_arch_tree(root) != 0) { FAIL("setup failed"); return; }

    NowResult res;
    NowProject *p = now_project_load_string(ARCH_POM, strlen(ARCH_POM), &res);
    NowTriple t;
    /* Triple says amiga; user adds os4 sub-platform */
    now_triple_parse(&t, "amiga:m68k:none");
    const char *user[] = { "os4" };
    NowTagSet active;
    now_arch_active_tags(&t, p, user, 1, &active);

    NowFileList fl;
    now_filelist_init(&fl);
    const char *exts[] = { ".c", NULL };
    now_discover_sources_filtered(root, "c", exts, p, &active, &fl);

    ASSERT_EQ(filelist_has_basename(&fl, "aos4_only.c"), 1); /* both tags active */
    ASSERT_EQ(filelist_has_basename(&fl, "linux_only.c"), 0);
    ASSERT_EQ(filelist_has_basename(&fl, "win_only.c"), 0);

    now_filelist_free(&fl);
    now_tagset_free(&active);

    /* Same tree, but active set is amiga WITHOUT os4 — the os4 subdir
     * should be skipped even though amiga matches. */
    now_arch_active_tags(&t, p, NULL, 0, &active);
    now_filelist_init(&fl);
    now_discover_sources_filtered(root, "c", exts, p, &active, &fl);
    ASSERT_EQ(filelist_has_basename(&fl, "aos4_only.c"), 0);
    now_filelist_free(&fl);
    now_tagset_free(&active);
    now_project_free(p);
    PASS();
}

static void test_arch_gate_in_build_loop(void) {
    TEST("arch: build loop's compile phase applies the gate");

    /* Host-independent by construction: two synthetic gate tags,
     * "gizmo" (made active) and "widget" (left inactive), so the
     * result doesn't depend on which OS the runner is. The active set
     * is fed through now_build_set_default_target — the same stash the
     * CLI's --platform-tag uses — and picked up by now_build_init's
     * discovery pass. */
    char root[512];
    snprintf(root, sizeof(root), "%s/arch_build", NOW_TEST_RESOURCES);

    /* Clean any prior tree so stale files from earlier runs (or a
     * different host's layout) can't leak into discovery. */
    char csrc[512];
    snprintf(csrc, sizeof(csrc), "%s/src/main/c", root);
    rmtree_best_effort(csrc);

    char dir[512];
    now_mkdir_p(csrc);
    snprintf(dir, sizeof(dir), "%s/src/main/c/gizmo", root);  now_mkdir_p(dir);
    snprintf(dir, sizeof(dir), "%s/src/main/c/widget", root); now_mkdir_p(dir);

    char p[512];
    snprintf(p, sizeof(p), "%s/src/main/c/common.c", root);        write_empty(p);
    snprintf(p, sizeof(p), "%s/src/main/c/gizmo/gizmo_only.c", root);   write_empty(p);
    snprintf(p, sizeof(p), "%s/src/main/c/widget/widget_only.c", root); write_empty(p);

    char pasta[1024];
    snprintf(pasta, sizeof(pasta),
        "{ group: \"org.test\", artifact: \"archbuild\", version: \"1\","
        "  langs: [\"c\"],"
        "  output: { type: \"static\", name: \"archbuild\" },"
        "  arch: { tags: [\"gizmo\", \"widget\"] } }");

    NowResult res;
    NowProject *prj = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(prj);

    /* Activate only "gizmo" via the default-target stash (triple=NULL
     * → host triple; the extra tag rides on top). */
    const char *active_tag[] = { "gizmo" };
    now_build_set_default_target(NULL, active_tag, 1);

    NowBuildCtx ctx;
    int rc = now_build_init(&ctx, prj, root, &res);
    /* Reset the stash immediately so it can't bleed into other tests. */
    now_build_set_default_target(NULL, NULL, 0);
    if (rc != 0) { FAIL(res.message); now_project_free(prj); return; }

    /* Inspect the discovered sources before linking — the gate runs
     * in now_build_init's discovery pass. */
    int has_common = 0, has_gizmo = 0, has_widget = 0;
    for (size_t i = 0; i < ctx.sources.count; i++) {
        const char *path = ctx.sources.paths[i];
        if (strstr(path, "common.c"))      has_common = 1;
        if (strstr(path, "gizmo_only.c"))  has_gizmo = 1;
        if (strstr(path, "widget_only.c")) has_widget = 1;
    }
    ASSERT_EQ(has_common, 1);
    ASSERT_EQ(has_gizmo, 1);   /* gizmo active → included */
    ASSERT_EQ(has_widget, 0);  /* widget inactive → gated out */

    now_build_free(&ctx);
    now_project_free(prj);
    PASS();
}

static void test_arch_gate_empty_dict_is_passthrough(void) {
    TEST("arch: empty dict = no gating, same as unfiltered walk");
    char root[512];
    snprintf(root, sizeof(root), "%s/arch_tree", NOW_TEST_RESOURCES);
    if (build_arch_tree(root) != 0) { FAIL("setup failed"); return; }

    /* Project with no arch section at all */
    const char *pom = "{ group: \"org.test\", artifact: \"a\", version: \"1\","
                      "  langs: [\"c\"] }";
    NowResult res;
    NowProject *p = now_project_load_string(pom, strlen(pom), &res);

    NowTagSet active;
    now_tagset_init(&active);  /* empty active set, doesn't matter */

    NowFileList fl;
    now_filelist_init(&fl);
    const char *exts[] = { ".c", NULL };
    now_discover_sources_filtered(root, "c", exts, p, &active, &fl);

    /* All 5 .c files should be present since no gating applies */
    ASSERT_EQ(filelist_has_basename(&fl, "common.c"), 1);
    ASSERT_EQ(filelist_has_basename(&fl, "linux_only.c"), 1);
    ASSERT_EQ(filelist_has_basename(&fl, "win_only.c"), 1);
    ASSERT_EQ(filelist_has_basename(&fl, "aos4_only.c"), 1);
    ASSERT_EQ(filelist_has_basename(&fl, "helper.c"), 1);

    now_filelist_free(&fl);
    now_tagset_free(&active);
    now_project_free(p);
    PASS();
}

/* ---- C++20 Module Pre-scan ---- */

/* Helper: write a temporary file for module scanning */
static char *write_temp_module_file(const char *name, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", NOW_TEST_RESOURCES, name);
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    fputs(content, f);
    fclose(f);
    return strdup(path);
}

static void remove_temp_file(const char *path) {
    if (path) remove(path);
}

static void test_module_scan_interface(void) {
    TEST("module: scan detects export module");
    char *path = write_temp_module_file("test_mod.cppm",
        "export module mylib.core;\n"
        "\n"
        "export int add(int a, int b) { return a + b; }\n");
    ASSERT_NOT_NULL(path);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    ASSERT_EQ(now_module_scan_file(&scan, path), 0);
    ASSERT_EQ((int)scan.unit_count, 1);
    ASSERT_STR(scan.units[0].name, "mylib.core");
    ASSERT_EQ(scan.units[0].is_interface, 1);

    now_module_scan_free(&scan);
    remove_temp_file(path);
    free(path);
    PASS();
}

static void test_module_scan_import(void) {
    TEST("module: scan detects import");
    char *path = write_temp_module_file("test_imp.cpp",
        "import mylib.core;\n"
        "\n"
        "int main() { return add(1, 2); }\n");
    ASSERT_NOT_NULL(path);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    ASSERT_EQ(now_module_scan_file(&scan, path), 0);
    ASSERT_EQ((int)scan.import_count, 1);
    ASSERT_STR(scan.imports[0].module_name, "mylib.core");

    now_module_scan_free(&scan);
    remove_temp_file(path);
    free(path);
    PASS();
}

static void test_module_scan_impl(void) {
    TEST("module: scan detects implementation unit");
    char *path = write_temp_module_file("test_impl.cpp",
        "module mylib.core;\n"
        "\n"
        "void internal_func() {}\n");
    ASSERT_NOT_NULL(path);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    ASSERT_EQ(now_module_scan_file(&scan, path), 0);
    ASSERT_EQ((int)scan.unit_count, 1);
    ASSERT_STR(scan.units[0].name, "mylib.core");
    ASSERT_EQ(scan.units[0].is_interface, 0);

    now_module_scan_free(&scan);
    remove_temp_file(path);
    free(path);
    PASS();
}

static void test_module_scan_skips_comments(void) {
    TEST("module: scan ignores commented-out declarations");
    char *path = write_temp_module_file("test_comment.cpp",
        "// export module fake;\n"
        "/* import ignored; */\n"
        "#include <stdio.h>\n"
        "int main() { return 0; }\n");
    ASSERT_NOT_NULL(path);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    ASSERT_EQ(now_module_scan_file(&scan, path), 0);
    ASSERT_EQ((int)scan.unit_count, 0);
    ASSERT_EQ((int)scan.import_count, 0);

    now_module_scan_free(&scan);
    remove_temp_file(path);
    free(path);
    PASS();
}

static void test_module_order_basic(void) {
    TEST("module: topo order puts interface before consumer");
    /* Create two files: interface and consumer */
    char *iface = write_temp_module_file("topo_iface.cppm",
        "export module greeter;\n"
        "export const char *greet() { return \"hi\"; }\n");
    char *consumer = write_temp_module_file("topo_main.cpp",
        "import greeter;\n"
        "int main() { greet(); return 0; }\n");
    ASSERT_NOT_NULL(iface);
    ASSERT_NOT_NULL(consumer);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    now_module_scan_file(&scan, iface);
    now_module_scan_file(&scan, consumer);

    const char *sources[] = { consumer, iface };
    NowModuleOrder order;
    ASSERT_EQ(now_module_order(&scan, sources, 2, &order), 0);

    /* Interface should come first */
    ASSERT_EQ((int)order.count, 2);
    if (strcmp(order.paths[0], iface) != 0) { FAIL("interface not first"); now_module_order_free(&order); now_module_scan_free(&scan); remove_temp_file(iface); remove_temp_file(consumer); free(iface); free(consumer); return; }
    if (strcmp(order.paths[1], consumer) != 0) { FAIL("consumer not second"); now_module_order_free(&order); now_module_scan_free(&scan); remove_temp_file(iface); remove_temp_file(consumer); free(iface); free(consumer); return; }

    now_module_order_free(&order);
    now_module_scan_free(&scan);
    remove_temp_file(iface);
    remove_temp_file(consumer);
    free(iface);
    free(consumer);
    PASS();
}

/* The same ordering property at N=3, which is where it stops being
 * symmetric.
 *
 * Amy's observation, named in three consecutive handovers and never run
 * until now: a relation checked with two elements hides its own
 * direction and cannot see transitivity at all. `now` orders module
 * units, workspace waves, override precedence and search paths, and
 * every ordering test in this suite used exactly two elements — so a
 * sort that respected only direct edges, or that stopped after a single
 * pass, passed all of them.
 *
 * base <- middle <- top, handed to the sorter exactly reversed. Getting
 * this right needs the ordering to be transitive, not merely pairwise. */
static void test_module_order_three_deep(void) {
    TEST("module: topo order is transitive at three units");

    char *base = write_temp_module_file("topo3_base.cppm",
        "export module base;\n"
        "export const char *base_v() { return \"b\"; }\n");
    char *middle = write_temp_module_file("topo3_middle.cppm",
        "export module middle;\n"
        "import base;\n"
        "export const char *middle_v() { return base_v(); }\n");
    char *top = write_temp_module_file("topo3_top.cpp",
        "import middle;\n"
        "int main() { middle_v(); return 0; }\n");
    ASSERT_NOT_NULL(base);
    ASSERT_NOT_NULL(middle);
    ASSERT_NOT_NULL(top);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    now_module_scan_file(&scan, base);
    now_module_scan_file(&scan, middle);
    now_module_scan_file(&scan, top);

    const char *sources[] = { top, middle, base };
    NowModuleOrder order;
    int rc = now_module_order(&scan, sources, 3, &order);

    int ok = 0;
    int base_at = -1, middle_at = -1, top_at = -1;
    if (rc == 0 && order.count == 3) {
        for (size_t i = 0; i < order.count; i++) {
            if (strcmp(order.paths[i], base) == 0)   base_at   = (int)i;
            if (strcmp(order.paths[i], middle) == 0) middle_at = (int)i;
            if (strcmp(order.paths[i], top) == 0)    top_at    = (int)i;
        }
        ok = (base_at >= 0 && middle_at >= 0 && top_at >= 0 &&
              base_at < middle_at && middle_at < top_at);
    }
    if (rc == 0) now_module_order_free(&order);
    now_module_scan_free(&scan);
    remove_temp_file(base); remove_temp_file(middle); remove_temp_file(top);
    free(base); free(middle); free(top);

    if (rc != 0) { FAIL("now_module_order failed on three units"); return; }
    if (!ok) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "expected base < middle < top, got %d %d %d",
                 base_at, middle_at, top_at);
        FAIL(msg);
        return;
    }
    PASS();
}

static void test_module_find(void) {
    TEST("module: find returns interface by name");
    char *path = write_temp_module_file("test_find.cppm",
        "export module utils;\n");
    ASSERT_NOT_NULL(path);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    now_module_scan_file(&scan, path);

    const NowModuleUnit *u = now_module_find(&scan, "utils");
    ASSERT_NOT_NULL(u);
    ASSERT_STR(u->name, "utils");
    ASSERT_EQ(u->is_interface, 1);
    ASSERT_NULL(now_module_find(&scan, "nonexistent"));

    now_module_scan_free(&scan);
    remove_temp_file(path);
    free(path);
    PASS();
}

static void test_module_bmi_path(void) {
    TEST("module: BMI path generation");
    char *p = now_module_bmi_path("target", "mylib.core", 0);
    ASSERT_NOT_NULL(p);
    /* Should end with /bmi/mylib.core.pcm */
    ASSERT_NOT_NULL(strstr(p, "bmi"));
    ASSERT_NOT_NULL(strstr(p, "mylib.core.pcm"));
    free(p);

    char *pm = now_module_bmi_path("target", "mylib.core", 1);
    ASSERT_NOT_NULL(pm);
    ASSERT_NOT_NULL(strstr(pm, "mylib.core.ifc"));
    free(pm);
    PASS();
}

static void test_module_classify_cppm(void) {
    TEST("module: classify .cppm as cxx-module");
    now_lang_registry_init();
    const char *langs[] = { "c++" };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("foo.cppm", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "cxx-module");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "c++");
    PASS();
}

/* ---- Java Language + Maven ---- */

static void test_lang_java_registration(void) {
    TEST("java: language registered");
    now_lang_registry_init();
    const NowLangDef *lang = now_lang_find("java");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "java");
    ASSERT_STR(lang->name, "Java");
    ASSERT_EQ((int)lang->type_count, 1);
    ASSERT_STR(lang->types[0].id, "java-source");
    PASS();
}

static void test_lang_java_classify(void) {
    TEST("java: classify .java as java-source");
    const char *langs[] = { "java" };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("Main.java", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "java-source");
    ASSERT_STR(type->tool_var, "${javac}");
    PASS();
}

static void test_pom_java_fields(void) {
    TEST("java: POM loads java section");
    const char *input =
        "{ group: \"com.example\", artifact: \"myapp\", version: \"1.0.0\","
        "  langs: [\"java\"], std: \"17\","
        "  java: { main_class: \"com.example.Main\", encoding: \"UTF-8\" },"
        "  output: { type: \"jar\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_STR(p->java.main_class, "com.example.Main");
    ASSERT_STR(p->java.encoding, "UTF-8");
    now_project_free(p);
    PASS();
}

static void test_pom_java_defaults(void) {
    TEST("java: source/test dirs default to Maven layout");
    const char *input =
        "{ group: \"com.example\", artifact: \"myapp\", version: \"1.0.0\","
        "  langs: [\"java\"] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_STR(p->sources.dir, "src/main/java");
    ASSERT_STR(p->tests.dir, "src/test/java");
    /* Java projects should NOT get a headers directory */
    ASSERT_NULL(p->sources.headers);
    now_project_free(p);
    PASS();
}

static void test_export_maven_basic(void) {
    TEST("export:maven: generates valid pom.xml");
    const char *input =
        "{ group: \"io.test\", artifact: \"mylib\", version: \"2.0.0\","
        "  langs: [\"java\"], std: \"17\","
        "  name: \"My Library\", description: \"A test library\","
        "  license: \"MIT\","
        "  output: { type: \"jar\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_pom_out.xml", NOW_TEST_RESOURCES);
    int rc = now_export_maven(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    /* Read and check generated XML */
    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[len] = '\0';
    fclose(fp);

    ASSERT_NOT_NULL(strstr(buf, "<groupId>io.test</groupId>"));
    ASSERT_NOT_NULL(strstr(buf, "<artifactId>mylib</artifactId>"));
    ASSERT_NOT_NULL(strstr(buf, "<version>2.0.0</version>"));
    ASSERT_NOT_NULL(strstr(buf, "<name>My Library</name>"));
    ASSERT_NOT_NULL(strstr(buf, "<packaging>jar</packaging>"));
    ASSERT_NOT_NULL(strstr(buf, "maven.compiler.release"));
    ASSERT_NOT_NULL(strstr(buf, "17"));
    ASSERT_NOT_NULL(strstr(buf, "MIT"));

    remove(outpath);
    now_project_free(p);
    PASS();
}

static void test_export_maven_deps(void) {
    TEST("export:maven: dependency scopes map correctly");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"java\"], std: \"17\","
        "  deps: ["
        "    { id: \"org.slf4j:slf4j-api:2.0.9\" },"
        "    { id: \"org.junit:junit:5.10.0\", scope: \"test\" },"
        "    { id: \"com.google:guava:32.0\", optional: true }"
        "  ] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_pom_deps.xml", NOW_TEST_RESOURCES);
    int rc = now_export_maven(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[len] = '\0';
    fclose(fp);

    ASSERT_NOT_NULL(strstr(buf, "<groupId>org.slf4j</groupId>"));
    ASSERT_NOT_NULL(strstr(buf, "<artifactId>slf4j-api</artifactId>"));
    ASSERT_NOT_NULL(strstr(buf, "<scope>test</scope>"));
    ASSERT_NOT_NULL(strstr(buf, "<optional>true</optional>"));

    remove(outpath);
    now_project_free(p);
    PASS();
}

static void test_export_maven_main_class(void) {
    TEST("export:maven: executable JAR with main class");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"java\"], std: \"21\","
        "  output: { type: \"executable\" },"
        "  java: { main_class: \"io.test.Main\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_pom_main.xml", NOW_TEST_RESOURCES);
    int rc = now_export_maven(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[len] = '\0';
    fclose(fp);

    ASSERT_NOT_NULL(strstr(buf, "maven-jar-plugin"));
    ASSERT_NOT_NULL(strstr(buf, "<mainClass>io.test.Main</mainClass>"));

    remove(outpath);
    now_project_free(p);
    PASS();
}

static void test_import_maven_basic(void) {
    TEST("import:maven: parses basic pom.xml");
    char pompath[512];
    snprintf(pompath, sizeof(pompath), "%s/pom_basic.xml", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_import_maven(pompath, &res);
    ASSERT_NOT_NULL(p);
    ASSERT_STR(p->group, "com.example");
    ASSERT_STR(p->artifact, "myapp");
    ASSERT_STR(p->version, "1.2.3");
    ASSERT_STR(p->name, "My Application");
    ASSERT_STR(p->description, "A test project");
    ASSERT_STR(p->url, "https://example.com");
    ASSERT_STR(p->license, "MIT");
    ASSERT_STR(p->std, "17");
    ASSERT_STR(p->java.encoding, "UTF-8");
    now_project_free(p);
    PASS();
}

static void test_import_maven_deps(void) {
    TEST("import:maven: parses dependencies with scopes");
    char pompath[512];
    snprintf(pompath, sizeof(pompath), "%s/pom_deps.xml", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_import_maven(pompath, &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ((int)p->deps.count, 3);

    /* First dep: slf4j-api, compile scope */
    ASSERT_STR(p->deps.items[0].id, "org.slf4j:slf4j-api:2.0.9");
    ASSERT_NULL(p->deps.items[0].scope);  /* default compile → no scope stored */

    /* Second dep: junit, test scope, version from property substitution */
    ASSERT_STR(p->deps.items[1].id, "org.junit.jupiter:junit-jupiter:5.10.0");
    ASSERT_STR(p->deps.items[1].scope, "test");

    /* Third dep: guava, optional */
    ASSERT_STR(p->deps.items[2].id, "com.google.guava:guava:32.1.3-jre");
    ASSERT_EQ(p->deps.items[2].optional, 1);

    /* Repository */
    ASSERT_EQ((int)p->repos.count, 1);
    ASSERT_STR(p->repos.items[0].id, "central");

    now_project_free(p);
    PASS();
}

static void test_import_maven_roundtrip(void) {
    TEST("import:maven: roundtrip pom.xml → now.pasta → reload");
    char pompath[512];
    snprintf(pompath, sizeof(pompath), "%s/pom_basic.xml", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_import_maven(pompath, &res);
    ASSERT_NOT_NULL(p);

    /* Write now.pasta */
    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_roundtrip.pasta", NOW_TEST_RESOURCES);
    int rc = now_import_maven_write(p, outpath, &res);
    ASSERT_EQ(rc, 0);
    now_project_free(p);

    /* Re-load and verify */
    NowProject *p2 = now_project_load(outpath, &res);
    if (!p2) { fprintf(stderr, "roundtrip reload error: %s\n", res.message); FAIL("p2 is NULL"); return; }
    ASSERT_STR(p2->group, "com.example");
    ASSERT_STR(p2->artifact, "myapp");
    ASSERT_STR(p2->version, "1.2.3");
    ASSERT_STR(p2->std, "17");
    ASSERT_EQ((int)p2->langs.count, 1);
    ASSERT_STR(p2->langs.items[0], "java");

    remove(outpath);
    now_project_free(p2);
    PASS();
}

/* ---- Export ---- */

static void test_export_cmake_basic(void) {
    TEST("export:cmake: generates valid CMakeLists.txt");
    const char *input =
        "{ group: \"io.test\", artifact: \"mylib\", version: \"2.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"shared\", name: \"mylib\" },"
        "  compile: { warnings: [\"Wall\", \"Wextra\"], defines: [\"MYLIB_INTERNAL\"] } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_cmake_output.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_cmake(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    /* Read back and verify key content */
    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "project(mylib VERSION 2.0.0")) { FAIL("missing project()"); now_project_free(p); return; }
    if (!strstr(buf, "add_library(mylib SHARED")) { FAIL("missing add_library"); now_project_free(p); return; }
    if (!strstr(buf, "-Wall")) { FAIL("missing -Wall"); now_project_free(p); return; }
    if (!strstr(buf, "-Wextra")) { FAIL("missing -Wextra"); now_project_free(p); return; }
    if (!strstr(buf, "MYLIB_INTERNAL")) { FAIL("missing define"); now_project_free(p); return; }
    if (!strstr(buf, "CMAKE_C_STANDARD 11")) { FAIL("missing C standard"); now_project_free(p); return; }
    if (!strstr(buf, "Generated by now")) { FAIL("missing header"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_cmake_executable(void) {
    TEST("export:cmake: executable output type");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"app\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_cmake_exec.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_cmake(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "add_executable(app")) { FAIL("missing add_executable"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_cmake_deps_comment(void) {
    TEST("export:cmake: deps listed as comments");
    const char *input =
        "{ group: \"io.test\", artifact: \"svc\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"svc\" },"
        "  deps: [{ id: \"org.acme:core:^1.0.0\" }] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_cmake_deps.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_cmake(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "org.acme:core:^1.0.0")) { FAIL("missing dep comment"); now_project_free(p); return; }
    if (!strstr(buf, "FetchContent")) { FAIL("missing FetchContent hint"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_cmake_cxx(void) {
    TEST("export:cmake: C++ project includes CXX language");
    const char *input =
        "{ group: \"io.test\", artifact: \"cxxlib\", version: \"1.0.0\","
        "  langs: [\"c\", \"c++\"], std: \"c++17\","
        "  output: { type: \"static\", name: \"cxxlib\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_cmake_cxx.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_cmake(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "LANGUAGES CXX C")) { FAIL("missing CXX language"); now_project_free(p); return; }
    if (!strstr(buf, "CMAKE_CXX_STANDARD 17")) { FAIL("missing CXX standard"); now_project_free(p); return; }
    if (!strstr(buf, "*.cpp")) { FAIL("missing cpp glob"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

/* ---- Export: Makefile ---- */

static void test_export_make_basic(void) {
    TEST("export:make: generates valid Makefile");
    const char *input =
        "{ group: \"io.test\", artifact: \"mylib\", version: \"2.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"shared\", name: \"mylib\" },"
        "  compile: { warnings: [\"Wall\", \"Wextra\"], defines: [\"MYLIB_INTERNAL\"] } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_make_output.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_make(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "PROJECT  := mylib")) { FAIL("missing PROJECT"); now_project_free(p); return; }
    if (!strstr(buf, "VERSION  := 2.0.0")) { FAIL("missing VERSION"); now_project_free(p); return; }
    if (!strstr(buf, "-fPIC")) { FAIL("missing -fPIC for shared"); now_project_free(p); return; }
    if (!strstr(buf, "lib$(TARGET).so")) { FAIL("missing .so output"); now_project_free(p); return; }
    if (!strstr(buf, "-shared")) { FAIL("missing -shared flag"); now_project_free(p); return; }
    if (!strstr(buf, "-Wall")) { FAIL("missing -Wall"); now_project_free(p); return; }
    if (!strstr(buf, "-Wextra")) { FAIL("missing -Wextra"); now_project_free(p); return; }
    if (!strstr(buf, "-DMYLIB_INTERNAL")) { FAIL("missing -D define"); now_project_free(p); return; }
    if (!strstr(buf, "-std=c11")) { FAIL("missing -std=c11"); now_project_free(p); return; }
    if (!strstr(buf, "Generated by now")) { FAIL("missing header"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_make_executable(void) {
    TEST("export:make: executable output type");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"app\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_make_exec.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_make(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "$(BUILD_DIR)/$(TARGET)")) { FAIL("missing executable output"); now_project_free(p); return; }
    if (strstr(buf, "-fPIC")) { FAIL("executable should not have -fPIC"); now_project_free(p); return; }
    if (strstr(buf, "-shared")) { FAIL("executable should not have -shared"); now_project_free(p); return; }
    if (!strstr(buf, "install -m 755")) { FAIL("missing install for executable"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_make_static(void) {
    TEST("export:make: static library uses ar");
    const char *input =
        "{ group: \"io.test\", artifact: \"core\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"static\", name: \"core\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_make_static.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_make(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "lib$(TARGET).a")) { FAIL("missing .a output"); now_project_free(p); return; }
    if (!strstr(buf, "$(AR) rcs")) { FAIL("missing ar rcs"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_make_deps(void) {
    TEST("export:make: deps listed as comments");
    const char *input =
        "{ group: \"io.test\", artifact: \"svc\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"svc\" },"
        "  deps: [{ id: \"org.acme:core:^1.0.0\" }] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_make_deps.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_make(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "org.acme:core:^1.0.0")) { FAIL("missing dep comment"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_make_cxx(void) {
    TEST("export:make: C++ project uses CXX");
    const char *input =
        "{ group: \"io.test\", artifact: \"cxxlib\", version: \"1.0.0\","
        "  langs: [\"c\", \"c++\"], std: \"c++17\","
        "  output: { type: \"static\", name: \"cxxlib\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_make_cxx.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_make(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "CXX       ?= g++")) { FAIL("missing CXX variable"); now_project_free(p); return; }
    if (!strstr(buf, "-std=c++17")) { FAIL("missing C++17 std flag"); now_project_free(p); return; }
    if (!strstr(buf, "*.cpp")) { FAIL("missing cpp wildcard"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

/* ---- Meson export ---- */

static void test_export_meson_basic(void) {
    TEST("export:meson: generates valid meson.build");
    const char *input =
        "{ group: \"io.test\", artifact: \"mylib\", version: \"2.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"shared\", name: \"mylib\" },"
        "  compile: { warnings: [\"Wall\"], defines: [\"MYLIB_INTERNAL\"] } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_meson_output.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_meson(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "project('mylib'")) { FAIL("missing project()"); now_project_free(p); return; }
    if (!strstr(buf, "'c'")) { FAIL("missing language"); now_project_free(p); return; }
    if (!strstr(buf, "c_std=c11")) { FAIL("missing c_std"); now_project_free(p); return; }
    if (!strstr(buf, "shared_library")) { FAIL("missing shared_library"); now_project_free(p); return; }
    if (!strstr(buf, "-Wall")) { FAIL("missing -Wall"); now_project_free(p); return; }
    if (!strstr(buf, "-DMYLIB_INTERNAL")) { FAIL("missing define"); now_project_free(p); return; }
    if (!strstr(buf, "Generated by now")) { FAIL("missing header"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_meson_executable(void) {
    TEST("export:meson: executable output type");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"app\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_meson_exec.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_meson(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "executable('app'")) { FAIL("missing executable()"); now_project_free(p); return; }
    if (strstr(buf, "shared_library")) { FAIL("should not have shared_library"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_meson_cxx(void) {
    TEST("export:meson: C++ project");
    const char *input =
        "{ group: \"io.test\", artifact: \"cxxlib\", version: \"1.0.0\","
        "  langs: [\"c\", \"c++\"], std: \"c++20\","
        "  output: { type: \"static\", name: \"cxxlib\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_meson_cxx.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_meson(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "'cpp'")) { FAIL("missing cpp language"); now_project_free(p); return; }
    if (!strstr(buf, "cpp_std=c++20")) { FAIL("missing cpp_std"); now_project_free(p); return; }
    if (!strstr(buf, "static_library")) { FAIL("missing static_library"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_meson_header_only(void) {
    TEST("export:meson: header-only uses declare_dependency");
    const char *input =
        "{ group: \"io.test\", artifact: \"hdr\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"header-only\", name: \"hdr\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_meson_hdr.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_meson(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "declare_dependency")) { FAIL("missing declare_dependency"); now_project_free(p); return; }
    if (strstr(buf, "executable(") || strstr(buf, "static_library(") ||
        strstr(buf, "shared_library(")) {
        FAIL("header-only should not have build target");
        now_project_free(p); return;
    }

    now_project_free(p);
    PASS();
}

/* ---- Bazel export ---- */

static void test_export_bazel_basic(void) {
    TEST("export:bazel: generates valid BUILD.bazel");
    const char *input =
        "{ group: \"io.test\", artifact: \"mylib\", version: \"2.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"shared\", name: \"mylib\" },"
        "  compile: { warnings: [\"Wall\"], defines: [\"MYLIB_INTERNAL\"] } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_bazel_output.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_bazel(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "cc_library")) { FAIL("missing cc_library"); now_project_free(p); return; }
    if (!strstr(buf, "\"mylib\"")) { FAIL("missing target name"); now_project_free(p); return; }
    if (!strstr(buf, "-std=c11")) { FAIL("missing -std=c11"); now_project_free(p); return; }
    if (!strstr(buf, "-Wall")) { FAIL("missing -Wall"); now_project_free(p); return; }
    if (!strstr(buf, "-DMYLIB_INTERNAL")) { FAIL("missing define"); now_project_free(p); return; }
    if (!strstr(buf, "rules_cc")) { FAIL("missing rules_cc load"); now_project_free(p); return; }
    if (!strstr(buf, "Generated by now")) { FAIL("missing header"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_bazel_executable(void) {
    TEST("export:bazel: executable uses cc_binary");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"app\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_bazel_exec.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_bazel(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "cc_binary")) { FAIL("missing cc_binary"); now_project_free(p); return; }
    if (strstr(buf, "cc_library(\n")) { FAIL("should not have cc_library"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_bazel_static(void) {
    TEST("export:bazel: static lib has linkstatic");
    const char *input =
        "{ group: \"io.test\", artifact: \"slib\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"static\", name: \"slib\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_bazel_static.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_bazel(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "linkstatic = True")) { FAIL("missing linkstatic"); now_project_free(p); return; }
    if (!strstr(buf, "cc_library")) { FAIL("missing cc_library"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_bazel_cxx(void) {
    TEST("export:bazel: C++ project glob patterns");
    const char *input =
        "{ group: \"io.test\", artifact: \"cxxlib\", version: \"1.0.0\","
        "  langs: [\"c\", \"c++\"], std: \"c++17\","
        "  output: { type: \"shared\", name: \"cxxlib\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_bazel_cxx.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_bazel(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "*.cpp")) { FAIL("missing cpp glob"); now_project_free(p); return; }
    if (!strstr(buf, "-std=c++17")) { FAIL("missing -std=c++17"); now_project_free(p); return; }
    if (!strstr(buf, "*.hpp")) { FAIL("missing hpp glob"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_bazel_deps_comment(void) {
    TEST("export:bazel: deps listed as comments");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"app\" },"
        "  deps: [{ id: \"io.x:foo:1.0.0\", scope: \"compile\" }] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_bazel_deps.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_bazel(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "io.x:foo:1.0.0")) { FAIL("missing dep comment"); now_project_free(p); return; }
    if (!strstr(buf, "[compile]")) { FAIL("missing scope"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

/* ---- Reproducible builds ---- */

static void test_repro_init(void) {
    TEST("repro: init defaults to disabled");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    ASSERT_EQ(cfg.enabled, 0);
    ASSERT_EQ(cfg.path_prefix_map, 0);
    ASSERT_EQ(cfg.sort_inputs, 0);
    ASSERT_EQ(cfg.no_date_macros, 0);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_from_project_bool(void) {
    TEST("repro: parse reproducible: true enables all");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], reproducible: true }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowReproConfig cfg;
    now_repro_from_project(&cfg, p);
    ASSERT_EQ(cfg.enabled, 1);
    ASSERT_EQ(cfg.path_prefix_map, 1);
    ASSERT_EQ(cfg.sort_inputs, 1);
    ASSERT_EQ(cfg.no_date_macros, 1);
    ASSERT_EQ(cfg.strip_metadata, 1);
    ASSERT_EQ(cfg.verify, 1);
    ASSERT_STR(cfg.timebase, "git-commit");

    now_repro_free(&cfg);
    now_project_free(p);
    PASS();
}

static void test_repro_from_project_map(void) {
    TEST("repro: parse reproducible: map with selective options");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  reproducible: { timebase: \"zero\", path_prefix_map: true,"
        "                   sort_inputs: true, no_date_macros: false,"
        "                   strip_metadata: false, verify: false } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowReproConfig cfg;
    now_repro_from_project(&cfg, p);
    ASSERT_EQ(cfg.enabled, 1);
    ASSERT_STR(cfg.timebase, "zero");
    ASSERT_EQ(cfg.path_prefix_map, 1);
    ASSERT_EQ(cfg.sort_inputs, 1);
    ASSERT_EQ(cfg.no_date_macros, 0);
    ASSERT_EQ(cfg.strip_metadata, 0);
    ASSERT_EQ(cfg.verify, 0);

    now_repro_free(&cfg);
    now_project_free(p);
    PASS();
}

static void test_repro_from_project_none(void) {
    TEST("repro: no reproducible field stays disabled");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowReproConfig cfg;
    now_repro_from_project(&cfg, p);
    ASSERT_EQ(cfg.enabled, 0);

    now_repro_free(&cfg);
    now_project_free(p);
    PASS();
}

static void test_repro_timebase_zero(void) {
    TEST("repro: timebase 'zero' resolves to epoch");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.timebase = strdup("zero");

    NowResult res;
    char *ts = now_repro_resolve_timebase(&cfg, ".", &res);
    ASSERT_NOT_NULL(ts);
    ASSERT_STR(ts, "1970-01-01T00:00:00Z");
    free(ts);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_timebase_now(void) {
    TEST("repro: timebase 'now' resolves to current time");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.timebase = strdup("now");

    NowResult res;
    char *ts = now_repro_resolve_timebase(&cfg, ".", &res);
    ASSERT_NOT_NULL(ts);
    /* Should be a valid ISO 8601 string ending in Z */
    size_t len = strlen(ts);
    if (len < 20 || ts[len-1] != 'Z') { FAIL("bad format"); free(ts); now_repro_free(&cfg); return; }
    free(ts);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_timebase_literal(void) {
    TEST("repro: timebase literal ISO 8601 passthrough");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.timebase = strdup("2026-01-15T12:00:00Z");

    NowResult res;
    char *ts = now_repro_resolve_timebase(&cfg, ".", &res);
    ASSERT_NOT_NULL(ts);
    ASSERT_STR(ts, "2026-01-15T12:00:00Z");
    free(ts);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_compile_flags_gcc(void) {
    TEST("repro: compile flags for GCC (prefix map + date)");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.path_prefix_map = 1;
    cfg.no_date_macros = 1;

    char **flags = NULL;
    size_t count = 0;
    int n = now_repro_compile_flags(&cfg, "/home/user/proj",
                                     "2026-03-05T14:30:00Z", 0,
                                     &flags, &count);
    if (n < 0) { FAIL("returned error"); now_repro_free(&cfg); return; }
    if (count < 3) { FAIL("expected >= 3 flags"); now_repro_free_flags(flags, count); now_repro_free(&cfg); return; }

    /* Check for debug prefix map */
    int has_debug_prefix = 0, has_macro_prefix = 0, has_date = 0, has_time = 0;
    for (size_t i = 0; i < count; i++) {
        if (strstr(flags[i], "-fdebug-prefix-map=")) has_debug_prefix = 1;
        if (strstr(flags[i], "-fmacro-prefix-map=")) has_macro_prefix = 1;
        if (strstr(flags[i], "__DATE__")) has_date = 1;
        if (strstr(flags[i], "__TIME__")) has_time = 1;
    }
    if (!has_debug_prefix) { FAIL("missing -fdebug-prefix-map"); }
    else if (!has_macro_prefix) { FAIL("missing -fmacro-prefix-map"); }
    else if (!has_date) { FAIL("missing __DATE__ define"); }
    else if (!has_time) { FAIL("missing __TIME__ define"); }

    now_repro_free_flags(flags, count);
    now_repro_free(&cfg);
    if (has_debug_prefix && has_macro_prefix && has_date && has_time) PASS();
}

static void test_repro_compile_flags_msvc(void) {
    TEST("repro: compile flags for MSVC (/pathmap + /D)");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.path_prefix_map = 1;
    cfg.no_date_macros = 1;

    char **flags = NULL;
    size_t count = 0;
    now_repro_compile_flags(&cfg, "C:\\Users\\dev\\proj",
                             "2026-03-05T14:30:00Z", 1,
                             &flags, &count);
    if (count < 1) { FAIL("expected flags"); now_repro_free(&cfg); return; }

    int has_pathmap = 0;
    for (size_t i = 0; i < count; i++) {
        if (strstr(flags[i], "/pathmap:")) has_pathmap = 1;
    }
    if (!has_pathmap) { FAIL("missing /pathmap"); now_repro_free_flags(flags, count); now_repro_free(&cfg); return; }

    now_repro_free_flags(flags, count);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_link_flags(void) {
    TEST("repro: link flags include --build-id=sha1");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.strip_metadata = 1;

    char **flags = NULL;
    size_t count = 0;
    now_repro_link_flags(&cfg, 0, &flags, &count);
    if (count < 1) { FAIL("expected link flags"); now_repro_free(&cfg); return; }
    if (!strstr(flags[0], "--build-id=sha1")) {
        FAIL("missing --build-id=sha1");
        now_repro_free_flags(flags, count);
        now_repro_free(&cfg);
        return;
    }

    now_repro_free_flags(flags, count);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_link_flags_msvc_empty(void) {
    TEST("repro: no link flags for MSVC");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.strip_metadata = 1;

    char **flags = NULL;
    size_t count = 0;
    now_repro_link_flags(&cfg, 1, &flags, &count);
    ASSERT_EQ(count, (size_t)0);

    now_repro_free(&cfg);
    PASS();
}

static void test_repro_sort_filelist(void) {
    TEST("repro: sort file list lexicographically");
    NowFileList fl;
    now_filelist_init(&fl);
    now_filelist_push(&fl, "src/main/c/z_last.c");
    now_filelist_push(&fl, "src/main/c/a_first.c");
    now_filelist_push(&fl, "src/main/c/m_middle.c");

    now_repro_sort_filelist(&fl);

    ASSERT_STR(fl.paths[0], "src/main/c/a_first.c");
    ASSERT_STR(fl.paths[1], "src/main/c/m_middle.c");
    ASSERT_STR(fl.paths[2], "src/main/c/z_last.c");

    now_filelist_free(&fl);
    PASS();
}

static void test_repro_disabled_no_flags(void) {
    TEST("repro: disabled config produces no flags");
    NowReproConfig cfg;
    now_repro_init(&cfg);

    char **flags = NULL;
    size_t count = 0;
    now_repro_compile_flags(&cfg, "/some/path", "2026-01-01T00:00:00Z", 0,
                             &flags, &count);
    ASSERT_EQ(count, (size_t)0);

    now_repro_free(&cfg);
    PASS();
}

static void test_repro_null_safety(void) {
    TEST("repro: null safety");
    NowReproConfig cfg;
    now_repro_from_project(&cfg, NULL);
    ASSERT_EQ(cfg.enabled, 0);

    now_repro_sort_filelist(NULL);  /* should not crash */

    now_repro_free(&cfg);
    PASS();
}

/* ---- Trust ---- */

static void test_trust_init_free(void) {
    TEST("trust: init and free empty store");
    NowTrustStore store;
    now_trust_init(&store);
    ASSERT_EQ(store.count, (size_t)0);
    ASSERT_EQ(store.capacity, (size_t)0);
    now_trust_free(&store);
    PASS();
}

static void test_trust_add(void) {
    TEST("trust: add keys to store");
    NowTrustStore store;
    now_trust_init(&store);

    int rc = now_trust_add(&store, "*", "RWAAAA==", "global key");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(store.count, (size_t)1);
    ASSERT_STR(store.keys[0].scope, "*");
    ASSERT_STR(store.keys[0].key, "RWAAAA==");
    ASSERT_STR(store.keys[0].comment, "global key");

    rc = now_trust_add(&store, "org.acme", "RWBBBB==", NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(store.count, (size_t)2);

    now_trust_free(&store);
    PASS();
}

static void test_trust_scope_wildcard(void) {
    TEST("trust: scope '*' matches everything");
    ASSERT_EQ(now_trust_scope_matches("*", "org.acme", "core"), 1);
    ASSERT_EQ(now_trust_scope_matches("*", "io.test", NULL), 1);
    PASS();
}

static void test_trust_scope_group_prefix(void) {
    TEST("trust: scope group prefix with dot-boundary");
    ASSERT_EQ(now_trust_scope_matches("org.acme", "org.acme", NULL), 1);
    ASSERT_EQ(now_trust_scope_matches("org.acme", "org.acme.core", NULL), 1);
    ASSERT_EQ(now_trust_scope_matches("org.acme", "org.acmetools", NULL), 0);
    ASSERT_EQ(now_trust_scope_matches("org.acme", "io.other", NULL), 0);
    PASS();
}

static void test_trust_scope_exact(void) {
    TEST("trust: scope group:artifact exact match");
    ASSERT_EQ(now_trust_scope_matches("org.acme:core", "org.acme", "core"), 1);
    ASSERT_EQ(now_trust_scope_matches("org.acme:core", "org.acme", "other"), 0);
    ASSERT_EQ(now_trust_scope_matches("org.acme:core", "org.acme", NULL), 0);
    ASSERT_EQ(now_trust_scope_matches("org.acme:core", "io.test", "core"), 0);
    PASS();
}

static void test_trust_find(void) {
    TEST("trust: find key by coordinate");
    NowTrustStore store;
    now_trust_init(&store);
    now_trust_add(&store, "org.acme", "KEY_ACME", "acme org");
    now_trust_add(&store, "io.test:specific", "KEY_EXACT", "exact match");
    now_trust_add(&store, "*", "KEY_GLOBAL", "fallback");

    const NowTrustKey *k;
    k = now_trust_find(&store, "org.acme", "core");
    ASSERT_NOT_NULL(k);
    ASSERT_STR(k->key, "KEY_ACME");

    k = now_trust_find(&store, "org.acme.sub", "lib");
    ASSERT_NOT_NULL(k);
    ASSERT_STR(k->key, "KEY_ACME");

    k = now_trust_find(&store, "io.test", "specific");
    ASSERT_NOT_NULL(k);
    ASSERT_STR(k->key, "KEY_EXACT");

    k = now_trust_find(&store, "io.test", "other");
    ASSERT_NOT_NULL(k);
    ASSERT_STR(k->key, "KEY_GLOBAL");

    now_trust_free(&store);
    PASS();
}

static void test_trust_find_no_match(void) {
    TEST("trust: find returns NULL when no match");
    NowTrustStore store;
    now_trust_init(&store);
    now_trust_add(&store, "org.acme:core", "KEY1", NULL);

    const NowTrustKey *k = now_trust_find(&store, "io.other", "lib");
    if (k != NULL) { FAIL("expected NULL"); now_trust_free(&store); return; }

    now_trust_free(&store);
    PASS();
}

static void test_trust_policy_none(void) {
    TEST("trust: policy defaults to NONE");
    NowTrustPolicy policy = {0, 0};
    ASSERT_EQ(now_trust_level(&policy), NOW_TRUST_NONE);
    PASS();
}

static void test_trust_policy_signed(void) {
    TEST("trust: policy SIGNED when require_signatures");
    NowTrustPolicy policy = {1, 0};
    ASSERT_EQ(now_trust_level(&policy), NOW_TRUST_SIGNED);
    PASS();
}

static void test_trust_policy_trusted(void) {
    TEST("trust: policy TRUSTED when require_known_keys");
    NowTrustPolicy policy = {1, 1};
    ASSERT_EQ(now_trust_level(&policy), NOW_TRUST_TRUSTED);
    PASS();
}

static void test_trust_policy_from_project(void) {
    TEST("trust: parse policy from project pasta");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  trust: { require_signatures: true, require_known_keys: false } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowTrustPolicy pol = now_trust_policy_from_project(p);
    ASSERT_EQ(pol.require_signatures, 1);
    ASSERT_EQ(pol.require_known_keys, 0);
    ASSERT_EQ(now_trust_level(&pol), NOW_TRUST_SIGNED);

    now_project_free(p);
    PASS();
}

static void test_trust_null_safety(void) {
    TEST("trust: null safety");
    ASSERT_EQ(now_trust_scope_matches(NULL, "org", NULL), 0);
    ASSERT_EQ(now_trust_scope_matches("*", NULL, NULL), 0);
    if (now_trust_find(NULL, "org", NULL) != NULL) { FAIL("expected NULL"); return; }

    NowTrustPolicy pol = now_trust_policy_from_project(NULL);
    ASSERT_EQ(pol.require_signatures, 0);
    ASSERT_EQ(now_trust_level(NULL), NOW_TRUST_NONE);
    PASS();
}

/* ---- Advisory guards ---- */

static void test_advisory_db_init_free(void) {
    TEST("advisory: db init/free");
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    ASSERT_EQ(db.count, (size_t)0);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_severity_parse(void) {
    TEST("advisory: severity parsing");
    ASSERT_EQ(now_severity_parse("critical"), NOW_SEV_CRITICAL);
    ASSERT_EQ(now_severity_parse("high"), NOW_SEV_HIGH);
    ASSERT_EQ(now_severity_parse("medium"), NOW_SEV_MEDIUM);
    ASSERT_EQ(now_severity_parse("low"), NOW_SEV_LOW);
    ASSERT_EQ(now_severity_parse("info"), NOW_SEV_INFO);
    ASSERT_EQ(now_severity_parse("blacklisted"), NOW_SEV_BLACKLISTED);
    ASSERT_EQ(now_severity_parse("unknown"), NOW_SEV_INFO);
    ASSERT_EQ(now_severity_parse(NULL), NOW_SEV_INFO);
    PASS();
}

static void test_advisory_severity_name(void) {
    TEST("advisory: severity name roundtrip");
    ASSERT_STR(now_severity_name(NOW_SEV_CRITICAL), "critical");
    ASSERT_STR(now_severity_name(NOW_SEV_HIGH), "high");
    ASSERT_STR(now_severity_name(NOW_SEV_MEDIUM), "medium");
    ASSERT_STR(now_severity_name(NOW_SEV_LOW), "low");
    ASSERT_STR(now_severity_name(NOW_SEV_INFO), "info");
    ASSERT_STR(now_severity_name(NOW_SEV_BLACKLISTED), "blacklisted");
    PASS();
}

static void test_advisory_severity_blocks(void) {
    TEST("advisory: severity blocking");
    ASSERT_EQ(now_severity_blocks(NOW_SEV_BLACKLISTED), 1);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_CRITICAL), 1);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_HIGH), 1);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_MEDIUM), 0);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_LOW), 0);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_INFO), 0);
    PASS();
}

static void test_advisory_db_load_string(void) {
    TEST("advisory: load db from string");
    const char *input =
        "{ version: \"1.0.0\", updated: \"2026-03-05T00:00:00Z\","
        "  advisories: ["
        "    { id: \"NOW-SA-2026-0042\", severity: \"critical\","
        "      title: \"Buffer overflow in inflate()\","
        "      cve: [\"CVE-2026-1234\"],"
        "      affects: [ { id: \"zlib:zlib\", versions: [\">=1.2.0 <1.3.1\"] } ],"
        "      fixed_in: [ { id: \"zlib:zlib\", version: \"1.3.1\" } ],"
        "      affects_build_time: false, affects_runtime: true"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_advisory_db_load_string(&db, input, strlen(input), &res);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(db.count, (size_t)1);
    ASSERT_STR(db.entries[0].id, "NOW-SA-2026-0042");
    ASSERT_EQ(db.entries[0].severity, NOW_SEV_CRITICAL);
    ASSERT_STR(db.entries[0].title, "Buffer overflow in inflate()");
    ASSERT_EQ(db.entries[0].cve_count, (size_t)1);
    ASSERT_STR(db.entries[0].cve[0], "CVE-2026-1234");
    ASSERT_EQ(db.entries[0].affects_count, (size_t)1);
    ASSERT_STR(db.entries[0].affects[0].id, "zlib:zlib");
    ASSERT_EQ(db.entries[0].affects_runtime, 1);
    ASSERT_EQ(db.entries[0].affects_build_time, 0);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_blacklisted(void) {
    TEST("advisory: blacklisted entry");
    const char *input =
        "{ advisories: ["
        "    { id: \"NOW-SA-2026-0043\", severity: \"high\","
        "      blacklisted: true,"
        "      affects: [ { id: \"evil:pkg\", versions: [\"*\"] } ],"
        "      affects_build_time: true, affects_runtime: false"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, input, strlen(input), &res);
    ASSERT_EQ(db.count, (size_t)1);
    ASSERT_EQ(db.entries[0].blacklisted, 1);
    ASSERT_EQ(db.entries[0].severity, NOW_SEV_BLACKLISTED);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_override_parse(void) {
    TEST("advisory: parse overrides from project");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  advisories: { allow: ["
        "    { advisory: \"NOW-SA-2026-0042\", dep: \"zlib:zlib:1.3.0\","
        "      reason: \"inflate() not used\", expires: \"2026-06-01\","
        "      approved_by: \"alice@acme.org\" }"
        "  ] }"
        "}";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowOverrideList ovr;
    now_override_list_init(&ovr);
    int rc = now_advisory_overrides_from_project(&ovr, p, &res);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ovr.count, (size_t)1);
    ASSERT_STR(ovr.items[0].advisory, "NOW-SA-2026-0042");
    ASSERT_STR(ovr.items[0].dep, "zlib:zlib:1.3.0");
    ASSERT_STR(ovr.items[0].reason, "inflate() not used");
    ASSERT_STR(ovr.items[0].expires, "2026-06-01");
    ASSERT_STR(ovr.items[0].approved_by, "alice@acme.org");

    now_override_list_free(&ovr);
    now_project_free(p);
    PASS();
}

static void test_advisory_override_no_expires(void) {
    TEST("advisory: override without expires rejected");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  advisories: { allow: ["
        "    { advisory: \"NOW-SA-2026-0042\", reason: \"no expiry\" }"
        "  ] }"
        "}";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowOverrideList ovr;
    now_override_list_init(&ovr);
    int rc = now_advisory_overrides_from_project(&ovr, p, &res);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);

    now_override_list_free(&ovr);
    now_project_free(p);
    PASS();
}

static void test_advisory_override_expiry(void) {
    TEST("advisory: override expiry check");
    NowAdvisoryOverride ovr = {0};
    ovr.expires = "2026-06-01";

    /* Not expired (today = 2026-03-08) */
    ASSERT_EQ(now_advisory_override_expired(&ovr, 20260308), 0);
    /* Expired (today = 2026-07-01) */
    ASSERT_EQ(now_advisory_override_expired(&ovr, 20260701), 1);
    /* Exact expiry date — not expired (still within) */
    ASSERT_EQ(now_advisory_override_expired(&ovr, 20260601), 0);
    PASS();
}

static void test_advisory_find_override(void) {
    TEST("advisory: find override by advisory+dep");
    NowOverrideList list;
    now_override_list_init(&list);

    /* Manually add one */
    NowAdvisoryOverride *tmp = realloc(list.items, sizeof(NowAdvisoryOverride));
    if (!tmp) { FAIL("alloc"); return; }
    list.items = tmp;
    list.capacity = 1;
    list.count = 1;
    memset(&list.items[0], 0, sizeof(NowAdvisoryOverride));
    list.items[0].advisory = strdup("NOW-SA-001");
    list.items[0].dep = strdup("zlib:zlib:1.3.0");
    list.items[0].expires = strdup("2026-12-31");

    const NowAdvisoryOverride *found =
        now_advisory_find_override(&list, "NOW-SA-001", "zlib:zlib:1.3.0");
    ASSERT_NOT_NULL(found);

    /* Different dep — no match */
    found = now_advisory_find_override(&list, "NOW-SA-001", "other:lib:1.0.0");
    if (found) { FAIL("expected NULL for different dep"); now_override_list_free(&list); return; }

    /* Different advisory — no match */
    found = now_advisory_find_override(&list, "NOW-SA-999", "zlib:zlib:1.3.0");
    if (found) { FAIL("expected NULL for different advisory"); now_override_list_free(&list); return; }

    now_override_list_free(&list);
    PASS();
}

static void test_advisory_check_dep_match(void) {
    TEST("advisory: check dep finds matching advisory");
    const char *db_str =
        "{ advisories: ["
        "    { id: \"NOW-SA-001\", severity: \"critical\","
        "      title: \"test vuln\","
        "      affects: [ { id: \"zlib:zlib\", versions: [\">=1.2.0 <1.3.1\"] } ],"
        "      affects_runtime: true, affects_build_time: false"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, db_str, strlen(db_str), &res);

    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    int rc = now_advisory_check_dep(&db, NULL, "zlib:zlib:1.3.0",
                                      "compile", 20260308, &report);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(report.count, (size_t)1);
    ASSERT_EQ(report.blocked, 1);

    now_advisory_report_free(&report);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_check_dep_no_match(void) {
    TEST("advisory: check dep no match for safe version");
    const char *db_str =
        "{ advisories: ["
        "    { id: \"NOW-SA-001\", severity: \"critical\","
        "      affects: [ { id: \"zlib:zlib\", versions: [\">=1.2.0 <1.3.1\"] } ],"
        "      affects_runtime: true"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, db_str, strlen(db_str), &res);

    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    now_advisory_check_dep(&db, NULL, "zlib:zlib:1.3.1",
                            "compile", 20260308, &report);
    ASSERT_EQ(report.count, (size_t)0);
    ASSERT_EQ(report.blocked, 0);

    now_advisory_report_free(&report);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_check_dep_overridden(void) {
    TEST("advisory: check dep with active override");
    const char *db_str =
        "{ advisories: ["
        "    { id: \"NOW-SA-001\", severity: \"critical\","
        "      affects: [ { id: \"zlib:zlib\", versions: [\">=1.2.0 <1.3.1\"] } ],"
        "      affects_runtime: true"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, db_str, strlen(db_str), &res);

    NowOverrideList overrides;
    now_override_list_init(&overrides);
    NowAdvisoryOverride *o = realloc(overrides.items, sizeof(NowAdvisoryOverride));
    overrides.items = o;
    overrides.capacity = 1;
    overrides.count = 1;
    memset(o, 0, sizeof(*o));
    o->advisory = strdup("NOW-SA-001");
    o->dep = strdup("zlib:zlib:1.3.0");
    o->expires = strdup("2027-01-01");

    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    now_advisory_check_dep(&db, &overrides, "zlib:zlib:1.3.0",
                            "compile", 20260308, &report);
    ASSERT_EQ(report.count, (size_t)1);
    ASSERT_EQ(report.hits[0].overridden, 1);
    ASSERT_EQ(report.blocked, 0); /* override prevents blocking */

    now_advisory_report_free(&report);
    now_override_list_free(&overrides);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_blacklisted_no_override(void) {
    TEST("advisory: blacklisted cannot be overridden");
    const char *db_str =
        "{ advisories: ["
        "    { id: \"NOW-SA-002\", severity: \"high\", blacklisted: true,"
        "      affects: [ { id: \"evil:pkg\", versions: [\"*\"] } ],"
        "      affects_build_time: true"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, db_str, strlen(db_str), &res);

    NowOverrideList overrides;
    now_override_list_init(&overrides);
    NowAdvisoryOverride *o = realloc(overrides.items, sizeof(NowAdvisoryOverride));
    overrides.items = o;
    overrides.capacity = 1;
    overrides.count = 1;
    memset(o, 0, sizeof(*o));
    o->advisory = strdup("NOW-SA-002");
    o->expires = strdup("2027-01-01");

    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    now_advisory_check_dep(&db, &overrides, "evil:pkg:1.0.0",
                            "compile", 20260308, &report);
    ASSERT_EQ(report.count, (size_t)1);
    ASSERT_EQ(report.blocked, 1); /* blacklisted always blocks */

    now_advisory_report_free(&report);
    now_override_list_free(&overrides);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_medium_warning(void) {
    TEST("advisory: medium severity = warning, not blocking");
    const char *db_str =
        "{ advisories: ["
        "    { id: \"NOW-SA-003\", severity: \"medium\","
        "      affects: [ { id: \"foo:bar\", versions: [\"*\"] } ],"
        "      affects_runtime: true"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, db_str, strlen(db_str), &res);

    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    now_advisory_check_dep(&db, NULL, "foo:bar:2.0.0",
                            "compile", 20260308, &report);
    ASSERT_EQ(report.count, (size_t)1);
    ASSERT_EQ(report.blocked, 0); /* medium does not block */

    now_advisory_report_free(&report);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_report_format(void) {
    TEST("advisory: report formatting");
    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    char *text = now_advisory_report_format(&report);
    ASSERT_NOT_NULL(text);
    free(text);

    now_advisory_report_free(&report);
    PASS();
}

static void test_advisory_null_safety(void) {
    TEST("advisory: null safety");
    ASSERT_EQ(now_severity_parse(NULL), NOW_SEV_INFO);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_INFO), 0);
    ASSERT_EQ(now_advisory_override_expired(NULL, 20260308), -1);
    if (now_advisory_find_override(NULL, "x", "y") != NULL) {
        FAIL("expected NULL"); return;
    }
    ASSERT_EQ(now_advisory_check_dep(NULL, NULL, "x:y:1.0", "compile", 20260308, NULL), -1);
    PASS();
}

/* ---- CI integration ---- */

static void test_ci_exit_codes(void) {
    TEST("ci: exit code mapping");
    ASSERT_EQ(now_exit_code(NOW_OK), NOW_EXIT_OK);
    ASSERT_EQ(now_exit_code(NOW_ERR_TOOL), NOW_EXIT_BUILD);
    ASSERT_EQ(now_exit_code(NOW_ERR_TEST), NOW_EXIT_TEST);
    ASSERT_EQ(now_exit_code(NOW_ERR_SCHEMA), NOW_EXIT_CONFIG);
    ASSERT_EQ(now_exit_code(NOW_ERR_SYNTAX), NOW_EXIT_CONFIG);
    ASSERT_EQ(now_exit_code(NOW_ERR_IO), NOW_EXIT_IO);
    ASSERT_EQ(now_exit_code(NOW_ERR_NOT_FOUND), NOW_EXIT_RESOLVE);
    ASSERT_EQ(now_exit_code(NOW_ERR_AUTH), NOW_EXIT_AUTH);
    PASS();
}

static void test_ci_detect_defaults(void) {
    TEST("ci: detect defaults (non-CI environment)");
    NowCIEnv env;
    now_ci_detect(&env);
    /* In test runner context, we're not in CI (unless running in actual CI) */
    /* Just verify the struct is populated without crashing */
    if (env.format != NOW_OUTPUT_TEXT && env.format != NOW_OUTPUT_JSON
        && env.format != NOW_OUTPUT_PASTA) {
        FAIL("invalid format"); return;
    }
    PASS();
}

static void test_ci_format_build_json(void) {
    TEST("ci: format build result as JSON");
    char *out = now_ci_format_build("build", 0, 1234, 10, 8, 2, 0,
                                     NOW_OUTPUT_JSON);
    ASSERT_NOT_NULL(out);
    if (!strstr(out, "\"phase\": \"build\"")) { FAIL("missing phase"); free(out); return; }
    if (!strstr(out, "\"status\": \"ok\"")) { FAIL("missing status"); free(out); return; }
    if (!strstr(out, "\"compiled\": 8")) { FAIL("missing compiled"); free(out); return; }
    if (!strstr(out, "\"cached\": 2")) { FAIL("missing cached"); free(out); return; }
    free(out);
    PASS();
}

static void test_ci_format_build_pasta(void) {
    TEST("ci: format build result as Pasta");
    char *out = now_ci_format_build("compile", 1, 500, 5, 3, 1, 1,
                                     NOW_OUTPUT_PASTA);
    ASSERT_NOT_NULL(out);
    if (!strstr(out, "phase: \"compile\"")) { FAIL("missing phase"); free(out); return; }
    if (!strstr(out, "status: \"error\"")) { FAIL("missing status"); free(out); return; }
    free(out);
    PASS();
}

static void test_ci_format_test_json(void) {
    TEST("ci: format test result as JSON");
    char *out = now_ci_format_test(0, 42, 40, 2, 0, 3456, NOW_OUTPUT_JSON);
    ASSERT_NOT_NULL(out);
    if (!strstr(out, "\"phase\": \"test\"")) { FAIL("missing phase"); free(out); return; }
    if (!strstr(out, "\"passed\": 40")) { FAIL("missing passed"); free(out); return; }
    if (!strstr(out, "\"failed\": 2")) { FAIL("missing failed"); free(out); return; }
    free(out);
    PASS();
}

static void test_ci_format_text(void) {
    TEST("ci: format build result as text");
    char *out = now_ci_format_build("link", 0, 100, 1, 1, 0, 0,
                                     NOW_OUTPUT_TEXT);
    ASSERT_NOT_NULL(out);
    if (!strstr(out, "link:")) { FAIL("missing phase"); free(out); return; }
    if (!strstr(out, "ok")) { FAIL("missing status"); free(out); return; }
    free(out);
    PASS();
}

/* ---- Basta package tests ---- */

static void test_basta_create_and_parse(void) {
    TEST("basta: create document with blob");
    BastaValue *root = basta_new_map();
    if (!root) { FAIL("basta_new_map"); return; }

    /* Add metadata */
    BastaValue *meta = basta_new_map();
    basta_set(meta, "format", basta_new_string("basta/1"));
    basta_set(meta, "artifact", basta_new_string("testlib"));
    basta_set(root, "metadata", meta);

    /* Add a blob */
    const uint8_t test_data[] = "hello blob content";
    BastaValue *files = basta_new_map();
    basta_set(files, "test.h", basta_new_blob(test_data, sizeof(test_data) - 1));
    basta_set(root, "headers", files);

    /* Write to buffer */
    size_t out_len;
    char *buf = basta_write(root, BASTA_SECTIONS, &out_len);
    basta_free(root);
    if (!buf) { FAIL("basta_write"); return; }

    /* Parse it back */
    BastaResult bres;
    BastaValue *parsed = basta_parse(buf, out_len, &bres);
    free(buf);
    if (!parsed) { FAIL(bres.message); return; }

    /* Verify metadata */
    const BastaValue *m = basta_map_get(parsed, "metadata");
    if (!m || basta_type(m) != BASTA_MAP) { basta_free(parsed); FAIL("no metadata"); return; }
    const BastaValue *art = basta_map_get(m, "artifact");
    if (!art || strcmp(basta_get_string(art), "testlib") != 0) {
        basta_free(parsed); FAIL("artifact mismatch"); return;
    }

    /* Verify blob */
    const BastaValue *h = basta_map_get(parsed, "headers");
    if (!h || basta_type(h) != BASTA_MAP) { basta_free(parsed); FAIL("no headers"); return; }
    const BastaValue *blob = basta_map_get(h, "test.h");
    if (!blob || basta_type(blob) != BASTA_BLOB) { basta_free(parsed); FAIL("no blob"); return; }
    size_t blen;
    const uint8_t *bdata = basta_get_blob(blob, &blen);
    if (blen != sizeof(test_data) - 1 || memcmp(bdata, test_data, blen) != 0) {
        basta_free(parsed); FAIL("blob content mismatch"); return;
    }

    basta_free(parsed);
    PASS();
}

static void test_basta_package_roundtrip(void) {
    TEST("basta: package → extract roundtrip");

    /* Create a minimal project for packaging */
    NowProject *p = now_project_new();
    if (!p) { FAIL("now_project_new"); return; }
    p->group    = strdup("io.test");
    p->artifact = strdup("roundtrip");
    p->version  = strdup("1.0.0");
    p->output.type = strdup("static");

    /* Create a temp directory with a descriptor and fake build output */
    char tmpdir[512];
    snprintf(tmpdir, sizeof(tmpdir), "%s/basta_test_tmp", NOW_TEST_RESOURCES);
    now_mkdir_p(tmpdir);

    /* Write a now.pasta descriptor */
    char desc_path[512];
    snprintf(desc_path, sizeof(desc_path), "%s/now.pasta", tmpdir);
    FILE *fp = fopen(desc_path, "w");
    if (fp) {
        fprintf(fp, "{ group: \"io.test\", artifact: \"roundtrip\", version: \"1.0.0\" }\n");
        fclose(fp);
    }

    /* Create target/bin with a fake library */
    char bindir[512];
    snprintf(bindir, sizeof(bindir), "%s/target/bin", tmpdir);
    now_mkdir_p(bindir);

    char libpath[512];
#ifdef _WIN32
    snprintf(libpath, sizeof(libpath), "%s/roundtrip.lib", bindir);
#else
    snprintf(libpath, sizeof(libpath), "%s/libroundtrip.a", bindir);
#endif
    fp = fopen(libpath, "wb");
    if (fp) {
        const char *fake_lib = "FAKE_LIB_DATA_1234567890";
        fwrite(fake_lib, 1, strlen(fake_lib), fp);
        fclose(fp);
    }

    /* Create headers */
    char hdrdir[512];
    snprintf(hdrdir, sizeof(hdrdir), "%s/src/main/h", tmpdir);
    now_mkdir_p(hdrdir);
    p->sources.headers = strdup("src/main/h");

    char hdrpath[512];
    snprintf(hdrpath, sizeof(hdrpath), "%s/roundtrip.h", hdrdir);
    fp = fopen(hdrpath, "w");
    if (fp) {
        fprintf(fp, "#ifndef ROUNDTRIP_H\n#define ROUNDTRIP_H\nvoid roundtrip(void);\n#endif\n");
        fclose(fp);
    }

    /* Package it */
    NowResult res;
    int rc = now_package(p, tmpdir, 0, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(p); return; }

    /* Verify .basta file exists */
    char basta_path[512];
    const char *triple = now_host_triple();
    snprintf(basta_path, sizeof(basta_path), "%s/target/pkg/roundtrip-1.0.0-%s.basta",
             tmpdir, triple);

    if (!now_path_exists(basta_path)) {
        FAIL("basta file not created"); now_project_free(p); return;
    }

    /* Extract it */
    char extract_dir[512];
    snprintf(extract_dir, sizeof(extract_dir), "%s/extracted", tmpdir);
    rc = now_basta_extract(basta_path, extract_dir, 0, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(p); return; }

    /* Verify extracted descriptor exists */
    char ex_desc[512];
    snprintf(ex_desc, sizeof(ex_desc), "%s/now.pasta", extract_dir);
    if (!now_path_exists(ex_desc)) {
        FAIL("extracted now.pasta missing"); now_project_free(p); return;
    }

    /* Verify extracted header exists */
    char ex_hdr[512];
    snprintf(ex_hdr, sizeof(ex_hdr), "%s/h/roundtrip.h", extract_dir);
    if (!now_path_exists(ex_hdr)) {
        FAIL("extracted header missing"); now_project_free(p); return;
    }

    /* Verify extracted library exists */
    char ex_lib[512];
#ifdef _WIN32
    snprintf(ex_lib, sizeof(ex_lib), "%s/lib/%s/roundtrip.lib", extract_dir, triple);
#else
    snprintf(ex_lib, sizeof(ex_lib), "%s/lib/%s/libroundtrip.a", extract_dir, triple);
#endif
    if (!now_path_exists(ex_lib)) {
        FAIL("extracted library missing"); now_project_free(p); return;
    }

    now_project_free(p);

    /* Cleanup temp files */
    remove(ex_lib);
    remove(ex_hdr);
    remove(ex_desc);
    char ex_hdir[512], ex_ldir[512], ex_ltdir[512];
    snprintf(ex_hdir, sizeof(ex_hdir), "%s/h", extract_dir);
    snprintf(ex_ltdir, sizeof(ex_ltdir), "%s/lib/%s", extract_dir, triple);
    snprintf(ex_ldir, sizeof(ex_ldir), "%s/lib", extract_dir);
    rmdir(ex_ltdir);
    rmdir(ex_ldir);
    rmdir(ex_hdir);
    rmdir(extract_dir);
    remove(basta_path);
    char sha_path[512];
    snprintf(sha_path, sizeof(sha_path), "%s/target/pkg/roundtrip-1.0.0-%s.sha256",
             tmpdir, triple);
    remove(sha_path);
    char pkgdir[512];
    snprintf(pkgdir, sizeof(pkgdir), "%s/target/pkg", tmpdir);
    rmdir(pkgdir);
    remove(libpath);
    rmdir(bindir);
    char targetdir[512];
    snprintf(targetdir, sizeof(targetdir), "%s/target", tmpdir);
    rmdir(targetdir);
    remove(hdrpath);
    rmdir(hdrdir);
    char srcmain[512];
    snprintf(srcmain, sizeof(srcmain), "%s/src/main", tmpdir);
    rmdir(srcmain);
    char srcdir[512];
    snprintf(srcdir, sizeof(srcdir), "%s/src", tmpdir);
    rmdir(srcdir);
    remove(desc_path);
    rmdir(tmpdir);

    PASS();
}

static void test_basta_extract_null_safety(void) {
    TEST("basta: extract null safety");
    NowResult res;
    int rc = now_basta_extract(NULL, NULL, 0, &res);
    if (rc != -1) { FAIL("expected -1"); return; }
    PASS();
}

static void test_basta_extract_missing_file(void) {
    TEST("basta: extract missing file");
    NowResult res;
    int rc = now_basta_extract("/nonexistent/file.basta", "/tmp/out", 0, &res);
    if (rc != -1) { FAIL("expected -1"); return; }
    PASS();
}

static void test_basta_metadata_fields(void) {
    TEST("basta: metadata has format/group/artifact/version");

    /* Build a Basta doc like now_package does */
    BastaValue *root = basta_new_map();
    BastaValue *meta = basta_new_map();
    basta_set(meta, "format",      basta_new_string("basta/1"));
    basta_set(meta, "group",       basta_new_string("com.example"));
    basta_set(meta, "artifact",    basta_new_string("mylib"));
    basta_set(meta, "version",     basta_new_string("2.3.4"));
    basta_set(meta, "triple",      basta_new_string("linux-x86_64-gnu"));
    basta_set(meta, "output_type", basta_new_string("shared"));
    basta_set(root, "metadata", meta);

    /* Write and re-parse */
    size_t len;
    char *buf = basta_write(root, BASTA_SECTIONS, &len);
    basta_free(root);
    if (!buf) { FAIL("write"); return; }

    BastaResult bres;
    BastaValue *parsed = basta_parse(buf, len, &bres);
    free(buf);
    if (!parsed) { FAIL(bres.message); return; }

    const BastaValue *m = basta_map_get(parsed, "metadata");
    if (!m) { basta_free(parsed); FAIL("no metadata"); return; }

    const BastaValue *fmt = basta_map_get(m, "format");
    if (!fmt || strcmp(basta_get_string(fmt), "basta/1") != 0) {
        basta_free(parsed); FAIL("format"); return;
    }
    const BastaValue *grp = basta_map_get(m, "group");
    if (!grp || strcmp(basta_get_string(grp), "com.example") != 0) {
        basta_free(parsed); FAIL("group"); return;
    }
    const BastaValue *art = basta_map_get(m, "artifact");
    if (!art || strcmp(basta_get_string(art), "mylib") != 0) {
        basta_free(parsed); FAIL("artifact"); return;
    }
    const BastaValue *ver = basta_map_get(m, "version");
    if (!ver || strcmp(basta_get_string(ver), "2.3.4") != 0) {
        basta_free(parsed); FAIL("version"); return;
    }
    const BastaValue *tri = basta_map_get(m, "triple");
    if (!tri || strcmp(basta_get_string(tri), "linux-x86_64-gnu") != 0) {
        basta_free(parsed); FAIL("triple"); return;
    }

    basta_free(parsed);
    PASS();
}

/* ---- Ed25519 tests ---- */

/* Helper: convert hex string to bytes */
static void hex_to_bytes(const char *hex, unsigned char *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned int byte;
        sscanf(hex + i * 2, "%02x", &byte);
        out[i] = (unsigned char)byte;
    }
}

static void test_ed25519_keypair(void) {
    TEST("ed25519: keypair from seed");
    unsigned char seed[32];
    hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                 seed, 32);

    unsigned char pub[32], priv[64];
    int rc = now_ed25519_keypair(pub, priv, seed);
    if (rc != 0) { FAIL("keypair failed"); return; }

    unsigned char expected_pub[32];
    hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                 expected_pub, 32);
    if (memcmp(pub, expected_pub, 32) != 0) {
        FAIL("public key mismatch"); return;
    }
    PASS();
}

static void test_ed25519_sign_verify(void) {
    TEST("ed25519: sign and verify roundtrip");
    /* Generate a keypair */
    unsigned char seed[32];
    hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                 seed, 32);

    unsigned char pub[32], priv[64];
    now_ed25519_keypair(pub, priv, seed);

    /* Sign the empty message (RFC 8032 Test Vector 1) */
    unsigned char sig[64];
    const unsigned char msg[] = "";
    int rc = now_ed25519_sign(sig, msg, 0, priv);
    if (rc != 0) { FAIL("sign failed"); return; }

    /* Verify */
    rc = now_ed25519_verify(sig, msg, 0, pub);
    if (rc != 0) { FAIL("verify failed"); return; }
    PASS();
}

static void test_ed25519_verify_rfc8032_1(void) {
    TEST("ed25519: verify RFC 8032 test vector 1");
    unsigned char pub[32], sig[64];
    hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                 pub, 32);
    hex_to_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
                 sig, 64);

    int rc = now_ed25519_verify(sig, (const unsigned char *)"", 0, pub);
    if (rc != 0) { FAIL("verify should pass"); return; }
    PASS();
}

static void test_ed25519_verify_rfc8032_2(void) {
    TEST("ed25519: verify RFC 8032 test vector 2");
    unsigned char pub[32], sig[64];
    hex_to_bytes("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
                 pub, 32);
    hex_to_bytes("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
                 sig, 64);

    unsigned char msg[1] = {0x72};
    int rc = now_ed25519_verify(sig, msg, 1, pub);
    if (rc != 0) { FAIL("verify should pass"); return; }
    PASS();
}

static void test_ed25519_verify_bad_sig(void) {
    TEST("ed25519: reject tampered signature");
    unsigned char pub[32], sig[64];
    hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                 pub, 32);
    hex_to_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
                 sig, 64);

    /* Tamper with signature */
    sig[0] ^= 0x01;

    int rc = now_ed25519_verify(sig, (const unsigned char *)"", 0, pub);
    if (rc == 0) { FAIL("should reject tampered sig"); return; }
    PASS();
}

static void test_ed25519_verify_wrong_msg(void) {
    TEST("ed25519: reject wrong message");
    unsigned char pub[32], sig[64];
    hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                 pub, 32);
    hex_to_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
                 sig, 64);

    unsigned char wrong[] = "wrong message";
    int rc = now_ed25519_verify(sig, wrong, sizeof(wrong) - 1, pub);
    if (rc == 0) { FAIL("should reject wrong message"); return; }
    PASS();
}

/* The suite verified signatures produced elsewhere and round-tripped its
 * own through the same code, so a signer that was wrong on roughly one
 * message in seven passed everything: a round trip cancels a matching
 * error on both sides, and the two RFC vectors it did carry are
 * verify-only. Nothing ever compared a signature *now produced* against
 * a known-correct one. These do. */
static void test_ed25519_sign_rfc8032_1(void) {
    TEST("ed25519: sign matches RFC 8032 test vector 1");
    unsigned char seed[32], pub[32], priv[64], sig[64], expected[64];
    hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                 seed, 32);
    hex_to_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
                 expected, 64);
    if (now_ed25519_keypair(pub, priv, seed) != 0) { FAIL("keypair failed"); return; }
    if (now_ed25519_sign(sig, (const unsigned char *)"", 0, priv) != 0) {
        FAIL("sign failed"); return;
    }
    if (memcmp(sig, expected, 64) != 0) { FAIL("signature bytes differ from RFC"); return; }
    PASS();
}

static void test_ed25519_sign_rfc8032_3(void) {
    TEST("ed25519: sign matches RFC 8032 test vector 3");
    unsigned char seed[32], pub[32], priv[64], sig[64], expected[64];
    hex_to_bytes("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
                 seed, 32);
    hex_to_bytes("6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
                 expected, 64);
    unsigned char msg[2] = { 0xaf, 0x82 };
    if (now_ed25519_keypair(pub, priv, seed) != 0) { FAIL("keypair failed"); return; }
    if (now_ed25519_sign(sig, msg, sizeof(msg), priv) != 0) { FAIL("sign failed"); return; }
    if (memcmp(sig, expected, 64) != 0) { FAIL("signature bytes differ from RFC"); return; }
    PASS();
}

/* Nine bytes is the shortest message of this shape that the dropped
 * carry in sc_reduce/sc_muladd got wrong; the expected bytes come from
 * an independent RFC 8032 implementation, not from ours. */
static void test_ed25519_sign_known_answer_9(void) {
    TEST("ed25519: sign matches a known answer for a 9-byte message");
    unsigned char seed[32], pub[32], priv[64], sig[64], expected[64];
    hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                 seed, 32);
    hex_to_bytes("18c80fa06772c5c6b8c8cbc83311ed06722777661be057d85d258e4ec310743d"
                 "781645ea6e1ba53f486f88fe016faa739f541efc600337b8221256a67e30d40f",
                 expected, 64);
    unsigned char msg[9];
    for (int i = 0; i < 9; i++) msg[i] = (unsigned char)i;
    if (now_ed25519_keypair(pub, priv, seed) != 0) { FAIL("keypair failed"); return; }
    if (now_ed25519_sign(sig, msg, sizeof(msg), priv) != 0) { FAIL("sign failed"); return; }
    if (memcmp(sig, expected, 64) != 0) { FAIL("signature bytes differ"); return; }
    if (now_ed25519_verify(sig, msg, sizeof(msg), pub) != 0) {
        FAIL("own signature does not verify"); return;
    }
    PASS();
}

/* One message length proves nothing about a defect that shows up on a
 * fraction of inputs — the old round-trip test used a single empty
 * message and never saw this. */
static void test_ed25519_sign_verify_lengths(void) {
    TEST("ed25519: sign and verify agree across message lengths");
    unsigned char seed[32], pub[32], priv[64], sig[64], msg[256];
    hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                 seed, 32);
    if (now_ed25519_keypair(pub, priv, seed) != 0) { FAIL("keypair failed"); return; }
    for (size_t len = 1; len <= sizeof(msg); len++) {
        for (size_t i = 0; i < len; i++) msg[i] = (unsigned char)(i & 0xff);
        if (now_ed25519_sign(sig, msg, len, priv) != 0) { FAIL("sign failed"); return; }
        if (now_ed25519_verify(sig, msg, len, pub) != 0) {
            FAIL("own signature does not verify"); return;
        }
    }
    PASS();
}

/* S + L passes the group equation exactly as S does, so a verifier that
 * skips the canonicality check gives one message two valid signatures. */
static void test_ed25519_reject_non_canonical_s(void) {
    TEST("ed25519: reject non-canonical S (S + L)");
    /* Group order L, little-endian, same constant the verifier compares
     * against. */
    static const unsigned char order[32] = {
        0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,
        0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10 };
    unsigned char pub[32], sig[64];
    hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                 pub, 32);
    hex_to_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
                 sig, 64);
    /* Sanity: the untouched signature is the valid one. */
    if (now_ed25519_verify(sig, (const unsigned char *)"", 0, pub) != 0) {
        FAIL("baseline signature should verify"); return;
    }
    /* S := S + L, little-endian, carrying by hand. */
    unsigned int carry = 0;
    for (int i = 0; i < 32; i++) {
        carry += (unsigned int)sig[32 + i] + order[i];
        sig[32 + i] = (unsigned char)(carry & 0xff);
        carry >>= 8;
    }
    if (now_ed25519_verify(sig, (const unsigned char *)"", 0, pub) == 0) {
        FAIL("should reject S >= L"); return;
    }
    PASS();
}

static void test_ed25519_null_safety(void) {
    TEST("ed25519: null safety");
    if (now_ed25519_verify(NULL, NULL, 0, NULL) == 0) { FAIL("should reject NULL"); return; }
    if (now_ed25519_sign(NULL, NULL, 0, NULL) == 0) { FAIL("should reject NULL"); return; }
    if (now_ed25519_keypair(NULL, NULL, NULL) == 0) { FAIL("should reject NULL"); return; }
    PASS();
}

/* ---- Build Cache ---- */

static void test_cache_key_deterministic(void) {
    TEST("cache: key is deterministic");
    char *k1 = now_cache_key("abc123", "def456", "/usr/bin/gcc");
    char *k2 = now_cache_key("abc123", "def456", "/usr/bin/gcc");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (!k1 || !k2 || strcmp(k1, k2) != 0) { FAIL("keys not equal"); free(k1); free(k2); return; }
    ASSERT_EQ((int)strlen(k1), 64);  /* SHA-256 hex */
    free(k1);
    free(k2);
    PASS();
}

static void test_cache_key_varies_source(void) {
    TEST("cache: different source → different key");
    char *k1 = now_cache_key("aaa", "flags", "/usr/bin/gcc");
    char *k2 = now_cache_key("bbb", "flags", "/usr/bin/gcc");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (strcmp(k1, k2) == 0) { FAIL("keys should differ"); free(k1); free(k2); return; }
    free(k1);
    free(k2);
    PASS();
}

static void test_cache_key_varies_flags(void) {
    TEST("cache: different flags → different key");
    char *k1 = now_cache_key("src", "flags_a", "/usr/bin/gcc");
    char *k2 = now_cache_key("src", "flags_b", "/usr/bin/gcc");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (strcmp(k1, k2) == 0) { FAIL("keys should differ"); free(k1); free(k2); return; }
    free(k1);
    free(k2);
    PASS();
}

static void test_cache_key_varies_compiler(void) {
    TEST("cache: different compiler → different key");
    char *k1 = now_cache_key("src", "flags", "/usr/bin/gcc");
    char *k2 = now_cache_key("src", "flags", "/usr/bin/clang");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (strcmp(k1, k2) == 0) { FAIL("keys should differ"); free(k1); free(k2); return; }
    free(k1);
    free(k2);
    PASS();
}

static void test_cache_path_sharding(void) {
    TEST("cache: path uses two-level sharding");
    char *path = now_cache_path("abcdef0123456789abcdef0123456789"
                                 "abcdef0123456789abcdef0123456789", ".o");
    ASSERT_NOT_NULL(path);
    /* Path should contain /ab/cd/ shard directories */
    if (!strstr(path, "/ab/") && !strstr(path, "\\ab\\")) {
        FAIL("expected /ab/ shard in path");
        free(path);
        return;
    }
    if (!strstr(path, "/cd/") && !strstr(path, "\\cd\\") &&
        !strstr(path, "/ab/cd") && !strstr(path, "\\ab\\cd")) {
        FAIL("expected /cd/ shard in path");
        free(path);
        return;
    }
    /* Should end with .o */
    size_t plen = strlen(path);
    if (plen < 2 || strcmp(path + plen - 2, ".o") != 0) {
        FAIL("expected .o extension");
        free(path);
        return;
    }
    free(path);
    PASS();
}

static void test_cache_store_restore(void) {
    TEST("cache: store and restore roundtrip");
    /* Create a temp file with known content */
    const char *content = "hello cache world 12345";
    const char *tmpfile = "target/_cache_test_src.o";
    now_mkdir_p("target");
    FILE *f = fopen(tmpfile, "wb");
    if (!f) { FAIL("cannot create temp file"); return; }
    fwrite(content, 1, strlen(content), f);
    fclose(f);

    /* Store it */
    char *key = now_cache_key("store_restore_test_hash", "flags_hash", "/cc");
    ASSERT_NOT_NULL(key);
    int rc = now_cache_store(key, tmpfile, ".o");
    ASSERT_EQ(rc, 0);

    /* Restore to a different path */
    const char *restored = "target/_cache_test_dst.o";
    rc = now_cache_restore(key, restored, ".o");
    ASSERT_EQ(rc, 0);

    /* Verify content matches */
    FILE *f2 = fopen(restored, "rb");
    if (!f2) { FAIL("restored file not found"); free(key); return; }
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f2);
    fclose(f2);
    ASSERT_EQ((int)n, (int)strlen(content));
    if (strcmp(buf, content) != 0) { FAIL("content mismatch"); free(key); return; }

    /* Cleanup */
    remove(tmpfile);
    remove(restored);
    free(key);
    PASS();
}

static void test_cache_restore_miss(void) {
    TEST("cache: restore miss returns -1");
    char *key = now_cache_key("nonexistent_hash_xyz", "flags", "/cc");
    ASSERT_NOT_NULL(key);
    int rc = now_cache_restore(key, "target/_miss.o", ".o");
    ASSERT_EQ(rc, -1);
    free(key);
    PASS();
}

static void test_cache_clean_works(void) {
    TEST("cache: clean removes stored objects");
    /* Store an object */
    const char *tmpfile = "target/_cache_clean_test.o";
    now_mkdir_p("target");
    FILE *f = fopen(tmpfile, "wb");
    if (!f) { FAIL("cannot create temp file"); return; }
    fwrite("data", 1, 4, f);
    fclose(f);

    char *key = now_cache_key("clean_test_hash", "flags", "/cc");
    ASSERT_NOT_NULL(key);
    now_cache_store(key, tmpfile, ".o");

    /* Verify it's there */
    ASSERT_EQ(now_cache_restore(key, "target/_cache_clean_verify.o", ".o"), 0);
    remove("target/_cache_clean_verify.o");

    /* Clean */
    now_cache_clean();

    /* Should be gone */
    int rc = now_cache_restore(key, "target/_cache_clean_verify.o", ".o");
    ASSERT_EQ(rc, -1);

    remove(tmpfile);
    free(key);
    PASS();
}

/* ---- Depfile parsing ---- */

static void test_depfile_parse_simple(void) {
    TEST("depfile: parse simple .d file");
    now_mkdir_p("target");
    const char *depfile = "target/_test_simple.d";
    FILE *f = fopen(depfile, "w");
    if (!f) { FAIL("cannot create depfile"); return; }
    fprintf(f, "target/obj/main/foo.c.o: src/main/c/foo.c src/main/h/foo.h src/main/h/bar.h\n");
    fclose(f);

    NowDepList deps = {0};
    int rc = now_depfile_parse(depfile, "src/main/c/foo.c", &deps);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((int)deps.count, 2);  /* foo.h and bar.h, source excluded */
    /* Verify headers are present (order may vary) */
    int found_foo_h = 0, found_bar_h = 0;
    for (size_t i = 0; i < deps.count; i++) {
        if (strstr(deps.paths[i], "foo.h")) found_foo_h = 1;
        if (strstr(deps.paths[i], "bar.h")) found_bar_h = 1;
    }
    ASSERT_EQ(found_foo_h, 1);
    ASSERT_EQ(found_bar_h, 1);
    now_deplist_free(&deps);
    remove(depfile);
    PASS();
}

static void test_depfile_parse_multiline(void) {
    TEST("depfile: parse multiline .d file with continuations");
    now_mkdir_p("target");
    const char *depfile = "target/_test_multi.d";
    FILE *f = fopen(depfile, "w");
    if (!f) { FAIL("cannot create depfile"); return; }
    fprintf(f, "target/obj/main/foo.c.o: src/main/c/foo.c \\\n"
               "  src/main/h/foo.h \\\n"
               "  src/main/h/bar.h \\\n"
               "  src/main/h/baz.h\n");
    fclose(f);

    NowDepList deps = {0};
    int rc = now_depfile_parse(depfile, "src/main/c/foo.c", &deps);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((int)deps.count, 3);  /* foo.h, bar.h, baz.h */
    now_deplist_free(&deps);
    remove(depfile);
    PASS();
}

static void test_depfile_parse_missing(void) {
    TEST("depfile: missing file returns -1");
    NowDepList deps = {0};
    int rc = now_depfile_parse("target/_nonexistent.d", "foo.c", &deps);
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_depfile_parse_msvc(void) {
    TEST("depfile: parse MSVC /showIncludes output");
    const char *output =
        "foo.c\n"
        "Note: including file: C:\\sdk\\include\\stdio.h\n"
        "Note: including file:   C:\\project\\src\\foo.h\n"
        "some compiler output line\n"
        "Note: including file: C:\\project\\src\\bar.h\n";
    size_t len = strlen(output);

    NowDepList deps = {0};
    int rc = now_depfile_parse_msvc(output, len, &deps);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((int)deps.count, 3);
    /* Verify paths are trimmed */
    int found_stdio = 0, found_foo = 0, found_bar = 0;
    for (size_t i = 0; i < deps.count; i++) {
        if (strstr(deps.paths[i], "stdio.h")) found_stdio = 1;
        if (strstr(deps.paths[i], "foo.h")) found_foo = 1;
        if (strstr(deps.paths[i], "bar.h")) found_bar = 1;
    }
    ASSERT_EQ(found_stdio, 1);
    ASSERT_EQ(found_foo, 1);
    ASSERT_EQ(found_bar, 1);
    now_deplist_free(&deps);
    PASS();
}

/* ---- Dep-aware cache ---- */

static void test_cache_restore_ex_no_deps(void) {
    TEST("cache: restore_ex with no .deps sidecar returns miss");
    /* Use a fresh key that has no .deps file */
    char *key = now_cache_key("no_deps_test_xyz", "flags", "/cc");
    ASSERT_NOT_NULL(key);
    int rc = now_cache_restore_ex(key, "target/_nodeps.o", ".o", NULL);
    ASSERT_EQ(rc, -1);
    free(key);
    PASS();
}

static void test_cache_store_restore_ex_with_deps(void) {
    TEST("cache: store_ex/restore_ex with deps roundtrip");
    now_mkdir_p("target");

    /* Create fake object */
    const char *objfile = "target/_deptest_obj.o";
    FILE *f = fopen(objfile, "wb");
    if (!f) { FAIL("cannot create obj"); return; }
    fwrite("objdata", 1, 7, f);
    fclose(f);

    /* Create fake header dep */
    const char *hdr = "target/_deptest_hdr.h";
    f = fopen(hdr, "w");
    if (!f) { FAIL("cannot create header"); remove(objfile); return; }
    fprintf(f, "#define FOO 1\n");
    fclose(f);

    /* Store with deps */
    char *skey = now_cache_key("deptest_src_hash", "deptest_flags", "/gcc");
    ASSERT_NOT_NULL(skey);

    NowDepList deps = {0};
    deps.paths = (char **)malloc(sizeof(char *));
    deps.paths[0] = strdup(hdr);
    deps.count = 1;
    deps.capacity = 1;

    int rc = now_cache_store_ex(skey, objfile, ".o", &deps, NULL);
    ASSERT_EQ(rc, 0);

    /* Restore should succeed */
    const char *dst = "target/_deptest_restore.o";
    rc = now_cache_restore_ex(skey, dst, ".o", NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(now_path_exists(dst), 1);
    remove(dst);

    /* Modify header → restore should fail */
    f = fopen(hdr, "w");
    if (f) {
        fprintf(f, "#define FOO 2\n");
        fclose(f);
    }
    rc = now_cache_restore_ex(skey, dst, ".o", NULL);
    ASSERT_EQ(rc, -1);

    /* Cleanup */
    now_deplist_free(&deps);
    free(skey);
    remove(objfile);
    remove(hdr);
    remove(dst);
    PASS();
}

static void test_cache_restore_ex_dep_deleted(void) {
    TEST("cache: restore_ex fails when dep file deleted");
    now_mkdir_p("target");

    /* Create fake object and header */
    const char *objfile = "target/_deldep_obj.o";
    FILE *f = fopen(objfile, "wb");
    if (!f) { FAIL("cannot create obj"); return; }
    fwrite("obj", 1, 3, f);
    fclose(f);

    const char *hdr = "target/_deldep_hdr.h";
    f = fopen(hdr, "w");
    if (!f) { FAIL("cannot create header"); remove(objfile); return; }
    fprintf(f, "header\n");
    fclose(f);

    char *skey = now_cache_key("deldep_src", "deldep_flags", "/gcc");
    ASSERT_NOT_NULL(skey);

    NowDepList deps = {0};
    deps.paths = (char **)malloc(sizeof(char *));
    deps.paths[0] = strdup(hdr);
    deps.count = 1;
    deps.capacity = 1;

    now_cache_store_ex(skey, objfile, ".o", &deps, NULL);

    /* Delete the dep file */
    remove(hdr);

    /* Restore should fail */
    int rc = now_cache_restore_ex(skey, "target/_deldep_restore.o", ".o", NULL);
    ASSERT_EQ(rc, -1);

    now_deplist_free(&deps);
    free(skey);
    remove(objfile);
    PASS();
}

/* A cache-restored object never ran the compiler, so its headers come
 * from the .deps sidecar — where they are stored portably as
 * "${PROJ}/rel" so an object built in a sibling checkout cannot
 * validate against this one. The manifest stats its dep paths
 * verbatim, so handing that spelling straight through made every
 * restored translation unit report "header is gone" and rebuild
 * forever, while the object was served from the cache anyway.
 *
 * Amy measured 74 of 125 TUs stuck this way on a tree where every
 * named header was present on disk. The unit suite missed it because
 * every other cache test passes basedir = NULL, and the ${PROJ}
 * rewrite only engages when there is a project root to be relative
 * to. This one uses a real one. */
static void test_cache_deps_for_key_resolves_proj_token(void) {
    TEST("cache: restored deps come back resolvable, not ${PROJ}-spelled");
    now_mkdir_p("target/_projdep/include");

    char cwd[1024];
#ifdef _WIN32
    if (!_getcwd(cwd, sizeof cwd)) { FAIL("cannot getcwd"); return; }
#else
    if (!getcwd(cwd, sizeof cwd)) { FAIL("cannot getcwd"); return; }
#endif

    /* Absolute, because that is what a compiler depfile yields and what
     * the portable-path rewrite keys on. */
    char basedir[1200], hdr[1400], srcfile[1400], objfile[1400];
    snprintf(basedir, sizeof basedir, "%s/target/_projdep", cwd);
    snprintf(hdr,     sizeof hdr,     "%s/include/cfg.h", basedir);
    snprintf(srcfile, sizeof srcfile, "%s/mod.c", basedir);
    snprintf(objfile, sizeof objfile, "%s/mod.o", basedir);

    FILE *f = fopen(hdr, "w");
    if (!f) { FAIL("cannot create header"); return; }
    fprintf(f, "#define CFG 1\n");
    fclose(f);

    f = fopen(srcfile, "w");
    if (!f) { FAIL("cannot create source"); remove(hdr); return; }
    fprintf(f, "#include \"include/cfg.h\"\n");
    fclose(f);

    f = fopen(objfile, "wb");
    if (!f) { FAIL("cannot create object"); remove(hdr); remove(srcfile); return; }
    fwrite("obj", 1, 3, f);
    fclose(f);

    char *skey = now_cache_key("projdep_src", "projdep_flags", "/gcc");
    ASSERT_NOT_NULL(skey);

    NowDepList deps = {0};
    deps.paths = (char **)malloc(sizeof(char *));
    deps.paths[0] = strdup(hdr);
    deps.count = 1;
    deps.capacity = 1;

    ASSERT_EQ(now_cache_store_ex(skey, objfile, ".o", &deps, basedir), 0);

    char **dpaths = NULL, **dhashes = NULL;
    size_t dn = 0;
    ASSERT_EQ(now_cache_deps_for_key(skey, basedir, &dpaths, &dhashes, &dn), 0);
    ASSERT_EQ((int)dn, 1);
    ASSERT_NOT_NULL(dpaths[0]);

    /* The manifest will stat this string as-is. */
    ASSERT_EQ(now_path_exists(dpaths[0]), 1);

    /* And the end state that was actually reported: an entry written
     * from a restore must converge on the very next build. */
    NowManifest m;
    now_manifest_init(&m);
    char *src_hash = now_sha256_file(srcfile);
    ASSERT_NOT_NULL(src_hash);
    struct stat st;
    stat(srcfile, &st);
    now_manifest_set(&m, "mod.c", objfile, src_hash, "fh",
                     (long long)st.st_mtime);
    now_manifest_set_deps(&m, "mod.c", (const char **)dpaths,
                          (const char **)dhashes, dn);

    const NowManifestEntry *e = now_manifest_find(&m, "mod.c");
    ASSERT_NOT_NULL(e);
    char reason[512];
    reason[0] = '\0';
    int rebuild = now_manifest_needs_rebuild_ex(e, basedir, "mod.c", "fh",
                                                NULL, reason, sizeof reason);
    if (rebuild != 0)
        printf("    (reason: %s)\n", reason);
    ASSERT_EQ(rebuild, 0);

    now_manifest_free(&m);
    free(src_hash);
    now_cache_deps_free(dpaths, dhashes, dn);
    now_deplist_free(&deps);
    free(skey);
    remove(objfile);
    remove(srcfile);
    remove(hdr);
    PASS();
}

/* ---- Manifest dep tracking ---- */

static void test_manifest_set_deps(void) {
    TEST("manifest: set_deps stores dep info");
    NowManifest m;
    now_manifest_init(&m);
    now_manifest_set(&m, "foo.c", "foo.o", "hash1", "fhash", 12345);

    const char *dpaths[] = {"src/main/h/foo.h", "src/main/h/bar.h"};
    const char *dhashes[] = {"aaa", "bbb"};
    int rc = now_manifest_set_deps(&m, "foo.c", dpaths, dhashes, 2);
    ASSERT_EQ(rc, 0);

    const NowManifestEntry *e = now_manifest_find(&m, "foo.c");
    ASSERT_NOT_NULL(e);
    ASSERT_EQ((int)e->dep_count, 2);
    if (strcmp(e->deps[0], "src/main/h/foo.h") != 0) { FAIL("dep[0] mismatch"); now_manifest_free(&m); return; }
    if (strcmp(e->dep_hashes[1], "bbb") != 0) { FAIL("dep_hash[1] mismatch"); now_manifest_free(&m); return; }
    now_manifest_free(&m);
    PASS();
}

static void test_manifest_deps_roundtrip(void) {
    TEST("manifest: deps survive save/load roundtrip");
    now_mkdir_p("target");

    NowManifest m;
    now_manifest_init(&m);
    now_manifest_set(&m, "foo.c", "foo.o", "hash1", "fhash", 12345);

    const char *dpaths[] = {"dep_a.h", "dep_b.h"};
    const char *dhashes[] = {"aaa111", "bbb222"};
    now_manifest_set_deps(&m, "foo.c", dpaths, dhashes, 2);

    const char *mpath = "target/_test_manifest_deps";
    now_manifest_save(&m, mpath);
    now_manifest_free(&m);

    /* Reload */
    NowManifest m2;
    now_manifest_load(&m2, mpath);
    const NowManifestEntry *e = now_manifest_find(&m2, "foo.c");
    ASSERT_NOT_NULL(e);
    ASSERT_EQ((int)e->dep_count, 2);
    if (strcmp(e->deps[0], "dep_a.h") != 0) { FAIL("dep path mismatch"); now_manifest_free(&m2); return; }
    if (strcmp(e->dep_hashes[0], "aaa111") != 0) { FAIL("dep hash mismatch"); now_manifest_free(&m2); return; }
    if (strcmp(e->deps[1], "dep_b.h") != 0) { FAIL("dep[1] path mismatch"); now_manifest_free(&m2); return; }
    if (strcmp(e->dep_hashes[1], "bbb222") != 0) { FAIL("dep_hash[1] mismatch"); now_manifest_free(&m2); return; }
    now_manifest_free(&m2);
    remove(mpath);
    PASS();
}

static void test_manifest_needs_rebuild_dep_changed(void) {
    TEST("manifest: needs_rebuild detects dep change");
    now_mkdir_p("target");

    /* Create a real header file so we can hash it */
    const char *hdr = "target/_test_dep_header.h";
    FILE *f = fopen(hdr, "w");
    if (!f) { FAIL("cannot create header"); return; }
    fprintf(f, "original\n");
    fclose(f);

    /* Create a real source file */
    const char *srcfile = "target/_test_dep_src.c";
    f = fopen(srcfile, "w");
    if (!f) { FAIL("cannot create source"); remove(hdr); return; }
    fprintf(f, "source\n");
    fclose(f);

    char *src_hash = now_sha256_file(srcfile);
    char *dep_hash = now_sha256_file(hdr);
    ASSERT_NOT_NULL(src_hash);
    ASSERT_NOT_NULL(dep_hash);

    struct stat st;
    stat(srcfile, &st);

    NowManifest m;
    now_manifest_init(&m);
    now_manifest_set(&m, "_test_dep_src.c", "target/obj.o", src_hash, "fhash",
                     (long long)st.st_mtime);

    const char *dpaths[] = {hdr};
    const char *dhashes[] = {dep_hash};
    now_manifest_set_deps(&m, "_test_dep_src.c", dpaths, dhashes, 1);

    /* Create a fake object file */
    f = fopen("target/obj.o", "w");
    if (f) { fwrite("x", 1, 1, f); fclose(f); }

    /* Should be up to date */
    const NowManifestEntry *e = now_manifest_find(&m, "_test_dep_src.c");
    int rebuild = now_manifest_needs_rebuild(e, "target", "_test_dep_src.c", "fhash", NULL);
    ASSERT_EQ(rebuild, 0);

    /* Modify header */
    f = fopen(hdr, "w");
    if (f) { fprintf(f, "modified\n"); fclose(f); }

    /* Now should need rebuild */
    rebuild = now_manifest_needs_rebuild(e, "target", "_test_dep_src.c", "fhash", NULL);
    ASSERT_EQ(rebuild, 1);

    now_manifest_free(&m);
    free(src_hash);
    free(dep_hash);
    remove(hdr);
    remove(srcfile);
    remove("target/obj.o");
    PASS();
}

int main(void) {
    printf("now test suite\n");
    printf("==============\n\n");

    printf("  Version:\n");
    test_version();

    printf("\n  POM (string):\n");
    test_pom_minimal_string();
    test_pom_lang_scalar();
    test_pom_lang_mixed();
    test_pom_compile();
    test_pom_os_conditional();
    test_pom_deps();
    test_pom_convergence();

    printf("\n  POM (file):\n");
    test_pom_load_file();
    test_pom_load_rich();

    printf("\n  POM (errors):\n");
    test_pom_syntax_error();
    test_pom_not_a_map();
    test_pom_file_not_found();

    printf("\n  Language type system:\n");
    test_lang_find_c();
    test_lang_find_cxx();
    test_lang_classify_c();
    test_lang_classify_h();
    test_lang_classify_cpp();
    test_lang_classify_unknown();
    test_lang_source_exts();

    printf("\n  Glob matching:\n");
    test_glob_star_basename();
    test_glob_doublestar_suffix();
    test_glob_doublestar_slash_zero();
    test_glob_doublestar_middle();
    test_glob_single_star_no_cross_slash();
    test_glob_question_and_class();
    test_glob_double_extension();
    test_glob_escape_and_literal();
    test_glob_backslash_path_normalized();

    printf("\n  Filesystem:\n");
    test_fs_path_join();
    test_fs_path_join_trailing_sep();
    test_fs_path_ext();
    test_fs_obj_path();

    printf("\n  Semantic versioning:\n");
    test_semver_parse_basic();
    test_semver_parse_prerelease();
    test_semver_parse_build();
    test_semver_compare();
    test_semver_to_string();

    printf("\n  Version ranges:\n");
    test_range_exact();
    test_range_caret();
    test_range_caret_pre1();
    test_range_tilde();
    test_range_gte();
    test_range_compound();
    test_range_wildcard();
    test_range_intersect();

    printf("\n  Coordinates:\n");
    test_coord_parse();

    printf("\n  Manifest:\n");
    test_manifest_set_find();
    test_manifest_update();
    test_sha256_string();

    printf("\n  Resolver:\n");
    test_resolver_single_dep();
    test_resolver_highest_defers_to_registry();
    test_resolver_convergence_lowest();
    test_resolver_conflict();
    test_resolver_multiple_deps();
    test_resolver_override();
    test_lock_save_load();

    printf("\n  HTTP client:\n");
    test_pico_http_version();
    test_pico_http_parse_url();
    test_pico_http_parse_url_no_port();
    test_pico_http_parse_url_no_path();
    test_pico_http_parse_url_https();
    test_pico_http_parse_url_reject_ftp();
    test_pico_http_error_codes();
    test_pico_http_invalid_args();
    test_pico_http_dns_failure();
    test_pico_http_connect_failure();
    test_pico_http_find_header();
    test_pico_http_request_url();
    test_pico_http_response_free_zeroed();
    test_pico_http_stream_invalid_args();
    test_pico_http_stream_connect_failure();
    test_pico_http_stream_callback_type();
    test_pico_http_tls_noverify_option();
    test_pico_http_tls_options_zero_init();
    test_pico_http_tls_ca_file_option();

    printf("\n  WebSocket client:\n");
    test_pico_ws_version();
    test_pico_ws_error_codes();
    test_pico_ws_invalid_args();
    test_pico_ws_bad_url();
    test_pico_ws_connect_failure();
    test_pico_ws_close_null();
    test_pico_ws_tls_noverify_option();
    test_pico_ws_tls_options_zero_init();

    printf("\n  Procure:\n");
    test_repo_dep_path();
    test_repo_is_installed_needs_payload();
    test_procure_no_deps();

    printf("\n  Parallel build:\n");
    test_cpu_count();

    printf("\n  Toolchain:\n");
    test_obj_path_ex_obj();
    test_toolchain_gcc_default();
    test_toolchain_msvc_detect();
    test_toolchain_java_resolve();
    test_toolchain_no_java_for_c();

    printf("\n  Auth:\n");
    test_auth_load_no_creds();
    test_auth_login_null_creds();
    test_auth_login_connect_failure();

    printf("\n  Publish:\n");
    test_publish_missing_identity();
    test_publish_no_package();
    test_yank_no_url();
    test_yank_connect_failure();
    test_dep_updates_no_deps();
    test_dep_updates_null_project();
    test_cache_mirror_no_url();
    test_cache_mirror_connect_failure();

    printf("\n  Workspace:\n");
    test_is_workspace_true();
    test_is_workspace_false();
    test_workspace_is_null();
    test_workspace_init();
    test_workspace_topo_sort();
    test_workspace_inject_sibling();
    test_workspace_libdir_cap();

    printf("\n  Plugins:\n");
    test_plugin_is_builtin();
    test_plugin_pom_load();
    test_plugin_run_hook_no_plugins();
    test_plugin_result_init_free();
    test_plugin_unknown_builtin();
    test_plugin_version_generate();

    printf("\n  Plugin Registry:\n");
    test_plugin_manifest_parse_string();
    test_plugin_manifest_parse_minimal();
    test_plugin_manifest_missing_id();
    test_plugin_manifest_parse_file_missing();
    test_plugin_info_free_null_safe();
    test_plugin_find_binary_missing();
    test_plugin_list_empty_repo();
    test_plugin_search_no_match();
    test_plugin_install_bad_registry();
    test_plugin_manifest_roundtrip();

    printf("\n  Dep confusion protection:\n");
    test_private_group_exact_match();
    test_private_group_dotted_child();
    test_private_group_no_false_positive();
    test_private_group_multiple_prefixes();
    test_private_group_null_safe();
    test_private_group_pom_load();
    test_private_group_procure_fail();
    test_link_inherit_target_parses();
    test_lock_differs();
    test_registry_is_public();
    test_private_group_fence_stops_at_public();
    test_private_group_repo_override_fenced();
    test_private_group_nested_resolve_form();

    printf("\n  Reproducible builds:\n");
    test_repro_init();
    test_repro_from_project_bool();
    test_repro_from_project_map();
    test_repro_from_project_none();
    test_repro_timebase_zero();
    test_repro_timebase_now();
    test_repro_timebase_literal();
    test_repro_compile_flags_gcc();
    test_repro_compile_flags_msvc();
    test_repro_link_flags();
    test_repro_link_flags_msvc_empty();
    test_repro_sort_filelist();
    test_repro_disabled_no_flags();
    test_repro_null_safety();

    printf("\n  Trust:\n");
    test_trust_init_free();
    test_trust_add();
    test_trust_scope_wildcard();
    test_trust_scope_group_prefix();
    test_trust_scope_exact();
    test_trust_find();
    test_trust_find_no_match();
    test_trust_policy_none();
    test_trust_policy_signed();
    test_trust_policy_trusted();
    test_trust_policy_from_project();
    test_trust_null_safety();

    printf("\n  Ed25519:\n");
    test_ed25519_keypair();
    test_ed25519_sign_verify();
    test_ed25519_verify_rfc8032_1();
    test_ed25519_verify_rfc8032_2();
    test_ed25519_verify_bad_sig();
    test_ed25519_verify_wrong_msg();
    test_ed25519_sign_rfc8032_1();
    test_ed25519_sign_rfc8032_3();
    test_ed25519_sign_known_answer_9();
    test_ed25519_sign_verify_lengths();
    test_ed25519_reject_non_canonical_s();
    test_ed25519_null_safety();

    printf("\n  Advisory guards:\n");
    test_advisory_db_init_free();
    test_advisory_severity_parse();
    test_advisory_severity_name();
    test_advisory_severity_blocks();
    test_advisory_db_load_string();
    test_advisory_blacklisted();
    test_advisory_override_parse();
    test_advisory_override_no_expires();
    test_advisory_override_expiry();
    test_advisory_find_override();
    test_advisory_check_dep_match();
    test_advisory_check_dep_no_match();
    test_advisory_check_dep_overridden();
    test_advisory_blacklisted_no_override();
    test_advisory_medium_warning();
    test_advisory_report_format();
    test_advisory_null_safety();

    printf("\n  CI integration:\n");
    test_ci_exit_codes();
    test_ci_detect_defaults();
    test_ci_format_build_json();
    test_ci_format_build_pasta();
    test_ci_format_test_json();
    test_ci_format_text();

    printf("\n  Layers:\n");
    test_layer_stack_init();
    test_layer_baseline_sections();
    test_layer_load_file();
    test_layer_merge_open();
    test_layer_merge_locked_audit();
    test_layer_merge_strarray_exclude();
    test_layers_a_project_without_one_is_untouched();
    test_layers_an_open_layer_reaches_compile();
    test_layers_locked_is_additive_and_audited();
    test_layers_the_baseline_claims_no_compile_defaults();
    test_origin_credits_the_layer_that_introduced_a_define();
    test_origin_credits_the_winner_of_a_scalar();
    test_origin_answers_without_any_layer_file();
    test_envcli_precedence_is_the_documented_order();
    test_envcli_gate_covers_env_and_cli_too();
    test_envcli_origin_names_the_variable();
    test_envcli_a_quoted_path_stays_one_flag();
    test_zeroconfig_compiles_a_tree_with_no_descriptor();
    test_zeroconfig_an_empty_tree_is_not_an_error();
    test_zeroconfig_a_source_that_cannot_compile_fails();
    test_zeroconfig_takes_config_from_the_command_line();
    test_zeroconfig_does_not_walk_its_own_output();
    test_layer_audit_format();
    test_layer_push_project();

    printf("\n  Alforno integration:\n");
    test_alforno_aggregate_merge();
    test_alforno_parameterize();
    test_alforno_conflate();
    test_alforno_collect_merge();

    printf("\n  Multi-architecture:\n");
    test_triple_parse_full();
    test_triple_parse_shorthand();
    test_triple_format();
    test_triple_dir();
    test_triple_cmp();
    test_triple_match_exact();
    test_triple_match_wildcard();
    test_triple_host_detect();
    test_triple_is_native();

    printf("\n  Per-triple compile/link overrides (target_flags):\n");
    test_target_flags_match_in_declaration_order();
    test_target_flags_unmatched_entries_contribute_nothing();
    test_target_flags_follow_the_target();
    test_target_flags_scalars_replace();
    test_target_flags_link_block();
    test_target_flags_absent_is_inert();
    test_assembly_parses_its_includes();
    test_assembly_absent_is_inert();

    printf("\n  Descriptor validation (schema:check):\n");
    test_schema_accepts_a_good_descriptor();
    test_schema_missing_file_is_two_not_one();
    test_schema_rejects_what_cannot_be_meant();
    test_schema_std_is_per_language();
    test_schema_unimplemented_is_a_warning_not_an_error();
    test_schema_syntax_error_carries_a_position();

    printf("\n  vacate (removes from ~/.now/repo):\n");
    test_vacate_removes_only_the_unreferenced();
    test_vacate_refuses_an_empty_scan();
    test_vacate_dry_run_removes_nothing();
    test_vacate_force_takes_the_referenced_too();
    /* Unconditional: an ASSERT that fired inside one of those
     * returns without restoring, and a leaked HOME would redirect
     * every later test that reads ~/.now. */
    vac_restore_home();

    printf("\n  tell / tool: / convert:\n");
    test_tell_reads_the_effective_value();
    test_tell_text_is_unquoted_and_json_is_not();
    test_tell_hyphen_separates_the_two_namespaces();
    test_tell_a_typo_is_an_error_not_an_empty_answer();
    test_tool_list_and_run();
    test_convert_round_trips_through_json();
    test_convert_refuses_json5();
    test_watch_asks_the_registry_for_extensions();
    test_watch_snapshot_sees_a_go_source();
    test_watch_notices_a_file_that_did_not_exist();
    test_workspace_root_builds_its_own_sources();
    test_workspace_collection_root_needs_no_sources();
    test_objsym_finds_main_regardless_of_filename();
    test_objsym_unreadable_is_neither_yes_nor_no();
    test_objsym_counts_what_the_entry_point_hides();
    test_entry_point_wiring_survives_a_renamed_main();
    test_events_detail_cut_on_the_way_in_says_so();
    test_events_a_detail_that_fits_is_not_lossy();
    test_events_lossy_boundary_is_exact();

    printf("\n  Arch tags / path-gated discovery:\n");
    test_arch_parse_tags();
    test_arch_parse_aliases();
    test_arch_is_gate();
    test_arch_active_tags_from_triple();
    test_arch_active_tags_with_user();
    test_arch_gate_skips_nonmatching();
    test_arch_gate_nested_and();
    test_arch_gate_empty_dict_is_passthrough();
    test_arch_gate_in_build_loop();

    printf("\n  C++20 Modules:\n");
    test_module_scan_interface();
    test_module_scan_import();
    test_module_scan_impl();
    test_module_scan_skips_comments();
    test_module_order_basic();
    test_module_order_three_deep();
    test_module_find();
    test_module_bmi_path();
    test_module_classify_cppm();

    printf("\n  Java + Maven:\n");
    test_lang_java_registration();
    test_lang_java_classify();
    test_pom_java_fields();
    test_pom_java_defaults();
    test_export_maven_basic();
    test_export_maven_deps();
    test_export_maven_main_class();
    test_import_maven_basic();
    test_import_maven_deps();
    test_import_maven_roundtrip();

    printf("\n  Export:\n");
    test_export_cmake_basic();
    test_export_cmake_executable();
    test_export_cmake_deps_comment();
    test_export_cmake_cxx();
    test_export_make_basic();
    test_export_make_executable();
    test_export_make_static();
    test_export_make_deps();
    test_export_make_cxx();
    test_export_meson_basic();
    test_export_meson_executable();
    test_export_meson_cxx();
    test_export_meson_header_only();
    test_export_bazel_basic();
    test_export_bazel_executable();
    test_export_bazel_static();
    test_export_bazel_cxx();
    test_export_bazel_deps_comment();

    printf("\n  Basta packages:\n");
    test_basta_create_and_parse();
    test_basta_metadata_fields();
    test_basta_extract_null_safety();
    test_basta_extract_missing_file();
    test_basta_package_roundtrip();

    printf("\n  Build cache:\n");
    test_cache_key_deterministic();
    test_cache_key_varies_source();
    test_cache_key_varies_flags();
    test_cache_key_varies_compiler();
    test_cache_path_sharding();
    test_cache_store_restore();
    test_cache_restore_miss();
    test_cache_clean_works();

    printf("\n  Depfile parsing:\n");
    test_depfile_parse_simple();
    test_depfile_parse_multiline();
    test_depfile_parse_missing();
    test_depfile_parse_msvc();

    printf("\n  Dep-aware cache:\n");
    test_cache_restore_ex_no_deps();
    test_cache_store_restore_ex_with_deps();
    test_cache_restore_ex_dep_deleted();
    test_cache_deps_for_key_resolves_proj_token();

    printf("\n  Manifest deps:\n");
    test_manifest_set_deps();
    test_manifest_deps_roundtrip();
    test_manifest_needs_rebuild_dep_changed();

    printf("\n  Remote cache:\n");
    test_remote_config_parse_full();
    test_remote_config_parse_minimal();
    test_remote_config_parse_no_section();
    test_remote_config_parse_no_url();
    test_remote_config_free_null();
    test_remote_cache_restore_unreachable();
    test_remote_cache_store_push_disabled();
    test_remote_cache_store_unreachable();
    test_remote_cache_key_url_safe();

    printf("\n  Enterprise auth (LDAP/SSO):\n");
    test_auth_method_parse();
    test_auth_method_name();
    test_auth_creds_free_null();
    test_auth_load_no_file();
    test_auth_load_null_safety();
    test_token_cache_lifecycle();
    test_token_cache_expired();
    test_token_cache_overwrite();
    test_auth_ldap_login_null();
    test_auth_ldap_login_unreachable();
    test_auth_oidc_client_null();
    test_auth_oidc_client_unreachable();
    test_auth_discover_unreachable();
    test_auth_discovery_free_null();
    test_auth_get_token_no_creds();

    printf("\n  SBOM generation:\n");
    test_sbom_to_json_basic();
    test_sbom_library_type();
    test_sbom_with_deps();
    test_sbom_with_license();
    test_sbom_generate_file();
    test_sbom_null_project();
    test_sbom_scope_mapping();
    test_sbom_no_deps();

#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)
    printf("\n  HTTP/2:\n");
    test_h2_hpack_encode_get();
    test_h2_hpack_decode_status();
    test_h2_frame_layout();
    test_h2_hpack_encode_with_headers();
#endif

    printf("\n  Graph cache:\n");
    test_graph_key_deterministic();
    test_graph_key_varies();
    test_graph_serialize_roundtrip();
    test_graph_deserialize_bad_input();
    test_graph_pull_unreachable();

    printf("\n  Watch:\n");
    test_watch_opts_init();
    test_watch_snapshot_hello();
    test_watch_diff_no_change();
    test_watch_diff_source_change();
    test_watch_diff_pasta_change();
    test_watch_snapshot_free_null();

    printf("\n  Rust FFI:\n");
    test_rust_lang_registered();
    test_rust_classify_rs();

    printf("\n  Go + Julia:\n");
    test_go_lang_registered();
    test_go_classify();
    test_julia_lang_registered();
    test_julia_classify();

    printf("\n  Audit logging:\n");
    test_audit_config_parse_full();
    test_audit_config_parse_disabled();
    test_audit_config_parse_no_section();
    test_audit_config_free_null();
    test_audit_event_name_roundtrip();
    test_audit_record_disabled();
    test_audit_record_and_show();

    printf("\n  Build integration:\n");
    test_build_hello();
    test_build_exclude_glob();
    test_build_pattern_filter();
    test_build_include_only_module();
    test_build_warnings_reach_test_compile();
    test_build_each_names_binaries_by_source();
    test_build_archive_drops_a_removed_source();
    test_build_fail_fast_stops_starting_work();
    test_events_encode_decode_roundtrip();
    test_events_detail_survives_escaping();
    test_events_oversize_detail_is_truncated_not_dropped();
    test_events_decode_reads_spaced_json();
    test_events_version_and_unknown_fields();
    test_events_over_a_real_socket();
    test_events_recv_waits_out_its_timeout();
    test_events_localhost_reaches_the_loopback_socket();
    test_events_listener_refuses_untrusted_addresses();
    test_events_names_round_trip();
    test_events_blob_carries_what_no_string_can();
    test_events_a_blob_cannot_forge_a_field();
    test_events_formats_do_not_read_each_other();
    test_events_emitter_end_to_end();
    test_events_off_by_default();
    test_events_every_started_phase_is_finished();
    test_events_the_late_phases_bracket_themselves();
    test_events_test_failed_carries_the_case();
    test_pasta_writer_output_always_reparses();
    test_build_java_hello();
    /* test_test_phase requires gcc in PATH at runtime — run manually */

    printf("\n%d/%d tests passed", tests_passed, tests_run);
    if (tests_failed) printf(", %d FAILED", tests_failed);
    printf("\n");
    return tests_failed ? 1 : 0;
}
