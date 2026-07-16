/*
 * C wrapper for bplist (binary plist) reader/writer
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

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

} /* extern "C" */
