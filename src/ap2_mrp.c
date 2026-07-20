/*
 * AirPlay 2 MediaRemote (MRP) SENDER
 *
 * Pushes now-playing state to an Apple receiver over the AirPlay 2
 * remote-control data channel (stream type 130): hand-rolled protobuf
 * ProtocolMessage encoding, 32-byte data-frame header, binary-plist payload
 * wrapper, and ChaCha20-Poly1305 channel encryption with HKDF-derived
 * DataStream keys. Protocol references and the wiring plan live in
 * MRP-DESIGN.md.
 *
 * Skeleton status: the wire primitives below are real and the piggyback
 * attach path is implemented end-to-end (key derivation, TCP connect,
 * handshake push); the sidecar bootstrap (ap2_mrp_start) and the incoming
 * protobuf dispatch are stubbed for the wiring pass. NOT yet in the Makefile.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include "cross_log.h"
#include "ap2_plist.h"
#include "ap2_mrp.h"

extern log_level *loglevel;

/* ---- Wire constants (sources cited in MRP-DESIGN.md) ---- */

/* HKDF-SHA512 salt/info strings for the AirPlay 2 logical channels
 * (pyatv pyatv/protocols/airplay/ap2_session.py). The DataStream salt is the
 * literal string with the SETUP "seed" appended in decimal. */
#define MRP_DATASTREAM_SALT_PREFIX  "DataStream-Salt"
#define MRP_DATASTREAM_OUTPUT_INFO  "DataStream-Output-Encryption-Key"
#define MRP_DATASTREAM_INPUT_INFO   "DataStream-Input-Encryption-Key"
/* Event channel strings kept here for the sidecar bootstrap (write/read are
 * swapped relative to our perspective; the receiver drives that channel). */
#define MRP_EVENTS_SALT             "Events-Salt"
#define MRP_EVENTS_WRITE_INFO       "Events-Write-Encryption-Key"
#define MRP_EVENTS_READ_INFO        "Events-Read-Encryption-Key"

/* Data channel SETUP constants (AP2_MRP_STREAM_TYPE_REMOTE_CONTROL etc.) live
 * in ap2_mrp.h so the SETUP request in ap2_client.c shares one definition. */

/* HAP channel framing (identical shape to ap2_hap.c: [2-byte LE length]
 * [ciphertext + 16-byte tag], AAD = the length field, nonce = 4 zero bytes +
 * 8-byte LE counter, max 1024 plaintext bytes per frame). */
#define MRP_FRAME_MAX    1024
#define MRP_TAG_SIZE     16
#define MRP_NONCE_SIZE   12
#define MRP_KEY_SIZE     32

/* Data-frame header: 32 bytes big-endian — size:u32 (includes the header),
 * message_type:12 raw bytes ("sync"+NULs from us, "rply"+NULs in replies),
 * command:4 bytes ("comm"), seqno:u64, padding:u32
 * (pyatv pyatv/protocols/airplay/channels.py DataHeader, struct ">I12s4sQI"). */
#define MRP_DATA_HDR_LEN 32

/* ProtocolMessage.Type values we use (pyatv protobuf/ProtocolMessage.proto). */
enum mrp_msg_type {
    MRP_MSG_SEND_COMMAND            = 1,
    MRP_MSG_SEND_COMMAND_RESULT     = 2,
    MRP_MSG_GET_STATE               = 3,
    MRP_MSG_SET_STATE               = 4,
    MRP_MSG_SET_ARTWORK             = 5,
    MRP_MSG_NOTIFICATION            = 11,
    MRP_MSG_DEVICE_INFO             = 15,
    MRP_MSG_CLIENT_UPDATES_CONFIG   = 16,
    MRP_MSG_PLAYBACK_QUEUE_REQUEST  = 32,
    MRP_MSG_TRANSACTION             = 33,
    MRP_MSG_CRYPTO_PAIRING          = 34,
    MRP_MSG_DEVICE_INFO_UPDATE      = 37,
    MRP_MSG_SET_CONNECTION_STATE    = 38,
    MRP_MSG_WAKE_DEVICE             = 41,
    MRP_MSG_GENERIC                 = 42,
    MRP_MSG_SET_NOW_PLAYING_CLIENT  = 46,
    MRP_MSG_SET_NOW_PLAYING_PLAYER  = 47,
    MRP_MSG_UPDATE_CLIENT           = 55,
    MRP_MSG_UPDATE_CONTENT_ITEM     = 56,
    MRP_MSG_REMOVE_CLIENT           = 53,
    MRP_MSG_REMOVE_PLAYER           = 54,
    MRP_MSG_UPDATE_OUTPUT_DEVICE    = 65,
    MRP_MSG_CONFIGURE_CONNECTION    = 120,
};

/* ProtocolMessage extension field numbers of the inner messages we emit
 * (pyatv protobuf sources, the "extend ProtocolMessage" blocks). */
enum mrp_ext_field {
    MRP_EXT_SET_STATE               = 9,
    MRP_EXT_SET_ARTWORK             = 10,
    MRP_EXT_DEVICE_INFO             = 20,
    MRP_EXT_CLIENT_UPDATES_CONFIG   = 21,
    MRP_EXT_TRANSACTION             = 38,
    MRP_EXT_SET_CONNECTION_STATE    = 42,
    MRP_EXT_SET_NOW_PLAYING_CLIENT  = 50,
    MRP_EXT_CONFIGURE_CONNECTION    = 94,
};

/* Enum values used inside the payload messages (pyatv protobuf sources). */
#define MRP_CONNECTION_STATE_CONNECTED     2   /* SetConnectionStateMessage */
#define MRP_CONNECTION_STATE_DISCONNECTED  3
#define MRP_PLAYBACK_STATE_PLAYING         1   /* Common.proto PlaybackState */
#define MRP_PLAYBACK_STATE_PAUSED          2
#define MRP_DEVICE_CLASS_IPHONE            1   /* Common.proto DeviceClass */
#define MRP_MEDIA_TYPE_AUDIO               1   /* ContentItemMetadata */
#define MRP_MEDIA_SUBTYPE_MUSIC            1

