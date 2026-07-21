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

static uint8_t *make_noninterleaved_jpeg(size_t *out_size)
{
    size_t sof = jpeg_marker_offset(k_baseline_jpeg,
                                    sizeof(k_baseline_jpeg), 0xC0);
    size_t sos = jpeg_marker_offset(k_baseline_jpeg,
                                    sizeof(k_baseline_jpeg), 0xDA);
    if (sof + 18 >= sizeof(k_baseline_jpeg) ||
        sos >= sizeof(k_baseline_jpeg))
        return NULL;

    size_t size = sos + 3 * 11 + 2;
    uint8_t *out = malloc(size);
    if (!out) return NULL;
    memcpy(out, k_baseline_jpeg, sos);
    out[sof + 11] = 0x44; /* valid for separate scans; 16 blocks if interleaved */

    size_t pos = sos;
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t component_id = out[sof + 10 + i * 3];
        uint8_t selectors = i == 0 ? 0x00 : 0x11;
        static const uint8_t scan_prefix[] = {
            0xFF, 0xDA, 0x00, 0x08, 0x01,
        };
        memcpy(out + pos, scan_prefix, sizeof(scan_prefix));
        pos += sizeof(scan_prefix);
        out[pos++] = component_id;
        out[pos++] = selectors;
        out[pos++] = 0;
        out[pos++] = 63;
        out[pos++] = 0;
        out[pos++] = 1; /* one structural entropy byte */
    }
    out[pos++] = 0xFF;
    out[pos++] = 0xD9;
    if (pos != size) {
        free(out);
        return NULL;
    }
    *out_size = size;
    return out;
}

static uint8_t *insert_empty_jpeg_segment(uint8_t marker, size_t *out_size)
{
    size_t size = sizeof(k_baseline_jpeg) + 4;
    uint8_t *out = malloc(size);
    if (!out) return NULL;
    memcpy(out, k_baseline_jpeg, 2);
    out[2] = 0xFF;
    out[3] = marker;
    out[4] = 0;
    out[5] = 2;
    memcpy(out + 6, k_baseline_jpeg + 2, sizeof(k_baseline_jpeg) - 2);
    *out_size = size;
    return out;
}

static uint8_t *insert_16bit_dqt(size_t *out_size)
{
    size_t size = sizeof(k_baseline_jpeg) + 133;
    uint8_t *out = malloc(size);
    if (!out) return NULL;
    memcpy(out, k_baseline_jpeg, 2);
    size_t pos = 2;
    out[pos++] = 0xFF;
    out[pos++] = 0xDB;
    out[pos++] = 0x00;
    out[pos++] = 0x83; /* 2-byte length + spec + 64 16-bit values */
    out[pos++] = 0x10; /* Pq=1, Tq=0 */
    for (size_t i = 0; i < 64; i++) {
        out[pos++] = 0;
        out[pos++] = 1;
    }
    memcpy(out + pos, k_baseline_jpeg + 2, sizeof(k_baseline_jpeg) - 2);
    pos += sizeof(k_baseline_jpeg) - 2;
    if (pos != size) {
        free(out);
        return NULL;
    }
    *out_size = size;
    return out;
}

