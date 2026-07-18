/*
 * Minimal binary plist (bplist00) builder for AirPlay 2 SETUP messages.
 *
 * Binary plist format (bplist00):
 *   Header: "bplist00" (8 bytes)
 *   Objects: encoded values (strings, ints, dicts, arrays, data, booleans)
 *   Offset table: offsets to each object
 *   Trailer: 32 bytes with metadata (offset size, ref size, num objects, etc.)
 *
 * Object types (high nibble of first byte):
 *   0x0 = null/bool/fill (0x08=false, 0x09=true)
 *   0x1 = int (low nibble = log2(byte count): 0=1B, 1=2B, 2=4B, 3=8B)
 *   0x4 = data (low nibble = length, or 0xF + int length)
 *   0x5 = string (ASCII, low nibble = length, or 0xF + int length)
 *   0xa = array (low nibble = count, or 0xF + int count)
 *   0xd = dict (low nibble = count, or 0xF + int count)
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include "ap2_plist.h"

/* Dynamic buffer */
typedef struct {
    uint8_t *data;
    int len;
    int cap;
} buf_t;

static void buf_init(buf_t *b) { b->cap = 512; b->data = malloc(b->cap); b->len = 0; }
static void buf_free(buf_t *b) { free(b->data); }
static void buf_grow(buf_t *b, int need) {
    while (b->len + need > b->cap) { b->cap *= 2; b->data = realloc(b->data, b->cap); }
}
static void buf_append(buf_t *b, const void *data, int len) {
    buf_grow(b, len); memcpy(b->data + b->len, data, len); b->len += len;
}
static void buf_append_byte(buf_t *b, uint8_t v) { buf_grow(b, 1); b->data[b->len++] = v; }

/* Object reference tracking */
#define MAX_OBJECTS 256

typedef enum {
    OBJ_STRING, OBJ_INT, OBJ_BOOL, OBJ_DATA, OBJ_ARRAY, OBJ_DICT
} obj_type_t;

typedef struct {
    obj_type_t type;
    union {
        struct { const char *val; } string;
        struct { int64_t val; } integer;
        struct { bool val; } boolean;
        struct { const uint8_t *val; size_t len; } data;
        struct { int start; int count; } array;  /* indices into refs */
        struct { int start; int count; } dict;   /* indices into refs (key,val pairs) */
    };
} object_t;

typedef struct {
    char *key;
    int val_obj;  /* index into objects */
} dict_entry_t;

struct ap2_plist {
    object_t objects[MAX_OBJECTS];
    int num_objects;

    /* Root dict entries */
    dict_entry_t root_entries[32];
    int root_count;

    /* Stream dict entries (inside streams array) */
    dict_entry_t stream_entries[32];
    int stream_count;
    bool has_stream;
};

static int add_object(struct ap2_plist *p, object_t obj) {
    if (p->num_objects >= MAX_OBJECTS) return -1;
    p->objects[p->num_objects] = obj;
    return p->num_objects++;
}

/* ---- Public API ---- */

struct ap2_plist *ap2_plist_create(void)
{
    struct ap2_plist *p = calloc(1, sizeof(*p));
    return p;
}

void ap2_plist_free(struct ap2_plist *p) {
    if (!p) return;
    for (int i = 0; i < p->root_count; i++) free(p->root_entries[i].key);
    for (int i = 0; i < p->stream_count; i++) free(p->stream_entries[i].key);
    free(p);
}

static int add_string_obj(struct ap2_plist *p, const char *val) {
    object_t o = { .type = OBJ_STRING, .string = { .val = val } };
    return add_object(p, o);
}
static int add_int_obj(struct ap2_plist *p, int64_t val) {
    object_t o = { .type = OBJ_INT, .integer = { .val = val } };
    return add_object(p, o);
}
static int add_bool_obj(struct ap2_plist *p, bool val) {
    object_t o = { .type = OBJ_BOOL, .boolean = { .val = val } };
    return add_object(p, o);
}
static int add_data_obj(struct ap2_plist *p, const uint8_t *data, size_t len) {
    object_t o = { .type = OBJ_DATA, .data = { .val = data, .len = len } };
    return add_object(p, o);
}

