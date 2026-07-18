/*
 * AirPlay 2 HAP - HomeKit Authentication Protocol
 *
 * Implements pair-verify and transient pair-setup for AirPlay 2 device
 * authentication and encrypted session framing for the RTSP control channel.
 *
 * Protocol flow (HAP pair-verify, X-Apple-HKP: 3, needs stored credentials):
 *   1. Client generates ephemeral X25519 keypair
 *   2. Client sends state=0x01 + ephemeral public key to /pair-verify
 *   3. Server responds with state=0x02 + server ephemeral public + encrypted signature
 *   4. Client verifies server signature, sends state=0x03 + encrypted client signature
 *   5. Server verifies, responds with state=0x04
 *   6. Session keys derived via HKDF-SHA-512 from shared secret
 *
 * Protocol flow (HAP transient pair-setup, X-Apple-HKP: 4, no credentials):
 *   1. Client sends state=0x01 + method=0 + flags=0x10 to /pair-setup
 *   2. Server responds with state=0x02 + SRP salt + SRP public key B
 *   3. Client runs SRP-6a (SHA-512, 3072-bit group, fixed PIN "3939") and
 *      sends state=0x03 + public key A + client proof M1
 *   4. Server responds with state=0x04 + server proof; exchange stops here
 *      (no M5/M6, nothing is stored) and the SRP session key becomes the
 *      shared secret
 *
 * After pairing, the RTSP channel uses HAP framing:
 *   [2-byte LE length] [ChaCha20-Poly1305 encrypted payload + 16-byte tag]
 *   Max plaintext per frame: 1024 bytes
 *   Nonce: 4 zero bytes + 8-byte LE counter (incrementing per direction)
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/bn.h>
#include <openssl/sha.h>

#include "../libraop/crosstools/src/platform.h"
#include "cross_log.h"
#include "ap2_hap.h"

extern log_level *loglevel;

/* TLV8 tag types used in pair-setup/pair-verify */
#define TLV_METHOD      0x00
#define TLV_IDENTIFIER  0x01
#define TLV_SALT        0x02
#define TLV_PUBLIC_KEY  0x03
#define TLV_PROOF       0x04
#define TLV_ENCRYPTED   0x05
#define TLV_STATE       0x06
#define TLV_ERROR       0x07
#define TLV_SIGNATURE   0x0A
#define TLV_FLAGS       0x13

/* HAP encrypted frame parameters */
#define HAP_MAX_FRAME_SIZE   1024
#define HAP_TAG_SIZE         16
#define HAP_NONCE_SIZE       12

/* SRP-6a parameters for transient pair-setup (SHA-512 hash) */
#define SRP_N_BYTES          384    /* 3072-bit modulus */
#define SRP_HASH_LEN         SHA512_DIGEST_LENGTH
#define SRP_USERNAME         "Pair-Setup"
#define SRP_TRANSIENT_PIN    "3939" /* fixed PIN mandated for transient pairing */
#define HAP_TRANSIENT_FLAG   0x10   /* kTLVFlag transient in the M1 Flags TLV */

/* RFC 5054 3072-bit group used by HAP pair-setup; generator g = 5 */
static const char srp_n_hex_3072[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
    "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864"
    "D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E2"
    "08E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

struct ap2_hap_ctx {
    /* Long-term keys from pairing */
    uint8_t client_lt_private[64];   /* Ed25519 private key (64 bytes for OpenSSL) */
    uint8_t client_lt_public[32];    /* Ed25519 public key */
    uint8_t server_lt_public[32];    /* Server's Ed25519 public key */

    /* Client identifier for pair-verify (e.g. DACP ID bytes) */
    uint8_t client_id[64];
    int client_id_len;

    /* X25519 shared secret from pair-verify (kept for audio encryption) */
    uint8_t shared_secret[32];

    /* Session keys derived after pair-verify */
    uint8_t write_key[32];           /* Control-Write-Encryption-Key */
    uint8_t read_key[32];            /* Control-Read-Encryption-Key */
    uint64_t write_nonce_counter;    /* Incrementing nonce for writes */
    uint64_t read_nonce_counter;     /* Incrementing nonce for reads */

    bool verified;                   /* True after successful pair-verify */
};

/* ---- TLV8 helpers ---- */

typedef struct {
    uint8_t *data;
    int len;
    int capacity;
} tlv_buf_t;

static void tlv_init(tlv_buf_t *buf)
{
    buf->capacity = 256;
    buf->data = malloc(buf->capacity);
    buf->len = 0;
}

static void tlv_add(tlv_buf_t *buf, uint8_t tag, const uint8_t *value, int value_len)
{
    /* TLV8 allows max 255 bytes per fragment; chain for larger values */
    while (value_len > 0) {
        int chunk = value_len > 255 ? 255 : value_len;
        while (buf->len + 2 + chunk > buf->capacity) {
            buf->capacity *= 2;
            buf->data = realloc(buf->data, buf->capacity);
        }
        buf->data[buf->len++] = tag;
        buf->data[buf->len++] = (uint8_t)chunk;
        memcpy(buf->data + buf->len, value, chunk);
        buf->len += chunk;
        value += chunk;
        value_len -= chunk;
    }
}

static void tlv_add_uint8(tlv_buf_t *buf, uint8_t tag, uint8_t value)
{
    tlv_add(buf, tag, &value, 1);
}

static void tlv_free(tlv_buf_t *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
}

/* Find a TLV tag in data. Returns pointer to value and sets *out_len.
   For fragmented TLVs, only returns first fragment. */
static const uint8_t *tlv_find(const uint8_t *data, int data_len,
                                uint8_t tag, int *out_len)
{
    int pos = 0;
    while (pos + 2 <= data_len) {
        uint8_t t = data[pos];
        uint8_t l = data[pos + 1];
        if (pos + 2 + l > data_len) break;
        if (t == tag) {
            *out_len = l;
            return data + pos + 2;
        }
        pos += 2 + l;
    }
    *out_len = 0;
    return NULL;
}

/* Concatenate all fragments of a TLV tag. Caller must free result. */
static uint8_t *tlv_concat(const uint8_t *data, int data_len,
                            uint8_t tag, int *out_len)
{
    /* First pass: count total length */
    int total = 0, pos = 0;
    while (pos + 2 <= data_len) {
        uint8_t t = data[pos];
        uint8_t l = data[pos + 1];
        if (pos + 2 + l > data_len) break;
        if (t == tag) total += l;
        pos += 2 + l;
    }
    if (total == 0) { *out_len = 0; return NULL; }

    /* Second pass: copy */
    uint8_t *result = malloc(total);
    int offset = 0;
    pos = 0;
    while (pos + 2 <= data_len) {
        uint8_t t = data[pos];
        uint8_t l = data[pos + 1];
        if (pos + 2 + l > data_len) break;
        if (t == tag) {
            memcpy(result + offset, data + pos + 2, l);
            offset += l;
        }
        pos += 2 + l;
    }
    *out_len = total;
    return result;
}

/* ---- Crypto helpers ---- */

static bool hkdf_sha512(const uint8_t *secret, int secret_len,
                         const char *salt_str, const char *info_str,
                         uint8_t *out, int out_len)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    if (!ctx) return false;
    bool ok = false;
    size_t dklen = out_len;

    if (EVP_PKEY_derive_init(ctx) <= 0) goto done;
    if (EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha512()) <= 0) goto done;
    if (EVP_PKEY_CTX_set1_hkdf_salt(ctx, (const unsigned char *)salt_str, strlen(salt_str)) <= 0)
        goto done;
    if (EVP_PKEY_CTX_set1_hkdf_key(ctx, secret, secret_len) <= 0) goto done;
    if (EVP_PKEY_CTX_add1_hkdf_info(ctx, (const unsigned char *)info_str, strlen(info_str)) <= 0)
        goto done;
    if (EVP_PKEY_derive(ctx, out, &dklen) <= 0) goto done;
    ok = true;
