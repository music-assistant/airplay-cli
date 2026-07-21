#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cross_log.h"
#include "artwork.h"
#include "ap2_client.h"
#include "ap2_mrp_sync.h"

#include "mrp_jpeg_fixture.h"
#include "mrp_jpeg_profile_fixtures.h"

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

typedef struct {
    int status;
    const uint8_t *expected;
    size_t expected_len;
    bool saw_expected;
} capture_sender_t;

static int capture_sender(void *opaque, const uint8_t *body, int body_len)
{
    capture_sender_t *sender = opaque;
    sender->saw_expected =
        bytes_contain(body, (size_t)body_len,
                      sender->expected, sender->expected_len);
    return sender->status;
}

static size_t jpeg_marker_offset(const uint8_t *data, size_t len,
                                 uint8_t marker)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == 0xFF && data[i + 1] == marker) return i;
    }
    return len;
}

static size_t jpeg_ac_scan_offset(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i + 9 < len; i++) {
        if (data[i] != 0xFF || data[i + 1] != 0xDA) continue;
        size_t segment_len = ((size_t)data[i + 2] << 8) | data[i + 3];
        if (segment_len < 6 || i + 2 + segment_len > len) continue;
        uint8_t components = data[i + 4];
        if (segment_len != 6 + 2 * (size_t)components) continue;
        size_t spectral_start = i + 5 + 2 * (size_t)components;
        if (data[spectral_start] > 0) return i;
    }
    return len;
}