void ap2_plist_add_string(struct ap2_plist *p, const char *key, const char *value) {
    int vi = add_string_obj(p, value);
    p->root_entries[p->root_count].key = strdup(key);
    p->root_entries[p->root_count].val_obj = vi;
    p->root_count++;
}
void ap2_plist_add_int(struct ap2_plist *p, const char *key, int64_t value) {
    int vi = add_int_obj(p, value);
    p->root_entries[p->root_count].key = strdup(key);
    p->root_entries[p->root_count].val_obj = vi;
    p->root_count++;
}
void ap2_plist_add_bool(struct ap2_plist *p, const char *key, bool value) {
    int vi = add_bool_obj(p, value);
    p->root_entries[p->root_count].key = strdup(key);
    p->root_entries[p->root_count].val_obj = vi;
    p->root_count++;
}
void ap2_plist_add_data(struct ap2_plist *p, const char *key, const uint8_t *data, size_t len) {
    int vi = add_data_obj(p, data, len);
    p->root_entries[p->root_count].key = strdup(key);
    p->root_entries[p->root_count].val_obj = vi;
    p->root_count++;
}

void ap2_plist_stream_begin(struct ap2_plist *p) { p->has_stream = true; p->stream_count = 0; }
void ap2_plist_stream_add_string(struct ap2_plist *p, const char *key, const char *value) {
    int vi = add_string_obj(p, value);
    p->stream_entries[p->stream_count].key = strdup(key);
    p->stream_entries[p->stream_count].val_obj = vi;
    p->stream_count++;
}
void ap2_plist_stream_add_int(struct ap2_plist *p, const char *key, int64_t value) {
    int vi = add_int_obj(p, value);
    p->stream_entries[p->stream_count].key = strdup(key);
    p->stream_entries[p->stream_count].val_obj = vi;
    p->stream_count++;
}
void ap2_plist_stream_add_bool(struct ap2_plist *p, const char *key, bool value) {
    int vi = add_bool_obj(p, value);
    p->stream_entries[p->stream_count].key = strdup(key);
    p->stream_entries[p->stream_count].val_obj = vi;
    p->stream_count++;
}
void ap2_plist_stream_add_data(struct ap2_plist *p, const char *key, const uint8_t *data, size_t len) {
    int vi = add_data_obj(p, data, len);
    p->stream_entries[p->stream_count].key = strdup(key);
    p->stream_entries[p->stream_count].val_obj = vi;
    p->stream_count++;
}
void ap2_plist_stream_end(struct ap2_plist *p) { /* nothing to do, stream_count is set */ }

/* ---- Serialization ---- */

/* Write a plist size/count prefix */
static void write_size(buf_t *b, uint8_t type_nibble, size_t count) {
    if (count < 15) {
        buf_append_byte(b, type_nibble | (uint8_t)count);
    } else {
        buf_append_byte(b, type_nibble | 0x0F);
        /* Write int object for extended count */
        if (count <= 0xFF) {
            buf_append_byte(b, 0x10); buf_append_byte(b, (uint8_t)count);
        } else if (count <= 0xFFFF) {
            buf_append_byte(b, 0x11);
            uint16_t be = htons((uint16_t)count);
            buf_append(b, &be, 2);
        } else {
            buf_append_byte(b, 0x12);
            uint32_t be = htonl((uint32_t)count);
            buf_append(b, &be, 4);
        }
    }
}

static void write_int_value(buf_t *b, int64_t val) {
    if (val >= 0 && val <= 0xFF) {
        buf_append_byte(b, 0x10); buf_append_byte(b, (uint8_t)val);
    } else if (val >= 0 && val <= 0xFFFF) {
        buf_append_byte(b, 0x11);
        uint16_t be = htons((uint16_t)val);
        buf_append(b, &be, 2);
    } else if (val >= 0 && val <= 0xFFFFFFFF) {
        buf_append_byte(b, 0x12);
        uint32_t be = htonl((uint32_t)val);
        buf_append(b, &be, 4);
    } else {
        buf_append_byte(b, 0x13);
        /* 8-byte big-endian */
        uint32_t hi = htonl((uint32_t)(val >> 32));
        uint32_t lo = htonl((uint32_t)(val & 0xFFFFFFFF));
        buf_append(b, &hi, 4);
        buf_append(b, &lo, 4);
    }
}

