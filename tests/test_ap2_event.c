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
#include "ap2_plist.h"
#include "cross_log.h"

#define KEY_SIZE 32
#define TAG_SIZE 16

struct command_capture {
    ap2_remote_command_t commands[16];
    int count;
};

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;

static void capture_remote_command(
    ap2_remote_command_t command, void *userdata)
{
    struct command_capture *capture = userdata;
    assert(capture && capture->count < 16);
    capture->commands[capture->count++] = command;
}

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

static struct ap2_mrp_ctx *create_mrp_with_callback(
    const uint8_t secret[KEY_SIZE], int event_sock,
    ap2_remote_command_cb_t callback, void *userdata)
{
    struct ap2_mrp_ctx *mrp = ap2_mrp_create(
        "127.0.0.1", 7000, NULL, "1A2B3D4EA1B2C3D4",
        "Music Assistant", "11111111-2222-4333-8444-555555555555",
        "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE", secret);
    assert(mrp);
    ap2_mrp_set_remote_command_callback(mrp, callback, userdata);
    assert(ap2_mrp_attach_events(mrp, event_sock));
    assert(ap2_mrp_event_status(mrp) == 1);
    return mrp;
}

static struct ap2_mrp_ctx *create_mrp(const uint8_t secret[KEY_SIZE],
                                      int event_sock)
{
    return create_mrp_with_callback(secret, event_sock, NULL, NULL);
}

static int build_event_request(uint8_t *encrypted, size_t encrypted_size,
                               const uint8_t key[KEY_SIZE],
                               uint64_t counter, int cseq,
                               const uint8_t *body, int body_len)
{
    uint8_t request[1024];
    int header_len = snprintf(
        (char *)request, sizeof(request),
        "POST /command RTSP/1.0\r\n"
        "CSeq: %d\r\n"
        "Server: AirTunes/1.0\r\n"
        "Content-Type: application/x-apple-binary-plist\r\n"
        "Content-Length: %d\r\n\r\n",
        cseq, body_len);
    assert(header_len > 0 && header_len + body_len < (int)sizeof(request));
    memcpy(request + header_len, body, (size_t)body_len);

    int request_len = header_len + body_len;
    assert((size_t)(request_len + 2 + TAG_SIZE) <= encrypted_size);
    return encrypt_frame(
        key, counter, request, request_len, encrypted);
}

static int build_typed_command_request(
    uint8_t *encrypted, size_t encrypted_size,
    const uint8_t key[KEY_SIZE], uint64_t counter, int cseq,
    const char *type, const char *value)
{
    ap2_pl_node *root = ap2_pl_dict();
    ap2_pl_dict_set(root, "type", ap2_pl_string(type));
    ap2_pl_dict_set(root, "value", ap2_pl_string(value));
    uint8_t *body = NULL;
    int body_len = ap2_pl_serialize(root, &body);
    ap2_pl_free(root);
    assert(body_len > 0 && body);
    int encrypted_len = build_event_request(
        encrypted, encrypted_size, key, counter, cseq, body, body_len);
    free(body);
    return encrypted_len;
}

static int build_command_request(uint8_t *encrypted, size_t encrypted_size,
                                 const uint8_t key[KEY_SIZE],
                                 uint64_t counter, int cseq,
                                 const char *value)
{
    return build_typed_command_request(
        encrypted, encrypted_size, key, counter, cseq,
        "sendMediaRemoteCommand", value);
}

static int build_embedded_nul_request(
    uint8_t *encrypted, size_t encrypted_size,
    const uint8_t key[KEY_SIZE], uint64_t counter, int cseq)
{
    ap2_pl_node *root = ap2_pl_dict();
    ap2_pl_dict_set(root, "type",
                    ap2_pl_string("sendMediaRemoteCommand"));
    ap2_pl_dict_set(root, "value", ap2_pl_string("playXjunk"));
    uint8_t *body = NULL;
    int body_len = ap2_pl_serialize(root, &body);
    ap2_pl_free(root);
    assert(body_len > 0 && body);

    bool replaced = false;
    for (int i = 0; i + 9 <= body_len; i++) {
        if (memcmp(body + i, "playXjunk", 9) == 0) {
            body[i + 4] = '\0';
            replaced = true;
            break;
        }
    }
    assert(replaced);
    int encrypted_len = build_event_request(
        encrypted, encrypted_size, key, counter, cseq, body, body_len);
    free(body);
    return encrypted_len;
}

static int build_nested_command_request(
    uint8_t *encrypted, size_t encrypted_size,
    const uint8_t key[KEY_SIZE], uint64_t counter, int cseq)
{
    ap2_pl_node *nested = ap2_pl_dict();
    ap2_pl_dict_set(nested, "type",
                    ap2_pl_string("sendMediaRemoteCommand"));
    ap2_pl_dict_set(nested, "value", ap2_pl_string("play"));
    ap2_pl_node *root = ap2_pl_dict();
    ap2_pl_dict_set(root, "payload", nested);
    uint8_t *body = NULL;
    int body_len = ap2_pl_serialize(root, &body);
    ap2_pl_free(root);
    assert(body_len > 0 && body);
    int encrypted_len = build_event_request(
        encrypted, encrypted_size, key, counter, cseq, body, body_len);
    free(body);
    return encrypted_len;
}