static size_t jpeg_ac_refinement_scan_offset(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i + 9 < len; i++) {
        if (data[i] != 0xFF || data[i + 1] != 0xDA) continue;
        size_t segment_len = ((size_t)data[i + 2] << 8) | data[i + 3];
        if (segment_len < 6 || i + 2 + segment_len > len) continue;
        uint8_t components = data[i + 4];
        if (segment_len != 6 + 2 * (size_t)components) continue;
        size_t spectral_start = i + 5 + 2 * (size_t)components;
        size_t approximation = spectral_start + 2;
        if (data[spectral_start] > 0 && (data[approximation] >> 4) > 0)
            return i;
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
    CHECK(info.parsed_strictly);
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", k_progressive_jpeg, sizeof(k_progressive_jpeg),
              &info) == AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(info.progressive && info.sof_marker == 0xC2 &&
          info.parsed_strictly);
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", k_grayscale_jpeg, sizeof(k_grayscale_jpeg),
              &info) == AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(info.components == 1 && info.parsed_strictly);

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
          AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE);

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
          AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE);

    uint8_t changed[sizeof(k_baseline_jpeg)];
    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    size_t sof = jpeg_marker_offset(changed, sizeof(changed), 0xC0);
    CHECK(sof + 11 < sizeof(changed));
    changed[sof + 1] = 0xC1;
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(info.sof_marker == 0xC1 && !info.parsed_strictly);

    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    changed[sof + 1] = 0xC2;
    changed[sof + 7] = 0x02;
    changed[sof + 8] = 0x59;
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE);

    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    changed[sof + 11] = 0x44;
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE);

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

    static const struct {
        const uint8_t *data;
        size_t len;
        uint8_t precision;
        uint8_t sof_marker;
        bool progressive;
    } unsupported[] = {
        {
            k_sof1_8bit_16dqt_jpeg, sizeof(k_sof1_8bit_16dqt_jpeg),
            8, 0xC1, false,
        },
        {
            k_sof1_12bit_jpeg, sizeof(k_sof1_12bit_jpeg),
            12, 0xC1, false,
        },
        {
            k_sof2_12bit_jpeg, sizeof(k_sof2_12bit_jpeg),
            12, 0xC2, true,
        },
    };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        CHECK(ap2_mrp_probe_artwork(
                  "image/jpeg", unsupported[i].data, unsupported[i].len,
                  &info) == AP2_MRP_ARTWORK_ACCEPTED);
        CHECK(!info.parsed_strictly);
        CHECK(info.precision == unsupported[i].precision);
        CHECK(info.sof_marker == unsupported[i].sof_marker);
        CHECK(info.progressive == unsupported[i].progressive);
    }

    uint8_t malformed_dqt[sizeof(k_sof1_8bit_16dqt_jpeg)];
    memcpy(malformed_dqt, k_sof1_8bit_16dqt_jpeg, sizeof(malformed_dqt));
    size_t dqt = jpeg_marker_offset(
        malformed_dqt, sizeof(malformed_dqt), 0xDB);
    CHECK(dqt + 6 < sizeof(malformed_dqt));
    malformed_dqt[dqt + 5] = 0;
    malformed_dqt[dqt + 6] = 0;
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", malformed_dqt, sizeof(malformed_dqt), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE);

    uint8_t malformed_scan[sizeof(k_sof1_12bit_jpeg)];
    memcpy(malformed_scan, k_sof1_12bit_jpeg, sizeof(malformed_scan));
    size_t sos = jpeg_marker_offset(
        malformed_scan, sizeof(malformed_scan), 0xDA);
    CHECK(sos + 5 < sizeof(malformed_scan));
    malformed_scan[sos + 5] = 2;
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", malformed_scan, sizeof(malformed_scan), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE);

    uint8_t malformed_progression[sizeof(k_sof2_12bit_jpeg)];
    memcpy(malformed_progression, k_sof2_12bit_jpeg,
           sizeof(malformed_progression));
    size_t ac_scan = jpeg_ac_scan_offset(
        malformed_progression, sizeof(malformed_progression));
    CHECK(ac_scan < sizeof(malformed_progression));
    uint8_t ac_components = malformed_progression[ac_scan + 4];
    size_t approximation = ac_scan + 7 + 2 * (size_t)ac_components;
    CHECK(approximation < sizeof(malformed_progression));
    malformed_progression[approximation] = 0x21;
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", malformed_progression,
              sizeof(malformed_progression), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE);

    memcpy(malformed_progression, k_sof2_12bit_jpeg,
           sizeof(malformed_progression));
    size_t refinement_scan = jpeg_ac_refinement_scan_offset(
        malformed_progression, sizeof(malformed_progression));
    CHECK(refinement_scan + 6 < sizeof(malformed_progression));
    malformed_progression[refinement_scan + 6] =
        (malformed_progression[refinement_scan + 6] & 0xF0) | 3;
    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", malformed_progression,
              sizeof(malformed_progression), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE);

    CHECK(ap2_mrp_probe_artwork(
              "image/jpeg", k_sof2_12bit_jpeg,
              sizeof(k_sof2_12bit_jpeg) - 1, &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE);

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

    capture_sender_t sender = {
        .status = 204,
        .expected = k_baseline_jpeg,
        .expected_len = sizeof(k_baseline_jpeg),
    };
    CHECK(ap2_mrp_send_nowplaying_command(
              mrp, capture_sender, &sender) == 204);
    CHECK(sender.saw_expected);
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

    static const struct {
        const uint8_t *data;
        size_t len;
    } unsupported[] = {
        {k_sof1_8bit_16dqt_jpeg, sizeof(k_sof1_8bit_16dqt_jpeg)},
        {k_sof1_12bit_jpeg, sizeof(k_sof1_12bit_jpeg)},
        {k_sof2_12bit_jpeg, sizeof(k_sof2_12bit_jpeg)},
    };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        CHECK(ap2_mrp_set_artwork(
            mrp, "image/jpeg", unsupported[i].data,
            (int)unsupported[i].len, &info));
        uint8_t *payload = NULL;
        int payload_len = 0;
        CHECK(ap2_mrp_build_nowplaying_command(
            mrp, &payload, &payload_len));
        CHECK(bytes_contain(
            payload, (size_t)payload_len,
            unsupported[i].data, unsupported[i].len));
        free(payload);
    }

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
    struct ap2_mrp_ctx *mrp;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool sender_entered;
    bool release_sender;
    bool replacement_started;
    bool replacement_done;
    bool sender_saw_artwork;
    bool replacement_ok;
    int response_status;
    int request_status;
} atomic_request_test_t;

static int blocking_sender(void *opaque, const uint8_t *body, int body_len)
{
    atomic_request_test_t *test = opaque;
    pthread_mutex_lock(&test->lock);
    test->sender_saw_artwork = bytes_contain(
        body, (size_t)body_len, k_baseline_jpeg, sizeof(k_baseline_jpeg));
    test->sender_entered = true;
    pthread_cond_broadcast(&test->cond);
    while (!test->release_sender)
        pthread_cond_wait(&test->cond, &test->lock);
    pthread_mutex_unlock(&test->lock);
    return test->response_status;
}