/* Apple epoch (CFAbsoluteTime): seconds between 1970-01-01 and 2001-01-01.
 * MRP timestamps are doubles on this timebase. */
#define MRP_APPLE_EPOCH_OFFSET 978307200.0

/* Cadence of the defensive state re-push from ap2_mrp_tick (seconds). */
#define MRP_STATE_REPUSH_S 15

struct ap2_mrp_ctx {
    /* Target + identity */
    char *host;
    int port;
    char *auth_credentials;   /* 192-hex HAP creds (sidecar pair-verify) */
    char *dacp_id;            /* stable sender identifier */
    char *name;               /* advertised display name */

    /* Channel crypto. The DataStream keys derive from the pairing shared
     * secret of the RTSP session that performed the type-130 SETUP. */
    uint8_t shared_secret[MRP_KEY_SIZE];
    bool have_secret;
    uint8_t out_key[MRP_KEY_SIZE];    /* DataStream-Output-Encryption-Key */
    uint8_t in_key[MRP_KEY_SIZE];     /* DataStream-Input-Encryption-Key */
    uint64_t out_counter;             /* per-direction nonce counters */
    uint64_t in_counter;

    /* Data channel connection */
    int sock;
    bool connected;
    uint64_t send_seqno;      /* constant per channel, random 33-bit (pyatv) */
    /* Sends come from the caller's cmdpipe thread and from tick (replies);
     * the lock keeps frames and the nonce sequence intact (same reasoning as
     * rtsp_lock in ap2_client.c). */
    pthread_mutex_t send_lock;

    /* Decrypted inbound stream, reassembled across TCP reads */
    uint8_t rx_enc[8192];     /* undecrypted channel bytes */
    int rx_enc_len;
    uint8_t rx_msg[16384];    /* decrypted data-frame bytes */
    int rx_msg_len;

    /* Now-playing state (owned copies) */
    char *title;
    char *artist;
    char *album;
    int duration_ms;
    int elapsed_ms;
    bool playing;
    double elapsed_set_at;    /* CFAbsoluteTime when elapsed_ms was captured */
    char *artwork_mime;
    uint8_t *artwork;
    int artwork_len;

    time_t last_state_push;
};

/* ---- Small helpers ---- */

static double mrp_cf_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6 - MRP_APPLE_EPOCH_OFFSET;
}