static void send_request(int fd, const uint8_t key[KEY_SIZE],
                         uint64_t counter, int cseq, const char *value)
{
    uint8_t encrypted[1200];
    int encrypted_len = build_command_request(
        encrypted, sizeof(encrypted), key, counter, cseq, value);
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
    struct command_capture capture = {0};
    struct ap2_mrp_ctx *mrp = create_mrp_with_callback(
        secret, sockets[0], capture_remote_command, &capture);

    int saved_stderr = dup(STDERR_FILENO);
    FILE *log_capture = tmpfile();
    assert(saved_stderr >= 0 && log_capture);
    assert(dup2(fileno(log_capture), STDERR_FILENO) >= 0);
    test_log_level = lDEBUG;

    uint8_t encrypted[1200];
    int encrypted_len = build_command_request(
        encrypted, sizeof(encrypted), receiver_key, 0, 42, "play");
    assert(write(sockets[1], encrypted, 1) == 1);
    ap2_mrp_tick(mrp);
    assert(write(sockets[1], encrypted + 1,
                 (size_t)(encrypted_len - 1)) == encrypted_len - 1);
    send_request(sockets[1], receiver_key, 1, 43, "paus");
    ap2_mrp_tick(mrp);
    read_response(sockets[1], sender_key, 0, 42);
    read_response(sockets[1], sender_key, 1, 43);

    static const char *values[] = {"plps", "nitm", "pitm"};
    for (int i = 0; i < 3; i++) {
        send_request(sockets[1], receiver_key, (uint64_t)i + 2,
                     i + 44, values[i]);
        ap2_mrp_tick(mrp);
        read_response(sockets[1], sender_key, (uint64_t)i + 2, i + 44);
    }

    send_request(sockets[1], receiver_key, 5, 47, "unknown");
    ap2_mrp_tick(mrp);
    read_response(sockets[1], sender_key, 5, 47);

    encrypted_len = build_typed_command_request(
        encrypted, sizeof(encrypted), receiver_key, 6, 48,
        "updateInfo", "play");
    assert(write(sockets[1], encrypted, (size_t)encrypted_len) == encrypted_len);
    ap2_mrp_tick(mrp);
    read_response(sockets[1], sender_key, 6, 48);

    encrypted_len = build_embedded_nul_request(
        encrypted, sizeof(encrypted), receiver_key, 7, 49);
    assert(write(sockets[1], encrypted, (size_t)encrypted_len) == encrypted_len);
    ap2_mrp_tick(mrp);
    read_response(sockets[1], sender_key, 7, 49);

    encrypted_len = build_nested_command_request(
        encrypted, sizeof(encrypted), receiver_key, 8, 50);
    assert(write(sockets[1], encrypted, (size_t)encrypted_len) == encrypted_len);
    ap2_mrp_tick(mrp);
    read_response(sockets[1], sender_key, 8, 50);

    uint8_t corrupt_bplist[40] = {0};
    memcpy(corrupt_bplist, "bplist00", 8);
    corrupt_bplist[14] = 1;  /* offset size */
    corrupt_bplist[15] = 1;  /* object-reference size */
    corrupt_bplist[23] = 1;  /* object count */
    corrupt_bplist[39] = 8;  /* invalid offset-table location */
    encrypted_len = build_event_request(
        encrypted, sizeof(encrypted), receiver_key, 9, 51,
        corrupt_bplist, (int)sizeof(corrupt_bplist));
    assert(write(sockets[1], encrypted, (size_t)encrypted_len) == encrypted_len);
    ap2_mrp_tick(mrp);
    read_response(sockets[1], sender_key, 9, 51);

    static const uint8_t invalid_body[] = {'t', 'e', 's', 't'};
    encrypted_len = build_event_request(
        encrypted, sizeof(encrypted), receiver_key, 10, 52,
        invalid_body, (int)sizeof(invalid_body));
    assert(write(sockets[1], encrypted, (size_t)encrypted_len) == encrypted_len);
    ap2_mrp_tick(mrp);
    read_response(sockets[1], sender_key, 10, 52);

    static const ap2_remote_command_t expected_commands[] = {
        AP2_REMOTE_COMMAND_PLAY,
        AP2_REMOTE_COMMAND_PAUSE,
        AP2_REMOTE_COMMAND_PLAY_PAUSE,
        AP2_REMOTE_COMMAND_NEXT,
        AP2_REMOTE_COMMAND_PREVIOUS,
    };
    static const char *expected_names[] = {
        "play", "pause", "play_pause", "next", "previous",
    };
    assert(capture.count == 5);
    for (int i = 0; i < capture.count; i++) {
        assert(capture.commands[i] == expected_commands[i]);
        assert(strcmp(
                   ap2_remote_command_name(capture.commands[i]),
                   expected_names[i]) == 0);
    }

    fflush(stderr);
    test_log_level = lSILENCE;
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    assert(fseek(log_capture, 0, SEEK_SET) == 0);
    char logs[8192];
    size_t logs_len = fread(logs, 1, sizeof(logs) - 1, log_capture);
    logs[logs_len] = '\0';
    fclose(log_capture);
    assert(strstr(logs, "content_type=application/x-apple-binary-plist"));
    assert(strstr(logs, "type=sendMediaRemoteCommand value=play"));
    assert(strstr(logs, "type=sendMediaRemoteCommand value=paus"));
    assert(strstr(logs, "type=sendMediaRemoteCommand value=plps"));
    assert(strstr(logs, "type=sendMediaRemoteCommand value=nitm"));
    assert(strstr(logs, "type=sendMediaRemoteCommand value=pitm"));
    assert(strstr(logs, "body parse failed"));
    assert(strstr(logs, "content_length=4 hex=74657374"));

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
                         100 + (int)counter, "play");
        ap2_mrp_tick(mrp);
    }
    assert(ap2_mrp_event_status(mrp) == 0);
    ap2_mrp_destroy(mrp);
    close(sockets[1]);
    return 0;
}
