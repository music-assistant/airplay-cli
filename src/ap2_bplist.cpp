/*
 * C wrapper for bplist (binary plist) reader/writer
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

/* bplist.h (from libraop) uses uint*_t but does not include <cstdint> itself.
 * libc++ leaks it transitively via <string>/<vector>, but libstdc++ does not,
 * so include it explicitly before pulling in bplist.h. */
/* Access private members of bplist for integer reading */
#define private public
#include "../libraop/src/bplist.h"
#undef private

#include "ap2_bplist.h"

struct ap2_bplist {
    bplist bp;
    std::string last_string;
    std::vector<uint8_t> last_data;
};

extern "C" {

struct ap2_bplist *ap2_bplist_create(void)
{
    return new ap2_bplist();
}

struct ap2_bplist *ap2_bplist_parse(const uint8_t *data, size_t len)
{
    auto bp = new ap2_bplist();
    std::vector<uint8_t> blob(data, data + len);
    bp->bp = bplist(blob);
    return bp;
}

void ap2_bplist_free(struct ap2_bplist *bp)
{
    delete bp;
}

void ap2_bplist_add_string(struct ap2_bplist *bp, const char *key, const char *value)
{
    if (bp) bp->bp.add(std::string(key), std::string(value));
}

void ap2_bplist_add_int(struct ap2_bplist *bp, const char *key, uint64_t value)
{
    if (bp) bp->bp.add((size_t)1, key, (int)bplist::INTEGER, (uint32_t)value);
}

void ap2_bplist_add_data(struct ap2_bplist *bp, const char *key,
                          const uint8_t *data, size_t len)
{
    if (bp) bp->bp.add((size_t)1, key, (int)bplist::DATA, data, len);
}

int ap2_bplist_serialize(struct ap2_bplist *bp, uint8_t **out)
{
    if (!bp) return 0;
    auto data = bp->bp.toData();
    *out = (uint8_t *)malloc(data.size());
    memcpy(*out, data.data(), data.size());
    return (int)data.size();
}

const char *ap2_bplist_get_string(struct ap2_bplist *bp, const char *key)
{
    if (!bp) return NULL;
    bp->last_string = bp->bp.getValueString(std::string(key));
    return bp->last_string.empty() ? NULL : bp->last_string.c_str();
}

uint64_t ap2_bplist_get_int(struct ap2_bplist *bp, const char *key)
{
    if (!bp) return 0;
    auto it = bp->bp.entries.find(std::string(key));
    if (it != bp->bp.entries.end()) return it->second.integer;
    return 0;
}

const uint8_t *ap2_bplist_get_data(struct ap2_bplist *bp, const char *key, size_t *len)
{
    if (!bp) { *len = 0; return NULL; }
    bp->last_data = bp->bp.getValueData(std::string(key));
    *len = bp->last_data.size();
    return bp->last_data.empty() ? NULL : bp->last_data.data();
}

/*
 * Standalone integer-by-key lookup with real bplist traversal (trailer ->
 * offset table -> dict keyRef/valueRef resolution), searching every dict in
 * the plist including nested ones (e.g. streams[0].dataPort). libraop's
 * simplified reader mis-reads integers, so port extraction must not use it.
 */

/* Read the size/count nibble at *pos; advances *pos past marker + extended count. */
static bool bp_read_count(const uint8_t *obj, size_t len, size_t *pos, size_t *count)
{
    if (*pos >= len) return false;
    uint8_t marker = obj[*pos];
    size_t n = marker & 0x0F;
    (*pos)++;
    if ((marker & 0xF0) == 0x10) { *count = (size_t)1 << n; return true; }  /* int: 2^n bytes */
    if (n != 0x0F) { *count = n; return true; }
    /* extended count: an int object follows inline */
    if (*pos >= len || (obj[*pos] & 0xF0) != 0x10) return false;
    size_t ibytes = (size_t)1 << (obj[*pos] & 0x0F);
    (*pos)++;
    if (*pos + ibytes > len) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < ibytes; i++) v = (v << 8) | obj[(*pos)++];
    *count = (size_t)v;
    return true;
}

