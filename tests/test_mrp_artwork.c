#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cross_log.h"
#include "artwork.h"
#include "ap2_client.h"
#include "ap2_mrp_sync.h"

#include "mrp_jpeg_fixture.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "%s:%d: check failed: %s\n",                         \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

static bool bytes_contain(const uint8_t *data, size_t data_len,
                          const void *needle, size_t needle_len)
{
    if (!data || !needle || needle_len == 0 || needle_len > data_len)
        return false;
    for (size_t i = 0; i <= data_len - needle_len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static bool bytes_contain_string(const uint8_t *data, size_t data_len,
                                 const char *needle)
{
    return bytes_contain(data, data_len, needle, strlen(needle));
}

static size_t jpeg_marker_offset(const uint8_t *data, size_t len,
                                 uint8_t marker)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == 0xFF && data[i + 1] == marker) return i;
    }
    return len;
}

static uint8_t *make_padded_jpeg(size_t target_size)
{
    if (target_size < sizeof(k_baseline_jpeg) + 4) return NULL;
    uint8_t *out = malloc(target_size);
    if (!out) return NULL;

    memcpy(out, k_baseline_jpeg, 2);
    size_t off = 2;
    size_t remaining = target_size - sizeof(k_baseline_jpeg);
    while (remaining > 0) {
        if (remaining < 4) {
            free(out);
            return NULL;
        }
        size_t total = remaining > 65537 ? 65537 : remaining;
        size_t tail = remaining - total;
        if (tail > 0 && tail < 4) total -= 4 - tail;
        uint16_t segment_len = (uint16_t)(total - 2);
        out[off++] = 0xFF;
        out[off++] = 0xFE;
        out[off++] = (uint8_t)(segment_len >> 8);
        out[off++] = (uint8_t)segment_len;
        memset(out + off, 'M', total - 4);
        off += total - 4;
        remaining -= total;
    }
    memcpy(out + off, k_baseline_jpeg + 2, sizeof(k_baseline_jpeg) - 2);
    off += sizeof(k_baseline_jpeg) - 2;
    if (off != target_size) {
        free(out);
        return NULL;
    }
    return out;
}

static bool write_all(int fd, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t written = write(fd, data + off, len - off);
        if (written <= 0) return false;
        off += (size_t)written;
    }
    return true;
}

static bool test_local_file_boundary(void)
{
    char path[] = "/tmp/cliairplay-mrp-artwork-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(write_all(fd, k_baseline_jpeg, sizeof(k_baseline_jpeg)));
    CHECK(close(fd) == 0);

    uint8_t *loaded = NULL;
    size_t loaded_len = 0;
    char content_type[ARTWORK_CONTENT_TYPE_SIZE];
    char error[160];
    CHECK(artwork_load_file(path, &loaded, &loaded_len, content_type,
                            error, sizeof(error)));
    CHECK(unlink(path) == 0);
    CHECK(loaded_len == sizeof(k_baseline_jpeg));
    CHECK(memcmp(loaded, k_baseline_jpeg, loaded_len) == 0);
    CHECK(strcmp(content_type, "image/jpeg") == 0);
    free(loaded);

    char invalid_path[] = "/tmp/cliairplay-invalid-artwork-XXXXXX";
    fd = mkstemp(invalid_path);
    CHECK(fd >= 0);
    static const uint8_t invalid[] = "not an image";
    CHECK(write_all(fd, invalid, sizeof(invalid) - 1));
    CHECK(close(fd) == 0);
    CHECK(!artwork_load_file(invalid_path, &loaded, &loaded_len, content_type,
                             error, sizeof(error)));
    CHECK(unlink(invalid_path) == 0);
    return true;
}