static void mrp_gen_uuid(char out[37])
{
    uint8_t b[16];
    RAND_bytes(b, sizeof(b));
    snprintf(out, 37,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static char *mrp_strdup(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void mrp_replace_str(char **dst, const char *src)
{
    free(*dst);
    *dst = mrp_strdup(src);
}

/* ---- Growable byte buffer ---- */

typedef struct {
    uint8_t *p;
    size_t len;
    size_t cap;
} mbuf;

static bool mbuf_reserve(mbuf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra) cap *= 2;
    uint8_t *np = realloc(b->p, cap);
    if (!np) return false;
    b->p = np;
    b->cap = cap;
    return true;
}

static bool mbuf_put(mbuf *b, const void *data, size_t n)
{
    if (!mbuf_reserve(b, n)) return false;
    memcpy(b->p + b->len, data, n);
    b->len += n;
    return true;
}

static bool mbuf_put_u8(mbuf *b, uint8_t v) { return mbuf_put(b, &v, 1); }

static void mbuf_free(mbuf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* ---- Protobuf primitives (proto2 wire format) ---- */

static bool pb_put_varint(mbuf *b, uint64_t v)
{
    do {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if (v) byte |= 0x80;
        if (!mbuf_put_u8(b, byte)) return false;
    } while (v);
    return true;
}

/* Field key: (field_number << 3) | wire_type. Wire types: 0 varint,
 * 1 64-bit LE, 2 length-delimited, 5 32-bit LE. */
static bool pb_put_key(mbuf *b, uint32_t field, uint32_t wire_type)
{
    return pb_put_varint(b, ((uint64_t)field << 3) | wire_type);
}

static bool pb_put_varint_field(mbuf *b, uint32_t field, uint64_t v)
{
    return pb_put_key(b, field, 0) && pb_put_varint(b, v);
}

static bool pb_put_bool_field(mbuf *b, uint32_t field, bool v)
{
    return pb_put_varint_field(b, field, v ? 1 : 0);
}

static bool pb_put_bytes_field(mbuf *b, uint32_t field,
                               const void *data, size_t n)
{
    return pb_put_key(b, field, 2) && pb_put_varint(b, n) &&
           (n == 0 || mbuf_put(b, data, n));
}

static bool pb_put_string_field(mbuf *b, uint32_t field, const char *s)
{
    return pb_put_bytes_field(b, field, s, strlen(s));
}

static bool pb_put_double_field(mbuf *b, uint32_t field, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    uint8_t le[8];
    for (int i = 0; i < 8; i++) le[i] = (bits >> (i * 8)) & 0xFF;
    return pb_put_key(b, field, 1) && mbuf_put(b, le, 8);
}

static bool pb_put_float_field(mbuf *b, uint32_t field, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    uint8_t le[4];
    for (int i = 0; i < 4; i++) le[i] = (bits >> (i * 8)) & 0xFF;
    return pb_put_key(b, field, 5) && mbuf_put(b, le, 4);
}

/* Embed a fully built submessage as a length-delimited field. */
static bool pb_put_msg_field(mbuf *b, uint32_t field, const mbuf *sub)
{
    return pb_put_bytes_field(b, field, sub->p, sub->len);
}

/* ---- ProtocolMessage envelope ----
 *
 * ProtocolMessage: type = field 1 (varint, MUST be emitted first — receivers
 * and pyatv both rely on the leading 0x08 byte), extension field = the inner
 * message (length-delimited), uniqueIdentifier = field 85 (string UUID). */
static bool mrp_envelope(mbuf *out, enum mrp_msg_type type,
                         enum mrp_ext_field ext_field, const mbuf *inner)
{
    char uuid[37];
    mrp_gen_uuid(uuid);
    if (!pb_put_varint_field(out, 1, (uint64_t)type)) return false;
    if (inner && !pb_put_msg_field(out, (uint32_t)ext_field, inner)) return false;
    return pb_put_string_field(out, 85, uuid);
}

/* ---- Message builders (field numbers from pyatv protobuf sources) ---- */

/* DEVICE_INFO_MESSAGE (ext 20). Must be the FIRST message on the channel or
 * the receiver will not respond (pyatv protocol.py). Values mirror what a
 * real sender advertises; com.apple.Music vs com.apple.TVRemote is a live
 * validation item (MRP-DESIGN.md §10). */
static bool build_device_info(struct ap2_mrp_ctx *m, mbuf *out)
{
    bool ok = true;
    mbuf inner = {0};
    ok &= pb_put_string_field(&inner, 1, m->dacp_id);           /* uniqueIdentifier */
    ok &= pb_put_string_field(&inner, 2, m->name);              /* name (required) */
    ok &= pb_put_string_field(&inner, 3, "iPhone");             /* localizedModelName */
    ok &= pb_put_string_field(&inner, 4, "21F90");              /* systemBuildVersion */
    ok &= pb_put_string_field(&inner, 5, "com.apple.Music");    /* applicationBundleIdentifier */
    ok &= pb_put_varint_field(&inner, 7, 1);                    /* protocolVersion */
    ok &= pb_put_varint_field(&inner, 8, 108);                  /* lastSupportedMessageType */
    ok &= pb_put_bool_field(&inner, 9, true);                   /* supportsSystemPairing */
    ok &= pb_put_bool_field(&inner, 10, true);                  /* allowsPairing */
    ok &= pb_put_bool_field(&inner, 13, true);                  /* supportsACL */
    ok &= pb_put_bool_field(&inner, 14, true);                  /* supportsSharedQueue */
    ok &= pb_put_bool_field(&inner, 15, true);                  /* supportsExtendedMotion */
    ok &= pb_put_varint_field(&inner, 17, 2);                   /* sharedQueueVersion */
    ok &= pb_put_varint_field(&inner, 21, MRP_DEVICE_CLASS_IPHONE); /* deviceClass */
    ok &= pb_put_varint_field(&inner, 22, 1);                   /* logicalDeviceCount */
    ok = ok && mrp_envelope(out, MRP_MSG_DEVICE_INFO, MRP_EXT_DEVICE_INFO, &inner);
    mbuf_free(&inner);
    return ok;
}

/* SET_CONNECTION_STATE_MESSAGE (ext 42): state = field 1. */
static bool build_set_connection_state(mbuf *out, int state)
{
    bool ok;
    mbuf inner = {0};
    ok = pb_put_varint_field(&inner, 1, (uint64_t)state);
    ok = ok && mrp_envelope(out, MRP_MSG_SET_CONNECTION_STATE,
                            MRP_EXT_SET_CONNECTION_STATE, &inner);
    mbuf_free(&inner);
    return ok;
}

/* CLIENT_UPDATES_CONFIG_MESSAGE (ext 21): we are a sender pushing state, not a
 * controller subscribing, so everything is off — the message merely completes
 * the handshake shape controllers use. */
static bool build_client_updates_config(mbuf *out)
{
    bool ok = true;
    mbuf inner = {0};
    ok &= pb_put_bool_field(&inner, 1, false);   /* artworkUpdates */
    ok &= pb_put_bool_field(&inner, 2, false);   /* nowPlayingUpdates */
    ok &= pb_put_bool_field(&inner, 3, false);   /* volumeUpdates */
    ok &= pb_put_bool_field(&inner, 4, false);   /* keyboardUpdates */
    ok &= pb_put_bool_field(&inner, 5, false);   /* outputDeviceUpdates */
    ok = ok && mrp_envelope(out, MRP_MSG_CLIENT_UPDATES_CONFIG,
                            MRP_EXT_CLIENT_UPDATES_CONFIG, &inner);
    mbuf_free(&inner);
    return ok;
}

/* SET_NOW_PLAYING_CLIENT_MESSAGE (ext 50): client = field 1 ->
 * NowPlayingClient { processIdentifier=1, bundleIdentifier=2, displayName=7 }.
 * Registers us as a now-playing origin on the receiver. */
static bool build_set_now_playing_client(struct ap2_mrp_ctx *m, mbuf *out)
{
    bool ok = true;
    mbuf client = {0}, inner = {0};
    ok &= pb_put_varint_field(&client, 1, (uint64_t)getpid());
    ok &= pb_put_string_field(&client, 2, "com.apple.Music");
    ok &= pb_put_string_field(&client, 7, m->name);
    ok = ok && pb_put_msg_field(&inner, 1, &client);
    ok = ok && mrp_envelope(out, MRP_MSG_SET_NOW_PLAYING_CLIENT,
                            MRP_EXT_SET_NOW_PLAYING_CLIENT, &inner);
    mbuf_free(&client);
    mbuf_free(&inner);
    return ok;
}

/* ContentItemMetadata for the current track (field numbers from
 * ContentItemMetadata.proto). */
static bool build_content_item_metadata(struct ap2_mrp_ctx *m, mbuf *meta)
{
    bool ok = true;
    double dur_s = m->duration_ms / 1000.0;
    double elapsed_s = m->elapsed_ms / 1000.0;
    ok &= pb_put_string_field(meta, 1, m->title ? m->title : "");     /* title */
    ok &= pb_put_string_field(meta, 6, m->album ? m->album : "");     /* albumName */
    ok &= pb_put_string_field(meta, 7, m->artist ? m->artist : "");   /* trackArtistName */
    if (m->duration_ms > 0)
        ok &= pb_put_double_field(meta, 14, dur_s);                   /* duration */
    ok &= pb_put_bool_field(meta, 19, m->artwork_len > 0);            /* artworkAvailable */
    ok &= pb_put_bool_field(meta, 27, true);                          /* isCurrentlyPlaying */
    if (m->artwork_mime && m->artwork_len > 0)
        ok &= pb_put_string_field(meta, 31, m->artwork_mime);         /* artworkMIMEType */
    ok &= pb_put_double_field(meta, 35, elapsed_s);                   /* elapsedTime */
    ok &= pb_put_float_field(meta, 39, m->playing ? 1.0f : 0.0f);     /* playbackRate */
    ok &= pb_put_varint_field(meta, 64, MRP_MEDIA_TYPE_AUDIO);        /* mediaType */
    ok &= pb_put_varint_field(meta, 65, MRP_MEDIA_SUBTYPE_MUSIC);     /* mediaSubType */
    ok &= pb_put_double_field(meta, 74, m->elapsed_set_at);           /* elapsedTimeTimestamp */
    return ok;
}

/* SET_STATE_MESSAGE (ext 9) carrying the full now-playing picture:
 * SetStateMessage { nowPlayingInfo=1, playbackQueue=3, displayName=5,
 * playbackState=6, playbackStateTimestamp=11 }; the queue holds one
 * ContentItem { identifier=1, metadata=2, artworkData=3 }. */
static bool build_set_state(struct ap2_mrp_ctx *m, bool include_artwork,
                            mbuf *out)
{
    bool ok = true;
    mbuf npi = {0}, meta = {0}, item = {0}, queue = {0}, inner = {0};
    double now_cf = mrp_cf_now();

    /* NowPlayingInfo (field numbers from NowPlayingInfo.proto) */
    ok &= pb_put_string_field(&npi, 1, m->album ? m->album : "");     /* album */
    ok &= pb_put_string_field(&npi, 2, m->artist ? m->artist : "");   /* artist */
    if (m->duration_ms > 0)
        ok &= pb_put_double_field(&npi, 3, m->duration_ms / 1000.0);  /* duration */
    ok &= pb_put_double_field(&npi, 4, m->elapsed_ms / 1000.0);       /* elapsedTime */
    ok &= pb_put_float_field(&npi, 5, m->playing ? 1.0f : 0.0f);      /* playbackRate */
    ok &= pb_put_double_field(&npi, 8, now_cf);                       /* timestamp */
    ok &= pb_put_string_field(&npi, 9, m->title ? m->title : "");     /* title */
    ok &= pb_put_bool_field(&npi, 12, true);                          /* isMusicApp */

    /* PlaybackQueue { location=1, contentItems=2 } with one ContentItem */
    ok = ok && build_content_item_metadata(m, &meta);
    ok &= pb_put_string_field(&item, 1, m->dacp_id);                  /* identifier */
    ok = ok && pb_put_msg_field(&item, 2, &meta);                     /* metadata */
    if (include_artwork && m->artwork && m->artwork_len > 0)
        ok &= pb_put_bytes_field(&item, 3, m->artwork,
                                 (size_t)m->artwork_len);             /* artworkData */
    ok &= pb_put_varint_field(&queue, 1, 0);                          /* location */
    ok = ok && pb_put_msg_field(&queue, 2, &item);                    /* contentItems */

    /* SetStateMessage */
    ok = ok && pb_put_msg_field(&inner, 1, &npi);
    ok = ok && pb_put_msg_field(&inner, 3, &queue);
    ok &= pb_put_string_field(&inner, 5, m->name);                    /* displayName */
    ok &= pb_put_varint_field(&inner, 6, m->playing ?                 /* playbackState */
                              MRP_PLAYBACK_STATE_PLAYING : MRP_PLAYBACK_STATE_PAUSED);
    ok &= pb_put_double_field(&inner, 11, now_cf);                    /* playbackStateTimestamp */

    ok = ok && mrp_envelope(out, MRP_MSG_SET_STATE, MRP_EXT_SET_STATE, &inner);

    mbuf_free(&npi);
    mbuf_free(&meta);
    mbuf_free(&item);
    mbuf_free(&queue);
    mbuf_free(&inner);
    return ok;
}

/* ---- Channel crypto (mirrors ap2_hap.c hkdf_sha512 / chacha helpers; those
 * are static there, and the MRP channel has its own keys and counters) ---- */

static bool mrp_hkdf_sha512(const uint8_t *secret, int secret_len,
                            const char *salt_str, const char *info_str,
                            uint8_t *out, int out_len)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    if (!ctx) return false;
    bool ok = false;
    size_t dklen = out_len;

    if (EVP_PKEY_derive_init(ctx) <= 0) goto done;
    if (EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha512()) <= 0) goto done;
    if (EVP_PKEY_CTX_set1_hkdf_salt(ctx, (const unsigned char *)salt_str,
                                    strlen(salt_str)) <= 0) goto done;
    if (EVP_PKEY_CTX_set1_hkdf_key(ctx, secret, secret_len) <= 0) goto done;
    if (EVP_PKEY_CTX_add1_hkdf_info(ctx, (const unsigned char *)info_str,
                                    strlen(info_str)) <= 0) goto done;
    if (EVP_PKEY_derive(ctx, out, &dklen) <= 0) goto done;
    ok = true;
done:
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

static void mrp_make_nonce(uint64_t counter, uint8_t nonce[MRP_NONCE_SIZE])
{
    memset(nonce, 0, MRP_NONCE_SIZE);
    for (int i = 0; i < 8; i++)
        nonce[4 + i] = (counter >> (i * 8)) & 0xFF;
}

static int mrp_chacha_encrypt(const uint8_t *key, const uint8_t *nonce,
                              const uint8_t *aad, int aad_len,
                              const uint8_t *pt, int pt_len,
                              uint8_t *ct, uint8_t tag[MRP_TAG_SIZE])
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int len, ct_len = 0;

    EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, MRP_NONCE_SIZE, NULL);
    EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce);
    if (aad && aad_len > 0)
        EVP_EncryptUpdate(ctx, NULL, &len, aad, aad_len);
    EVP_EncryptUpdate(ctx, ct, &len, pt, pt_len);
    ct_len = len;
    EVP_EncryptFinal_ex(ctx, ct + ct_len, &len);
    ct_len += len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, MRP_TAG_SIZE, tag);
    EVP_CIPHER_CTX_free(ctx);
    return ct_len;
}

