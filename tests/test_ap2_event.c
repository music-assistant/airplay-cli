#include <assert.h>
#include <stdlib.h>
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

static int encrypt_frame(const uint8_t key[KEY_SIZE], uint64_t counter,
                         const uint8_t *plain, int plain_len, uint8_t *frame)
{
    uint8_t aad[2] = {plain_len & 0xff, (plain_len >> 8) & 0xff};
    uint8_t nonce[12];
    make_nonce(counter, nonce);
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

static int decrypt_frame(const uint8_t key[KEY_SIZE], uint64_t counter,
                         const uint8_t *frame, int frame_len, uint8_t *plain)
{
    assert(frame_len >= 2 + TAG_SIZE);
    int cipher_len = frame[0] | (frame[1] << 8);
    assert(frame_len == cipher_len + 2 + TAG_SIZE);
    uint8_t nonce[12];
    make_nonce(counter, nonce);

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

static struct ap2_mrp_ctx *create_mrp(const uint8_t secret[KEY_SIZE],
                                      int event_sock)
{
    struct ap2_mrp_ctx *mrp = ap2_mrp_create(
        "127.0.0.1", 7000, NULL, "1A2B3D4EA1B2C3D4",
        "Music Assistant", "11111111-2222-4333-8444-555555555555",
        "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE", secret);
    assert(mrp);
    assert(ap2_mrp_attach_events(mrp, event_sock));
    assert(ap2_mrp_event_status(mrp) == 1);
    return mrp;
}

static void send_request(int fd, const uint8_t key[KEY_SIZE],
                         uint64_t counter, int cseq)
{
    char request[256];
    int request_len = snprintf(
        request, sizeof(request),
        "POST /command RTSP/1.0\r\n"
        "CSeq: %d\r\n"
        "Server: AirTunes/1.0\r\n"
        "Content-Length: 4\r\n\r\n"
        "test",
        cseq);
    assert(request_len > 0 && request_len < (int)sizeof(request));
    uint8_t encrypted[512];
    int encrypted_len = encrypt_frame(
        key, counter, (const uint8_t *)request, request_len, encrypted);
    assert(write(fd, encrypted, (size_t)encrypted_len) == encrypted_len);
}

static void read_response(int fd, const uint8_t key[KEY_SIZE],
                          uint64_t counter, int cseq)
{
    uint8_t response_frame[2048], response[2048];
    assert(recv(fd, response_frame, 2, MSG_WAITALL) == 2);
    int cipher_len = response_frame[0] | (response_frame[1] << 8);
    int remaining = cipher_len + TAG_SIZE;
    assert(recv(fd, response_frame + 2, (size_t)remaining, MSG_WAITALL) ==
           remaining);
    int response_len = decrypt_frame(
        key, counter, response_frame, remaining + 2, response);
    response[response_len] = '\0';
    char expected_cseq[32];
    snprintf(expected_cseq, sizeof(expected_cseq), "CSeq: %d\r\n", cseq);
    assert(strstr((char *)response, "RTSP/1.0 200 OK\r\n"));
    assert(strstr((char *)response, "Content-Length: 0\r\n"));
    assert(strstr((char *)response, "Audio-Latency: 0\r\n"));
    assert(strstr((char *)response, "Server: AirTunes/1.0\r\n"));
    assert(strstr((char *)response, expected_cseq));
}

static bool contains_bytes(const uint8_t *haystack, int haystack_len,
                           const uint8_t *needle, int needle_len)
{
    for (int i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, (size_t)needle_len) == 0)
            return true;
    }
    return false;
}

