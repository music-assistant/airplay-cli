/*
 * C wrapper for bplist (binary plist) reader/writer
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_BPLIST_H_
#define __AP2_BPLIST_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

struct ap2_bplist;

/* Create empty bplist for writing */
struct ap2_bplist *ap2_bplist_create(void);

/* Parse a binary plist from raw bytes */
struct ap2_bplist *ap2_bplist_parse(const uint8_t *data, size_t len);

/* Free a bplist */
void ap2_bplist_free(struct ap2_bplist *bp);

/* Add a string key-value pair */
void ap2_bplist_add_string(struct ap2_bplist *bp, const char *key, const char *value);

/* Add an integer key-value pair */
void ap2_bplist_add_int(struct ap2_bplist *bp, const char *key, uint64_t value);

/* Add a data (byte array) key-value pair */
void ap2_bplist_add_data(struct ap2_bplist *bp, const char *key,
                          const uint8_t *data, size_t len);

/* Serialize to binary plist bytes. Caller must free *out. Returns length. */
int ap2_bplist_serialize(struct ap2_bplist *bp, uint8_t **out);

/* Get a string value by key. Returns NULL if not found. Do not free. */
const char *ap2_bplist_get_string(struct ap2_bplist *bp, const char *key);

/* Get an integer value by key. Returns 0 if not found. */
uint64_t ap2_bplist_get_int(struct ap2_bplist *bp, const char *key);

/* Get a data value by key. Returns NULL if not found. Sets *len. Do not free. */
const uint8_t *ap2_bplist_get_data(struct ap2_bplist *bp, const char *key, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* __AP2_BPLIST_H_ */