static int mrp_chacha_decrypt(const uint8_t *key, const uint8_t *nonce,
                              const uint8_t *aad, int aad_len,
                              const uint8_t *ct, int ct_len,
                              const uint8_t *tag, uint8_t *pt)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int len, pt_len = 0;

    EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, MRP_NONCE_SIZE, NULL);
    EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce);
    if (aad && aad_len > 0)
        EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_len);
    EVP_DecryptUpdate(ctx, pt, &len, ct, ct_len);
    pt_len = len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, MRP_TAG_SIZE, (void *)tag);
    int ok = EVP_DecryptFinal_ex(ctx, pt + pt_len, &len);
    pt_len += len;
    EVP_CIPHER_CTX_free(ctx);
    return ok == 1 ? pt_len : -1;
}

/* Encrypt a plaintext blob into HAP channel frames with our out_key/counter.
 * Caller frees *out. Returns total byte count or -1. */
static int mrp_channel_encrypt(struct ap2_mrp_ctx *m, const uint8_t *in,
                               int in_len, uint8_t **out)
{
    int num_frames = (in_len + MRP_FRAME_MAX - 1) / MRP_FRAME_MAX;
    int total = num_frames * (2 + MRP_TAG_SIZE) + in_len;
    *out = malloc(total);
    if (!*out) return -1;

    int out_off = 0, in_off = 0;
    while (in_off < in_len) {
        int chunk = in_len - in_off;
        if (chunk > MRP_FRAME_MAX) chunk = MRP_FRAME_MAX;

        uint8_t len_bytes[2] = { chunk & 0xFF, (chunk >> 8) & 0xFF };
        memcpy(*out + out_off, len_bytes, 2);
        out_off += 2;

        uint8_t nonce[MRP_NONCE_SIZE];
        mrp_make_nonce(m->out_counter++, nonce);

        uint8_t tag[MRP_TAG_SIZE];
        if (mrp_chacha_encrypt(m->out_key, nonce, len_bytes, 2,
                               in + in_off, chunk,
                               *out + out_off, tag) < 0) {
            free(*out);
            *out = NULL;
            return -1;
        }
        out_off += chunk;
        memcpy(*out + out_off, tag, MRP_TAG_SIZE);
        out_off += MRP_TAG_SIZE;
        in_off += chunk;
    }
    return out_off;
}