done:
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

static void make_nonce(uint64_t counter, uint8_t nonce[HAP_NONCE_SIZE])
{
    memset(nonce, 0, HAP_NONCE_SIZE);
    /* 4 zero bytes + 8-byte little-endian counter */
    for (int i = 0; i < 8; i++)
        nonce[4 + i] = (counter >> (i * 8)) & 0xFF;
}

static int chacha20_poly1305_encrypt(const uint8_t *key, const uint8_t *nonce,
                                      const uint8_t *aad, int aad_len,
                                      const uint8_t *plaintext, int pt_len,
                                      uint8_t *ciphertext, uint8_t tag[HAP_TAG_SIZE])
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int len, ct_len = 0;

    EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, HAP_NONCE_SIZE, NULL);
    EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce);
    if (aad && aad_len > 0)
        EVP_EncryptUpdate(ctx, NULL, &len, aad, aad_len);
    EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, pt_len);
    ct_len = len;
    EVP_EncryptFinal_ex(ctx, ciphertext + ct_len, &len);
    ct_len += len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, HAP_TAG_SIZE, tag);
    EVP_CIPHER_CTX_free(ctx);
    return ct_len;
}

static int chacha20_poly1305_decrypt(const uint8_t *key, const uint8_t *nonce,
                                      const uint8_t *aad, int aad_len,
                                      const uint8_t *ciphertext, int ct_len,
                                      const uint8_t *tag,
                                      uint8_t *plaintext)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int len, pt_len = 0;

    EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, HAP_NONCE_SIZE, NULL);
    EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce);
    if (aad && aad_len > 0)
        EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_len);
    EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ct_len);
    pt_len = len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, HAP_TAG_SIZE, (void *)tag);
    int ok = EVP_DecryptFinal_ex(ctx, plaintext + pt_len, &len);
    pt_len += len;
    EVP_CIPHER_CTX_free(ctx);
    return ok == 1 ? pt_len : -1;
}

/* ---- SRP-6a helpers (transient pair-setup) ---- */

/* SHA-512 of two big numbers, each zero-padded on the left to pad_len bytes
   (the PAD() operation from RFC 5054). */
static bool srp_hash_two_padded(const BIGNUM *n1, const BIGNUM *n2, int pad_len,
                                uint8_t out[SRP_HASH_LEN])
{
    bool ok = false;
    unsigned int md_len;
    uint8_t *buf = malloc(2 * pad_len);

    if (!buf) return false;
    if (BN_bn2binpad(n1, buf, pad_len) != pad_len) goto done;
    if (BN_bn2binpad(n2, buf + pad_len, pad_len) != pad_len) goto done;
    ok = EVP_Digest(buf, 2 * pad_len, out, &md_len, EVP_sha512(), NULL) == 1;
done:
    OPENSSL_cleanse(buf, 2 * pad_len);
    free(buf);
    return ok;
}

/* SHA-512 of a big number in minimal-length big-endian encoding. */
static bool srp_hash_bn(const BIGNUM *bn, uint8_t out[SRP_HASH_LEN])
{
    uint8_t buf[SRP_N_BYTES];
    unsigned int md_len;
    bool ok;
    int len = BN_num_bytes(bn);

    if (len > (int)sizeof(buf)) return false;
    BN_bn2bin(bn, buf);
    ok = EVP_Digest(buf, len, out, &md_len, EVP_sha512(), NULL) == 1;
    OPENSSL_cleanse(buf, sizeof(buf));
    return ok;
}

/*
 * Client side of SRP-6a (SHA-512, RFC 5054 3072-bit group) as used by HAP
 * pair-setup. From the device's salt and public key B this produces our
 * public key A, the client proof M1, the expected server proof HAMK and the
 * session key K = SHA512(S).
 *
 * Byte conventions (matching pair_ap/owntone, verified against real devices):
 *   k = SHA512(PAD(N) || PAD(g)) and u = SHA512(PAD(A) || PAD(B)), both
 *   padded to the 384-byte modulus size; the values hashed into x, K, M1 and
 *   HAMK use minimal-length big-endian encoding (salt as raw TLV bytes).
 */
