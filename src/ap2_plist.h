/*
 * Minimal binary plist builder for AirPlay 2 SETUP messages.
 *
 * Supports flat dicts, nested dicts inside arrays, strings, integers,
 * booleans, and data (byte arrays). Just enough for AP2 SETUP payloads.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_PLIST_H_
#define __AP2_PLIST_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct ap2_plist;

/* Create a new plist builder (root is a dict). */
struct ap2_plist *ap2_plist_create(void);

/* Free the plist builder. */
void ap2_plist_free(struct ap2_plist *p);

/* Add a string value to the root dict. */
void ap2_plist_add_string(struct ap2_plist *p, const char *key, const char *value);

/* Add an integer value to the root dict. */
void ap2_plist_add_int(struct ap2_plist *p, const char *key, int64_t value);

/* Add a boolean value to the root dict. */
void ap2_plist_add_bool(struct ap2_plist *p, const char *key, bool value);

/* Add a data (byte array) value to the root dict. */
void ap2_plist_add_data(struct ap2_plist *p, const char *key,
                         const uint8_t *data, size_t len);

/*
 * Add a "streams" array containing a single dict with the given key-value pairs.
 * This is the specific structure needed for AP2 stream SETUP:
 *   {"streams": [{"type": 96, "ct": 2, ...}]}
 *
 * Call ap2_plist_stream_add_* to add entries to the stream dict,
 * then call ap2_plist_stream_end to finalize.
 */
void ap2_plist_stream_begin(struct ap2_plist *p);
void ap2_plist_stream_add_string(struct ap2_plist *p, const char *key, const char *value);
void ap2_plist_stream_add_int(struct ap2_plist *p, const char *key, int64_t value);
void ap2_plist_stream_add_bool(struct ap2_plist *p, const char *key, bool value);
void ap2_plist_stream_add_data(struct ap2_plist *p, const char *key,
                                const uint8_t *data, size_t len);
void ap2_plist_stream_end(struct ap2_plist *p);

/* Serialize to binary plist format. Caller must free *out. Returns length. */
int ap2_plist_serialize(struct ap2_plist *p, uint8_t **out);

#endif /* __AP2_PLIST_H_ */
