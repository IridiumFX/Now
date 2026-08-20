/*
 * now_ed25519.c — Ed25519 digital signatures (RFC 8032) for `now`
 *
 * The primitives are apennines' (`t2/crypto/ec.c`); this file is the
 * `now`-shaped surface over them: our key file layout, our base64, our
 * NowResult errors, and NOW_API symbols so nothing outside has to link
 * a vendored one directly.
 *
 * It used to carry a hand-rolled implementation — SHA-512, GF(2^255-19)
 * field arithmetic, the group law and scalar reduction, ~770 lines. On
 * 2026-08-20 that implementation was measured against RFC 8032 and got
 * 43 of 300 signatures wrong, because sc_reduce and sc_muladd both
 * dropped the final carry pass. Its verifier disagreed with the spec on
 * 12 of 300 — in both directions, including one signature it accepted
 * that a correct verifier rejects.
 *
 * Three Ed25519s existed on this machine. apennines' was already
 * compiled into this binary and had been cross-checked (cookbook found
 * their sc_muladd bug in March and they fixed it); cookbook's is
 * correct too. Only ours was wrong, and ours is the one that signs
 * releases. Both of the others measure 0/300 against the reference.
 *
 * So: one implementation, the one that gets looked at. Do not
 * reintroduce a second.
 */

#include "now_trust.h"
/* apennines entropy source — wrapped by now_entropy() below so the CLI
 * never has to link against a vendored symbol directly. */
#include "apennines/t1/random/entropy.h"
#include "apennines/t2/crypto/ec.h"
#include "now_fs.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ================================================================
 * Ed25519 API
 * ================================================================ */

/* Derive a public key from a 32-byte seed */
NOW_API int now_entropy(unsigned char *out, size_t len) {
    if (!out) return -1;
    /* Thin wrapper on purpose: the point is that the symbol crossing the
     * library boundary is one of ours and carries NOW_API, not that the
     * entropy source changes. */
    return entropy_get_system(out, (u64)len) == 0 ? 0 : -1;
}

/* now's key file is seed(32) || pub(32); apennines' privkey is the
 * expanded scalar(32) || prefix(32). Keep our layout — it is what
 * ~/.now/signing.key already holds and what `now keygen` printed to
 * every consumer — and expand from the seed at the boundary. That is
 * one SHA-512 and one scalar multiply per signature, which for a build
 * tool is nothing. */
NOW_API int now_ed25519_keypair(unsigned char pub[32],
                                 unsigned char priv[64],
                                 const unsigned char seed[32]) {
    if (!pub || !priv || !seed) return -1;

    ed25519_seed s;
    memcpy(s.data, seed, 32);

    ed25519_keypair kp;
    if (ed25519_keygen_from_seed(&kp, &s) != 0) return -1;

    memcpy(pub, kp.pub.data, 32);
    memcpy(priv, seed, 32);
    memcpy(priv + 32, kp.pub.data, 32);
    return 0;
}

/* Sign a message. sig is 64 bytes. */
NOW_API int now_ed25519_sign(unsigned char sig[64],
                               const unsigned char *msg, size_t msg_len,
                               const unsigned char priv[64]) {
    if (!sig || !msg || !priv) return -1;

    /* Derive the public half from the seed rather than trusting the
     * copy stored beside it. They can only disagree if the key file was
     * corrupted or hand-edited, and a signature made under a public key
     * that is not the seed's cannot verify — better to sign correctly
     * than to reproduce whatever is on disk. */
    ed25519_seed s;
    memcpy(s.data, priv, 32);

    ed25519_keypair kp;
    if (ed25519_keygen_from_seed(&kp, &s) != 0) return -1;

    return ed25519_sign(sig, &kp.priv, &kp.pub, msg, (u64)msg_len) == 0
           ? 0 : -1;
}

/* Verify an Ed25519 signature.
 * Returns 0 if valid, -1 if invalid. */
NOW_API int now_ed25519_verify(const unsigned char *sig,
                                const unsigned char *msg, size_t msg_len,
                                const unsigned char *pub_key) {
    if (!sig || !msg || !pub_key) return -1;

    ed25519_pubkey pk;
    memcpy(pk.data, pub_key, 32);

    unsigned long valid = 0;
    if (ed25519_verify(&valid, &pk, sig, msg, (u64)msg_len) != 0) return -1;
    return valid ? 0 : -1;
}

/* ================================================================
 * File-level signature verification (replaces minisign delegation)
 * ================================================================ */

