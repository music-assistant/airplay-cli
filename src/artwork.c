/*
 * Local artwork loading helpers.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "artwork.h"

static void artwork_error(char *error, size_t error_size, const char *fmt, ...)
{
    if (!error || error_size == 0) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(error, error_size, fmt, args);
    va_end(args);
}

static const char *artwork_detect_type(const uint8_t *data, size_t size)
{
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return "image/jpeg";
    if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "image/png";
    if (size >= 6 &&
        (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0))
        return "image/gif";
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0 &&
        memcmp(data + 8, "WEBP", 4) == 0)
        return "image/webp";
    return NULL;
}

bool artwork_load_file(const char *path, uint8_t **data, size_t *size,
                       char content_type[ARTWORK_CONTENT_TYPE_SIZE],
                       char *error, size_t error_size)
{
    if (!path || !*path || !data || !size || !content_type) {
        artwork_error(error, error_size, "invalid artwork file arguments");
        return false;
    }
    *data = NULL;
    *size = 0;
    content_type[0] = '\0';

    FILE *file = fopen(path, "rb");
    if (!file) {
        artwork_error(error, error_size, "cannot open artwork file: %s",
                      strerror(errno));
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        artwork_error(error, error_size, "cannot seek artwork file");
        fclose(file);
        return false;
    }
    long file_size = ftell(file);
    if (file_size < 0) {
        artwork_error(error, error_size,
                      "cannot determine artwork file size");
        fclose(file);
        return false;
    }
    if (file_size == 0 || file_size > INT_MAX) {
        artwork_error(error, error_size, file_size == 0
                          ? "artwork file is empty"
                          : "artwork file is too large");
        fclose(file);
        return false;
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        artwork_error(error, error_size, "cannot rewind artwork file");
        fclose(file);
        return false;
    }

    uint8_t *image = malloc((size_t)file_size);
    if (!image) {
        artwork_error(error, error_size, "out of memory loading artwork");
        fclose(file);
        return false;
    }
    size_t got = fread(image, 1, (size_t)file_size, file);
    bool read_ok = got == (size_t)file_size && !ferror(file);
    fclose(file);
    if (!read_ok) {
        artwork_error(error, error_size, "cannot read complete artwork file");
        free(image);
        return false;
    }

    const char *mime = artwork_detect_type(image, got);
    if (!mime) {
        artwork_error(error, error_size,
                      "artwork file is not JPEG, PNG, GIF, or WebP");
        free(image);
        return false;
    }

    snprintf(content_type, ARTWORK_CONTENT_TYPE_SIZE, "%s", mime);
    *data = image;
    *size = got;
    return true;
}
