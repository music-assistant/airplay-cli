/*
 * AirPlay 2 HAP - Header
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_HAP_H_
#define __AP2_HAP_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * HAP pair-verify context.
 *
 * Credentials format (192 hex chars = 96 bytes):
 *   bytes 0-63:  Client Ed25519 private key (64 bytes)
 *   bytes 64-95: Server Ed25519 public key (32 bytes)
 */

struct ap2_hap_ctx;

/* Create HAP context from hex credentials string (192 chars). */
struct ap2_hap_ctx *ap2_hap_create(const char *credentials_hex);

/* Set the client identifier used during pair-verify (e.g. DACP ID as raw bytes). */
void ap2_hap_set_client_id(struct ap2_hap_ctx *ctx, const uint8_t *id, int id_len);

/* Destroy HAP context. */
void ap2_hap_destroy(struct ap2_hap_ctx *ctx);

/*
 * Perform pair-verify over an established TCP connection.
 *
 * :param ctx: HAP context with credentials.
 * :param sock_fd: Connected TCP socket to the device.
 * :returns: true on success, false on failure.
 *
 * On success, the context holds encryption keys for the session.
 */
bool ap2_hap_pair_verify(struct ap2_hap_ctx *ctx, int sock_fd);

/* Encrypt data for sending to the device. Caller must free output. */
int ap2_hap_encrypt(struct ap2_hap_ctx *ctx, const uint8_t *in, int in_len,
                    uint8_t **out);

/* Decrypt data received from the device. Caller must free output. */
int ap2_hap_decrypt(struct ap2_hap_ctx *ctx, const uint8_t *in, int in_len,
                    uint8_t **out);

/* Save/restore read nonce counter (for retry-safe decryption). */
uint64_t ap2_hap_save_read_counter(struct ap2_hap_ctx *ctx);
void ap2_hap_restore_read_counter(struct ap2_hap_ctx *ctx, uint64_t counter);

/* Get the X25519 shared secret (32 bytes) - used as audio encryption key. */
const uint8_t *ap2_hap_get_shared_secret(struct ap2_hap_ctx *ctx);

#endif /* __AP2_HAP_H_ */