static void test_artwork_snapshot_generation(const uint8_t secret[KEY_SIZE])
{
    static const uint8_t first_art[] =
        {0xff, 0xd8, 0xf1, 0x11, 0x22, 0x33, 0xff, 0xd9};
    static const uint8_t second_art[] =
        {0xff, 0xd8, 0xe2, 0x91, 0x82, 0x73, 0xff, 0xd9};
    struct ap2_mrp_ctx *mrp = ap2_mrp_create(
        "127.0.0.1", 7000, NULL, "1A2B3D4EA1B2C3D4",
        "Music Assistant", "11111111-2222-4333-8444-555555555555",
        "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE", secret);
    assert(mrp);

    assert(ap2_mrp_set_artwork(
        mrp, "image/jpeg", first_art, (int)sizeof(first_art), NULL));
    uint64_t first_generation = ap2_mrp_artwork_generation(mrp);
    uint8_t *first_snapshot = NULL;
    int first_len = 0;
    assert(ap2_mrp_build_nowplaying_command(
        mrp, &first_snapshot, &first_len));
    assert(contains_bytes(
        first_snapshot, first_len, first_art, (int)sizeof(first_art)));

    assert(ap2_mrp_set_artwork(
        mrp, "image/jpeg", second_art, (int)sizeof(second_art), NULL));
    ap2_mrp_mark_artwork_sent_if_generation(mrp, first_generation);

    uint8_t *second_snapshot = NULL;
    int second_len = 0;
    assert(ap2_mrp_build_nowplaying_command(
        mrp, &second_snapshot, &second_len));
    assert(contains_bytes(
        second_snapshot, second_len, second_art, (int)sizeof(second_art)));

    uint64_t second_generation = ap2_mrp_artwork_generation(mrp);
    ap2_mrp_mark_artwork_sent_if_generation(mrp, second_generation);
    uint8_t *reference_snapshot = NULL;
    int reference_len = 0;
    assert(ap2_mrp_build_nowplaying_command(
        mrp, &reference_snapshot, &reference_len));
    assert(!contains_bytes(
        reference_snapshot, reference_len,
        second_art, (int)sizeof(second_art)));

    free(first_snapshot);
    free(second_snapshot);
    free(reference_snapshot);
    ap2_mrp_destroy(mrp);
}

int main(void)
{
    uint8_t secret[KEY_SIZE];
    for (int i = 0; i < KEY_SIZE; i++) secret[i] = (uint8_t)i;
    uint8_t receiver_key[KEY_SIZE], sender_key[KEY_SIZE];
    derive_key(secret, "Events-Write-Encryption-Key", receiver_key);
    derive_key(secret, "Events-Read-Encryption-Key", sender_key);
    test_artwork_snapshot_generation(secret);

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct timeval timeout = {.tv_sec = 2};
    assert(setsockopt(sockets[1], SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout)) == 0);
    struct ap2_mrp_ctx *mrp = create_mrp(secret, sockets[0]);

    char request[256];
    int request_len = snprintf(
        request, sizeof(request),
        "POST /command RTSP/1.0\r\nCSeq: 42\r\n"
        "Server: AirTunes/1.0\r\nContent-Length: 4\r\n\r\ntest");
    uint8_t encrypted[512];
    int encrypted_len = encrypt_frame(
        receiver_key, 0, (const uint8_t *)request, request_len, encrypted);
    assert(write(sockets[1], encrypted, 1) == 1);
    ap2_mrp_tick(mrp);
    assert(write(sockets[1], encrypted + 1,
                 (size_t)(encrypted_len - 1)) == encrypted_len - 1);
    send_request(sockets[1], receiver_key, 1, 43);
    ap2_mrp_tick(mrp);
    read_response(sockets[1], sender_key, 0, 42);
    read_response(sockets[1], sender_key, 1, 43);
    ap2_mrp_destroy(mrp);
    close(sockets[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    mrp = create_mrp(secret, sockets[0]);
    close(sockets[1]);
    ap2_mrp_tick(mrp);
    assert(ap2_mrp_event_status(mrp) == 0);
    ap2_mrp_destroy(mrp);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int small_buffer = 1024;
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                     &small_buffer, sizeof(small_buffer)) == 0);
    assert(setsockopt(sockets[1], SOL_SOCKET, SO_RCVBUF,
                     &small_buffer, sizeof(small_buffer)) == 0);
    mrp = create_mrp(secret, sockets[0]);
    uint64_t counter = 0;
    for (int batch = 0;
         batch < 20 && ap2_mrp_event_status(mrp) == 1;
         batch++) {
        for (int i = 0; i < 10; i++, counter++)
            send_request(sockets[1], receiver_key, counter,
                         100 + (int)counter);
        ap2_mrp_tick(mrp);
    }
    assert(ap2_mrp_event_status(mrp) == 0);
    ap2_mrp_destroy(mrp);
    close(sockets[1]);
    return 0;
}