static bool test_mrp_probe_boundary(void)
{
    ap2_mrp_artwork_info_t info;
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", k_baseline_jpeg, sizeof(k_baseline_jpeg), &info) ==
          AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(info.bytes == sizeof(k_baseline_jpeg));
    CHECK(info.width == 1 && info.height == 1 && info.sof_marker == 0xC0);

    CHECK(ap2_mrp_probe_artwork(
              "image/png", k_baseline_jpeg, sizeof(k_baseline_jpeg), &info) ==
          AP2_MRP_ARTWORK_UNSUPPORTED_TYPE);
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", k_baseline_jpeg, sizeof(k_baseline_jpeg) - 1,
              &info) == AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE);

    static const uint8_t minimal[] = {
        0xFF, 0xD8, 0x01, 0x02, 0x03, 0xFF, 0xD9,
    };
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", minimal, sizeof(minimal), &info) ==
          AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(info.sof_marker == 0 && info.width == 0 && info.height == 0);

    static const uint8_t empty_tables[] = {
        0xFF, 0xD8,
        0xFF, 0xFE, 0x00, 0x07, 'f', 'a', 'k', 'e', '!',
        0xFF, 0xDB, 0x00, 0x02,
        0xFF, 0xC4, 0x00, 0x02,
        0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01, 0x00, 0x01,
        0x01, 0x01, 0x11, 0x00,
        0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00,
        0x01, 0xFF, 0xD9,
    };
    CHECK(sizeof(empty_tables) == 45);
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", empty_tables, sizeof(empty_tables), &info) ==
          AP2_MRP_ARTWORK_ACCEPTED);

    uint8_t changed[sizeof(k_baseline_jpeg)];
    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    size_t sof = jpeg_marker_offset(changed, sizeof(changed), 0xC0);
    CHECK(sof + 11 < sizeof(changed));
    changed[sof + 1] = 0xC1;
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(info.sof_marker == 0xC1);

    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    changed[sof + 1] = 0xC2;
    changed[sof + 7] = 0x02;
    changed[sof + 8] = 0x59;
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(info.progressive && info.width == 601);

    static const size_t matrix_sizes[] = {
        44032, 61440, 65535, 65536, 66560, 102400, 153600,
    };
    for (size_t i = 0; i < sizeof(matrix_sizes) / sizeof(matrix_sizes[0]); i++) {
        uint8_t *matrix_jpeg = make_padded_jpeg(matrix_sizes[i]);
        CHECK(matrix_jpeg != NULL);
        CHECK(ap2_mrp_probe_artwork(
                  "image/jpeg", matrix_jpeg, matrix_sizes[i], &info) ==
              AP2_MRP_ARTWORK_ACCEPTED);
        CHECK(info.bytes == matrix_sizes[i]);
        free(matrix_jpeg);
    }

    uint8_t *oversize =
        calloc(AP2_MRP_ARTWORK_STAGING_MAX_BYTES + 1, 1);
    CHECK(oversize != NULL);
    memcpy(oversize, k_baseline_jpeg, sizeof(k_baseline_jpeg));
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", oversize,
              AP2_MRP_ARTWORK_STAGING_MAX_BYTES + 1, &info) ==
          AP2_MRP_ARTWORK_STAGING_LIMIT);
    free(oversize);
    return true;
}

static bool test_decoder_valid_profile_fixtures(void)
{
    static const struct {
        const char *path;
        uint8_t sof_marker;
        uint8_t precision;
    } cases[] = {
        {"tests/fixtures/sof1-8bit-dqt16.jpg", 0xC1, 8},
        {"tests/fixtures/sof1-12bit.jpg", 0xC1, 12},
        {"tests/fixtures/sof2-12bit.jpg", 0xC2, 12},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t *data = NULL;
        size_t size = 0;
        char content_type[ARTWORK_CONTENT_TYPE_SIZE];
        char error[160];
        CHECK(artwork_load_file(
            cases[i].path, &data, &size, content_type, error, sizeof(error)));
        ap2_mrp_artwork_info_t info;
        CHECK(ap2_mrp_probe_artwork(
                  content_type, data, size, &info) ==
              AP2_MRP_ARTWORK_ACCEPTED);
        CHECK(info.sof_marker == cases[i].sof_marker);
        CHECK(info.precision == cases[i].precision);
        if (i == 0) {
            static const uint8_t dqt16[] = {0xFF, 0xDB, 0x00, 0x83, 0x10};
            CHECK(bytes_contain(data, size, dqt16, sizeof(dqt16)));
        }
        free(data);
    }
    puts("Decoder-valid SOF1/12-bit/16-bit-DQT probes passed");
    return true;
}