/* Base64 decode (standard alphabet, no padding requirement) */
static int b64_decode(const char *in, size_t in_len,
                       uint8_t *out, size_t *out_len) {
    static const char b64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t j = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (size_t i = 0; i < in_len; i++) {
        if (in[i] == '=' || in[i] == '\n' || in[i] == '\r') continue;
        const char *pos = strchr(b64_chars, in[i]);
        if (!pos) return -1;
        int val = (int)(pos - b64_chars);
        acc = (acc << 6) | (uint32_t)val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[j++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    *out_len = j;
    return 0;
}

/* Read file contents into a malloc'd buffer */
static char *ed_read_file(const char *path, size_t *out_len) {
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

NOW_API int now_verify_file(const char *archive_path, const char *sig_path,
                             const char *pubkey_b64, NowResult *result) {
    if (!archive_path || !sig_path || !pubkey_b64) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "verify: archive, sig, and key required");
        }
        return -1;
    }

    if (!now_path_exists(archive_path)) {
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "verify: archive not found: %s", archive_path);
        }
        return -1;
    }
    if (!now_path_exists(sig_path)) {
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "verify: signature not found: %s", sig_path);
        }
        return -1;
    }

    /* Decode the public key from base64 */
    uint8_t pub_raw[64];
    size_t pub_len;
    if (b64_decode(pubkey_b64, strlen(pubkey_b64), pub_raw, &pub_len) != 0 ||
        pub_len != 32) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "verify: invalid public key (expected 32 bytes, got %zu)",
                     pub_len);
        }
        return -1;
    }

    /* Read the signature file (64 bytes raw or base64) */
    size_t sig_file_len;
    char *sig_file = ed_read_file(sig_path, &sig_file_len);
    if (!sig_file) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "verify: cannot read signature file");
        }
        return -1;
    }

    uint8_t sig_raw[64];
    if (sig_file_len == 64) {
        /* Raw binary signature */
        memcpy(sig_raw, sig_file, 64);
    } else {
        /* Try base64 decode */
        size_t sig_decoded_len;
        if (b64_decode(sig_file, sig_file_len, sig_raw, &sig_decoded_len) != 0 ||
            sig_decoded_len != 64) {
            free(sig_file);
            if (result) {
                result->code = NOW_ERR_SCHEMA;
                snprintf(result->message, sizeof(result->message),
                         "verify: invalid signature (expected 64 bytes)");
            }
            return -1;
        }
    }
    free(sig_file);

    /* Read the archive data */
    size_t msg_len;
    char *msg_data = ed_read_file(archive_path, &msg_len);
    if (!msg_data) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "verify: cannot read archive");
        }
        return -1;
    }

    /* Verify the Ed25519 signature */
    int rc = now_ed25519_verify(sig_raw, (const unsigned char *)msg_data,
                                 msg_len, pub_raw);
    free(msg_data);

    if (rc == 0) {
        if (result) result->code = NOW_OK;
        return 0;
    }

    if (result) {
        result->code = NOW_ERR_AUTH;
        snprintf(result->message, sizeof(result->message),
                 "signature verification failed");
    }
    return -1;
}

/* ================================================================
 *  Signing: key management and detached file signatures
 *
 *  Until this existed, `now` could verify a signature but never
 *  produce one — so `trust: { require_signatures: true }` could not
 *  be satisfied by anything now published, and `now verify` needed a
 *  .sig the tool had no way to create.
 *
 *  Key file: ~/.now/signing.key, the raw 64-byte Ed25519 private key
 *  (seed || public). Binary, not base64 — it never needs to be
 *  pasted anywhere, unlike the public half.
 *
 *  Signature file: the raw 64 signature bytes. now_verify_file
 *  accepts raw-64 or base64; raw keeps producer and consumer simple.
 * ================================================================ */

static const char b64_enc_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

NOW_API char *now_b64_encode(const unsigned char *in, size_t len) {
    if (!in) return NULL;
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i + 2 < len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[j++] = b64_enc_chars[(v >> 18) & 63];
        out[j++] = b64_enc_chars[(v >> 12) & 63];
        out[j++] = b64_enc_chars[(v >>  6) & 63];
        out[j++] = b64_enc_chars[v & 63];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        int rem = 1;
        if (i + 1 < len) { v |= (uint32_t)in[i+1] << 8; rem = 2; }
        out[j++] = b64_enc_chars[(v >> 18) & 63];
        out[j++] = b64_enc_chars[(v >> 12) & 63];
        out[j++] = (rem == 2) ? b64_enc_chars[(v >> 6) & 63] : '=';
        out[j++] = '=';
    }
    out[j] = '\0';
    return out;
}

NOW_API char *now_signing_key_path(void) {
    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    const char *env = getenv("NOW_SIGNING_KEY");
    if (env && *env) return strdup(env);
    if (!home) return NULL;
    char *dot = now_path_join(home, ".now");
    if (!dot) return NULL;
    char *p = now_path_join(dot, "signing.key");
    free(dot);
    return p;
}

NOW_API int now_signing_key_load(unsigned char priv[64]) {
    char *path = now_signing_key_path();
    if (!path) return -1;
    FILE *fp = fopen(path, "rb");
    free(path);
    if (!fp) return -1;
    size_t n = fread(priv, 1, 64, fp);
    fclose(fp);
    return (n == 64) ? 0 : -1;
}

NOW_API int now_sign_file(const char *path, const char *sig_path,
                           const unsigned char priv[64], NowResult *result) {
    if (!path || !sig_path || !priv) return -1;

    size_t len = 0;
    char *data = ed_read_file(path, &len);
    if (!data) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "sign: cannot read %s", path);
        }
        return -1;
    }

    unsigned char sig[64];
    int rc = now_ed25519_sign(sig, (const unsigned char *)data, len, priv);
    free(data);
    if (rc != 0) {
        if (result) {
            result->code = NOW_ERR_TOOL;
            snprintf(result->message, sizeof(result->message), "sign: failed");
        }
        return -1;
    }

    FILE *fp = fopen(sig_path, "wb");
    if (!fp) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "sign: cannot write %s", sig_path);
        }
        return -1;
    }
    size_t w = fwrite(sig, 1, 64, fp);
    fclose(fp);
    return (w == 64) ? 0 : -1;
}