/* ---- Data-frame construction and send ---- */

/* Write the 32-byte big-endian data-frame header. */
static void mrp_write_data_header(uint8_t hdr[MRP_DATA_HDR_LEN],
                                  const char msg_type4[4], const char cmd4[4],
                                  uint64_t seqno, uint32_t payload_len)
{
    uint32_t size = MRP_DATA_HDR_LEN + payload_len;
    memset(hdr, 0, MRP_DATA_HDR_LEN);
    hdr[0] = (size >> 24) & 0xFF;
    hdr[1] = (size >> 16) & 0xFF;
    hdr[2] = (size >> 8) & 0xFF;
    hdr[3] = size & 0xFF;
    memcpy(hdr + 4, msg_type4, 4);       /* 12-byte field, rest stays zero */
    memcpy(hdr + 16, cmd4, 4);
    for (int i = 0; i < 8; i++)
        hdr[16 + 4 + i] = (seqno >> ((7 - i) * 8)) & 0xFF;
    /* padding u32 at offset 28 stays zero */
}

/* Send raw bytes through the encrypted channel (holds send_lock). */
static bool mrp_send_raw(struct ap2_mrp_ctx *m, const uint8_t *data, int len)
{
    if (m->sock < 0) return false;
    pthread_mutex_lock(&m->send_lock);
    uint8_t *enc = NULL;
    int enc_len = mrp_channel_encrypt(m, data, len, &enc);
    bool ok = false;
    if (enc_len > 0 && enc)
        ok = write(m->sock, enc, enc_len) == enc_len;
    free(enc);
    pthread_mutex_unlock(&m->send_lock);
    return ok;
}

/* Wrap one serialized ProtocolMessage into a "sync"/"comm" data frame:
 * payload = bplist {"params": {"data": <varint length + protobuf>}}
 * (pyatv channels.py send_protobuf / encode_protobufs). */
