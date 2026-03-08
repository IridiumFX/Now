/*
 * main.c — now CLI entry point
 *
 * Usage:
 *   now build          procure → build → link
 *   now compile        procure → compile only
 *   now link           link only (no rebuild)
 *   now test           procure → build → link → test
 *   now procure        dependency resolution only
 *   now publish        package → publish to registry
 *   now clean          delete target/
 *   now version        print version
 */

#include "now_pom.h"
#include "now.h"
#include "now_fs.h"
#include "now_procure.h"
#include "now_package.h"
#include "now_plugin.h"
#include "now_ci.h"
#include "now_workspace.h"
#include "now_arch.h"
#include "now_layer.h"
#include "now_export.h"
#include "now_trust.h"
#include "now_repro.h"
#include "now_advisory.h"
#include "pasta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <direct.h>
  #define getcwd_compat _getcwd
#else
  #include <unistd.h>
  #define getcwd_compat getcwd
#endif

static void usage(void) {
    fprintf(stderr,
        "now %s — native build tool for C/C++\n\n"
        "Usage: now <phase> [options]\n\n"
        "Phases:\n"
        "  build      Procure deps, generate, compile sources, link output\n"
        "  compile    Procure deps, generate, compile only (no link)\n"
        "  generate   Run generate-phase plugins only\n"
        "  link       Link only (no compile)\n"
        "  test       Build and run tests\n"
        "  procure    Resolve and download dependencies\n"
        "  package    Assemble distributable archive in target/pkg/\n"
        "  install    Install to local repo (~/.now/repo/)\n"
        "  publish    Upload package to remote registry\n"
        "  export:cmake Generate CMakeLists.txt from now.pasta\n"
        "  export:make  Generate Makefile from now.pasta\n"
        "  layers:show  Show layer stack and effective configuration\n"
        "  trust:list   List trusted keys\n"
        "  trust:add    Add key: trust:add <scope> <key> [comment]\n"
        "  verify       Verify archive signature\n"
        "  reproducible:check  Build twice and compare output hashes\n"
        "  advisory:check      Check deps against advisory database\n"
        "  advisory:update     Fetch latest advisory database\n"
        "  layers:audit Report advisory lock violations\n"
        "  ci         Build, test, report (CI mode with structured output)\n"
        "  clean      Delete target/ directory\n"
        "  version    Print version and exit\n\n"
        "Options:\n"
        "  -v              Verbose output\n"
        "  -j N            Parallel jobs (default: CPU count)\n"
        "  --output FMT    Output format: text, json, pasta\n"
        "  --locked        Fail if lock file is inconsistent\n"
        "  --offline       No network access\n"
        "  --target TRIPLE Target platform triple (os:arch:variant)\n"
        "  --no-color      Disable ANSI colors\n"
        "  -h              Show this help\n",
        now_version());
}

