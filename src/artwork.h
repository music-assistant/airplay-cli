#ifndef __ARTWORK_H_
#define __ARTWORK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*artwork_pump_cb)(void *arg);

/*
 * Load artwork from a local path or an http:// URL.
 *
 * The returned byte buffer is owned by the caller. content_type is inferred
 * from the image signature rather than a filename or HTTP header.
 */
bool artwork_load(const char *source, uint8_t **data, size_t *size,
                  char content_type[32], artwork_pump_cb pump, void *pump_arg,
                  char *error, size_t error_size);

#endif /* __ARTWORK_H_ */