static bool mrp_send_protobuf(struct ap2_mrp_ctx *m, const mbuf *msg)
{
    if (!m->connected) return false;

    /* varint length prefix + message bytes */
    mbuf blob = {0};
    bool ok = pb_put_varint(&blob, msg->len) && mbuf_put(&blob, msg->p, msg->len);
    if (!ok) {
        mbuf_free(&blob);
        return false;
    }

    /* {"params": {"data": blob}} */
    ap2_pl_node *params = ap2_pl_dict();
    ap2_pl_dict_set(params, "data", ap2_pl_data(blob.p, blob.len));
    ap2_pl_node *root = ap2_pl_dict();
    ap2_pl_dict_set(root, "params", params);
    uint8_t *plist = NULL;
    int plist_len = ap2_pl_serialize(root, &plist);
    ap2_pl_free(root);
    mbuf_free(&blob);
    if (plist_len <= 0 || !plist) {
        free(plist);
        return false;
    }

    uint8_t *frame = malloc(MRP_DATA_HDR_LEN + (size_t)plist_len);
    if (!frame) {
        free(plist);
        return false;
    }
    mrp_write_data_header(frame, "sync", "comm", m->send_seqno,
                          (uint32_t)plist_len);
    memcpy(frame + MRP_DATA_HDR_LEN, plist, plist_len);
    free(plist);

    ok = mrp_send_raw(m, frame, MRP_DATA_HDR_LEN + plist_len);
    free(frame);
    return ok;
}

/* Reply frame ("rply", zero command) acknowledging a receiver "sync" frame. */
static bool mrp_send_reply(struct ap2_mrp_ctx *m, uint64_t seqno)
{
    uint8_t hdr[MRP_DATA_HDR_LEN];
    static const char zero4[4] = {0, 0, 0, 0};
    mrp_write_data_header(hdr, "rply", zero4, seqno, 0);
    return mrp_send_raw(m, hdr, sizeof(hdr));
}

/* Build + push the full current state (metadata, progress, artwork). */
static bool mrp_push_state(struct ap2_mrp_ctx *m, bool include_artwork)
{
    mbuf msg = {0};
    bool ok = build_set_state(m, include_artwork, &msg);
    ok = ok && mrp_send_protobuf(m, &msg);
    mbuf_free(&msg);
    if (ok) m->last_state_push = time(NULL);
    LOG_DEBUG("[MRP] SET_STATE push (%s artwork) -> %s",
              include_artwork ? "with" : "without", ok ? "sent" : "failed");
    return ok;
}

/* ---- Inbound handling ---- */

/* Decrypt whatever channel frames are complete in rx_enc into rx_msg. */
static void mrp_drain_encrypted(struct ap2_mrp_ctx *m)
{
    int off = 0;
    while (off + 2 <= m->rx_enc_len) {
        int chunk = m->rx_enc[off] | (m->rx_enc[off + 1] << 8);
        if (off + 2 + chunk + MRP_TAG_SIZE > m->rx_enc_len) break;

        uint8_t len_bytes[2] = { m->rx_enc[off], m->rx_enc[off + 1] };
        uint8_t nonce[MRP_NONCE_SIZE];
        mrp_make_nonce(m->in_counter++, nonce);

        if (m->rx_msg_len + chunk <= (int)sizeof(m->rx_msg)) {
            int n = mrp_chacha_decrypt(m->in_key, nonce, len_bytes, 2,
                                       m->rx_enc + off + 2, chunk,
                                       m->rx_enc + off + 2 + chunk,
                                       m->rx_msg + m->rx_msg_len);
            if (n < 0) {
                LOG_ERROR("[MRP] channel frame decryption failed");
                m->connected = false;
                return;
            }
            m->rx_msg_len += n;
        } else {
            LOG_WARN("[MRP] inbound message buffer full, dropping frame");
        }
        off += 2 + chunk + MRP_TAG_SIZE;
    }
    if (off > 0) {
        memmove(m->rx_enc, m->rx_enc + off, m->rx_enc_len - off);
        m->rx_enc_len -= off;
    }
}

/* Consume complete 32-byte-header data frames from rx_msg. Incoming protobufs
 * are only counted for now; dispatching them (SendCommand transport controls
 * from the receiver: play/pause/next from the Siri remote) is a wiring-pass
 * item (MRP-DESIGN.md §8). */
static void mrp_process_frames(struct ap2_mrp_ctx *m)
{
    int off = 0;
    while (m->rx_msg_len - off >= MRP_DATA_HDR_LEN) {
        const uint8_t *h = m->rx_msg + off;
        uint32_t size = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                        ((uint32_t)h[2] << 8) | h[3];
        if (size < MRP_DATA_HDR_LEN || size > sizeof(m->rx_msg)) {
            LOG_ERROR("[MRP] bad data frame size %u, resetting buffer", size);
            m->rx_msg_len = 0;
            return;
        }
        if ((int)size > m->rx_msg_len - off) break;   /* incomplete */

        uint64_t seqno = 0;
        for (int i = 0; i < 8; i++) seqno = (seqno << 8) | h[20 + i];

        if (memcmp(h + 4, "sync", 4) == 0) {
            LOG_DEBUG("[MRP] inbound sync frame (%u bytes, seq %llu)",
                      size, (unsigned long long)seqno);
            mrp_send_reply(m, seqno);
        } else {
            LOG_DEBUG("[MRP] inbound frame %.4s (%u bytes)", (const char *)(h + 4), size);
        }
        off += (int)size;
    }
    if (off > 0) {
        memmove(m->rx_msg, m->rx_msg + off, m->rx_msg_len - off);
        m->rx_msg_len -= off;
    }
}

/* ---- Public API ---- */

struct ap2_mrp_ctx *ap2_mrp_create(const char *host, int port,
                                    const char *auth_credentials,
                                    const char *dacp_id,
                                    const char *device_name,
                                    const uint8_t *reuse_shared_secret)
{
    if (!host) return NULL;
    struct ap2_mrp_ctx *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->host = mrp_strdup(host);
    m->port = port;
    m->auth_credentials = auth_credentials ? mrp_strdup(auth_credentials) : NULL;
    m->dacp_id = mrp_strdup(dacp_id && *dacp_id ? dacp_id : "cliairplay");
    m->name = mrp_strdup(device_name && *device_name ? device_name : "Music Assistant");
    m->sock = -1;
    pthread_mutex_init(&m->send_lock, NULL);

    if (reuse_shared_secret) {
        memcpy(m->shared_secret, reuse_shared_secret, MRP_KEY_SIZE);
        m->have_secret = true;
    }

    /* pyatv picks a random 33-bit seqno (randrange(0x100000000, 0x1FFFFFFFF))
     * and keeps it constant for every frame on the channel. */
    uint32_t r = 0;
    RAND_bytes((uint8_t *)&r, sizeof(r));
    m->send_seqno = 0x100000000ULL | r;

    return m;
}