static uint8_t *make_restart_jpeg(size_t *out_size,
                                  size_t *dri_offset,
                                  size_t *restart_offset)
{
    size_t sos = jpeg_marker_offset(k_baseline_jpeg,
                                    sizeof(k_baseline_jpeg), 0xDA);
    if (sos + 4 >= sizeof(k_baseline_jpeg)) return NULL;
    size_t sos_len =
        ((size_t)k_baseline_jpeg[sos + 2] << 8) |
        k_baseline_jpeg[sos + 3];
    size_t entropy = sos + 2 + sos_len;
    if (entropy >= sizeof(k_baseline_jpeg) - 2) return NULL;

    size_t size = sizeof(k_baseline_jpeg) + 8;
    uint8_t *out = malloc(size);
    if (!out) return NULL;
    size_t pos = 0;
    memcpy(out, k_baseline_jpeg, sos);
    pos += sos;
    *dri_offset = pos;
    static const uint8_t dri[] = {0xFF, 0xDD, 0x00, 0x04, 0x00, 0x01};
    memcpy(out + pos, dri, sizeof(dri));
    pos += sizeof(dri);
    memcpy(out + pos, k_baseline_jpeg + sos, entropy - sos + 1);
    pos += entropy - sos + 1;
    *restart_offset = pos;
    out[pos++] = 0xFF;
    out[pos++] = 0xD0;
    memcpy(out + pos, k_baseline_jpeg + entropy + 1,
           sizeof(k_baseline_jpeg) - entropy - 1);
    pos += sizeof(k_baseline_jpeg) - entropy - 1;
    if (pos != size) {
        free(out);
        return NULL;
    }
    *out_size = size;
    return out;
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
        if (tail > 0 && tail < 4) {
            size_t shift = 4 - tail;
            total -= shift;
        }
        uint16_t segment_len = (uint16_t)(total - 2);
        out[off++] = 0xFF;
        out[off++] = 0xFE; /* legal JPEG comment segment */
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

    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", k_progressive_jpeg, sizeof(k_progressive_jpeg),
              &info) == AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(info.progressive && info.sof_marker == 0xC2 &&
          info.components == 3);
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", k_grayscale_jpeg, sizeof(k_grayscale_jpeg),
              &info) == AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(!info.progressive && info.sof_marker == 0xC0 &&
          info.components == 1);
    uint8_t changed[sizeof(k_baseline_jpeg)];
    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    size_t sof = jpeg_marker_offset(changed, sizeof(changed), 0xC0);
    CHECK(sof + 9 < sizeof(changed));
    changed[sof + 7] = 0x02;
    changed[sof + 8] = 0x59; /* 601 pixels */
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_ACCEPTED);
    CHECK(info.width == 601 && info.height == 1);

    size_t noninterleaved_size = 0;
    uint8_t *noninterleaved =
        make_noninterleaved_jpeg(&noninterleaved_size);
    CHECK(noninterleaved != NULL);
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", noninterleaved, noninterleaved_size, &info) ==
          AP2_MRP_ARTWORK_ACCEPTED);
    free(noninterleaved);

    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    changed[sof + 11] = 0x44;
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    static const size_t matrix_sizes[] = {
        44032, 61440, 65535, 65536, 66560, 102400, 153600,
    };
    for (size_t i = 0; i < sizeof(matrix_sizes) / sizeof(matrix_sizes[0]); i++) {
        uint8_t *matrix_jpeg = make_padded_jpeg(matrix_sizes[i]);
        CHECK(matrix_jpeg != NULL);
        CHECK(ap2_mrp_validate_artwork(
                  "image/jpeg", matrix_jpeg, matrix_sizes[i], &info) ==
              AP2_MRP_ARTWORK_ACCEPTED);
        CHECK(info.bytes == matrix_sizes[i]);
        free(matrix_jpeg);
    }

    uint8_t *oversize =
        calloc(AP2_MRP_ARTWORK_SAFETY_MAX_BYTES + 1, 1);
    CHECK(oversize != NULL);
    memcpy(oversize, k_baseline_jpeg, sizeof(k_baseline_jpeg));
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", oversize,
              AP2_MRP_ARTWORK_SAFETY_MAX_BYTES + 1, &info) ==
          AP2_MRP_ARTWORK_SAFETY_LIMIT);
    free(oversize);
    return true;
}