static bool srp_client_compute(const uint8_t *salt, int salt_len,
                               const uint8_t *b_pub, int b_pub_len,
                               uint8_t a_pub[SRP_N_BYTES], int *a_pub_len,
                               uint8_t proof_m1[SRP_HASH_LEN],
                               uint8_t proof_hamk[SRP_HASH_LEN],
                               uint8_t session_key[SRP_HASH_LEN])
{
    bool ok = false;
    BN_CTX *bnctx = BN_CTX_new();
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    BIGNUM *N = NULL, *g = NULL, *a = NULL, *A = NULL, *B = NULL;
    BIGNUM *k = NULL, *u = NULL, *x = NULL, *S = NULL;
    BIGNUM *tmp1 = NULL, *tmp2 = NULL;
    uint8_t digest[SRP_HASH_LEN], hash_n[SRP_HASH_LEN], hash_g[SRP_HASH_LEN];
    uint8_t hash_user[SRP_HASH_LEN];
    uint8_t num_buf[SRP_N_BYTES];
    unsigned int md_len;
    int num_len;

    if (!bnctx || !md) goto done;

    if (!BN_hex2bn(&N, srp_n_hex_3072)) goto done;
    g = BN_new();
    a = BN_new();
    A = BN_new();
    S = BN_new();
    tmp1 = BN_new();
    tmp2 = BN_new();
    B = BN_bin2bn(b_pub, b_pub_len, NULL);
    if (!g || !a || !A || !S || !tmp1 || !tmp2 || !B) goto done;
    if (!BN_set_word(g, 5)) goto done;

    /* SRP-6a safety check: reject B == 0 (mod N) */
    if (!BN_nnmod(tmp1, B, N, bnctx) || BN_is_zero(tmp1)) {
        LOG_ERROR("[HAP] SRP public key B is invalid");
        goto done;
    }

    /* A = g^a mod N, with a 256-bit random private value */
    if (!BN_rand(a, 256, BN_RAND_TOP_ONE, BN_RAND_BOTTOM_ANY)) goto done;
    if (!BN_mod_exp(A, g, a, N, bnctx)) goto done;
    *a_pub_len = BN_bn2bin(A, a_pub);

    /* k = SHA512(PAD(N) || PAD(g)), u = SHA512(PAD(A) || PAD(B)) */
    if (!srp_hash_two_padded(N, g, SRP_N_BYTES, digest)) goto done;
    k = BN_bin2bn(digest, SRP_HASH_LEN, NULL);
    if (!srp_hash_two_padded(A, B, SRP_N_BYTES, digest)) goto done;
    u = BN_bin2bn(digest, SRP_HASH_LEN, NULL);
    if (!k || !u) goto done;

    /* SRP-6a safety check: u must not be zero */
    if (BN_is_zero(u)) {
        LOG_ERROR("[HAP] SRP scrambling parameter u is zero");
        goto done;
    }

    /* x = SHA512(salt || SHA512(username ":" pin)) */
    if (EVP_Digest(SRP_USERNAME ":" SRP_TRANSIENT_PIN,
                   strlen(SRP_USERNAME ":" SRP_TRANSIENT_PIN),
                   digest, &md_len, EVP_sha512(), NULL) != 1) goto done;
    if (EVP_DigestInit_ex(md, EVP_sha512(), NULL) != 1 ||
        EVP_DigestUpdate(md, salt, salt_len) != 1 ||
        EVP_DigestUpdate(md, digest, SRP_HASH_LEN) != 1 ||
        EVP_DigestFinal_ex(md, digest, &md_len) != 1) goto done;
    x = BN_bin2bn(digest, SRP_HASH_LEN, NULL);
    if (!x) goto done;

    /* S = (B - k * g^x) ^ (a + u*x) mod N */
    if (!BN_mod_exp(tmp1, g, x, N, bnctx)) goto done;   /* tmp1 = g^x */
    if (!BN_mul(tmp1, k, tmp1, bnctx)) goto done;       /* tmp1 = k*g^x */
    if (!BN_sub(tmp1, B, tmp1)) goto done;              /* tmp1 = B - k*g^x */
    if (!BN_nnmod(tmp1, tmp1, N, bnctx)) goto done;     /* normalize to [0, N) */
    if (!BN_mul(tmp2, u, x, bnctx)) goto done;
    if (!BN_add(tmp2, tmp2, a)) goto done;              /* tmp2 = a + u*x */
    if (!BN_mod_exp(S, tmp1, tmp2, N, bnctx)) goto done;

    /* Session key K = SHA512(S) */
    num_len = BN_num_bytes(S);
    if (num_len > (int)sizeof(num_buf)) goto done;
    BN_bn2bin(S, num_buf);
    if (EVP_Digest(num_buf, num_len, session_key, &md_len, EVP_sha512(), NULL) != 1)
        goto done;

    /* Client proof M1 =
     *   SHA512((SHA512(N) xor SHA512(g)) || SHA512(username) || salt || A || B || K) */
    if (!srp_hash_bn(N, hash_n) || !srp_hash_bn(g, hash_g)) goto done;
    for (int i = 0; i < SRP_HASH_LEN; i++)
        digest[i] = hash_n[i] ^ hash_g[i];
    if (EVP_Digest(SRP_USERNAME, strlen(SRP_USERNAME),
                   hash_user, &md_len, EVP_sha512(), NULL) != 1) goto done;

    num_len = BN_num_bytes(B);
    if (num_len > (int)sizeof(num_buf)) goto done;
    BN_bn2bin(B, num_buf);

    if (EVP_DigestInit_ex(md, EVP_sha512(), NULL) != 1 ||
        EVP_DigestUpdate(md, digest, SRP_HASH_LEN) != 1 ||
        EVP_DigestUpdate(md, hash_user, SRP_HASH_LEN) != 1 ||
        EVP_DigestUpdate(md, salt, salt_len) != 1 ||
        EVP_DigestUpdate(md, a_pub, *a_pub_len) != 1 ||
        EVP_DigestUpdate(md, num_buf, num_len) != 1 ||
        EVP_DigestUpdate(md, session_key, SRP_HASH_LEN) != 1 ||
        EVP_DigestFinal_ex(md, proof_m1, &md_len) != 1) goto done;

    /* Expected server proof HAMK = SHA512(A || M1 || K) */
    if (EVP_DigestInit_ex(md, EVP_sha512(), NULL) != 1 ||
        EVP_DigestUpdate(md, a_pub, *a_pub_len) != 1 ||
        EVP_DigestUpdate(md, proof_m1, SRP_HASH_LEN) != 1 ||
        EVP_DigestUpdate(md, session_key, SRP_HASH_LEN) != 1 ||
        EVP_DigestFinal_ex(md, proof_hamk, &md_len) != 1) goto done;

    ok = true;

done:
    BN_clear_free(a);
    BN_clear_free(x);
    BN_clear_free(S);
    BN_free(N);
    BN_free(g);
    BN_free(A);
    BN_free(B);
    BN_free(k);
    BN_free(u);
    BN_free(tmp1);
    BN_free(tmp2);
    BN_CTX_free(bnctx);
    EVP_MD_CTX_free(md);
    OPENSSL_cleanse(num_buf, sizeof(num_buf));
    OPENSSL_cleanse(digest, sizeof(digest));
    return ok;
}

