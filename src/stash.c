#include "stash.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern bool tqvc_debug;

/* ── Binary helpers (same pattern as character.c) ──────────────────────── */

static uint32_t read_u32(const uint8_t *data, size_t offset) {
    uint32_t val;
    memcpy(&val, data + offset, 4);
    return val;
}

static float read_f32(const uint8_t *data, size_t offset) {
    float val;
    memcpy(&val, data + offset, 4);
    return val;
}

/* Read a length-prefixed string key. Returns NULL on empty/invalid.
 * Sets *next_offset to position after the string. */
static char *read_string(const uint8_t *data, size_t offset, size_t file_size,
                          size_t *next_offset) {
    if (offset + 4 > file_size) { if (next_offset) *next_offset = file_size; return NULL; }
    uint32_t len = read_u32(data, offset);
    if (len == 0 || offset + 4 + len > file_size) {
        if (next_offset) *next_offset = offset + 4;
        return NULL;
    }
    char *str = malloc(len + 1);
    memcpy(str, data + offset + 4, len);
    str[len] = '\0';
    if (next_offset) *next_offset = offset + 4 + len;
    return str;
}

/* Skip a key string and return its name (caller must free).
 * Also validates that the key matches 'expected' if non-NULL. */
static char *read_key(const uint8_t *data, size_t *offset, size_t file_size) {
    return read_string(data, *offset, file_size, offset);
}

/* Read a key and validate it matches expected. Returns true on match. */
static bool expect_key(const uint8_t *data, size_t *offset, size_t file_size,
                       const char *expected) {
    char *key = read_key(data, offset, file_size);
    if (!key) return false;
    bool ok = (strcmp(key, expected) == 0);
    if (!ok && tqvc_debug)
        printf("stash: expected key '%s', got '%s' at offset %zu\n",
               expected, key, *offset);
    free(key);
    return ok;
}

/* Read uint32 value after a key, advancing offset. */
static uint32_t read_val_u32(const uint8_t *data, size_t *offset, size_t file_size) {
    if (*offset + 4 > file_size) return 0;
    uint32_t val = read_u32(data, *offset);
    *offset += 4;
    return val;
}

/* Read float32 value after a key, advancing offset. */
static float read_val_f32(const uint8_t *data, size_t *offset, size_t file_size) {
    if (*offset + 4 > file_size) return 0.0f;
    float val = read_f32(data, *offset);
    *offset += 4;
    return val;
}

/* Read a string value (length-prefixed). Caller must free. */
static char *read_val_str(const uint8_t *data, size_t *offset, size_t file_size) {
    return read_string(data, *offset, file_size, offset);
}

/* Peek at the next key without consuming it. Returns true if it matches. */
static bool peek_key(const uint8_t *data, size_t offset, size_t file_size,
                     const char *expected) {
    if (offset + 4 > file_size) return false;
    uint32_t len = read_u32(data, offset);
    if (len == 0 || offset + 4 + len > file_size) return false;
    if (len != strlen(expected)) return false;
    return memcmp(data + offset + 4, expected, len) == 0;
}

/* ── ByteBuf: growable byte buffer for encoding ────────────────────────── */

typedef struct { uint8_t *data; size_t size; size_t cap; } ByteBuf;

static void bb_init(ByteBuf *b, size_t cap) {
    b->data = malloc(cap);
    b->size = 0;
    b->cap  = cap;
}

static void bb_ensure(ByteBuf *b, size_t need) {
    if (b->size + need <= b->cap) return;
    while (b->cap < b->size + need) b->cap *= 2;
    b->data = realloc(b->data, b->cap);
}

static void bb_write(ByteBuf *b, const void *src, size_t len) {
    bb_ensure(b, len);
    memcpy(b->data + b->size, src, len);
    b->size += len;
}

static void bb_write_u32(ByteBuf *b, uint32_t val) {
    bb_write(b, &val, 4);
}

static void bb_write_f32(ByteBuf *b, float val) {
    bb_write(b, &val, 4);
}

/* Write a length-prefixed string: [4-byte len][bytes] */
static void bb_write_str(ByteBuf *b, const char *s) {
    uint32_t len = s ? (uint32_t)strlen(s) : 0;
    bb_write_u32(b, len);
    if (len > 0) bb_write(b, s, len);
}

/* Write a key-value pair where value is uint32: [key_str][4-byte val] */
static void bb_write_key_u32(ByteBuf *b, const char *key, uint32_t val) {
    bb_write_str(b, key);
    bb_write_u32(b, val);
}