static bool test_malformed_jpeg_structures(void)
{
    ap2_mrp_artwork_info_t info;
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
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", empty_tables, sizeof(empty_tables), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    uint8_t changed[sizeof(k_baseline_jpeg)];
    size_t dqt = jpeg_marker_offset(k_baseline_jpeg,
                                    sizeof(k_baseline_jpeg), 0xDB);
    size_t dht = jpeg_marker_offset(k_baseline_jpeg,
                                    sizeof(k_baseline_jpeg), 0xC4);
    size_t sof = jpeg_marker_offset(k_baseline_jpeg,
                                    sizeof(k_baseline_jpeg), 0xC0);
    size_t sos = jpeg_marker_offset(k_baseline_jpeg,
                                    sizeof(k_baseline_jpeg), 0xDA);
    CHECK(dqt + 5 < sizeof(changed));
    CHECK(dht + 5 < sizeof(changed));
    CHECK(sof + 12 < sizeof(changed));
    CHECK(sos + 6 < sizeof(changed));

    size_t inserted_size = 0;
    uint8_t *inserted = insert_empty_jpeg_segment(0xDB, &inserted_size);
    CHECK(inserted != NULL);
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", inserted, inserted_size, &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);
    free(inserted);

    inserted = insert_empty_jpeg_segment(0xC4, &inserted_size);
    CHECK(inserted != NULL);
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", inserted, inserted_size, &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);
    free(inserted);

    inserted = insert_16bit_dqt(&inserted_size);
    CHECK(inserted != NULL);
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", inserted, inserted_size, &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);
    free(inserted);

    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    changed[dqt + 5] = 0; /* zero quantizer */
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    size_t huffman_symbols = 0;
    for (size_t i = 0; i < 16; i++)
        huffman_symbols += changed[dht + 5 + i];
    CHECK(huffman_symbols >= 3 && huffman_symbols <= 258);
    memset(changed + dht + 5, 0, 16);
    changed[dht + 5] = 3;
    changed[dht + 20] = (uint8_t)(huffman_symbols - 3);
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    uint8_t progressive[sizeof(k_progressive_jpeg)];
    size_t progressive_dht = jpeg_marker_offset(
        k_progressive_jpeg, sizeof(k_progressive_jpeg), 0xC4);
    CHECK(progressive_dht + 6 < sizeof(progressive));
    CHECK(k_progressive_jpeg[progressive_dht + 5] == 1);
    CHECK(k_progressive_jpeg[progressive_dht + 6] == 1);
    memcpy(progressive, k_progressive_jpeg, sizeof(progressive));
    progressive[progressive_dht + 5] = 2;
    progressive[progressive_dht + 6] = 0; /* complete tree: all-ones code */
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", progressive, sizeof(progressive), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    size_t ac_scan = jpeg_ac_scan_offset(
        k_progressive_jpeg, sizeof(k_progressive_jpeg));
    CHECK(ac_scan < sizeof(progressive));
    uint8_t ac_components = k_progressive_jpeg[ac_scan + 4];
    size_t approximation = ac_scan + 7 + 2 * (size_t)ac_components;
    CHECK(approximation < sizeof(progressive));
    memcpy(progressive, k_progressive_jpeg, sizeof(progressive));
    progressive[approximation] = 0x21; /* refinement before an AC first scan */
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", progressive, sizeof(progressive), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    changed[sof + 12] = 3; /* undefined quantization table */
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    changed[sos + 5] = 0x7F; /* scan component absent from SOF */
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    changed[sos + 6] = 0x22; /* undefined DC/AC Huffman tables */
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    size_t sos_len =
        ((size_t)k_baseline_jpeg[sos + 2] << 8) |
        k_baseline_jpeg[sos + 3];
    size_t entropy = sos + 2 + sos_len;
    CHECK(entropy + 3 < sizeof(changed));

    uint8_t empty_scan[sizeof(k_baseline_jpeg)];
    memcpy(empty_scan, k_baseline_jpeg, entropy);
    empty_scan[entropy] = 0xFF;
    empty_scan[entropy + 1] = 0xD9;
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", empty_scan, entropy + 2, &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    memcpy(changed, k_baseline_jpeg, sizeof(changed));
    changed[entropy] = 0xFF;
    changed[entropy + 1] = 0xFF;
    changed[entropy + 2] = 0x00; /* invalid fill + stuffing */
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", changed, sizeof(changed), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    uint8_t trailing[sizeof(k_baseline_jpeg) + 1];
    memcpy(trailing, k_baseline_jpeg, sizeof(k_baseline_jpeg));
    trailing[sizeof(k_baseline_jpeg)] = 0;
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", trailing, sizeof(trailing), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    uint8_t duplicate_eoi[sizeof(k_baseline_jpeg) + 2];
    memcpy(duplicate_eoi, k_baseline_jpeg, sizeof(k_baseline_jpeg));
    duplicate_eoi[sizeof(k_baseline_jpeg)] = 0xFF;
    duplicate_eoi[sizeof(k_baseline_jpeg) + 1] = 0xD9;
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", duplicate_eoi, sizeof(duplicate_eoi), &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);

    size_t restart_size = 0;
    size_t dri_offset = 0;
    size_t restart_offset = 0;
    uint8_t *restart = make_restart_jpeg(
        &restart_size, &dri_offset, &restart_offset);
    CHECK(restart != NULL);
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", restart, restart_size, &info) ==
          AP2_MRP_ARTWORK_ACCEPTED);
    restart[dri_offset + 4] = 0;
    restart[dri_offset + 5] = 0;
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", restart, restart_size, &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);
    restart[dri_offset + 5] = 1;
    restart[restart_offset + 1] = 0xD1;
    CHECK(ap2_mrp_validate_artwork(
              "image/jpeg", restart, restart_size, &info) ==
          AP2_MRP_ARTWORK_INVALID_JPEG);
    free(restart);
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

    uint8_t *large = make_padded_jpeg(153600);
    CHECK(large != NULL);
    CHECK(ap2_mrp_set_artwork(mrp, "image/jpeg", large, 153600, &info));
    CHECK(info.bytes == 153600);
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
    CHECK(info.result == AP2_MRP_ARTWORK_INVALID_JPEG);

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

static bool validate_generated_case(const char *path)
{
    uint8_t *data = NULL;
    size_t size = 0;
    char content_type[ARTWORK_CONTENT_TYPE_SIZE];
    char error[160];
    CHECK(artwork_load_file(path, &data, &size, content_type,
                            error, sizeof(error)));
    ap2_mrp_artwork_info_t info;
    ap2_mrp_artwork_result_t result =
        ap2_mrp_validate_artwork(content_type, data, size, &info);
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
        !test_mrp_validation_boundary() ||
        !test_malformed_jpeg_structures() ||
        !test_nowplaying_command_payload())
        return 1;
    for (int i = 1; i < argc; i++) {
        if (!validate_generated_case(argv[i])) return 1;
    }
    puts("MRP artwork tests passed");
    return 0;
}