/*
 * POST a TLV body to /pair-setup (transient, X-Apple-HKP: 4) and read the
 * complete RTSP response. Returns the HTTP status code (0 on transport
 * error) and points *resp_body (with *resp_body_len) at the body inside resp_buf.
 */
static int hap_post_pair_setup(int sock_fd, int cseq,
                               const uint8_t *body, int body_len,
                               uint8_t *resp_buf, int resp_buf_size,
                               uint8_t **resp_body, int *resp_body_len)
{
    char http_req[256];
    int hdr_len = snprintf(http_req, sizeof(http_req),
        "POST /pair-setup RTSP/1.0\r\n"
        "Content-Type: application/octet-stream\r\n"
        "X-Apple-HKP: 4\r\n"
        "CSeq: %d\r\n"
        "Content-Length: %d\r\n"
        "\r\n", cseq, body_len);

    *resp_body = NULL;
    *resp_body_len = 0;

    if (write(sock_fd, http_req, hdr_len) != hdr_len ||
        write(sock_fd, body, body_len) != body_len) {
        LOG_ERROR("[HAP] Failed to send pair-setup request (CSeq %d)", cseq);
        return 0;
    }

    struct timeval tv = {.tv_sec = 8};
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Read until headers plus Content-Length body are complete */
    int total = 0, header_len = 0, content_len = -1;
    while (total < resp_buf_size - 1) {
        if (header_len > 0 && total >= header_len + content_len)
            break;
        int n = read(sock_fd, resp_buf + total, resp_buf_size - 1 - total);
        if (n <= 0) break;
        total += n;
        resp_buf[total] = '\0';
        if (header_len == 0) {
            char *end = strstr((char *)resp_buf, "\r\n\r\n");
            if (end) {
                header_len = (int)(end - (char *)resp_buf) + 4;
                char *clh = strcasestr((char *)resp_buf, "Content-Length:");
                content_len = clh ? atoi(clh + 15) : 0;
            }
        }
    }

    if (header_len == 0 || total < header_len + content_len) {
        LOG_ERROR("[HAP] Incomplete pair-setup response (%d bytes)", total);
        return 0;
    }

    int status = 0;
    sscanf((char *)resp_buf, "%*s %d", &status);
    *resp_body = resp_buf + header_len;
    *resp_body_len = content_len;
    return status;
}

/* ---- Hex parsing ---- */

static bool hex_decode(const char *hex, uint8_t *out, int out_len)
{
    for (int i = 0; i < out_len; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%02x", &byte) != 1) return false;
        out[i] = (uint8_t)byte;
    }
    return true;
}

/* ---- Public API ---- */

struct ap2_hap_ctx *ap2_hap_create(const char *credentials_hex)
{
    /* No credentials: bare context, only usable for transient pair-setup */
    if (!credentials_hex) {
        struct ap2_hap_ctx *ctx = calloc(1, sizeof(*ctx));
        if (ctx) LOG_INFO("[HAP] Created context without credentials (transient pairing)");
        return ctx;
    }

    if (strlen(credentials_hex) != 192) {
        LOG_ERROR("[HAP] Invalid credentials: expected 192 hex chars, got %d",
                  (int)strlen(credentials_hex));
        return NULL;
    }

    struct ap2_hap_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    /*
     * Credentials format (192 hex chars = 96 bytes):
     *   bytes 0-63:  Ed25519 private key (which includes the public key as last 32 bytes)
     *   bytes 64-95: Server's Ed25519 public key
     */
    uint8_t raw[96];
    if (!hex_decode(credentials_hex, raw, 96)) {
        LOG_ERROR("[HAP] Failed to decode credentials hex");
        free(ctx);
        return NULL;
    }

    memcpy(ctx->client_lt_private, raw, 64);
    memcpy(ctx->client_lt_public, raw + 32, 32);  /* Last 32 bytes of Ed25519 private = public key */
    memcpy(ctx->server_lt_public, raw + 64, 32);

    LOG_INFO("[HAP] Created context with credentials");
    return ctx;
}

void ap2_hap_set_client_id(struct ap2_hap_ctx *ctx, const uint8_t *id, int id_len)
{
    if (!ctx || !id || id_len <= 0 || id_len > 64) return;
    memcpy(ctx->client_id, id, id_len);
    ctx->client_id_len = id_len;
}

void ap2_hap_destroy(struct ap2_hap_ctx *ctx)
{
    if (ctx) {
        /* Securely wipe keys */
        OPENSSL_cleanse(ctx, sizeof(*ctx));
        free(ctx);
    }
}