static uint64_t bp_read_be(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

int ap2_bplist_get_root_string(const uint8_t *data, size_t len, const char *key,
                               char *out, size_t out_size)
{
    if (!key || !out || !out_size) return 0;
    out[0] = '\0';
    if (!data || len < 40 || memcmp(data, "bplist00", 8) != 0) return 0;

    const uint8_t *tr = data + len - 32;
    uint8_t ofs_size = tr[6], ref_size = tr[7];
    uint64_t num_objects = bp_read_be(tr + 8, 8);
    uint64_t table_ofs = bp_read_be(tr + 24, 8);
    if (!ofs_size || ofs_size > 8 || !ref_size || ref_size > 8) return 0;
    size_t object_end = len - 32;
    if (!num_objects || table_ofs < 8 || table_ofs > object_end) return 0;
    if (num_objects > (object_end - table_ofs) / ofs_size) return 0;
    size_t keylen = strlen(key);

    auto obj_ofs = [&](uint64_t ref, size_t *pos) -> bool {
        if (ref >= num_objects) return false;
        uint64_t ofs = bp_read_be(
            data + table_ofs + ref * ofs_size, ofs_size);
        if (ofs < 8 || ofs >= table_ofs) return false;
        *pos = (size_t)ofs;
        return true;
    };

    uint64_t root_ref = bp_read_be(tr + 16, 8);
    size_t pos;
    if (!obj_ofs(root_ref, &pos) || (data[pos] & 0xf0) != 0xd0) return 0;
    size_t count;
    if (!bp_read_count(data, (size_t)table_ofs, &pos, &count) ||
        count > (table_ofs - pos) / (2 * ref_size))
        return 0;

    for (size_t i = 0; i < count; i++) {
        uint64_t kref = bp_read_be(data + pos + i * ref_size, ref_size);
        size_t kpos;
        if (!obj_ofs(kref, &kpos) || (data[kpos] & 0xf0) != 0x50)
            continue;
        size_t kcount;
        if (!bp_read_count(data, (size_t)table_ofs, &kpos, &kcount) ||
            kcount != keylen || kcount > table_ofs - kpos ||
            memcmp(data + kpos, key, keylen) != 0)
            continue;

        uint64_t vref = bp_read_be(
            data + pos + (count + i) * ref_size, ref_size);
        size_t vpos;
        if (!obj_ofs(vref, &vpos) || (data[vpos] & 0xf0) != 0x50)
            return 0;
        size_t chars;
        if (!bp_read_count(data, (size_t)table_ofs, &vpos, &chars) ||
            chars > table_ofs - vpos || chars >= out_size ||
            memchr(data + vpos, '\0', chars))
            return 0;
        memcpy(out, data + vpos, chars);
        out[chars] = '\0';
        return 1;
    }
    return 0;
}

int ap2_bplist_find_uint(const uint8_t *data, size_t len, const char *key, uint64_t *out)
{
    if (!data || !key || !out || len < 40 ||
        memcmp(data, "bplist00", 8) != 0)
        return 0;

    const uint8_t *tr = data + len - 32;
    uint8_t ofs_size = tr[6], ref_size = tr[7];
    uint64_t num_objects = bp_read_be(tr + 8, 8);
    uint64_t table_ofs = bp_read_be(tr + 24, 8);
    if (!ofs_size || ofs_size > 8 || !ref_size || ref_size > 8) return 0;
    size_t object_end = len - 32;
    if (!num_objects || table_ofs < 8 || table_ofs > object_end) return 0;
    if (num_objects > (object_end - table_ofs) / ofs_size) return 0;
    size_t keylen = strlen(key);

    /* Object offset lookup */
    auto obj_ofs = [&](uint64_t ref, size_t *pos) -> bool {
        if (ref >= num_objects) return false;
        uint64_t ofs = bp_read_be(
            data + table_ofs + ref * ofs_size, ofs_size);
        if (ofs < 8 || ofs >= table_ofs) return false;
        *pos = (size_t)ofs;
        return true;
    };

    for (uint64_t o = 0; o < num_objects; o++) {
        size_t pos;
        if (!obj_ofs(o, &pos) || (data[pos] & 0xF0) != 0xD0) continue;

        size_t count;
        if (!bp_read_count(data, (size_t)table_ofs, &pos, &count)) continue;
        if (count > (table_ofs - pos) / (2 * ref_size)) continue;

        for (size_t i = 0; i < count; i++) {
            uint64_t kref = bp_read_be(data + pos + i * ref_size, ref_size);
            size_t kpos;
            if (!obj_ofs(kref, &kpos) ||
                (data[kpos] & 0xF0) != 0x50)
                continue;   /* ASCII string key */
            size_t kcount;
            if (!bp_read_count(
                    data, (size_t)table_ofs, &kpos, &kcount))
                continue;
            if (kcount != keylen || kcount > table_ofs - kpos) continue;
            if (memcmp(data + kpos, key, keylen) != 0) continue;

            uint64_t vref = bp_read_be(data + pos + (count + i) * ref_size, ref_size);
            size_t vpos;
            if (!obj_ofs(vref, &vpos) ||
                (data[vpos] & 0xF0) != 0x10)
                return 0;   /* not an integer */
            size_t vbytes;
            if (!bp_read_count(
                    data, (size_t)table_ofs, &vpos, &vbytes))
                return 0;
            if (!vbytes || vbytes > 8 || vbytes > table_ofs - vpos)
                return 0;
            *out = bp_read_be(data + vpos, vbytes);
            return 1;
        }
    }
    return 0;
}

} /* extern "C" */