static void *send_request_thread(void *opaque)
{
    atomic_request_test_t *test = opaque;
    ap2_mrp_serial_lock(&test->serial);
    test->request_status = ap2_mrp_send_nowplaying_command(
        test->mrp, blocking_sender, test);
    ap2_mrp_serial_unlock(&test->serial);
    return NULL;
}

typedef struct {
    ap2_mrp_serial_t *serial;
    struct ap2_mrp_ctx *mrp;
    int response_status;
    int request_status;
} scoped_request_test_t;

static int status_sender(void *opaque, const uint8_t *body, int body_len)
{
    scoped_request_test_t *test = opaque;
    (void)body;
    (void)body_len;
    return test->response_status;
}

static void *send_scoped_request_thread(void *opaque)
{
    scoped_request_test_t *test = opaque;
    ap2_mrp_serial_lock(test->serial);
    test->request_status = ap2_mrp_send_nowplaying_command(
        test->mrp, status_sender, test);
    ap2_mrp_serial_unlock(test->serial);
    return NULL;
}

static void *replace_artwork_thread(void *opaque)
{
    atomic_request_test_t *test = opaque;
    pthread_mutex_lock(&test->lock);
    test->replacement_started = true;
    pthread_cond_broadcast(&test->cond);
    pthread_mutex_unlock(&test->lock);

    ap2_mrp_serial_lock(&test->serial);
    bool ok = ap2_mrp_set_artwork(
        test->mrp, "image/jpeg", k_sof1_12bit_jpeg,
        (int)sizeof(k_sof1_12bit_jpeg), NULL);
    ap2_mrp_serial_unlock(&test->serial);

    pthread_mutex_lock(&test->lock);
    test->replacement_ok = ok;
    test->replacement_done = true;
    pthread_cond_broadcast(&test->cond);
    pthread_mutex_unlock(&test->lock);
    return NULL;
}

typedef struct {
    struct ap2_mrp_ctx *mrp;
    bool saw_original;
    bool replacement_ok;
} generation_sender_t;

static int replacing_sender(void *opaque, const uint8_t *body, int body_len)
{
    generation_sender_t *sender = opaque;
    sender->saw_original = bytes_contain(
        body, (size_t)body_len, k_baseline_jpeg, sizeof(k_baseline_jpeg));
    sender->replacement_ok = ap2_mrp_set_artwork(
        sender->mrp, "image/jpeg", k_sof2_12bit_jpeg,
        (int)sizeof(k_sof2_12bit_jpeg), NULL);
    return 204;
}