static bool test_nowplaying_command_payload(void)
{
    struct ap2_mrp_ctx *mrp = ap2_mrp_create(
        "127.0.0.1", 7000, NULL, "0011223344556677", "Test sender",
        "11111111-1111-1111-1111-111111111111",
        "22222222-2222-2222-2222-222222222222", NULL);
    CHECK(mrp != NULL);
    CHECK(ap2_mrp_set_metadata(mrp, "Title", "Artist", "Album", 180000));
    CHECK(ap2_mrp_set_progress(mrp, 15000, 180000, true));

    ap2_mrp_artwork_info_t info;
    CHECK(ap2_mrp_set_artwork(mrp, "image/jpeg", k_baseline_jpeg,
                              (int)sizeof(k_baseline_jpeg), &info));

    uint8_t *first = NULL;
    int first_len = 0;
    CHECK(ap2_mrp_build_nowplaying_command(mrp, &first, &first_len));
    CHECK(first_len > (int)sizeof(k_baseline_jpeg));
    CHECK(memcmp(first, "bplist00", 8) == 0);
    CHECK(bytes_contain_string(first, (size_t)first_len,
                               "updateMRNowPlayingInfo"));
    CHECK(bytes_contain_string(
        first, (size_t)first_len,
        "kMRMediaRemoteNowPlayingInfoArtworkIdentifier"));
    CHECK(bytes_contain_string(
        first, (size_t)first_len,
        "kMRMediaRemoteNowPlayingInfoArtworkMIMEType"));
    CHECK(bytes_contain_string(
        first, (size_t)first_len,
        "kMRMediaRemoteNowPlayingInfoArtworkData"));
    CHECK(bytes_contain(first, (size_t)first_len,
                        k_baseline_jpeg, sizeof(k_baseline_jpeg)));

    ap2_mrp_mark_artwork_sent(mrp);
    uint8_t *cached = NULL;
    int cached_len = 0;
    CHECK(ap2_mrp_build_nowplaying_command(mrp, &cached, &cached_len));
    CHECK(cached_len < first_len);
    CHECK(!bytes_contain_string(
        cached, (size_t)cached_len,
        "kMRMediaRemoteNowPlayingInfoArtworkData"));
    free(first);
    free(cached);

    uint8_t *large = make_padded_jpeg(153600);
    CHECK(large != NULL);
    CHECK(ap2_mrp_set_artwork(mrp, "image/jpeg", large, 153600, &info));
    uint8_t *large_payload = NULL;
    int large_payload_len = 0;
    CHECK(ap2_mrp_build_nowplaying_command(
        mrp, &large_payload, &large_payload_len));
    CHECK(large_payload_len > 153600);
    CHECK(bytes_contain(large_payload, (size_t)large_payload_len,
                        large, 153600));
    free(large_payload);
    free(large);

    CHECK(!ap2_mrp_set_artwork(mrp, "image/jpeg", k_baseline_jpeg,
                               (int)sizeof(k_baseline_jpeg) - 1, &info));
    CHECK(info.result == AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE);
    uint8_t *cleared = NULL;
    int cleared_len = 0;
    CHECK(ap2_mrp_build_nowplaying_command(mrp, &cleared, &cleared_len));
    CHECK(!bytes_contain_string(
        cleared, (size_t)cleared_len,
        "kMRMediaRemoteNowPlayingInfoArtworkIdentifier"));
    free(cleared);
    ap2_mrp_destroy(mrp);
    return true;
}

typedef struct {
    ap2_mrp_serial_t serial;
    atomic_int active;
    atomic_int max_active;
} serial_test_ctx_t;

static void *serial_test_worker(void *arg)
{
    serial_test_ctx_t *ctx = arg;
    for (int i = 0; i < 1000; i++) {
        ap2_mrp_serial_lock(&ctx->serial);
        int active = atomic_fetch_add(&ctx->active, 1) + 1;
        int observed = atomic_load(&ctx->max_active);
        while (active > observed &&
               !atomic_compare_exchange_weak(
                   &ctx->max_active, &observed, active)) {}
        sched_yield();
        atomic_fetch_sub(&ctx->active, 1);
        ap2_mrp_serial_unlock(&ctx->serial);
    }
    return NULL;
}

static bool test_push_result_scope(void)
{
    ap2_mrp_push_result_t first = ap2_mrp_push_result_empty();
    ap2_mrp_push_result_t second = ap2_mrp_push_result_empty();
    first.overall_status = 503;
    first.nowplaying_status = 400;
    CHECK(second.overall_status == -1);
    CHECK(second.nowplaying_status == 0);

    serial_test_ctx_t ctx = {0};
    ap2_mrp_serial_init(&ctx.serial);
    pthread_t threads[4];
    for (size_t i = 0; i < 4; i++)
        CHECK(pthread_create(
                  &threads[i], NULL, serial_test_worker, &ctx) == 0);
    for (size_t i = 0; i < 4; i++)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(atomic_load(&ctx.active) == 0);
    CHECK(atomic_load(&ctx.max_active) == 1);
    ap2_mrp_serial_destroy(&ctx.serial);
    puts("MRP push status scope and serialization tests passed");
    return true;
}

static bool probe_generated_case(const char *path)
{
    uint8_t *data = NULL;
    size_t size = 0;
    char content_type[ARTWORK_CONTENT_TYPE_SIZE];
    char error[160];
    CHECK(artwork_load_file(path, &data, &size, content_type,
                            error, sizeof(error)));
    ap2_mrp_artwork_info_t info;
    ap2_mrp_artwork_result_t result =
        ap2_mrp_probe_artwork(content_type, data, size, &info);
    free(data);
    CHECK(result == AP2_MRP_ARTWORK_ACCEPTED);
    printf("%s: %zu bytes %ux%u SOF=0x%02x components=%u progressive=%d\n",
           path, size, info.width, info.height, info.sof_marker,
           info.components, info.progressive);
    return true;
}

int main(int argc, char **argv)
{
    if (!test_local_file_boundary() ||
        !test_mrp_probe_boundary() ||
        !test_decoder_valid_profile_fixtures() ||
        !test_nowplaying_command_payload() ||
        !test_push_result_scope())
        return 1;
    for (int i = 1; i < argc; i++) {
        if (!probe_generated_case(argv[i])) return 1;
    }
    puts("MRP artwork probe tests passed");
    return 0;
}
