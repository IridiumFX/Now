/*
 * now_auth.h — Shared authentication for registry operations
 *
 * Loads credentials from ~/.now/credentials.pasta and exchanges
 * them for a JWT via POST /auth/token with Basic auth.
 */
#ifndef NOW_AUTH_H
#define NOW_AUTH_H

#include "now.h"

/* Credentials loaded from ~/.now/credentials.pasta */
typedef struct {
    char *username;   /* registry username (may be NULL for static tokens) */
    char *token;      /* API key / password */
} NowCredentials;

/* Load credentials for a registry URL from ~/.now/credentials.pasta.
 * Matches by URL prefix. Sets fields in *creds (caller must free via
 * now_auth_creds_free). Returns 0 if found, -1 if no match. */
NOW_API int now_auth_load(const char *registry_url, NowCredentials *creds);
NOW_API void now_auth_creds_free(NowCredentials *creds);

/* Exchange credentials for a JWT via POST /auth/token.
 * Builds Authorization: Basic base64(username:token) header.
 * On success, returns 0 and sets *jwt_out to a malloc'd JWT string.
 * Caller must free *jwt_out. Returns -1 on error. */
NOW_API int now_auth_login(const char *host, int port, const char *path_prefix,
                           const NowCredentials *creds, int use_tls,
                           char **jwt_out, NowResult *result);

#endif /* NOW_AUTH_H */