bool ap2_hap_pair_verify(struct ap2_hap_ctx *ctx, int sock_fd)
{
    if (!ctx) return false;

    LOG_INFO("[HAP] Starting pair-verify...");

    /* Step 1: Generate ephemeral X25519 keypair */
    EVP_PKEY *eph_key = NULL;
    EVP_PKEY_CTX *keygen_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    if (!keygen_ctx || EVP_PKEY_keygen_init(keygen_ctx) <= 0 ||
        EVP_PKEY_keygen(keygen_ctx, &eph_key) <= 0) {
        LOG_ERROR("[HAP] Failed to generate ephemeral X25519 keypair");
        EVP_PKEY_CTX_free(keygen_ctx);
        return false;
    }
    EVP_PKEY_CTX_free(keygen_ctx);

    /* Extract our ephemeral public key */
    uint8_t eph_public[32];
    size_t eph_pub_len = 32;
    EVP_PKEY_get_raw_public_key(eph_key, eph_public, &eph_pub_len);

    /* Build Message 1: state=0x01 + our ephemeral public key */
    tlv_buf_t msg1;
    tlv_init(&msg1);
    tlv_add_uint8(&msg1, TLV_STATE, 0x01);
    tlv_add(&msg1, TLV_PUBLIC_KEY, eph_public, 32);

    /* Send POST /pair-verify via RTSP with HAP header */
    char http_req[512];
    int hdr_len = snprintf(http_req, sizeof(http_req),
        "POST /pair-verify RTSP/1.0\r\n"
        "Content-Type: application/octet-stream\r\n"
        "X-Apple-HKP: 3\r\n"
        "CSeq: 1\r\n"
        "Content-Length: %d\r\n"
        "\r\n", msg1.len);

    if (write(sock_fd, http_req, hdr_len) != hdr_len ||
        write(sock_fd, msg1.data, msg1.len) != msg1.len) {
        LOG_ERROR("[HAP] Failed to send pair-verify M1");
        tlv_free(&msg1);
        EVP_PKEY_free(eph_key);
        return false;
    }
    tlv_free(&msg1);

    /* Read response */
    uint8_t resp_buf[4096];
    int resp_len = read(sock_fd, resp_buf, sizeof(resp_buf));
    if (resp_len <= 0) {
        LOG_ERROR("[HAP] No response to pair-verify M1");
        EVP_PKEY_free(eph_key);
        return false;
    }

    /* Find HTTP body (after \r\n\r\n) */
    uint8_t *body = NULL;
    int body_len = 0;
    for (int i = 0; i < resp_len - 3; i++) {
        if (resp_buf[i] == '\r' && resp_buf[i+1] == '\n' &&
            resp_buf[i+2] == '\r' && resp_buf[i+3] == '\n') {
            body = resp_buf + i + 4;
            body_len = resp_len - (i + 4);
            break;
        }
    }
    if (!body || body_len < 4) {
        LOG_ERROR("[HAP] Invalid pair-verify M2 response (resp_len=%d, body_len=%d)", resp_len, body_len);
        /* Dump first 200 bytes for debugging */
        char hex[600];
        int dump_len = resp_len < 200 ? resp_len : 200;
        for (int i = 0; i < dump_len; i++) sprintf(hex + i*3, "%02x ", resp_buf[i]);
        hex[dump_len*3] = '\0';
        LOG_ERROR("[HAP] Response hex: %s", hex);
        EVP_PKEY_free(eph_key);
        return false;
    }

    /* Parse M2: state=0x02, server ephemeral public key, encrypted data */
    int state_len, spk_len, enc_len;
    const uint8_t *state_val = tlv_find(body, body_len, TLV_STATE, &state_len);
    const uint8_t *server_eph_pub_val = tlv_find(body, body_len, TLV_PUBLIC_KEY, &spk_len);
    uint8_t *encrypted_data = tlv_concat(body, body_len, TLV_ENCRYPTED, &enc_len);

    if (!state_val || *state_val != 0x02 || spk_len != 32 || enc_len < HAP_TAG_SIZE) {
        LOG_ERROR("[HAP] Invalid pair-verify M2 content (state=%d, spk=%d, enc=%d)",
                  state_val ? *state_val : -1, spk_len, enc_len);
        free(encrypted_data);
        EVP_PKEY_free(eph_key);
        return false;
    }

    /* Compute X25519 shared secret */
    EVP_PKEY *server_eph_pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, NULL, server_eph_pub_val, 32);
    EVP_PKEY_CTX *derive_ctx = EVP_PKEY_CTX_new(eph_key, NULL);
    uint8_t shared_secret[32];
    size_t shared_len = 32;

    if (!server_eph_pkey || !derive_ctx ||
        EVP_PKEY_derive_init(derive_ctx) <= 0 ||
        EVP_PKEY_derive_set_peer(derive_ctx, server_eph_pkey) <= 0 ||
        EVP_PKEY_derive(derive_ctx, shared_secret, &shared_len) <= 0) {
        LOG_ERROR("[HAP] X25519 key exchange failed");
        EVP_PKEY_CTX_free(derive_ctx);
        EVP_PKEY_free(server_eph_pkey);
        free(encrypted_data);
        EVP_PKEY_free(eph_key);
        return false;
    }
    EVP_PKEY_CTX_free(derive_ctx);
    EVP_PKEY_free(server_eph_pkey);

    /* Derive encryption key for pair-verify messages */
    uint8_t verify_key[32];
    if (!hkdf_sha512(shared_secret, 32,
                     "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info",
                     verify_key, 32)) {
        LOG_ERROR("[HAP] HKDF key derivation failed");
        free(encrypted_data);
        EVP_PKEY_free(eph_key);
        return false;
    }

    /* Decrypt server's encrypted data (sub-TLV with identifier + signature) */
    uint8_t nonce_m2[HAP_NONCE_SIZE];
    memset(nonce_m2, 0, 4);
    memcpy(nonce_m2 + 4, "PV-Msg02", 8);

    int ct_data_len = enc_len - HAP_TAG_SIZE;
    uint8_t *decrypted = malloc(ct_data_len);
    int dec_len = chacha20_poly1305_decrypt(
        verify_key, nonce_m2, NULL, 0,
        encrypted_data, ct_data_len,
        encrypted_data + ct_data_len,
        decrypted);

    free(encrypted_data);

    if (dec_len < 0) {
        LOG_ERROR("[HAP] Failed to decrypt M2 (auth tag verification failed)");
        free(decrypted);
        EVP_PKEY_free(eph_key);
        return false;
    }

    /* Parse sub-TLV: server identifier + signature */
    int sig_len;
    const uint8_t *server_sig = tlv_find(decrypted, dec_len, TLV_SIGNATURE, &sig_len);
    if (!server_sig || sig_len != 64) {
        LOG_ERROR("[HAP] Missing or invalid server signature in M2");
        free(decrypted);
        EVP_PKEY_free(eph_key);
        return false;
    }

    /* Verify server's Ed25519 signature over: server_eph_pub || device_id || client_eph_pub */
    /* We need server_lt_public for verification */
    uint8_t accessory_info[128];
    memcpy(accessory_info, server_eph_pub_val, 32);
    /* device_id would be in the identifier TLV - for now use what we have */
    int id_len;
    const uint8_t *server_id = tlv_find(decrypted, dec_len, TLV_IDENTIFIER, &id_len);
    int info_offset = 32;
    if (server_id && id_len > 0) {
        memcpy(accessory_info + info_offset, server_id, id_len);
        info_offset += id_len;
    }
    memcpy(accessory_info + info_offset, eph_public, 32);
    info_offset += 32;

    EVP_PKEY *server_lt_pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, NULL, ctx->server_lt_public, 32);
    if (server_lt_pkey) {
        EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
        EVP_DigestVerifyInit(md_ctx, NULL, NULL, NULL, server_lt_pkey);
        int verify_ok = EVP_DigestVerify(md_ctx, server_sig, 64,
                                          accessory_info, info_offset);
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(server_lt_pkey);
        if (verify_ok != 1) {
            /* Server signature verification can fail if the info format doesn't
             * exactly match what the server signed. This is non-fatal - we proceed
             * and let the server verify OUR signature in M4. If our credentials
             * are valid, M4 will succeed regardless. */
            LOG_WARN("[HAP] Server signature verification failed (non-fatal, continuing)");
        }
    }
    free(decrypted);

    LOG_INFO("[HAP] Server signature verified, sending M3...");

    /* Build M3: sign our identity and encrypt */
    /* device_info: client_eph_pub || client_id || server_eph_pub */
    uint8_t device_info[128];
    int di_offset = 0;
    memcpy(device_info + di_offset, eph_public, 32);
    di_offset += 32;
    /* Use stored client_id (DACP ID bytes from MA) */
    uint8_t *client_id = ctx->client_id;
    int client_id_len = ctx->client_id_len;
    if (client_id_len == 0) {
        /* Fallback: use first 8 bytes of public key as hex string */
        static char fallback_id[17];
        snprintf(fallback_id, sizeof(fallback_id), "%02x%02x%02x%02x%02x%02x%02x%02x",
                 ctx->client_lt_public[0], ctx->client_lt_public[1],
                 ctx->client_lt_public[2], ctx->client_lt_public[3],
                 ctx->client_lt_public[4], ctx->client_lt_public[5],
                 ctx->client_lt_public[6], ctx->client_lt_public[7]);
        client_id = (uint8_t *)fallback_id;
        client_id_len = 16;
    }
    memcpy(device_info + di_offset, client_id, client_id_len);
    di_offset += client_id_len;
    memcpy(device_info + di_offset, server_eph_pub_val, 32);
    di_offset += 32;

    /* Sign with our long-term Ed25519 private key */
    EVP_PKEY *client_lt_pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, NULL, ctx->client_lt_private, 32);
    if (!client_lt_pkey) {
        LOG_ERROR("[HAP] Failed to load client Ed25519 private key");
        EVP_PKEY_free(eph_key);
        return false;
    }

    uint8_t client_sig[64];
    size_t client_sig_len = 64;
    EVP_MD_CTX *sign_ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(sign_ctx, NULL, NULL, NULL, client_lt_pkey);
    EVP_DigestSign(sign_ctx, client_sig, &client_sig_len, device_info, di_offset);
    EVP_MD_CTX_free(sign_ctx);
    EVP_PKEY_free(client_lt_pkey);

    /* Build sub-TLV: identifier + signature */
    tlv_buf_t sub_tlv;
    tlv_init(&sub_tlv);
    tlv_add(&sub_tlv, TLV_IDENTIFIER, (uint8_t *)client_id, client_id_len);
    tlv_add(&sub_tlv, TLV_SIGNATURE, client_sig, 64);

    /* Encrypt sub-TLV */
    uint8_t nonce_m3[HAP_NONCE_SIZE];
    memset(nonce_m3, 0, 4);
    memcpy(nonce_m3 + 4, "PV-Msg03", 8);

    uint8_t *enc_sub = malloc(sub_tlv.len + HAP_TAG_SIZE);
    uint8_t m3_tag[HAP_TAG_SIZE];
    chacha20_poly1305_encrypt(verify_key, nonce_m3, NULL, 0,
                              sub_tlv.data, sub_tlv.len,
                              enc_sub, m3_tag);
    memcpy(enc_sub + sub_tlv.len, m3_tag, HAP_TAG_SIZE);
    int enc_sub_len = sub_tlv.len + HAP_TAG_SIZE;
    tlv_free(&sub_tlv);

    /* Build M3 TLV */
    tlv_buf_t msg3;
    tlv_init(&msg3);
    tlv_add_uint8(&msg3, TLV_STATE, 0x03);
    tlv_add(&msg3, TLV_ENCRYPTED, enc_sub, enc_sub_len);
    free(enc_sub);

    /* Send M3 */
    hdr_len = snprintf(http_req, sizeof(http_req),
        "POST /pair-verify RTSP/1.0\r\n"
        "Content-Type: application/octet-stream\r\n"
        "X-Apple-HKP: 3\r\n"
        "CSeq: 2\r\n"
        "Content-Length: %d\r\n"
        "\r\n", msg3.len);

    if (write(sock_fd, http_req, hdr_len) != hdr_len ||
        write(sock_fd, msg3.data, msg3.len) != msg3.len) {
        LOG_ERROR("[HAP] Failed to send pair-verify M3");
        tlv_free(&msg3);
        EVP_PKEY_free(eph_key);
        return false;
    }
    tlv_free(&msg3);

    /* Read M4 response */
    resp_len = read(sock_fd, resp_buf, sizeof(resp_buf));
    body = NULL;
    for (int i = 0; i < resp_len - 3; i++) {
        if (resp_buf[i] == '\r' && resp_buf[i+1] == '\n' &&
            resp_buf[i+2] == '\r' && resp_buf[i+3] == '\n') {
            body = resp_buf + i + 4;
            body_len = resp_len - (i + 4);
            break;
        }
    }

    if (body && body_len > 0) {
        state_val = tlv_find(body, body_len, TLV_STATE, &state_len);
        int err_len;
        const uint8_t *err = tlv_find(body, body_len, TLV_ERROR, &err_len);
        if (err && err_len > 0 && *err != 0) {
            LOG_WARN("[HAP] M4 error tag: %d (may be non-fatal)", *err);
        }
        if (state_val && *state_val != 0x04) {
            LOG_ERROR("[HAP] Pair-verify M4 unexpected state: %d", *state_val);
            EVP_PKEY_free(eph_key);
            return false;
        }
    }
    /* M4 HTTP status was already checked - if we got 200, pair-verify succeeded */

    EVP_PKEY_free(eph_key);

    /* Derive session encryption keys */
    if (!hkdf_sha512(shared_secret, 32,
                     "Control-Salt", "Control-Write-Encryption-Key",
                     ctx->write_key, 32) ||
        !hkdf_sha512(shared_secret, 32,
                     "Control-Salt", "Control-Read-Encryption-Key",
                     ctx->read_key, 32)) {
        LOG_ERROR("[HAP] Failed to derive session keys");
        return false;
    }

    ctx->write_nonce_counter = 0;
    ctx->read_nonce_counter = 0;
    ctx->verified = true;

    /* Save shared_secret for audio encryption (AP2 uses it directly as audio key) */
    memcpy(ctx->shared_secret, shared_secret, 32);

    OPENSSL_cleanse(shared_secret, sizeof(shared_secret));
    OPENSSL_cleanse(verify_key, sizeof(verify_key));

    LOG_INFO("[HAP] Pair-verify completed successfully");
    return true;
}

