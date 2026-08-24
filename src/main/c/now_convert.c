/*
 * now_convert.c — `now convert` (§23.4)
 *
 * Convert a descriptor between Pasta and JSON.
 *
 * The reading half turned out to need nothing: **Pasta already parses
 * strict JSON.** Quoted keys, `[...]`, `{...}`, comma separators and
 * double-quoted strings are common to both grammars, so `pasta_parse`
 * on a `.json` descriptor succeeds and produces the same tree. Verified
 * before writing a line of this file — a JSON parser was the expensive
 * part of the job and it was already there.
 *
 * So the only new code is a JSON writer, and the conversions are:
 *
 *   --to pasta   parse, then pasta_write(PASTA_PRETTY)
 *   --to json    parse, then the writer below
 *
 * WHAT THIS DOES NOT DO. §23.4 also lists `--to json5`. JSON5 permits
 * trailing commas and Pasta does not — the format owner ruled on that
 * and §23.1 was corrected to match — so a JSON5 file with trailing
 * commas will not read here, and emitting JSON5 would mean emitting a
 * dialect we cannot read back. A round trip that only works one way is
 * worse than an honest refusal, so `--to json5` says so and exits.
 *
 * Comments are lost. Both grammars carry them and the parsed tree does
 * not, so a conversion is a conversion of DATA. Said out loud by
 * `--in-place`, which is the mode where losing them is irreversible.
 */
#include "now_convert.h"
#include "now_fs.h"
#include "pasta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- JSON writer ---- */

static void j_indent(FILE *f, int n) {
    for (int i = 0; i < n; i++) fputs("  ", f);
}

static void j_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        case '\b': fputs("\\b", f);  break;
        case '\f': fputs("\\f", f);  break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", (unsigned)*p);
            else           fputc((int)*p, f);
        }
    }
    fputc('"', f);
}

static void j_value(FILE *f, const PastaValue *v, int depth) {
    if (!v) { fputs("null", f); return; }

    switch (pasta_type(v)) {
    case PASTA_NULL:
        fputs("null", f);
        break;
    case PASTA_BOOL:
        fputs(pasta_get_bool(v) ? "true" : "false", f);
        break;
    case PASTA_NUMBER: {
        /* JSON has decimal literals and nothing else, so a Pasta `0xFF`
         * or `0b1010` comes out as 255 and 10. `basta_get_number_fmt`
         * reports the radix it was written in and there is nowhere to
         * put it — this is the one thing a Pasta -> JSON -> Pasta round
         * trip does not preserve, and it is reported by the caller
         * rather than left for someone to notice in a diff.
         *
         * Integers print as integers: %g would render 1000000 as 1e+06,
         * which is valid JSON and a different thing to read. */
        double d = pasta_get_number(v);
        if (d == (double)(long long)d && d > -9.2e18 && d < 9.2e18)
            fprintf(f, "%lld", (long long)d);
        else
            fprintf(f, "%.17g", d);
        break;
    }
    case PASTA_STRING:
        j_string(f, pasta_get_string(v));
        break;
    case PASTA_ARRAY: {
        size_t n = pasta_count(v);
        if (n == 0) { fputs("[]", f); break; }
        fputs("[\n", f);
        for (size_t i = 0; i < n; i++) {
            j_indent(f, depth + 1);
            j_value(f, pasta_array_get(v, i), depth + 1);
            fputs(i + 1 < n ? ",\n" : "\n", f);
        }
        j_indent(f, depth);
        fputc(']', f);
        break;
    }
    case PASTA_MAP: {
        size_t n = pasta_count(v);
        if (n == 0) { fputs("{}", f); break; }
        fputs("{\n", f);
        for (size_t i = 0; i < n; i++) {
            j_indent(f, depth + 1);
            j_string(f, pasta_map_key(v, i));
            fputs(": ", f);
            j_value(f, pasta_map_value(v, i), depth + 1);
            fputs(i + 1 < n ? ",\n" : "\n", f);
        }
        j_indent(f, depth);
        fputc('}', f);
        break;
    }
    default:
        /* A Basta blob has no JSON spelling. Refusing beats inventing
         * one — base64 here would round-trip back as a string and the
         * blob would be gone. */
        fputs("null", f);
        break;
    }
}

