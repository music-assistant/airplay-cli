#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cross_log.h"
#include "artwork.h"
#include "ap2_mrp.h"

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

static bool test_mrp_validation_boundary(void)
{
    ap2_mrp_artwork_info_t info;
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", k_baseline_jpeg, sizeof(k_baseline_jpeg), &info) ==
          AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(info.result == AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(info.bytes == sizeof(k_baseline_jpeg));
    CHECK(info.width == 1 && info.height == 1);

    CHECK(ap2_mrp_validate_artwork(
              "image/png", k_baseline_jpeg, sizeof(k_baseline_jpeg), &info) ==
          AP2_MRP_ARTWORK_UNSUPPORTED_TYPE);
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", k_baseline_jpeg, sizeof(k_baseline_jpeg) - 1,
              &info) == AP2_MRP_ARTWORK_INVALID_JPEG);

    uint8_t changed[sizeof(k_baseline_jpeg)];
    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    size_t sof = jpeg_marker_offset(changed, sizeof(changed), 0xC0);
    CHECK(sof + 9 < sizeof(changed));
    changed[sof + 1] = 0xC2;
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_NOT_BASELINE);

    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    changed[sof + 7] = 0x02;
    changed[sof + 8] = 0x59; /* 601 pixels */
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_INVALID_DIMENSIONS);
    CHECK(info.width == 601 && info.height == 1);

    uint8_t *oversize = calloc(AP2_MRP_ARTWORK_MAX_BYTES + 1, 1);
    CHECK(oversize != NULL);
    memcpy(oversize, k_baseline_jpeg, sizeof(k_baseline_jpeg));
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", oversize, AP2_MRP_ARTWORK_MAX_BYTES + 1, &info) ==
          AP2_MRP_ARTWORK_TOO_LARGE);
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
    CHECK(info.result == AP2_MRP_ARTWORK_ACCEPTED);

    uint8_t *first = NULL;
    int first_len = 0;
    CHECK(ap2_mrp_build_nowplaying_command(mrp, &first, &first_len));
    CHECK(first_len > (int)sizeof(k_baseline_jpeg));
    CHECK(memcmp(first, "bplist00", 8) == 0);
    CHECK(bytes_contain_string(first, (size_t)first_len,
                               "updateMRNowPlayingInfo"));
    CHECK(bytes_contain_string(first, (size_t)first_len, "npi-text"));
    CHECK(bytes_contain_string(
        first, (size_t)first_len,
        "kMRMediaRemoteNowPlayingInfoArtworkIdentifier"));
    CHECK(bytes_contain_string(
        first, (size_t)first_len,
        "kMRMediaRemoteNowPlayingInfoArtworkMIMEType"));
    CHECK(bytes_contain_string(
        first, (size_t)first_len,
        "kMRMediaRemoteNowPlayingInfoArtworkData"));
    CHECK(bytes_contain_string(first, (size_t)first_len, "image/jpeg"));
    CHECK(bytes_contain(first, (size_t)first_len,
                        k_baseline_jpeg, sizeof(k_baseline_jpeg)));

    ap2_mrp_mark_artwork_sent(mrp);
    uint8_t *cached = NULL;
    int cached_len = 0;
    CHECK(ap2_mrp_build_nowplaying_command(mrp, &cached, &cached_len));
    CHECK(cached_len < first_len);
    CHECK(bytes_contain_string(
        cached, (size_t)cached_len,
        "kMRMediaRemoteNowPlayingInfoArtworkIdentifier"));
    CHECK(bytes_contain_string(
        cached, (size_t)cached_len,
        "kMRMediaRemoteNowPlayingInfoArtworkMIMEType"));
    CHECK(!bytes_contain_string(
        cached, (size_t)cached_len,
        "kMRMediaRemoteNowPlayingInfoArtworkData"));
    CHECK(!bytes_contain(cached, (size_t)cached_len,
                         k_baseline_jpeg, sizeof(k_baseline_jpeg)));
    free(first);
    free(cached);

    ap2_mrp_clear_artwork(mrp);
    uint8_t *handler_cleared = NULL;
    int handler_cleared_len = 0;
    CHECK(ap2_mrp_build_nowplaying_command(
        mrp, &handler_cleared, &handler_cleared_len));
    CHECK(!bytes_contain_string(
        handler_cleared, (size_t)handler_cleared_len,
        "kMRMediaRemoteNowPlayingInfoArtworkIdentifier"));
    CHECK(!bytes_contain_string(
        handler_cleared, (size_t)handler_cleared_len,
        "kMRMediaRemoteNowPlayingInfoArtworkData"));
    free(handler_cleared);

    CHECK(ap2_mrp_set_artwork(mrp, "image/jpeg", k_baseline_jpeg,
                              (int)sizeof(k_baseline_jpeg), &info));
    uint8_t progressive[sizeof(k_baseline_jpeg)];
    memcpy(progressive, k_baseline_jpeg, sizeof(progressive));
    size_t sof = jpeg_marker_offset(progressive, sizeof(progressive), 0xC0);
    CHECK(sof + 1 < sizeof(progressive));
    progressive[sof + 1] = 0xC2;
    CHECK(!ap2_mrp_set_artwork(mrp, "image/jpeg", progressive,
                               (int)sizeof(progressive), &info));
    CHECK(info.result == AP2_MRP_ARTWORK_NOT_BASELINE);

    uint8_t *cleared = NULL;
    int cleared_len = 0;
    CHECK(ap2_mrp_build_nowplaying_command(mrp, &cleared, &cleared_len));
    CHECK(!bytes_contain_string(
        cleared, (size_t)cleared_len,
        "kMRMediaRemoteNowPlayingInfoArtworkIdentifier"));
    CHECK(!bytes_contain_string(
        cleared, (size_t)cleared_len,
        "kMRMediaRemoteNowPlayingInfoArtworkMIMEType"));
    CHECK(!bytes_contain_string(
        cleared, (size_t)cleared_len,
        "kMRMediaRemoteNowPlayingInfoArtworkData"));
    free(cleared);
    ap2_mrp_destroy(mrp);
    return true;
}

int main(void)
{
    if (!test_local_file_boundary() ||
        !test_mrp_validation_boundary() ||
        !test_nowplaying_command_payload())
        return 1;
    puts("MRP artwork tests passed");
    return 0;
}
