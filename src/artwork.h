/*
 * Local artwork loading helpers.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __ARTWORK_H_
#define __ARTWORK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARTWORK_CONTENT_TYPE_SIZE 32

/*
 * Load a local image and infer its MIME type from the file signature.
 *
 * JPEG, PNG, GIF, and WebP are accepted for the existing DMAP path. The caller
 * owns *data. Files larger than INT_MAX are rejected because the downstream
 * RAOP/AP2 artwork APIs use an int byte count.
 */
bool artwork_load_file(const char *path, uint8_t **data, size_t *size,
                       char content_type[ARTWORK_CONTENT_TYPE_SIZE],
                       char *error, size_t error_size);

#endif /* __ARTWORK_H_ */