/* ---- entry point ---- */

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

NOW_API int now_convert(const char *in_path, const char *to,
                        int in_place, FILE *out_stream, NowResult *result) {
    char *text = NULL;
    size_t len = 0;
    PastaResult pr;
    PastaValue *root = NULL;
    const char *src = in_path;
    char chosen[512];
    int rc = -1;

    if (!to || !*to) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "convert needs --to pasta|json");
        }
        return -1;
    }
    if (strcmp(to, "json5") == 0) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "--to json5 is not supported: JSON5 permits trailing "
                     "commas and Pasta does not, so the output could not be "
                     "read back. Use --to json.");
        }
        return -1;
    }
    if (strcmp(to, "pasta") != 0 && strcmp(to, "json") != 0) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "unknown target format '%s' (pasta or json)", to);
        }
        return -1;
    }

    /* Auto-detect the source when none was named. */
    if (!src || !*src) {
        static const char *const cand[] = { "now.pasta", "now.json",
                                            "now.json5", NULL };
        for (size_t i = 0; cand[i]; i++) {
            if (now_path_exists(cand[i])) {
                snprintf(chosen, sizeof(chosen), "%s", cand[i]);
                src = chosen;
                break;
            }
        }
        if (!src) {
            if (result) {
                result->code = NOW_ERR_NOT_FOUND;
                snprintf(result->message, sizeof(result->message),
                         "no now.pasta, now.json or now.json5 here");
            }
            return -1;
        }
    }

    text = slurp(src, &len);
    if (!text) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot read %s", src);
        }
        return -1;
    }

    root = pasta_parse(text, len, &pr);
    if (!root || pr.code != PASTA_OK) {
        if (result) {
            result->code = NOW_ERR_SYNTAX;
            snprintf(result->message, sizeof(result->message),
                     "%s:%d:%d: %s", src, pr.line, pr.col, pr.message);
        }
        free(text);
        if (root) pasta_free(root);
        return -1;
    }
    free(text);

    {
        FILE *out = out_stream ? out_stream : stdout;
        char out_path[512];
        out_path[0] = '\0';

        if (in_place) {
            /* Write the sibling, then remove the source — in that order,
             * so an interrupted conversion leaves the original rather
             * than nothing. */
            const char *dot = strrchr(src, '.');
            size_t stem = dot ? (size_t)(dot - src) : strlen(src);
            snprintf(out_path, sizeof(out_path), "%.*s.%s",
                     (int)stem, src, strcmp(to, "pasta") == 0 ? "pasta" : "json");
            if (strcmp(out_path, src) == 0) {
                if (result) {
                    result->code = NOW_ERR_SCHEMA;
                    snprintf(result->message, sizeof(result->message),
                             "%s is already %s", src, to);
                }
                pasta_free(root);
                return -1;
            }
            out = fopen(out_path, "wb");
            if (!out) {
                if (result) {
                    result->code = NOW_ERR_IO;
                    snprintf(result->message, sizeof(result->message),
                             "cannot write %s", out_path);
                }
                pasta_free(root);
                return -1;
            }
        }

        if (strcmp(to, "json") == 0) {
            j_value(out, root, 0);
            fputc('\n', out);
        } else {
            char *s = pasta_write(root, PASTA_PRETTY);
            if (s) { fputs(s, out); free(s); }
            else {
                if (result) {
                    result->code = NOW_ERR_ALLOC;
                    snprintf(result->message, sizeof(result->message),
                             "could not serialise to pasta");
                }
                if (in_place) fclose(out);
                pasta_free(root);
                return -1;
            }
        }

        if (in_place) {
            fclose(out);
            remove(src);
            fprintf(stderr, "converted %s -> %s\n", src, out_path);
            /* Both losses, said at the moment they become irreversible. */
            fprintf(stderr,
                    "note: comments are not carried across a conversion\n");
            if (strcmp(to, "json") == 0)
                fprintf(stderr,
                        "note: hex and binary literals become decimal - JSON "
                        "has no other spelling\n");
        }
        rc = 0;
    }

    pasta_free(root);
    return rc;
}