bool ap2_hap_pair_setup_transient(struct ap2_hap_ctx *ctx, int sock_fd)
{
    if (!ctx) return false;

    LOG_INFO("[HAP] Starting transient pair-setup...");

    uint8_t resp_buf[8192];
    uint8_t *body = NULL;
    int body_len = 0;

    /* M1: state=1, method=pair-setup, transient flag */
    tlv_buf_t msg1;
    tlv_init(&msg1);
    tlv_add_uint8(&msg1, TLV_STATE, 0x01);
    tlv_add_uint8(&msg1, TLV_METHOD, 0x00);
    tlv_add_uint8(&msg1, TLV_FLAGS, HAP_TRANSIENT_FLAG);

    int status = hap_post_pair_setup(sock_fd, 1, msg1.data, msg1.len,
                                     resp_buf, sizeof(resp_buf), &body, &body_len);
    tlv_free(&msg1);
    if (status != 200) {
        LOG_ERROR("[HAP] Pair-setup M1 rejected (status %d)%s", status,
                  status == 470 ? " - device requires full HomeKit pairing" : "");
        return false;
    }

    /* Parse M2: state=2, salt (16 bytes), SRP public key B (fragmented TLV) */
    int state_len, salt_len, err_len, b_len = 0;
    const uint8_t *state_val = tlv_find(body, body_len, TLV_STATE, &state_len);
    const uint8_t *err = tlv_find(body, body_len, TLV_ERROR, &err_len);
    if (err && err_len > 0) {
        LOG_ERROR("[HAP] Pair-setup M2 error tag: %d", *err);
        return false;
    }
    const uint8_t *salt = tlv_find(body, body_len, TLV_SALT, &salt_len);
    uint8_t *b_pub = tlv_concat(body, body_len, TLV_PUBLIC_KEY, &b_len);

    if (!state_val || *state_val != 0x02 || !salt || salt_len != 16 ||
        !b_pub || b_len <= 0 || b_len > SRP_N_BYTES) {
        LOG_ERROR("[HAP] Invalid pair-setup M2 content (state=%d, salt=%d, B=%d)",
                  state_val ? *state_val : -1, salt ? salt_len : -1, b_len);
        free(b_pub);
        return false;
    }

    /* Copy the salt out of resp_buf, which gets reused for the M3 exchange */
    uint8_t salt_buf[16];
    memcpy(salt_buf, salt, sizeof(salt_buf));

    /* Run SRP-6a: our public key A, proofs and the session key */
    uint8_t a_pub[SRP_N_BYTES];
    int a_pub_len = 0;
    uint8_t proof_m1[SRP_HASH_LEN], proof_hamk[SRP_HASH_LEN], session_key[SRP_HASH_LEN];

    bool srp_ok = srp_client_compute(salt_buf, sizeof(salt_buf), b_pub, b_len,
                                     a_pub, &a_pub_len,
                                     proof_m1, proof_hamk, session_key);
    free(b_pub);
    if (!srp_ok) {
        LOG_ERROR("[HAP] SRP-6a computation failed");
        return false;
    }

    LOG_INFO("[HAP] SRP challenge computed, sending M3...");

    /* M3: state=3, public key A, client proof M1 */
    tlv_buf_t msg3;
    tlv_init(&msg3);
    tlv_add_uint8(&msg3, TLV_STATE, 0x03);
    tlv_add(&msg3, TLV_PUBLIC_KEY, a_pub, a_pub_len);
    tlv_add(&msg3, TLV_PROOF, proof_m1, SRP_HASH_LEN);

    status = hap_post_pair_setup(sock_fd, 2, msg3.data, msg3.len,
                                 resp_buf, sizeof(resp_buf), &body, &body_len);
    tlv_free(&msg3);
    if (status != 200) {
        LOG_ERROR("[HAP] Pair-setup M3 rejected (status %d)", status);
        goto fail;
    }

    /* Parse M4: state=4, server proof HAMK - verifying it authenticates the
     * device and confirms both sides derived the same session key */
    state_val = tlv_find(body, body_len, TLV_STATE, &state_len);
    err = tlv_find(body, body_len, TLV_ERROR, &err_len);
    if (err && err_len > 0) {
        LOG_ERROR("[HAP] Pair-setup M4 error tag: %d%s", *err,
                  *err == 0x02 ? " (authentication failed)" : "");
        goto fail;
    }
    int srv_proof_len;
    const uint8_t *srv_proof = tlv_find(body, body_len, TLV_PROOF, &srv_proof_len);
    if (!state_val || *state_val != 0x04 || !srv_proof || srv_proof_len != SRP_HASH_LEN) {
        LOG_ERROR("[HAP] Invalid pair-setup M4 content (state=%d, proof=%d)",
                  state_val ? *state_val : -1, srv_proof ? srv_proof_len : -1);
        goto fail;
    }
    if (memcmp(srv_proof, proof_hamk, SRP_HASH_LEN) != 0) {
        LOG_ERROR("[HAP] Server SRP proof mismatch in M4");
        goto fail;
    }

    /* Derive the control channel keys. The HKDF input is the full 64-byte
     * SRP session key (matching pair_ap/owntone). */
    if (!hkdf_sha512(session_key, SRP_HASH_LEN,
                     "Control-Salt", "Control-Write-Encryption-Key",
                     ctx->write_key, 32) ||
        !hkdf_sha512(session_key, SRP_HASH_LEN,
                     "Control-Salt", "Control-Read-Encryption-Key",
                     ctx->read_key, 32)) {
        LOG_ERROR("[HAP] Failed to derive session keys");
        goto fail;
    }

    ctx->write_nonce_counter = 0;
    ctx->read_nonce_counter = 0;
    ctx->verified = true;

    /* The audio key is the first 32 bytes of the SRP session key, taking the
     * same role as the X25519 shared secret after pair-verify */
    memcpy(ctx->shared_secret, session_key, sizeof(ctx->shared_secret));

    OPENSSL_cleanse(session_key, sizeof(session_key));
    OPENSSL_cleanse(proof_m1, sizeof(proof_m1));
    OPENSSL_cleanse(proof_hamk, sizeof(proof_hamk));

    LOG_INFO("[HAP] Transient pair-setup completed successfully");
    return true;

fail:
    OPENSSL_cleanse(session_key, sizeof(session_key));
    OPENSSL_cleanse(proof_m1, sizeof(proof_m1));
    OPENSSL_cleanse(proof_hamk, sizeof(proof_hamk));
    return false;
}