/* Write a key-value pair where value is float32: [key_str][4-byte val] */
static void bb_write_key_f32(ByteBuf *b, const char *key, float val) {
    bb_write_str(b, key);
    bb_write_f32(b, val);
}

/* Write a key-value pair where value is a string: [key_str][val_str] */
static void bb_write_key_str(ByteBuf *b, const char *key, const char *val) {
    bb_write_str(b, key);
    bb_write_str(b, val ? val : "");
}

/* Write raw bytes with length prefix (for fName which may contain non-ASCII) */
static void bb_write_raw_bytes(ByteBuf *b, const char *data, int len) {
    bb_write_u32(b, (uint32_t)len);
    if (len > 0) bb_write(b, data, (size_t)len);
}

/* ── CRC32 ─────────────────────────────────────────────────────────────── */

static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};

static uint32_t compute_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[data[i] ^ (crc & 0xFF)];
    return crc;
}

/* ── Stash loading ─────────────────────────────────────────────────────── */

TQStash *stash_load(const char *filepath) {
    if (!filepath) return NULL;

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        if (tqvc_debug) printf("stash_load: cannot open %s\n", filepath);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 20) { fclose(f); return NULL; }

    uint8_t *data = malloc((size_t)file_size);
    if (fread(data, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    size_t sz = (size_t)file_size;
    size_t off = 0;

    /* Skip CRC (4 bytes) */
    off = 4;

    /* begin_block */
    if (!expect_key(data, &off, sz, "begin_block")) { free(data); return NULL; }
    uint32_t begin_val = read_val_u32(data, &off, sz);

    /* stashVersion */
    if (!expect_key(data, &off, sz, "stashVersion")) { free(data); return NULL; }
    int version = (int)read_val_u32(data, &off, sz);

    /* fName — raw bytes, not standard CString for the value */
    if (!expect_key(data, &off, sz, "fName")) { free(data); return NULL; }
    uint32_t name_len = read_val_u32(data, &off, sz);
    char *stash_name = NULL;
    int stash_name_len = 0;
    if (name_len > 0 && off + name_len <= sz) {
        stash_name = malloc(name_len + 1);
        memcpy(stash_name, data + off, name_len);
        stash_name[name_len] = '\0';
        stash_name_len = (int)name_len;
        off += name_len;
    }

    /* sackWidth */
    if (!expect_key(data, &off, sz, "sackWidth")) { free(stash_name); free(data); return NULL; }
    int width = (int)read_val_u32(data, &off, sz);

    /* sackHeight */
    if (!expect_key(data, &off, sz, "sackHeight")) { free(stash_name); free(data); return NULL; }
    int height = (int)read_val_u32(data, &off, sz);

    /* numItems */
    if (!expect_key(data, &off, sz, "numItems")) { free(stash_name); free(data); return NULL; }
    int num_items = (int)read_val_u32(data, &off, sz);

    if (tqvc_debug)
        printf("stash_load: %s — version=%d, name=%s, %dx%d, %d items\n",
               filepath, version, stash_name ? stash_name : "(null)",
               width, height, num_items);

    TQStash *stash = calloc(1, sizeof(TQStash));
    stash->filepath = strdup(filepath);
    stash->stash_version = version;
    stash->stash_name = stash_name;
    stash->stash_name_len = stash_name_len;
    stash->sack_width = width;
    stash->sack_height = height;
    stash->begin_block_val = begin_val;

    /* Parse items */
    stash->sack.items = NULL;
    stash->sack.num_items = 0;

    for (int i = 0; i < num_items; i++) {
        /* stackCount */
        if (!expect_key(data, &off, sz, "stackCount")) break;
        int stack_count = (int)read_val_u32(data, &off, sz);

        /* begin_block (item) */
        if (!expect_key(data, &off, sz, "begin_block")) break;
        read_val_u32(data, &off, sz); /* beginBlockCrap2 — not needed */

        /* baseName */
        if (!expect_key(data, &off, sz, "baseName")) break;
        char *base_name = read_val_str(data, &off, sz);

        /* prefixName */
        if (!expect_key(data, &off, sz, "prefixName")) break;
        char *prefix_name = read_val_str(data, &off, sz);

        /* suffixName */
        if (!expect_key(data, &off, sz, "suffixName")) break;
        char *suffix_name = read_val_str(data, &off, sz);

        /* relicName */
        if (!expect_key(data, &off, sz, "relicName")) break;
        char *relic_name = read_val_str(data, &off, sz);

        /* relicBonus */
        if (!expect_key(data, &off, sz, "relicBonus")) break;
        char *relic_bonus = read_val_str(data, &off, sz);

        /* seed */
        if (!expect_key(data, &off, sz, "seed")) break;
        uint32_t seed = read_val_u32(data, &off, sz);

        /* var1 */
        if (!expect_key(data, &off, sz, "var1")) break;
        uint32_t var1 = read_val_u32(data, &off, sz);

        /* Atlantis fields (optional) */
        char *relic_name2 = NULL;
        char *relic_bonus2 = NULL;
        uint32_t var2 = 0;
        if (peek_key(data, off, sz, "relicName2")) {
            expect_key(data, &off, sz, "relicName2");
            relic_name2 = read_val_str(data, &off, sz);

            if (expect_key(data, &off, sz, "relicBonus2"))
                relic_bonus2 = read_val_str(data, &off, sz);

            if (expect_key(data, &off, sz, "var2"))
                var2 = read_val_u32(data, &off, sz);
        }

        /* end_block (item) */
        if (!expect_key(data, &off, sz, "end_block")) {
            free(base_name); free(prefix_name); free(suffix_name);
            free(relic_name); free(relic_bonus);
            free(relic_name2); free(relic_bonus2);
            break;
        }
        read_val_u32(data, &off, sz); /* endBlockCrap2 */

        /* xOffset (float) */
        if (!expect_key(data, &off, sz, "xOffset")) {
            free(base_name); free(prefix_name); free(suffix_name);
            free(relic_name); free(relic_bonus);
            free(relic_name2); free(relic_bonus2);
            break;
        }
        float x_off = read_val_f32(data, &off, sz);

        /* yOffset (float) */
        if (!expect_key(data, &off, sz, "yOffset")) {
            free(base_name); free(prefix_name); free(suffix_name);
            free(relic_name); free(relic_bonus);
            free(relic_name2); free(relic_bonus2);
            break;
        }
        float y_off = read_val_f32(data, &off, sz);

        /* Build item */
        TQVaultItem item = {0};
        item.seed = seed;
        item.base_name = base_name;
        item.prefix_name = prefix_name;
        item.suffix_name = suffix_name;
        item.relic_name = relic_name;
        item.relic_bonus = relic_bonus;
        item.relic_name2 = relic_name2;
        item.relic_bonus2 = relic_bonus2;
        item.var1 = var1;
        item.var2 = var2;
        item.point_x = (int)x_off;
        item.point_y = (int)y_off;
        item.stack_size = stack_count + 1;

        stash->sack.items = realloc(stash->sack.items,
            (size_t)(stash->sack.num_items + 1) * sizeof(TQVaultItem));
        stash->sack.items[stash->sack.num_items] = item;
        stash->sack.num_items++;
    }

    /* Final end_block (stash-level) */
    if (peek_key(data, off, sz, "end_block")) {
        expect_key(data, &off, sz, "end_block");
        read_val_u32(data, &off, sz);
    }

    free(data);

    if (tqvc_debug)
        printf("stash_load: parsed %d items from %s\n",
               stash->sack.num_items, filepath);

    return stash;
}

/* ── Stash saving ──────────────────────────────────────────────────────── */

int stash_save(TQStash *stash) {
    if (!stash || !stash->filepath) return -1;

    ByteBuf b;
    bb_init(&b, 4096);

    /* CRC placeholder (4 bytes of zero) */
    bb_write_u32(&b, 0);

    /* Header */
    bb_write_key_u32(&b, "begin_block", stash->begin_block_val);
    bb_write_key_u32(&b, "stashVersion", (uint32_t)stash->stash_version);

    /* fName — write key as CString, value as raw bytes */
    bb_write_str(&b, "fName");
    if (stash->stash_name && stash->stash_name_len > 0)
        bb_write_raw_bytes(&b, stash->stash_name, stash->stash_name_len);
    else
        bb_write_u32(&b, 0);

    bb_write_key_u32(&b, "sackWidth", (uint32_t)stash->sack_width);
    bb_write_key_u32(&b, "sackHeight", (uint32_t)stash->sack_height);

    /* numItems */
    bb_write_key_u32(&b, "numItems", (uint32_t)stash->sack.num_items);

    /* Items */
    for (int i = 0; i < stash->sack.num_items; i++) {
        TQVaultItem *item = &stash->sack.items[i];

        /* stackCount = stack_size - 1 */
        int sc = item->stack_size > 1 ? item->stack_size - 1 : 0;
        bb_write_key_u32(&b, "stackCount", (uint32_t)sc);

        /* Item block */
        bb_write_key_u32(&b, "begin_block", stash->begin_block_val);

        bb_write_key_str(&b, "baseName",   item->base_name);
        bb_write_key_str(&b, "prefixName", item->prefix_name);
        bb_write_key_str(&b, "suffixName", item->suffix_name);
        bb_write_key_str(&b, "relicName",  item->relic_name);
        bb_write_key_str(&b, "relicBonus", item->relic_bonus);
        bb_write_key_u32(&b, "seed", item->seed);
        bb_write_key_u32(&b, "var1", item->var1);

        /* Atlantis fields — always write for modern TQAE saves */
        bb_write_key_str(&b, "relicName2",  item->relic_name2);
        bb_write_key_str(&b, "relicBonus2", item->relic_bonus2);
        bb_write_key_u32(&b, "var2", item->var2);

        uint32_t end_val = 0;  /* endBlockCrap2 — TQ engine ignores this value */
        bb_write_key_u32(&b, "end_block", end_val);

        /* Position as float */
        bb_write_key_f32(&b, "xOffset", (float)item->point_x);
        bb_write_key_f32(&b, "yOffset", (float)item->point_y);
    }

    /* Final end_block */
    bb_write_key_u32(&b, "end_block", 0);

    /* Compute CRC32 over entire buffer (including the zero placeholder) */
    uint32_t crc = compute_crc32(b.data, b.size);
    memcpy(b.data, &crc, 4);

    /* Write .dxb file */
    FILE *f = fopen(stash->filepath, "wb");
    if (!f) {
        fprintf(stderr, "stash_save: cannot write %s\n", stash->filepath);
        free(b.data);
        return -1;
    }
    fwrite(b.data, 1, b.size, f);
    fclose(f);

    /* Write .dxg backup: change the extension in fName from 'b' to 'g' */
    char dxg_path[1024];
    snprintf(dxg_path, sizeof(dxg_path), "%s", stash->filepath);
    size_t plen = strlen(dxg_path);
    if (plen >= 4 && dxg_path[plen - 1] == 'b')
        dxg_path[plen - 1] = 'g';

    /* Find fName in the buffer and change the last char from 'b' to 'g' */
    uint8_t *dxg_data = malloc(b.size);
    memcpy(dxg_data, b.data, b.size);

    /* The fName string is at a known location. Scan for it. */
    if (stash->stash_name && stash->stash_name_len > 0) {
        /* Find the fName value in the buffer by searching for the name bytes */
        for (size_t si = 0; si + (size_t)stash->stash_name_len <= b.size; si++) {
            if (memcmp(dxg_data + si, stash->stash_name, (size_t)stash->stash_name_len) == 0) {
                /* Check if last char is 'b' or 'B' */
                size_t last = si + (size_t)stash->stash_name_len - 1;
                if (dxg_data[last] == 'b' || dxg_data[last] == 'B')
                    dxg_data[last] = 'g';
                break;
            }
        }
    }

    /* Zero CRC and recompute */
    memset(dxg_data, 0, 4);
    crc = compute_crc32(dxg_data, b.size);
    memcpy(dxg_data, &crc, 4);

    f = fopen(dxg_path, "wb");
    if (f) {
        fwrite(dxg_data, 1, b.size, f);
        fclose(f);
    }

    free(dxg_data);
    free(b.data);

    stash->dirty = false;
    if (tqvc_debug)
        printf("stash_save: wrote %s (%zu bytes, %d items)\n",
               stash->filepath, b.size, stash->sack.num_items);

    return 0;
}

/* ── Stash cleanup ─────────────────────────────────────────────────────── */

void stash_free(TQStash *stash) {
    if (!stash) return;
    free(stash->filepath);
    free(stash->stash_name);
    for (int i = 0; i < stash->sack.num_items; i++)
        vault_item_free_strings(&stash->sack.items[i]);
    free(stash->sack.items);
    free(stash);
}

/* ── Path building ─────────────────────────────────────────────────────── */

char *stash_build_path(StashType type, const char *char_folder_name) {
    if (!global_config.save_folder) return NULL;
    char path[1024];
    switch (type) {
    case STASH_TRANSFER:
        snprintf(path, sizeof(path), "%s/SaveData/Sys/winsys.dxb",
                 global_config.save_folder);
        break;
    case STASH_PLAYER:
        if (!char_folder_name) return NULL;
        snprintf(path, sizeof(path), "%s/SaveData/Main/%s/winsys.dxb",
                 global_config.save_folder, char_folder_name);
        break;
    case STASH_RELIC_VAULT:
        snprintf(path, sizeof(path), "%s/SaveData/Sys/miscsys.dxb",
                 global_config.save_folder);
        break;
    default:
        return NULL;
    }
    return strdup(path);
}