/* Recursively delete a directory tree */
static int rmdir_recursive(const char *path) {
#ifdef _WIN32
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", path);
    return system(cmd);
#else
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    return system(cmd);
#endif
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *phase = argv[1];
    int verbose = 0;
    int jobs = 0;  /* 0 = auto (CPU count) */
    const char *repo_url = NULL;
    const char *output_fmt = NULL;
    int flag_locked = 0;
    int flag_offline = 0;
    int flag_no_color = 0;
    const char *target_str = NULL;

    /* Check for flags in remaining args */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            verbose = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
        else if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc)
            repo_url = argv[++i];
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            output_fmt = argv[++i];
        else if (strcmp(argv[i], "--locked") == 0)
            flag_locked = 1;
        else if (strcmp(argv[i], "--offline") == 0)
            flag_offline = 1;
        else if (strcmp(argv[i], "--no-color") == 0)
            flag_no_color = 1;
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc)
            target_str = argv[++i];
        else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
            jobs = atoi(argv[++i]);
            if (jobs < 1) jobs = 1;
        }
        else if (strncmp(argv[i], "-j", 2) == 0 && argv[i][2] >= '1' && argv[i][2] <= '9') {
            jobs = atoi(argv[i] + 2);
            if (jobs < 1) jobs = 1;
        }
    }

    /* Handle version and help */
    if (strcmp(phase, "version") == 0 || strcmp(phase, "--version") == 0) {
        printf("now %s\n", now_version());
        return 0;
    }
    if (strcmp(phase, "-h") == 0 || strcmp(phase, "--help") == 0
        || strcmp(phase, "help") == 0) {
        usage();
        return 0;
    }

    /* Handle clean */
    if (strcmp(phase, "clean") == 0) {
        char cwd[512];
        if (!getcwd_compat(cwd, sizeof(cwd))) {
            fprintf(stderr, "error: cannot determine working directory\n");
            return 1;
        }
        char *target = now_path_join(cwd, "target");
        if (target && now_path_exists(target)) {
            if (verbose) fprintf(stderr, "cleaning %s\n", target);
            rmdir_recursive(target);
        }
        free(target);
        return 0;
    }

    /* Trust store commands (no project file needed) */
    if (strcmp(phase, "trust:list") == 0) {
        NowTrustStore store;
        now_trust_init(&store);
        NowResult result;
        memset(&result, 0, sizeof(result));
        int rc = now_trust_load(&store, &result);
        if (rc != 0) {
            fprintf(stderr, "error: %s\n", result.message);
            now_trust_free(&store);
            return 1;
        }
        if (store.count == 0) {
            printf("No trusted keys. Use 'now trust:add' to add keys.\n");
        } else {
            printf("Trusted keys (%zu):\n\n", store.count);
            for (size_t i = 0; i < store.count; i++) {
                printf("  [%zu] scope: %-20s key: %.16s...",
                       i, store.keys[i].scope, store.keys[i].key);
                if (store.keys[i].comment)
                    printf("  (%s)", store.keys[i].comment);
                printf("\n");
            }
        }
        now_trust_free(&store);
        return 0;
    }

    if (strcmp(phase, "trust:add") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: now trust:add <scope> <key> [comment]\n");
            return 1;
        }
        const char *scope = argv[2];
        const char *key = argv[3];
        const char *comment = (argc > 4) ? argv[4] : NULL;

        NowTrustStore store;
        now_trust_init(&store);
        NowResult result;
        memset(&result, 0, sizeof(result));

        now_trust_load(&store, &result);
        int rc = now_trust_add(&store, scope, key, comment);
        if (rc != 0) {
            fprintf(stderr, "error: failed to add key\n");
            now_trust_free(&store);
            return 1;
        }
        rc = now_trust_save(&store, &result);
        if (rc != 0) {
            fprintf(stderr, "error: %s\n", result.message);
            now_trust_free(&store);
            return 1;
        }
        printf("added key for scope '%s'\n", scope);
        now_trust_free(&store);
        return 0;
    }

    /* All other phases need a project descriptor */
    char cwd[512];
    if (!getcwd_compat(cwd, sizeof(cwd))) {
        fprintf(stderr, "error: cannot determine working directory\n");
        return 1;
    }

    /* Find now.pasta in current directory */
    char *descriptor = now_path_join(cwd, "now.pasta");
    if (!descriptor || !now_path_exists(descriptor)) {
        free(descriptor);
        fprintf(stderr, "error: no now.pasta found in %s\n", cwd);
        return 3;
    }

    NowResult result;
    memset(&result, 0, sizeof(result));

    NowProject *project = now_project_load(descriptor, &result);
    free(descriptor);
    if (!project) {
        fprintf(stderr, "error: %s\n", result.message);
        return 3;
    }

    int rc = 0;

    if (strcmp(phase, "procure") == 0) {
        NowProcureOpts opts = {0};
        rc = now_procure(project, &opts, &result);
        if (rc != 0)
            fprintf(stderr, "error: %s\n", result.message);
        else if (verbose)
            fprintf(stderr, "procure: done\n");

    } else if (strcmp(phase, "generate") == 0) {
        NowPluginResult gen;
        now_plugin_result_init(&gen);
        rc = now_plugin_run_hook(project, cwd, NOW_HOOK_GENERATE,
                                  verbose, &gen, &result);
        if (rc != 0)
            fprintf(stderr, "error: %s\n", result.message);
        else if (verbose)
            fprintf(stderr, "generate: %zu source(s) produced\n",
                    gen.sources.count);
        now_plugin_result_free(&gen);

    } else if (strcmp(phase, "build") == 0) {
        if (now_is_workspace(project)) {
            NowWorkspace ws;
            rc = now_workspace_init(&ws, project, cwd, &result);
            if (rc == 0)
                rc = now_workspace_build(&ws, verbose, jobs, &result);
            now_workspace_free(&ws);
        } else {
            rc = now_build(project, cwd, verbose, jobs, &result);
        }
        if (rc != 0)
            fprintf(stderr, "error: %s\n", result.message);

    } else if (strcmp(phase, "compile") == 0) {
        rc = now_compile(project, cwd, verbose, jobs, &result);
        if (rc != 0)
            fprintf(stderr, "error: %s\n", result.message);

    } else if (strcmp(phase, "test") == 0) {
        rc = now_test(project, cwd, verbose, jobs, &result);
        if (rc != 0)
            fprintf(stderr, "error: %s\n", result.message);

    } else if (strcmp(phase, "package") == 0) {
        /* Build first, then package */
        rc = now_build(project, cwd, verbose, jobs, &result);
        if (rc != 0) {
            fprintf(stderr, "error: %s\n", result.message);
        } else {
            rc = now_package(project, cwd, verbose, &result);
            if (rc != 0)
                fprintf(stderr, "error: %s\n", result.message);
        }

    } else if (strcmp(phase, "install") == 0) {
        /* Build first, then install */
        rc = now_build(project, cwd, verbose, jobs, &result);
        if (rc != 0) {
            fprintf(stderr, "error: %s\n", result.message);
        } else {
            rc = now_install(project, cwd, verbose, &result);
            if (rc != 0)
                fprintf(stderr, "error: %s\n", result.message);
        }

    } else if (strcmp(phase, "publish") == 0) {
        /* Build → package → publish */
        rc = now_build(project, cwd, verbose, jobs, &result);
        if (rc != 0) {
            fprintf(stderr, "error: %s\n", result.message);
        } else {
            rc = now_package(project, cwd, verbose, &result);
            if (rc != 0) {
                fprintf(stderr, "error: %s\n", result.message);
            } else {
                rc = now_publish(project, cwd, repo_url, verbose, &result);
                if (rc != 0)
                    fprintf(stderr, "error: %s\n", result.message);
            }
        }

    } else if (strcmp(phase, "export:cmake") == 0) {
        char *out = now_path_join(cwd, "CMakeLists.txt");
        if (!out) {
            fprintf(stderr, "error: cannot construct output path\n");
            rc = 1;
        } else {
            rc = now_export_cmake(project, cwd, out, &result);
            if (rc != 0)
                fprintf(stderr, "error: %s\n", result.message);
            else
                printf("wrote %s\n", out);
            free(out);
        }

    } else if (strcmp(phase, "export:make") == 0) {
        char *out = now_path_join(cwd, "Makefile");
        if (!out) {
            fprintf(stderr, "error: cannot construct output path\n");
            rc = 1;
        } else {
            rc = now_export_make(project, cwd, out, &result);
            if (rc != 0)
                fprintf(stderr, "error: %s\n", result.message);
            else
                printf("wrote %s\n", out);
            free(out);
        }

    } else if (strcmp(phase, "layers:show") == 0) {
        NowLayerStack stack;
        now_layer_stack_init(&stack);
        now_layer_discover(&stack, cwd, &result);
        now_layer_push_project(&stack, project);

        printf("Layer stack (%zu layers):\n\n", stack.count);
        for (size_t i = 0; i < stack.count; i++) {
            const NowLayer *l = &stack.layers[i];
            printf("  [%zu] %s", i, l->id);
            if (l->path) printf("  (%s)", l->path);
            printf("\n");
            for (size_t j = 0; j < l->section_count; j++) {
                const NowLayerSection *s = &l->sections[j];
                printf("      @%-20s %s",
                       s->name,
                       s->policy == NOW_POLICY_LOCKED ? "locked" : "open");
                if (s->description)
                    printf("  — %s", s->description);
                printf("\n");
            }
        }

        /* Show effective config if --effective */
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--effective") == 0) {
                printf("\nEffective configuration:\n");
                const char *section_names[] = {
                    "compile", "repos", "toolchain", "advisory",
                    "private_groups", "link", NULL
                };
                NowAuditReport audit;
                now_audit_init(&audit);
                for (int s = 0; section_names[s]; s++) {
                    PastaValue *eff = (PastaValue *)now_layer_merge_section(
                        &stack, section_names[s], &audit);
                    if (eff && pasta_count(eff) > 0) {
                        char *text = pasta_write(eff, PASTA_PRETTY);
                        if (text) {
                            printf("\n  @%s: %s\n", section_names[s], text);
                            free(text);
                        }
                    }
                    if (eff) pasta_free(eff);
                }
                now_audit_free(&audit);
                break;
            }
        }

        now_layer_stack_free(&stack);

    } else if (strcmp(phase, "layers:audit") == 0) {
        NowLayerStack stack;
        now_layer_stack_init(&stack);
        now_layer_discover(&stack, cwd, &result);
        now_layer_push_project(&stack, project);

        NowAuditReport audit;
        now_audit_init(&audit);

        /* Merge all known sections to collect violations */
        const char *section_names[] = {
            "compile", "repos", "toolchain", "advisory",
            "private_groups", "link", NULL
        };
        for (int i = 0; section_names[i]; i++) {
            PastaValue *eff = (PastaValue *)now_layer_merge_section(
                &stack, section_names[i], &audit);
            if (eff) pasta_free(eff);
        }

        char *report = now_audit_format(&audit);
        if (report) {
            printf("%s", report);
            free(report);
        }

        rc = (audit.count > 0) ? 1 : 0;
        now_audit_free(&audit);
        now_layer_stack_free(&stack);

    } else if (strcmp(phase, "ci") == 0) {
        /* CI lifecycle: build → test with structured output and report */
        NowCIEnv ci_env;
        now_ci_detect(&ci_env);
        if (flag_locked)  ci_env.locked = 1;
        if (flag_offline) ci_env.offline = 1;
        if (flag_no_color) ci_env.no_color = 1;
        if (output_fmt) {
            if (strcmp(output_fmt, "json") == 0)
                ci_env.format = NOW_OUTPUT_JSON;
            else if (strcmp(output_fmt, "pasta") == 0)
                ci_env.format = NOW_OUTPUT_PASTA;
            else if (strcmp(output_fmt, "text") == 0)
                ci_env.format = NOW_OUTPUT_TEXT;
        }
        rc = now_ci_run(project, cwd, &ci_env, jobs, &result);
        if (rc != 0 && ci_env.format == NOW_OUTPUT_TEXT)
            fprintf(stderr, "error: %s\n", result.message);

    } else if (strcmp(phase, "reproducible:check") == 0) {
        NowReproConfig repro_cfg;
        now_repro_from_project(&repro_cfg, project);
        if (!repro_cfg.enabled) {
            fprintf(stderr, "note: reproducible: not set in now.pasta — "
                            "checking anyway\n");
            repro_cfg.enabled = 1;
            if (!repro_cfg.timebase)
                repro_cfg.timebase = strdup("zero");
            repro_cfg.path_prefix_map = 1;
            repro_cfg.sort_inputs = 1;
            repro_cfg.no_date_macros = 1;
            repro_cfg.strip_metadata = 1;
        }

        rc = now_repro_check(project, cwd, verbose, jobs, &result);
        if (rc == 0)
            printf("reproducible:check — PASS (outputs match)\n");
        else if (rc == 1)
            fprintf(stderr, "reproducible:check — FAIL (outputs differ)\n");
        else
            fprintf(stderr, "error: %s\n", result.message);
        now_repro_free(&repro_cfg);

    } else if (strcmp(phase, "advisory:check") == 0) {
        NowAdvisoryDB adb;
        now_advisory_db_init(&adb);
        int arc = now_advisory_db_load(&adb, &result);
        if (arc != 0) {
            fprintf(stderr, "error: %s\n", result.message);
            now_advisory_db_free(&adb);
            rc = 1;
        } else if (adb.count == 0) {
            printf("No advisory database found. Run 'now advisory:update' first.\n");
        } else {
            /* Get today's date as YYYYMMDD */
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            int today = (tm->tm_year + 1900) * 10000 +
                        (tm->tm_mon + 1) * 100 + tm->tm_mday;

            NowAdvisoryReport report2;
            now_advisory_report_init(&report2);
            rc = now_advisory_check_project(&adb, project, today,
                                             &report2, &result);
            char *text = now_advisory_report_format(&report2);
            if (text) { printf("%s", text); free(text); }
            now_advisory_report_free(&report2);
        }
        now_advisory_db_free(&adb);

    } else if (strcmp(phase, "advisory:update") == 0) {
        /* For v1, advisory:update is a placeholder — db must be manually placed */
        printf("advisory:update — not yet implemented (requires HTTP client)\n"
               "Place advisory database at ~/.now/advisories/now-advisory-db.pasta\n");
        rc = 0;

    } else if (strcmp(phase, "verify") == 0) {
        /* Verify archive signature against trust store */
        if (argc < 4) {
            fprintf(stderr, "usage: now verify <archive> <sigfile>\n");
            rc = 1;
        } else {
            const char *archive = argv[2];
            const char *sigfile = argv[3];

            NowTrustPolicy policy = now_trust_policy_from_project(project);
            NowTrustLevel level = now_trust_level(&policy);

            if (level == NOW_TRUST_NONE && verbose)
                fprintf(stderr, "note: trust policy is 'none' — verifying anyway\n");

            /* Look up key from trust store */
            NowTrustStore store;
            now_trust_init(&store);
            now_trust_load(&store, &result);

            const NowTrustKey *tk = NULL;
            if (project->group[0])
                tk = now_trust_find(&store, project->group, project->artifact);

            if (!tk && level == NOW_TRUST_TRUSTED) {
                fprintf(stderr, "error: no trusted key found for %s:%s\n",
                        project->group, project->artifact);
                rc = 1;
            } else if (tk) {
                rc = now_verify_file(archive, sigfile, tk->key, &result);
                if (rc != 0)
                    fprintf(stderr, "error: %s\n", result.message);
                else
                    printf("signature verified\n");
            } else {
                fprintf(stderr, "error: no key available for verification\n");
                rc = 1;
            }

            now_trust_free(&store);
        }

    } else if (strcmp(phase, "-h") == 0 || strcmp(phase, "--help") == 0
               || strcmp(phase, "help") == 0) {
        usage();

    } else {
        fprintf(stderr, "error: unknown phase '%s'\n\n", phase);
        usage();
        rc = 1;
    }

    now_project_free(project);
    return rc;
}