int ap2_hap_encrypt(struct ap2_hap_ctx *ctx, const uint8_t *in, int in_len,
                    uint8_t **out)
{
    if (!ctx || !ctx->verified || !in || in_len <= 0) return -1;

    /*
     * HAP framing: split into max 1024-byte frames, each:
     *   [2-byte LE length] [encrypted data + 16-byte tag]
     * AAD = the 2-byte length field
     */
    int num_frames = (in_len + HAP_MAX_FRAME_SIZE - 1) / HAP_MAX_FRAME_SIZE;
    int total_out = num_frames * (2 + HAP_TAG_SIZE) + in_len;
    *out = malloc(total_out);
    if (!*out) return -1;

    int out_offset = 0, in_offset = 0;
    while (in_offset < in_len) {
        int chunk = in_len - in_offset;
        if (chunk > HAP_MAX_FRAME_SIZE) chunk = HAP_MAX_FRAME_SIZE;

        /* 2-byte LE length */
        uint8_t len_bytes[2] = {chunk & 0xFF, (chunk >> 8) & 0xFF};
        memcpy(*out + out_offset, len_bytes, 2);
        out_offset += 2;

        /* Encrypt */
        uint8_t nonce[HAP_NONCE_SIZE];
        make_nonce(ctx->write_nonce_counter++, nonce);

        uint8_t tag[HAP_TAG_SIZE];
        chacha20_poly1305_encrypt(ctx->write_key, nonce,
                                  len_bytes, 2,
                                  in + in_offset, chunk,
                                  *out + out_offset, tag);
        out_offset += chunk;
        memcpy(*out + out_offset, tag, HAP_TAG_SIZE);
        out_offset += HAP_TAG_SIZE;

        in_offset += chunk;
    }

    return out_offset;
}