void ap2_mrp_destroy(struct ap2_mrp_ctx *m)
{
    if (!m) return;
    ap2_mrp_stop(m);
    pthread_mutex_destroy(&m->send_lock);
    OPENSSL_cleanse(m->shared_secret, sizeof(m->shared_secret));
    OPENSSL_cleanse(m->out_key, sizeof(m->out_key));
    OPENSSL_cleanse(m->in_key, sizeof(m->in_key));
    free(m->host);
    free(m->auth_credentials);
    free(m->dacp_id);
    free(m->name);
    free(m->title);
    free(m->artist);
    free(m->album);
    free(m->artwork_mime);
    free(m->artwork);
    free(m);
}

bool ap2_mrp_attach(struct ap2_mrp_ctx *m, int data_port, uint64_t seed)
{
    if (!m || data_port <= 0) return false;
    if (!m->have_secret) {
        LOG_ERROR("[MRP] attach requires the session's pairing shared secret");
        return false;
    }

    /* Derive the DataStream channel keys: HKDF-SHA512, salt =
     * "DataStream-Salt" + decimal seed, info = the Output/Input strings. */
    char salt[64];
    snprintf(salt, sizeof(salt), MRP_DATASTREAM_SALT_PREFIX "%llu",
             (unsigned long long)seed);
    if (!mrp_hkdf_sha512(m->shared_secret, MRP_KEY_SIZE, salt,
                         MRP_DATASTREAM_OUTPUT_INFO, m->out_key, MRP_KEY_SIZE) ||
        !mrp_hkdf_sha512(m->shared_secret, MRP_KEY_SIZE, salt,
                         MRP_DATASTREAM_INPUT_INFO, m->in_key, MRP_KEY_SIZE)) {
        LOG_ERROR("[MRP] DataStream key derivation failed");
        return false;
    }
    m->out_counter = 0;
    m->in_counter = 0;

    /* TCP connect to the receiver's dataPort */
    m->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (m->sock < 0) return false;
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(m->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(m->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)data_port),
    };
    inet_pton(AF_INET, m->host, &addr.sin_addr);
    if (connect(m->sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("[MRP] data channel connect to %s:%d failed: %s",
                  m->host, data_port, strerror(errno));
        close(m->sock);
        m->sock = -1;
        return false;
    }
    int one = 1;
    setsockopt(m->sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    m->connected = true;
    LOG_INFO("[MRP] data channel connected to %s:%d", m->host, data_port);

    /* Opening sequence: DEVICE_INFO must be first (receiver stays silent
     * otherwise), then connection state, updates config, now-playing client
     * registration, and the initial full state. */
    bool ok = true;
    mbuf msg = {0};
    ok = ok && build_device_info(m, &msg) && mrp_send_protobuf(m, &msg);
    mbuf_free(&msg);
    ok = ok && build_set_connection_state(&msg, MRP_CONNECTION_STATE_CONNECTED) &&
         mrp_send_protobuf(m, &msg);
    mbuf_free(&msg);
    ok = ok && build_client_updates_config(&msg) && mrp_send_protobuf(m, &msg);
    mbuf_free(&msg);
    ok = ok && build_set_now_playing_client(m, &msg) && mrp_send_protobuf(m, &msg);
    mbuf_free(&msg);
    ok = ok && mrp_push_state(m, m->artwork_len > 0);

    if (!ok) {
        LOG_ERROR("[MRP] handshake push failed");
        ap2_mrp_stop(m);
        return false;
    }
    LOG_INFO("[MRP] handshake pushed (device info, connection state, "
             "now-playing client, initial state)");
    return true;
}

bool ap2_mrp_start(struct ap2_mrp_ctx *m)
{
    if (!m) return false;
    /* Sidecar bootstrap for the metadata-only display mode: own TCP
     * connection to host:port, HAP pair-verify with auth_credentials (client
     * identity from dacp_id), session SETUP with isRemoteControlOnly=true and
     * timingProtocol="None", event channel attach, RECORD, then the type-130
     * data channel SETUP and ap2_mrp_attach() on its result. Needs the RTSP +
     * HAP plumbing from ap2_client.c/ap2_hap.c — wiring pass, see
     * MRP-DESIGN.md §9. */
    LOG_WARN("[MRP] standalone remote-control session not implemented yet "
             "(metadata-only mode); use ap2_mrp_attach from an audio session");
    return false;
}

void ap2_mrp_stop(struct ap2_mrp_ctx *m)
{
    if (!m) return;
    if (m->connected) {
        mbuf msg = {0};
        if (build_set_connection_state(&msg, MRP_CONNECTION_STATE_DISCONNECTED))
            mrp_send_protobuf(m, &msg);
        mbuf_free(&msg);
        m->connected = false;
    }
    if (m->sock >= 0) {
        close(m->sock);
        m->sock = -1;
    }
    m->rx_enc_len = 0;
    m->rx_msg_len = 0;
}

bool ap2_mrp_set_metadata(struct ap2_mrp_ctx *m, const char *title,
                          const char *artist, const char *album,
                          int duration_ms)
{
    if (!m) return false;
    mrp_replace_str(&m->title, title);
    mrp_replace_str(&m->artist, artist);
    mrp_replace_str(&m->album, album);
    m->duration_ms = duration_ms > 0 ? duration_ms : 0;
    if (!m->connected) return true;
    return mrp_push_state(m, false);
}

bool ap2_mrp_set_artwork(struct ap2_mrp_ctx *m, const char *mime,
                         const uint8_t *data, int len)
{
    if (!m || !data || len <= 0) return false;
    uint8_t *copy = malloc((size_t)len);
    if (!copy) return false;
    memcpy(copy, data, (size_t)len);
    free(m->artwork);
    m->artwork = copy;
    m->artwork_len = len;
    mrp_replace_str(&m->artwork_mime, mime ? mime : "image/jpeg");
    if (!m->connected) return true;
    return mrp_push_state(m, true);
}

bool ap2_mrp_set_progress(struct ap2_mrp_ctx *m, int elapsed_ms,
                          int duration_ms, bool playing)
{
    if (!m) return false;
    m->elapsed_ms = elapsed_ms > 0 ? elapsed_ms : 0;
    if (duration_ms > 0) m->duration_ms = duration_ms;
    m->playing = playing;
    m->elapsed_set_at = mrp_cf_now();
    if (!m->connected) return true;
    /* The receiver extrapolates position from elapsedTime + timestamp +
     * playbackRate, so a progress change is a state push, not a stream. */
    return mrp_push_state(m, false);
}

bool ap2_mrp_set_playing(struct ap2_mrp_ctx *m, bool playing)
{
    if (!m) return false;
    /* Advance the stored elapsed to now before flipping so the frozen (pause)
     * or resumed (play) position matches what the receiver extrapolated up to
     * this instant, then re-anchor the timestamp at now. */
    double now = mrp_cf_now();
    if (m->playing && m->elapsed_set_at > 0.0) {
        double advanced_ms = (now - m->elapsed_set_at) * 1000.0;
        if (advanced_ms > 0.0) m->elapsed_ms += (int)advanced_ms;
    }
    m->playing = playing;
    m->elapsed_set_at = now;
    if (!m->connected) return true;
    return mrp_push_state(m, false);
}

void ap2_mrp_tick(struct ap2_mrp_ctx *m)
{
    if (!m || !m->connected || m->sock < 0) return;

    /* Drain inbound bytes without blocking */
    struct pollfd pfd = { .fd = m->sock, .events = POLLIN };
    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        int space = (int)sizeof(m->rx_enc) - m->rx_enc_len;
        if (space <= 0) break;
        ssize_t n = read(m->sock, m->rx_enc + m->rx_enc_len, (size_t)space);
        if (n <= 0) {
            LOG_WARN("[MRP] data channel closed by receiver");
            m->connected = false;
            close(m->sock);
            m->sock = -1;
            return;
        }
        m->rx_enc_len += (int)n;
        mrp_drain_encrypted(m);
        mrp_process_frames(m);
        if (!m->connected) return;
    }

    /* Defensive periodic re-push: keeps the system now-playing session warm
     * (standby prevention hinges on it, MRP-DESIGN.md §6). */
    if (m->playing && time(NULL) - m->last_state_push >= MRP_STATE_REPUSH_S)
        mrp_push_state(m, false);
}