int ap2_plist_serialize(struct ap2_plist *p, uint8_t **out)
{
    if (!p) return 0;

    /*
     * Strategy: write objects sequentially, track offsets, then write offset table + trailer.
     * Object indices are assigned as we write.
     */

    buf_t objs;
    buf_init(&objs);

    /* Header */
    buf_append(&objs, "bplist00", 8);

    /* We need to know all object count and assign indices first.
     * Layout:
     *   obj 0: root dict
     *   obj 1..N: root key strings
     *   obj N+1..2N: root value objects
     *   If has_stream:
     *     obj X: "streams" key string
     *     obj X+1: array (1 element)
     *     obj X+2: stream dict
     *     obj X+3..M: stream key strings
     *     obj M+1..P: stream value objects
     */

    int total_root = p->root_count + (p->has_stream ? 1 : 0);
    int total_objects = 1;  /* root dict */
    total_objects += total_root;   /* root key strings */
    total_objects += p->root_count; /* root value objects */
    if (p->has_stream) {
        total_objects += 1;  /* array */
        total_objects += 1;  /* stream dict */
        total_objects += p->stream_count; /* stream key strings */
        total_objects += p->stream_count; /* stream value objects */
    }

    /* Offset table */
    int *offsets = calloc(total_objects, sizeof(int));
    int obj_idx = 0;

    /* Determine ref size (1 byte if < 256 objects) */
    uint8_t ref_size = (total_objects < 256) ? 1 : 2;

    /* Write root dict: 0xDN where N = entry count */
    offsets[obj_idx++] = objs.len;
    write_size(&objs, 0xD0, total_root);

    /* Key refs (will be filled in) */
    int root_keys_pos = objs.len;
    for (int i = 0; i < total_root; i++) buf_append_byte(&objs, 0); /* placeholder */
    /* Value refs */
    int root_vals_pos = objs.len;
    for (int i = 0; i < total_root; i++) buf_append_byte(&objs, 0);

    /* Write root keys and values */
    int key_idx = obj_idx;
    int val_idx = key_idx + total_root;

    /* Root entries (non-stream) */
    for (int i = 0; i < p->root_count; i++) {
        /* Key string */
        offsets[key_idx + i] = objs.len;
        objs.data[root_keys_pos + i] = key_idx + i;
        const char *key = p->root_entries[i].key;
        write_size(&objs, 0x50, strlen(key));
        buf_append(&objs, key, strlen(key));

        /* Value */
        offsets[val_idx + i] = objs.len;
        objs.data[root_vals_pos + i] = val_idx + i;
        object_t *vo = &p->objects[p->root_entries[i].val_obj];
        switch (vo->type) {
            case OBJ_STRING:
                write_size(&objs, 0x50, strlen(vo->string.val));
                buf_append(&objs, vo->string.val, strlen(vo->string.val));
                break;
            case OBJ_INT:
                write_int_value(&objs, vo->integer.val);
                break;
            case OBJ_BOOL:
                buf_append_byte(&objs, vo->boolean.val ? 0x09 : 0x08);
                break;
            case OBJ_DATA:
                write_size(&objs, 0x40, vo->data.len);
                buf_append(&objs, vo->data.val, vo->data.len);
                break;
            default: break;
        }
    }

    /* Stream entry in root dict */
    if (p->has_stream) {
        int stream_key_idx = key_idx + p->root_count;
        int array_idx = val_idx + p->root_count;
        int dict_idx = array_idx + 1;
        int skey_start = dict_idx + 1;
        int sval_start = skey_start + p->stream_count;

        /* "streams" key */
        offsets[stream_key_idx] = objs.len;
        objs.data[root_keys_pos + p->root_count] = stream_key_idx;
        write_size(&objs, 0x50, 7);
        buf_append(&objs, "streams", 7);

        /* streams value = array */
        offsets[array_idx] = objs.len;
        objs.data[root_vals_pos + p->root_count] = array_idx;
        write_size(&objs, 0xA0, 1);  /* array with 1 element */
        buf_append_byte(&objs, dict_idx);  /* ref to stream dict */

        /* Stream dict */
        offsets[dict_idx] = objs.len;
        write_size(&objs, 0xD0, p->stream_count);
        int sdict_keys_pos = objs.len;
        for (int i = 0; i < p->stream_count; i++) buf_append_byte(&objs, 0);
        int sdict_vals_pos = objs.len;
        for (int i = 0; i < p->stream_count; i++) buf_append_byte(&objs, 0);

        /* Stream keys and values */
        for (int i = 0; i < p->stream_count; i++) {
            /* Key */
            offsets[skey_start + i] = objs.len;
            objs.data[sdict_keys_pos + i] = skey_start + i;
            const char *key = p->stream_entries[i].key;
            write_size(&objs, 0x50, strlen(key));
            buf_append(&objs, key, strlen(key));

            /* Value */
            offsets[sval_start + i] = objs.len;
            objs.data[sdict_vals_pos + i] = sval_start + i;
            object_t *vo = &p->objects[p->stream_entries[i].val_obj];
            switch (vo->type) {
                case OBJ_STRING:
                    write_size(&objs, 0x50, strlen(vo->string.val));
                    buf_append(&objs, vo->string.val, strlen(vo->string.val));
                    break;
                case OBJ_INT:
                    write_int_value(&objs, vo->integer.val);
                    break;
                case OBJ_BOOL:
                    buf_append_byte(&objs, vo->boolean.val ? 0x09 : 0x08);
                    break;
                case OBJ_DATA:
                    write_size(&objs, 0x40, vo->data.len);
                    buf_append(&objs, vo->data.val, vo->data.len);
                    break;
                default: break;
            }
        }
    }

    /* Offset table */
    int offset_table_start = objs.len;
    uint8_t ofs_size = 1;
    if (offset_table_start > 0xFF) ofs_size = 2;
    if (offset_table_start > 0xFFFF) ofs_size = 4;

    for (int i = 0; i < total_objects; i++) {
        uint32_t ofs = offsets[i];
        for (int j = ofs_size - 1; j >= 0; j--) {
            buf_append_byte(&objs, (ofs >> (j * 8)) & 0xFF);
        }
    }

    /* Trailer (32 bytes) */
    uint8_t trailer[32];
    memset(trailer, 0, 32);
    trailer[6] = ofs_size;   /* offset size */
    trailer[7] = ref_size;   /* object ref size */
    /* num objects (8 bytes BE) */
    uint32_t no = htonl(total_objects);
    memcpy(trailer + 12, &no, 4);
    /* top object offset = 0 */
    /* offset table start (8 bytes BE) */
    uint32_t ots = htonl(offset_table_start);
    memcpy(trailer + 28, &ots, 4);

    buf_append(&objs, trailer, 32);

    free(offsets);
    *out = objs.data;
    return objs.len;
}