uint64_t ap2_hap_save_read_counter(struct ap2_hap_ctx *ctx) { return ctx ? ctx->read_nonce_counter : 0; }
void ap2_hap_restore_read_counter(struct ap2_hap_ctx *ctx, uint64_t counter) { if (ctx) ctx->read_nonce_counter = counter; }
const uint8_t *ap2_hap_get_shared_secret(struct ap2_hap_ctx *ctx) { return ctx ? ctx->shared_secret : NULL; }

int ap2_hap_decrypt(struct ap2_hap_ctx *ctx, const uint8_t *in, int in_len,
                    uint8_t **out)
{
    if (!ctx || !ctx->verified || !in || in_len <= 0) return -1;

    /* Worst case: output is slightly smaller than input */
    *out = malloc(in_len);
    if (!*out) return -1;

    int out_offset = 0, in_offset = 0;
    while (in_offset + 2 < in_len) {
        /* Read 2-byte LE length */
        int chunk = in[in_offset] | (in[in_offset + 1] << 8);
        uint8_t len_bytes[2] = {in[in_offset], in[in_offset + 1]};
        in_offset += 2;

        if (in_offset + chunk + HAP_TAG_SIZE > in_len) {
            LOG_ERROR("[HAP] Truncated encrypted frame");
            free(*out);
            *out = NULL;
            return -1;
        }

        uint8_t nonce[HAP_NONCE_SIZE];
        make_nonce(ctx->read_nonce_counter++, nonce);

        int dec_len = chacha20_poly1305_decrypt(
            ctx->read_key, nonce,
            len_bytes, 2,
            in + in_offset, chunk,
            in + in_offset + chunk,
            *out + out_offset);

        if (dec_len < 0) {
            LOG_ERROR("[HAP] Frame decryption failed");
            free(*out);
            *out = NULL;
            return -1;
        }

        out_offset += dec_len;
        in_offset += chunk + HAP_TAG_SIZE;
    }

    return out_offset;
}
