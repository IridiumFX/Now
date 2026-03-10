/*
 * now_auth.c — Shared authentication for registry operations
 *
 * Loads credentials from ~/.now/credentials.pasta and exchanges
 * them for a JWT via POST /auth/token with Basic auth.
 */

#include "now_auth.h"
#include "now_fs.h"
#include "pico_http.h"

#include <pasta.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Base64 encoding (for Basic auth) ---- */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const unsigned char *in, size_t len,
                           char *out, size_t out_cap) {
    size_t i = 0, j = 0;
    while (i < len && j + 4 < out_cap) {
        unsigned int a = in[i++];
        unsigned int b = (i < len) ? in[i++] : 0;
        unsigned int c = (i < len) ? in[i++] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : b64_table[(triple >> 6) & 0x3F];
        out[j++] = (i > len)     ? '=' : b64_table[triple & 0x3F];
    }
    out[j] = '\0';
}

/* ---- File reading helper ---- */

static char *read_file_all(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    *out_len = n;
    return buf;
}

/* ---- Credential loading ---- */

NOW_API int now_auth_load(const char *registry_url, NowCredentials *creds) {
    if (!creds) return -1;
    creds->username = NULL;
    creds->token = NULL;

    if (!registry_url) return -1;

    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home) return -1;

    char *dot_now = now_path_join(home, ".now");
    if (!dot_now) return -1;
    char *cred_path = now_path_join(dot_now, "credentials.pasta");
    free(dot_now);
    if (!cred_path) return -1;

    size_t cred_len;
    char *cred_data = read_file_all(cred_path, &cred_len);
    free(cred_path);
    if (!cred_data) return -1;

    PastaValue *root = pasta_parse(cred_data, cred_len, NULL);
    free(cred_data);
    if (!root || pasta_type(root) != PASTA_MAP) { pasta_free(root); return -1; }

    const PastaValue *registries = pasta_map_get(root, "registries");
    if (!registries || pasta_type(registries) != PASTA_ARRAY) {
        pasta_free(root);
        return -1;
    }

    int found = -1;
    size_t nregs = pasta_count(registries);
    for (size_t i = 0; i < nregs; i++) {
        const PastaValue *entry = pasta_array_get(registries, i);
        if (!entry || pasta_type(entry) != PASTA_MAP) continue;

        const PastaValue *url_val = pasta_map_get(entry, "url");
        if (!url_val || pasta_type(url_val) != PASTA_STRING) continue;

        const char *entry_url = pasta_get_string(url_val);
        if (!entry_url) continue;

        /* Match by URL prefix */
        if (strncmp(registry_url, entry_url, strlen(entry_url)) == 0) {
            /* Extract token */
            const PastaValue *tok = pasta_map_get(entry, "token");
            if (tok && pasta_type(tok) == PASTA_STRING) {
                const char *s = pasta_get_string(tok);
                if (s) creds->token = strdup(s);
            }

            /* Extract username */
            const PastaValue *user = pasta_map_get(entry, "username");
            if (user && pasta_type(user) == PASTA_STRING) {
                const char *s = pasta_get_string(user);
                if (s) creds->username = strdup(s);
            }

            found = 0;
            break;
        }
    }

    pasta_free(root);
    return found;
}

NOW_API void now_auth_creds_free(NowCredentials *creds) {
    if (!creds) return;
    free(creds->username);
    free(creds->token);
    creds->username = NULL;
    creds->token = NULL;
}

/* ---- JWT token exchange ---- */

NOW_API int now_auth_login(const char *host, int port, const char *path_prefix,
                           const NowCredentials *creds, int use_tls,
                           char **jwt_out, NowResult *result) {
    (void)use_tls; /* TODO: TLS support in pico_http v0.4 */

    if (!jwt_out) return -1;
    *jwt_out = NULL;

    if (!host || !creds || !creds->username || !creds->token) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "credentials missing username or token for auth login");
        }
        return -1;
    }

    /* Build Basic auth: base64(username:token) */
    size_t ulen = strlen(creds->username);
    size_t tlen = strlen(creds->token);
    size_t raw_len = ulen + 1 + tlen;  /* username:token */
    char *raw = (char *)malloc(raw_len + 1);
    if (!raw) return -1;
    snprintf(raw, raw_len + 1, "%s:%s", creds->username, creds->token);

    /* base64 output: 4 * ceil(raw_len / 3) + 1 */
    size_t b64_cap = ((raw_len + 2) / 3) * 4 + 1;
    char *b64 = (char *)malloc(b64_cap);
    if (!b64) { free(raw); return -1; }
    base64_encode((const unsigned char *)raw, raw_len, b64, b64_cap);
    free(raw);

    /* Build Authorization: Basic <b64> header */
    char auth_buf[1024];
    snprintf(auth_buf, sizeof(auth_buf), "Basic %s", b64);
    free(b64);

    PicoHttpHeader headers[1];
    headers[0].name  = "Authorization";
    headers[0].value = auth_buf;

    PicoHttpOptions opts = {0};
    opts.headers = headers;
    opts.header_count = 1;

    /* Build path: {prefix}/auth/token */
    char path[1024];
    snprintf(path, sizeof(path), "%s/auth/token",
             (path_prefix && *path_prefix) ? path_prefix : "");

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));

    int rc = pico_http_post(host, port, path,
                            NULL, NULL, 0,  /* no body */
                            &opts, &res);

    if (rc != PICO_OK) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "POST /auth/token failed: %s", pico_http_strerror(rc));
        }
        return -1;
    }

    if (res.status != 200) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "auth failed: registry returned HTTP %d", res.status);
        }
        pico_http_response_free(&res);
        return -1;
    }

    /* Parse JWT from JSON response: {"token":"<jwt>"} */
    if (res.body && res.body_len > 0) {
        const char *tkey = strstr(res.body, "\"token\":\"");
        if (tkey) {
            tkey += 9; /* skip "token":" */
            const char *tend = strchr(tkey, '"');
            if (tend) {
                size_t jlen = (size_t)(tend - tkey);
                *jwt_out = (char *)malloc(jlen + 1);
                if (*jwt_out) {
                    memcpy(*jwt_out, tkey, jlen);
                    (*jwt_out)[jlen] = '\0';
                }
            }
        }
    }

    pico_http_response_free(&res);

    if (!*jwt_out) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "auth response missing token field");
        }
        return -1;
    }

    return 0;
}