/* ---- General nested plist node API ---- */

enum { PLN_DICT, PLN_ARRAY, PLN_STRING, PLN_INT, PLN_BOOL, PLN_DATA };

struct ap2_pl_node {
    int type;
    int index;            /* assigned during serialization */
    char *str;            /* PLN_STRING */
    int64_t ival;         /* PLN_INT */
    bool bval;            /* PLN_BOOL */
    uint8_t *data;        /* PLN_DATA */
    size_t dlen;
    /* Containers: keys is non-NULL only for dicts (key string nodes),
     * vals holds dict values or array elements. */
    struct ap2_pl_node **keys;
    struct ap2_pl_node **vals;
    int n;
    int cap;
};

static struct ap2_pl_node *pl_new(int type)
{
    struct ap2_pl_node *n = calloc(1, sizeof(*n));
    if (n) n->type = type;
    return n;
}

ap2_pl_node *ap2_pl_dict(void) { return pl_new(PLN_DICT); }
ap2_pl_node *ap2_pl_array(void) { return pl_new(PLN_ARRAY); }

ap2_pl_node *ap2_pl_string(const char *s)
{
    struct ap2_pl_node *n = pl_new(PLN_STRING);
    if (n) n->str = strdup(s ? s : "");
    return n;
}

ap2_pl_node *ap2_pl_int(int64_t v)
{
    struct ap2_pl_node *n = pl_new(PLN_INT);
    if (n) n->ival = v;
    return n;
}

ap2_pl_node *ap2_pl_bool(bool b)
{
    struct ap2_pl_node *n = pl_new(PLN_BOOL);
    if (n) n->bval = b;
    return n;
}

ap2_pl_node *ap2_pl_data(const uint8_t *d, size_t len)
{
    struct ap2_pl_node *n = pl_new(PLN_DATA);
    if (!n) return NULL;
    n->data = malloc(len ? len : 1);
    if (d && len) memcpy(n->data, d, len);
    n->dlen = len;
    return n;
}

static void pl_grow(struct ap2_pl_node *c)
{
    if (c->n < c->cap) return;
    c->cap = c->cap ? c->cap * 2 : 8;
    c->vals = realloc(c->vals, c->cap * sizeof(*c->vals));
    if (c->type == PLN_DICT)
        c->keys = realloc(c->keys, c->cap * sizeof(*c->keys));
}

void ap2_pl_dict_set(ap2_pl_node *dict, const char *key, ap2_pl_node *val)
{
    if (!dict || dict->type != PLN_DICT || !val) return;
    pl_grow(dict);
    dict->keys[dict->n] = ap2_pl_string(key);
    dict->vals[dict->n] = val;
    dict->n++;
}

