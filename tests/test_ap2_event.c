#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/kdf.h>

#include "ap2_mrp.h"
#include "cross_log.h"

#define KEY_SIZE 32
#define TAG_SIZE 16

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;

static void derive_key(const uint8_t secret[KEY_SIZE], const char *info,
                       uint8_t key[KEY_SIZE])
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    size_t len = KEY_SIZE;
    assert(ctx);
    assert(EVP_PKEY_derive_init(ctx) > 0);
    assert(EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha512()) > 0);
    assert(EVP_PKEY_CTX_set1_hkdf_salt(
               ctx, (const unsigned char *)"Events-Salt", 11) > 0);
    assert(EVP_PKEY_CTX_set1_hkdf_key(ctx, secret, KEY_SIZE) > 0);
    assert(EVP_PKEY_CTX_add1_hkdf_info(
               ctx, (const unsigned char *)info, strlen(info)) > 0);
    assert(EVP_PKEY_derive(ctx, key, &len) > 0);
    EVP_PKEY_CTX_free(ctx);
}

static void make_nonce(uint64_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 12);
    for (int i = 0; i < 8; i++)
        nonce[4 + i] = (uint8_t)(counter >> (i * 8));
}

static int encrypt_frame(const uint8_t key[KEY_SIZE], const uint8_t *plain,
                         int plain_len, uint8_t *frame)
{
    uint8_t aad[2] = {plain_len & 0xff, (plain_len >> 8) & 0xff};
    uint8_t nonce[12];
    make_nonce(0, nonce);
    memcpy(frame, aad, 2);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0, total = 0;
    assert(ctx);
    assert(EVP_EncryptInit_ex(
               ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) > 0);
    assert(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) > 0);
    assert(EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) > 0);
    assert(EVP_EncryptUpdate(ctx, NULL, &len, aad, 2) > 0);
    assert(EVP_EncryptUpdate(
               ctx, frame + 2, &len, plain, plain_len) > 0);
    total = len;
    assert(EVP_EncryptFinal_ex(ctx, frame + 2 + total, &len) > 0);
    total += len;
    assert(EVP_CIPHER_CTX_ctrl(
               ctx, EVP_CTRL_AEAD_GET_TAG, TAG_SIZE,
               frame + 2 + total) > 0);
    EVP_CIPHER_CTX_free(ctx);
    return total + 2 + TAG_SIZE;
}

static int decrypt_frame(const uint8_t key[KEY_SIZE], const uint8_t *frame,
                         int frame_len, uint8_t *plain)
{
    assert(frame_len >= 2 + TAG_SIZE);
    int cipher_len = frame[0] | (frame[1] << 8);
    assert(frame_len == cipher_len + 2 + TAG_SIZE);
    uint8_t nonce[12];
    make_nonce(0, nonce);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0, total = 0;
    assert(ctx);
    assert(EVP_DecryptInit_ex(
               ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) > 0);
    assert(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) > 0);
    assert(EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) > 0);
    assert(EVP_DecryptUpdate(ctx, NULL, &len, frame, 2) > 0);
    assert(EVP_DecryptUpdate(
               ctx, plain, &len, frame + 2, cipher_len) > 0);
    total = len;
    assert(EVP_CIPHER_CTX_ctrl(
               ctx, EVP_CTRL_AEAD_SET_TAG, TAG_SIZE,
               (void *)(frame + 2 + cipher_len)) > 0);
    assert(EVP_DecryptFinal_ex(ctx, plain + total, &len) > 0);
    total += len;
    EVP_CIPHER_CTX_free(ctx);
    return total;
}

int main(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct timeval timeout = {.tv_sec = 2};
    assert(setsockopt(sockets[1], SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout)) == 0);

    uint8_t secret[KEY_SIZE];
    for (int i = 0; i < KEY_SIZE; i++) secret[i] = (uint8_t)i;
    struct ap2_mrp_ctx *mrp = ap2_mrp_create(
        "127.0.0.1", 7000, NULL, "1A2B3D4EA1B2C3D4",
        "Music Assistant", "11111111-2222-4333-8444-555555555555",
        "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE", secret);
    assert(mrp);
    assert(ap2_mrp_attach_events(mrp, sockets[0]));
    assert(ap2_mrp_event_status(mrp) == 1);

    uint8_t receiver_key[KEY_SIZE], sender_key[KEY_SIZE];
    derive_key(secret, "Events-Write-Encryption-Key", receiver_key);
    derive_key(secret, "Events-Read-Encryption-Key", sender_key);

    static const char request[] =
        "POST /command RTSP/1.0\r\n"
        "CSeq: 42\r\n"
        "Server: AirTunes/1.0\r\n"
        "Content-Length: 4\r\n\r\n"
        "test";
    uint8_t encrypted[2048];
    int encrypted_len = encrypt_frame(
        receiver_key, (const uint8_t *)request,
        (int)strlen(request), encrypted);

    /* Exercise TCP fragmentation across the HAP length prefix. */
    assert(write(sockets[1], encrypted, 1) == 1);
    ap2_mrp_tick(mrp);
    assert(write(sockets[1], encrypted + 1,
                 (size_t)(encrypted_len - 1)) == encrypted_len - 1);
    ap2_mrp_tick(mrp);

    uint8_t response_frame[2048], response[2048];
    ssize_t response_frame_len = read(
        sockets[1], response_frame, sizeof(response_frame));
    assert(response_frame_len > 0);
    int response_len = decrypt_frame(
        sender_key, response_frame, (int)response_frame_len, response);
    response[response_len] = '\0';
    assert(strstr((char *)response, "RTSP/1.0 200 OK\r\n"));
    assert(strstr((char *)response, "Content-Length: 0\r\n"));
    assert(strstr((char *)response, "Audio-Latency: 0\r\n"));
    assert(strstr((char *)response, "Server: AirTunes/1.0\r\n"));
    assert(strstr((char *)response, "CSeq: 42\r\n"));

    ap2_mrp_destroy(mrp);
    close(sockets[0]);
    close(sockets[1]);
    return 0;
}