static bool test_push_result_scope(void)
{
    ap2_mrp_push_result_t first = ap2_mrp_push_result_empty();
    ap2_mrp_push_result_t second = ap2_mrp_push_result_empty();
    first.overall_status = 503;
    first.nowplaying_status = 400;
    CHECK(second.overall_status == -1);
    CHECK(second.nowplaying_status == 0);

    struct ap2_mrp_ctx *mrp = ap2_mrp_create(
        "127.0.0.1", 7000, NULL, "0011223344556677", "Test sender",
        "11111111-1111-1111-1111-111111111111",
        "22222222-2222-2222-2222-222222222222", NULL);
    CHECK(mrp != NULL);
    CHECK(ap2_mrp_set_artwork(
        mrp, "image/jpeg", k_baseline_jpeg,
        (int)sizeof(k_baseline_jpeg), NULL));

    atomic_request_test_t test = {
        .mrp = mrp,
        .response_status = 207,
    };
    CHECK(pthread_mutex_init(&test.lock, NULL) == 0);
    CHECK(pthread_cond_init(&test.cond, NULL) == 0);
    ap2_mrp_serial_init(&test.serial);
    pthread_t request_thread;
    pthread_t scoped_request_thread;
    pthread_t replacement_thread;
    CHECK(pthread_create(
              &request_thread, NULL, send_request_thread, &test) == 0);

    pthread_mutex_lock(&test.lock);
    while (!test.sender_entered)
        pthread_cond_wait(&test.cond, &test.lock);
    pthread_mutex_unlock(&test.lock);

    scoped_request_test_t scoped_request = {
        .serial = &test.serial,
        .mrp = mrp,
        .response_status = 418,
    };
    CHECK(pthread_create(
              &scoped_request_thread, NULL,
              send_scoped_request_thread, &scoped_request) == 0);
    CHECK(pthread_create(
              &replacement_thread, NULL, replace_artwork_thread, &test) == 0);

    pthread_mutex_lock(&test.lock);
    while (!test.replacement_started)
        pthread_cond_wait(&test.cond, &test.lock);
    struct timespec deadline;
    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_nsec += 100 * 1000 * 1000;
    if (deadline.tv_nsec >= 1000 * 1000 * 1000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000 * 1000 * 1000;
    }
    int wait_status = 0;
    while (!test.replacement_done && wait_status == 0)
        wait_status = pthread_cond_timedwait(
            &test.cond, &test.lock, &deadline);
    CHECK(wait_status == ETIMEDOUT);
    CHECK(!test.replacement_done);
    test.release_sender = true;
    pthread_cond_broadcast(&test.cond);
    pthread_mutex_unlock(&test.lock);

    CHECK(pthread_join(request_thread, NULL) == 0);
    CHECK(pthread_join(scoped_request_thread, NULL) == 0);
    CHECK(pthread_join(replacement_thread, NULL) == 0);
    CHECK(test.request_status == 207);
    CHECK(scoped_request.request_status == 418);
    CHECK(test.sender_saw_artwork);
    CHECK(test.replacement_ok && test.replacement_done);

    uint8_t *pending = NULL;
    int pending_len = 0;
    CHECK(ap2_mrp_build_nowplaying_command(mrp, &pending, &pending_len));
    CHECK(bytes_contain(
        pending, (size_t)pending_len,
        k_sof1_12bit_jpeg, sizeof(k_sof1_12bit_jpeg)));
    free(pending);

    capture_sender_t failed = {
        .status = 503,
        .expected = k_sof1_12bit_jpeg,
        .expected_len = sizeof(k_sof1_12bit_jpeg),
    };
    ap2_mrp_serial_lock(&test.serial);
    CHECK(ap2_mrp_send_nowplaying_command(
              mrp, capture_sender, &failed) == 503);
    ap2_mrp_serial_unlock(&test.serial);
    CHECK(failed.saw_expected);
    pending = NULL;
    pending_len = 0;
    CHECK(ap2_mrp_build_nowplaying_command(mrp, &pending, &pending_len));
    CHECK(bytes_contain(
        pending, (size_t)pending_len,
        k_sof1_12bit_jpeg, sizeof(k_sof1_12bit_jpeg)));
    free(pending);

    ap2_mrp_serial_lock(&test.serial);
    CHECK(ap2_mrp_set_artwork(
        mrp, "image/jpeg", k_baseline_jpeg,
        (int)sizeof(k_baseline_jpeg), NULL));
    generation_sender_t replacing = {.mrp = mrp};
    CHECK(ap2_mrp_send_nowplaying_command(
              mrp, replacing_sender, &replacing) == 204);
    ap2_mrp_serial_unlock(&test.serial);
    CHECK(replacing.saw_original && replacing.replacement_ok);
    pending = NULL;
    pending_len = 0;
    CHECK(ap2_mrp_build_nowplaying_command(mrp, &pending, &pending_len));
    CHECK(bytes_contain(
        pending, (size_t)pending_len,
        k_sof2_12bit_jpeg, sizeof(k_sof2_12bit_jpeg)));
    free(pending);

    CHECK(ap2_mrp_send_nowplaying_command(NULL, capture_sender, &failed) == -1);
    CHECK(ap2_mrp_send_nowplaying_command(mrp, NULL, NULL) == -1);
    ap2_mrp_serial_destroy(&test.serial);
    CHECK(pthread_cond_destroy(&test.cond) == 0);
    CHECK(pthread_mutex_destroy(&test.lock) == 0);
    ap2_mrp_destroy(mrp);
    puts("MRP request status, serialization and generation tests passed");
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
        !test_nowplaying_command_payload() ||
        !test_push_result_scope())
        return 1;
    for (int i = 1; i < argc; i++) {
        if (!probe_generated_case(argv[i])) return 1;
    }
    puts("MRP artwork probe tests passed");
    return 0;
}