bool ap2_mrp_is_connected(struct ap2_mrp_ctx *m)
{
    return m && m->connected;
}

bool ap2_mrp_build_nowplaying_command(struct ap2_mrp_ctx *m,
                                      uint8_t **out, int *out_len)
{
    if (!m || !out || !out_len) return false;

    /* Push path A (MRP-DESIGN.md §4): a bplist body for POST /command on the
     * main encrypted RTSP channel, carrying MediaRemote now-playing keys.
     * Real iOS senders push this shape (openairplay/airplay2-receiver
     * handle_command observes params.params.kMRMediaRemoteNowPlayingInfo*
     * under a top-level "params" dict); the outer "type" string is our best
     * guess — the reference receiver ignores it. Durations/elapsed/rate are
     * plist reals (MediaRemote uses doubles). */
    ap2_pl_node *info = ap2_pl_dict();
    ap2_pl_dict_set(info, "kMRMediaRemoteNowPlayingInfoTitle",
                    ap2_pl_string(m->title ? m->title : ""));
    ap2_pl_dict_set(info, "kMRMediaRemoteNowPlayingInfoArtist",
                    ap2_pl_string(m->artist ? m->artist : ""));
    ap2_pl_dict_set(info, "kMRMediaRemoteNowPlayingInfoAlbum",
                    ap2_pl_string(m->album ? m->album : ""));
    if (m->duration_ms > 0)
        ap2_pl_dict_set(info, "kMRMediaRemoteNowPlayingInfoDuration",
                        ap2_pl_real((double)m->duration_ms / 1000.0));
    ap2_pl_dict_set(info, "kMRMediaRemoteNowPlayingInfoElapsedTime",
                    ap2_pl_real((double)m->elapsed_ms / 1000.0));
    ap2_pl_dict_set(info, "kMRMediaRemoteNowPlayingInfoPlaybackRate",
                    ap2_pl_real(m->playing ? 1.0 : 0.0));
    if (m->artwork && m->artwork_len > 0) {
        ap2_pl_dict_set(info, "kMRMediaRemoteNowPlayingInfoArtworkData",
                        ap2_pl_data(m->artwork, (size_t)m->artwork_len));
        ap2_pl_dict_set(info, "kMRMediaRemoteNowPlayingInfoArtworkMIMEType",
                        ap2_pl_string(m->artwork_mime ? m->artwork_mime
                                                      : "image/jpeg"));
    }

    ap2_pl_node *params = ap2_pl_dict();
    ap2_pl_dict_set(params, "params", info);
    ap2_pl_node *root = ap2_pl_dict();
    ap2_pl_dict_set(root, "type", ap2_pl_string("updateNowPlayingInfo"));
    ap2_pl_dict_set(root, "params", params);

    int len = ap2_pl_serialize(root, out);
    ap2_pl_free(root);
    if (len <= 0) return false;
    *out_len = len;
    return true;
}