void ap2_pl_array_append(ap2_pl_node *arr, ap2_pl_node *val)
{
    if (!arr || arr->type != PLN_ARRAY || !val) return;
    pl_grow(arr);
    arr->vals[arr->n++] = val;
}

void ap2_pl_free(ap2_pl_node *node)
{
    if (!node) return;
    if (node->type == PLN_DICT) {
        for (int i = 0; i < node->n; i++) {
            ap2_pl_free(node->keys[i]);
            ap2_pl_free(node->vals[i]);
        }
    } else if (node->type == PLN_ARRAY) {
        for (int i = 0; i < node->n; i++) ap2_pl_free(node->vals[i]);
    }
    free(node->keys);
    free(node->vals);
    free(node->str);
    free(node->data);
    free(node);
}

/* Depth-first index assignment: root is object 0 (the trailer's top object),
 * every other node gets a unique index as it is first visited. */
static void pl_assign(struct ap2_pl_node *n, struct ap2_pl_node ***all,
                      int *count, int *cap)
{
    n->index = *count;
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *all = realloc(*all, *cap * sizeof(**all));
    }
    (*all)[(*count)++] = n;
    if (n->type == PLN_DICT) {
        for (int i = 0; i < n->n; i++) pl_assign(n->keys[i], all, count, cap);
        for (int i = 0; i < n->n; i++) pl_assign(n->vals[i], all, count, cap);
    } else if (n->type == PLN_ARRAY) {
        for (int i = 0; i < n->n; i++) pl_assign(n->vals[i], all, count, cap);
    }
}

static void write_ref(buf_t *b, int idx, int ref_size)
{
    for (int j = ref_size - 1; j >= 0; j--)
        buf_append_byte(b, (idx >> (j * 8)) & 0xFF);
}

int ap2_pl_serialize(const ap2_pl_node *root, uint8_t **out)
{
    if (!root) return 0;

    struct ap2_pl_node **all = NULL;
    int total = 0, cap = 0;
    pl_assign((struct ap2_pl_node *)root, &all, &total, &cap);

    uint8_t ref_size = (total < 256) ? 1 : 2;

    buf_t objs;
    buf_init(&objs);
    buf_append(&objs, "bplist00", 8);

    int *offsets = calloc(total, sizeof(int));
    for (int i = 0; i < total; i++) {
        struct ap2_pl_node *n = all[i];
        offsets[n->index] = objs.len;
        switch (n->type) {
        case PLN_DICT:
            write_size(&objs, 0xD0, n->n);
            for (int k = 0; k < n->n; k++) write_ref(&objs, n->keys[k]->index, ref_size);
            for (int k = 0; k < n->n; k++) write_ref(&objs, n->vals[k]->index, ref_size);
            break;
        case PLN_ARRAY:
            write_size(&objs, 0xA0, n->n);
            for (int k = 0; k < n->n; k++) write_ref(&objs, n->vals[k]->index, ref_size);
            break;
        case PLN_STRING:
            write_size(&objs, 0x50, strlen(n->str));
            buf_append(&objs, n->str, strlen(n->str));
            break;
        case PLN_INT:
            write_int_value(&objs, n->ival);
            break;
        case PLN_BOOL:
            buf_append_byte(&objs, n->bval ? 0x09 : 0x08);
            break;
        case PLN_DATA:
            write_size(&objs, 0x40, n->dlen);
            buf_append(&objs, n->data, n->dlen);
            break;
        default: break;
        }
    }

    int offset_table_start = objs.len;
    uint8_t ofs_size = 1;
    if (offset_table_start > 0xFF) ofs_size = 2;
    if (offset_table_start > 0xFFFF) ofs_size = 4;

    for (int i = 0; i < total; i++) {
        uint32_t ofs = offsets[i];
        for (int j = ofs_size - 1; j >= 0; j--)
            buf_append_byte(&objs, (ofs >> (j * 8)) & 0xFF);
    }

    uint8_t trailer[32];
    memset(trailer, 0, 32);
    trailer[6] = ofs_size;
    trailer[7] = ref_size;
    uint32_t no = htonl((uint32_t)total);
    memcpy(trailer + 12, &no, 4);
    uint32_t ots = htonl((uint32_t)offset_table_start);
    memcpy(trailer + 28, &ots, 4);
    buf_append(&objs, trailer, 32);

    free(offsets);
    free(all);
    *out = objs.data;
    return objs.len;
}
