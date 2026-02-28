#include "character.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

static uint32_t read_u32(const uint8_t *data, size_t offset) {
    uint32_t val;
    memcpy(&val, data + offset, 4);
    return val;
}

static char* read_string(const uint8_t *data, size_t offset, size_t *next_offset) {
    uint32_t len = read_u32(data, offset);
    if (len == 0 || len > 1024) {
        if (next_offset) *next_offset = offset + 4;
        return NULL;
    }

    char *str = malloc(len + 1);
    memcpy(str, data + offset + 4, len);
    str[len] = '\0';
    if (next_offset) *next_offset = offset + 4 + len;
    return str;
}

static char* read_string_utf16_as_ascii(const uint8_t *data, size_t offset, size_t *next_offset) {
    uint32_t len = read_u32(data, offset);
    if (len == 0 || len > 1024) {
        if (next_offset) *next_offset = offset + 4;
        return NULL;
    }

    char *str = malloc(len + 1);
    for (uint32_t i = 0; i < len; i++) {
        str[i] = (char)data[offset + 4 + i * 2];
    }
    str[len] = '\0';
    if (next_offset) *next_offset = offset + 4 + len * 2;
    return str;
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

/* Write a length-prefixed string: [4-byte len][bytes] */
static void bb_write_str(ByteBuf *b, const char *s) {
    uint32_t len = s ? (uint32_t)strlen(s) : 0;
    bb_write_u32(b, len);
    if (len > 0) bb_write(b, s, len);
}

/* Write a key-value pair where value is a string: [key_str][val_str] */
static void bb_write_key_str(ByteBuf *b, const char *key, const char *val) {
    bb_write_str(b, key);
    bb_write_str(b, val ? val : "");
}

/* Write a key-value pair where value is uint32: [key_str][4-byte val] */
static void bb_write_key_u32(ByteBuf *b, const char *key, uint32_t val) {
    bb_write_str(b, key);
    bb_write_u32(b, val);
}

/* ── Encode inventory blob ─────────────────────────────────────────────── */

static void encode_inventory_blob(TQCharacter *chr, uint8_t **out, size_t *out_size) {
    ByteBuf b;
    bb_init(&b, 4096);

    bb_write_key_u32(&b, "numberOfSacks", (uint32_t)chr->num_inv_sacks);
    bb_write_key_u32(&b, "currentlyFocusedSackNumber", 0);
    bb_write_key_u32(&b, "currentlySelectedSackNumber", 0);

    for (int s = 0; s < chr->num_inv_sacks; s++) {
        TQVaultSack *sack = &chr->inv_sacks[s];

        bb_write_key_u32(&b, "begin_block", 0);
        bb_write_key_u32(&b, "tempBool", 0);

        /* Compute expanded item count (stacked items expand to multiple entries) */
        uint32_t expanded_count = 0;
        for (int i = 0; i < sack->num_items; i++) {
            int ss = sack->items[i].stack_size;
            expanded_count += (uint32_t)(ss > 1 ? ss : 1);
        }
        bb_write_key_u32(&b, "size", expanded_count);

        for (int i = 0; i < sack->num_items; i++) {
            TQVaultItem *item = &sack->items[i];
            int stack = item->stack_size > 1 ? item->stack_size : 1;

            for (int si = 0; si < stack; si++) {
                /* Outer begin_block (Sack type) */
                bb_write_key_u32(&b, "begin_block", 0);
                /* Inner begin_block */
                bb_write_key_u32(&b, "begin_block", 0);

                bb_write_key_str(&b, "baseName",    item->base_name);
                bb_write_key_str(&b, "prefixName",  item->prefix_name);
                bb_write_key_str(&b, "suffixName",  item->suffix_name);
                bb_write_key_str(&b, "relicName",   item->relic_name);
                bb_write_key_str(&b, "relicBonus",  item->relic_bonus);
                bb_write_key_u32(&b, "seed",        si == 0 ? item->seed : (uint32_t)rand());
                bb_write_key_u32(&b, "var1",        item->var1);

                if (chr->has_atlantis) {
                    bb_write_key_str(&b, "relicName2",  item->relic_name2);
                    bb_write_key_str(&b, "relicBonus2", item->relic_bonus2);
                    bb_write_key_u32(&b, "var2",        item->var2);
                }

                /* Inner end_block */
                bb_write_key_u32(&b, "end_block", 0);

                /* First entry uses real position; extras use (-1, -1) */
                if (si == 0) {
                    bb_write_key_u32(&b, "pointX", (uint32_t)item->point_x);
                    bb_write_key_u32(&b, "pointY", (uint32_t)item->point_y);
                } else {
                    bb_write_key_u32(&b, "pointX", (uint32_t)-1);
                    bb_write_key_u32(&b, "pointY", (uint32_t)-1);
                }

                /* Outer end_block */
                bb_write_key_u32(&b, "end_block", 0);
            }
        }

        /* Sack end_block */
        bb_write_key_u32(&b, "end_block", 0);
    }

    *out = b.data;
    *out_size = b.size;
}

/* ── Encode equipment blob ─────────────────────────────────────────────── */

static void encode_equipment_blob(TQCharacter *chr, uint8_t **out, size_t *out_size) {
    ByteBuf b;
    bb_init(&b, 2048);

    bb_write_key_u32(&b, "equipmentCtrlIOStreamVersion", 2);

    for (int slot = 0; slot < 12; slot++) {
        /* Weapon set wrappers: begin at slots 7 and 9 */
        if (slot == 7 || slot == 9) {
            bb_write_key_u32(&b, "begin_block", 0);
            bb_write_key_u32(&b, "alternate", slot == 9 ? 1 : 0);
        }

        TQItem *eq = chr->equipment[slot];

        /* Inner begin_block (Equipment type: single nesting) */
        bb_write_key_u32(&b, "begin_block", 0);

        bb_write_key_str(&b, "baseName",    eq ? eq->base_name    : NULL);
        bb_write_key_str(&b, "prefixName",  eq ? eq->prefix_name  : NULL);
        bb_write_key_str(&b, "suffixName",  eq ? eq->suffix_name  : NULL);
        bb_write_key_str(&b, "relicName",   eq ? eq->relic_name   : NULL);
        bb_write_key_str(&b, "relicBonus",  eq ? eq->relic_bonus  : NULL);
        bb_write_key_u32(&b, "seed",        eq ? eq->seed : 0);
        bb_write_key_u32(&b, "var1",        eq ? eq->var1 : 0);

        if (chr->has_atlantis) {
            bb_write_key_str(&b, "relicName2",  eq ? eq->relic_name2  : NULL);
            bb_write_key_str(&b, "relicBonus2", eq ? eq->relic_bonus2 : NULL);
            bb_write_key_u32(&b, "var2",        eq ? eq->var2 : 0);
        }

        /* Inner end_block */
        bb_write_key_u32(&b, "end_block", 0);

        /* itemAttached: 1 if slot has item AND not secondary weapon set (slots 9, 10) */
        int attached = 0;
        if (eq && eq->base_name && slot != 9 && slot != 10)
            attached = 1;
        bb_write_key_u32(&b, "itemAttached", (uint32_t)attached);

        /* Weapon set wrappers: end at slots 8 and 10 */
        if (slot == 8 || slot == 10) {
            bb_write_key_u32(&b, "end_block", 0);
        }
    }

    /* Final equipment end_block */
    bb_write_key_u32(&b, "end_block", 0);

    *out = b.data;
    *out_size = b.size;
}

TQCharacter* character_load(const char *filepath) {
    if (tqvc_debug) printf("character_load: %s\n", filepath);
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        perror("Error opening character file");
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    TQCharacter *character = calloc(1, sizeof(TQCharacter));
    if (!character) { fclose(file); return NULL; }

    character->filepath  = strdup(filepath);
    character->data_size = (size_t)size;
    character->raw_data  = malloc((size_t)size);
    character->has_atlantis = true;  /* all modern TQAE saves have Atlantis */
    if (!character->raw_data) { character_free(character); fclose(file); return NULL; }

    if (fread(character->raw_data, 1, (size_t)size, file) != (size_t)size) {
        perror("Error reading character file");
        character_free(character);
        fclose(file);
        return NULL;
    }
    fclose(file);

    size_t offset = 0;
    int temp_count = 0;   /* tracks "temp" key occurrences: 1=difficulty, 2-6=base stats */

    /* Equipment parser state */
    int in_equipment  = 0;
    int equipment_slot = 0;
    int equip_end_pending = 0;  /* set when all 12 equipment slots parsed */

    /*
     * Inventory parser state machine
     * ───────────────────────────────
     * Trigger: key "itemPositionsSavedAsGridCoords" → inv_state = 1
     *
     *  0  scanning (not yet in inventory section)
     *  1  expect "numberOfSacks"
     *  2  expect "currentlyFocusedSackNumber"
     *  3  expect "currentlySelectedSackNumber"
     *  4  expect begin_block  (start of a sack)
     *  5  expect "tempBool"
     *  6  expect "size"       (item count for this sack)
     *  7  reading items: begin_block (next item) or end_block (sack done)
     *  8  item outer block open — expect inner begin_block
     *  9  item inner block open — reading baseName/prefix/etc., end_block → 10
     * 10  after inner end_block — reading pointX/pointY then outer end_block → 7
     */
    int inv_state         = 0;
    int inv_num_sacks     = 0;   /* from numberOfSacks */
    int inv_sack_idx      = -1;  /* current sack index (0-based) */
    int inv_items_expected = 0;  /* from "size" key */
    int inv_items_read    = 0;   /* items fully parsed in current sack */
    TQVaultItem *cur_inv_item = NULL;

    while (offset + 4 <= (size_t)size) {
        size_t pre_key_offset = offset;  /* save offset before key length */
        uint32_t len = read_u32(character->raw_data, offset);
        if (len > 0 && len < 256 && offset + 4 + len <= (size_t)size) {
            int printable = 1;
            for (uint32_t i = 0; i < len; i++) {
                if (!isprint(character->raw_data[offset + 4 + i])) {
                    printable = 0; break;
                }
            }

            if (printable) {
                char *key = malloc(len + 1);
                memcpy(key, character->raw_data + offset + 4, len);
                key[len] = '\0';
                offset += 4 + len;

                /* ── Section triggers ──────────────────────────────── */
                if (strcmp(key, "itemPositionsSavedAsGridCoords") == 0) {
                    offset += 4;   /* skip value */
                    inv_state = 1;

                } else if (strcmp(key, "useAlternate") == 0) {
                    offset += 4;
                    character->equip_block_start = offset;
                    in_equipment  = 1;
                    equipment_slot = 0;

                /* ── Inventory section header ───────────────────────── */
                } else if (inv_state == 1 && strcmp(key, "numberOfSacks") == 0) {
                    character->inv_block_start = pre_key_offset;
                    inv_num_sacks = (int)read_u32(character->raw_data, offset);
                    offset += 4;
                    inv_state = 2;

                } else if (inv_state == 2 && strcmp(key, "currentlyFocusedSackNumber") == 0) {
                    offset += 4;
                    inv_state = 3;

                } else if (inv_state == 3 && strcmp(key, "currentlySelectedSackNumber") == 0) {
                    offset += 4;
                    inv_sack_idx = -1;
                    inv_state = 4;

                /* ── begin_block ───────────────────────────────────── */
                } else if (strcmp(key, "begin_block") == 0) {
                    offset += 4;
                    if (inv_state == 4) {
                        /* Start of a sack */
                        inv_sack_idx++;
                        if (inv_sack_idx < 4) {
                            character->inv_sacks[inv_sack_idx].items     = NULL;
                            character->inv_sacks[inv_sack_idx].num_items = 0;
                        }
                        inv_state = 5;
                    } else if (inv_state == 7 && inv_items_read < inv_items_expected) {
                        /* Outer item block */
                        cur_inv_item = calloc(1, sizeof(TQVaultItem));
                        inv_state = 8;
                    } else if (inv_state == 8) {
                        /* Inner item block */
                        inv_state = 9;
                    }
                    /* All other states: value already consumed, nothing else to do */

                /* ── end_block ─────────────────────────────────────── */
                } else if (strcmp(key, "end_block") == 0) {
                    offset += 4;
                    if (inv_state == 9) {
                        /* Inner block closes */
                        inv_state = 10;
                    } else if (inv_state == 10) {
                        /* Outer block closes → finalise item */
                        if (cur_inv_item) {
                            if (cur_inv_item->base_name
                                    && inv_sack_idx >= 0 && inv_sack_idx < 4) {
                                TQVaultSack *sk = &character->inv_sacks[inv_sack_idx];
                                if (cur_inv_item->point_x == -1 && cur_inv_item->point_y == -1
                                        && sk->num_items > 0) {
                                    /* Stackable extra: merge into previous item */
                                    sk->items[sk->num_items - 1].stack_size++;
                                    vault_item_free_strings(cur_inv_item);
                                } else {
                                    cur_inv_item->stack_size = 1;
                                    sk->items = realloc(sk->items,
                                                        (sk->num_items + 1) * sizeof(TQVaultItem));
                                    sk->items[sk->num_items++] = *cur_inv_item;
                                }
                            } else {
                                vault_item_free_strings(cur_inv_item);
                            }
                            free(cur_inv_item);
                            cur_inv_item = NULL;
                        }
                        inv_items_read++;
                        inv_state = 7;
                    } else if (inv_state == 7) {
                        /* Sack ends */
                        character->num_inv_sacks = inv_sack_idx + 1;
                        inv_items_expected = 0;
                        inv_items_read     = 0;
                        if (inv_sack_idx + 1 >= inv_num_sacks) {
                            inv_state = 0;   /* done with all sacks */
                            character->inv_block_end = offset;
                            if (tqvc_debug)
                                printf("  inventory done: %d sacks\n", character->num_inv_sacks);
                        } else {
                            inv_state = 4;   /* read next sack */
                        }
                    } else if (equip_end_pending) {
                        /* Final equipment end_block */
                        character->equip_block_end = offset;
                        equip_end_pending = 0;
                    }

                /* ── Sack header fields ─────────────────────────────── */
                } else if (strcmp(key, "tempBool") == 0) {
                    offset += 4;
                    if (inv_state == 5) inv_state = 6;

                } else if (strcmp(key, "size") == 0) {
                    if (inv_state == 6) {
                        inv_items_expected = (int)read_u32(character->raw_data, offset);
                        inv_items_read     = 0;
                        inv_state = 7;
                    }
                    offset += 4;

                /* ── Item string fields ──────────────────────────────── */
                } else if (strcmp(key, "baseName") == 0) {
                    char *val = read_string(character->raw_data, offset, &offset);
                    if (in_equipment) {
                        if (val && *val && equipment_slot < 12) {
                            free(character->equipment[equipment_slot]);
                            character->equipment[equipment_slot] = calloc(1, sizeof(TQItem));
                            character->equipment[equipment_slot]->base_name = val;
                        } else { free(val); }
                    } else if (inv_state == 9 && cur_inv_item) {
                        if (val && *val) {
                            free(cur_inv_item->base_name);
                            cur_inv_item->base_name = val;
                        } else { free(val); }
                    } else { free(val); }

                } else if (strcmp(key, "prefixName") == 0 ||
                           strcmp(key, "suffixName")  == 0 ||
                           strcmp(key, "relicName")   == 0 ||
                           strcmp(key, "relicBonus")  == 0 ||
                           strcmp(key, "relicName2")  == 0 ||
                           strcmp(key, "relicBonus2") == 0) {
                    char *val = read_string(character->raw_data, offset, &offset);
                    if (in_equipment && equipment_slot < 12 && character->equipment[equipment_slot]) {
                        TQItem *eq = character->equipment[equipment_slot];
                        if      (strcmp(key, "prefixName")  == 0) eq->prefix_name  = val;
                        else if (strcmp(key, "suffixName")  == 0) eq->suffix_name  = val;
                        else if (strcmp(key, "relicName")   == 0) eq->relic_name   = val;
                        else if (strcmp(key, "relicBonus")  == 0) eq->relic_bonus  = val;
                        else if (strcmp(key, "relicName2")  == 0) eq->relic_name2  = val;
                        else if (strcmp(key, "relicBonus2") == 0) eq->relic_bonus2 = val;
                        else free(val);
                    } else if (inv_state == 9 && cur_inv_item) {
                        TQVaultItem *vi = cur_inv_item;
                        if      (strcmp(key, "prefixName")  == 0) { free(vi->prefix_name);  vi->prefix_name  = val; }
                        else if (strcmp(key, "suffixName")  == 0) { free(vi->suffix_name);  vi->suffix_name  = val; }
                        else if (strcmp(key, "relicName")   == 0) { free(vi->relic_name);   vi->relic_name   = val; }
                        else if (strcmp(key, "relicBonus")  == 0) { free(vi->relic_bonus);  vi->relic_bonus  = val; }
                        else if (strcmp(key, "relicName2")  == 0) { free(vi->relic_name2);  vi->relic_name2  = val; }
                        else if (strcmp(key, "relicBonus2") == 0) { free(vi->relic_bonus2); vi->relic_bonus2 = val; }
                        else free(val);
                    } else { free(val); }

                /* ── Item integer fields ────────────────────────────── */
                } else if (strcmp(key, "seed") == 0) {
                    uint32_t v = read_u32(character->raw_data, offset);
                    offset += 4;
                    if (in_equipment && equipment_slot < 12 && character->equipment[equipment_slot])
                        character->equipment[equipment_slot]->seed = v;
                    else if (inv_state == 9 && cur_inv_item)
                        cur_inv_item->seed = v;

                } else if (strcmp(key, "var1") == 0) {
                    uint32_t v = read_u32(character->raw_data, offset);
                    offset += 4;
                    if (in_equipment && equipment_slot < 12 && character->equipment[equipment_slot])
                        character->equipment[equipment_slot]->var1 = v;
                    else if (inv_state == 9 && cur_inv_item)
                        cur_inv_item->var1 = v;

                } else if (strcmp(key, "var2") == 0) {
                    uint32_t v = read_u32(character->raw_data, offset);
                    offset += 4;
                    if (in_equipment && equipment_slot < 12 && character->equipment[equipment_slot])
                        character->equipment[equipment_slot]->var2 = v;
                    else if (inv_state == 9 && cur_inv_item)
                        cur_inv_item->var2 = v;

                /* ── Item position fields ───────────────────────────── */
                } else if (strcmp(key, "pointX") == 0) {
                    if (inv_state == 10 && cur_inv_item)
                        cur_inv_item->point_x = (int)read_u32(character->raw_data, offset);
                    offset += 4;

                } else if (strcmp(key, "pointY") == 0) {
                    if (inv_state == 10 && cur_inv_item)
                        cur_inv_item->point_y = (int)read_u32(character->raw_data, offset);
                    offset += 4;

                /* ── Equipment machinery ────────────────────────────── */
                } else if (strcmp(key, "itemAttached") == 0) {
                    offset += 4;
                    if (in_equipment) {
                        equipment_slot++;
                        if (equipment_slot >= 12) {
                            in_equipment = 0;
                            equip_end_pending = 1;
                        }
                    }

                /* ── Character stats ────────────────────────────────── */
                } else if (strcmp(key, "myPlayerName") == 0) {
                    free(character->character_name);
                    character->character_name = read_string_utf16_as_ascii(character->raw_data, offset, &offset);

                } else if (strcmp(key, "playerCharacterClass") == 0) {
                    free(character->class_name);
                    character->class_name = read_string(character->raw_data, offset, &offset);

                } else if (strcmp(key, "temp") == 0) {
                    temp_count++;
                    float fval;
                    memcpy(&fval, character->raw_data + offset, 4);
                    offset += 4;
                    switch (temp_count) {
                        case 2: character->strength     = fval; break;
                        case 3: character->dexterity    = fval; break;
                        case 4: character->intelligence = fval; break;
                        case 5: character->health       = fval; break;
                        case 6: character->mana         = fval; break;
                        default: break;
                    }

                } else if (strcmp(key, "playerLevel") == 0 ||
                           strcmp(key, "currentStats.charLevel") == 0) {
                    character->level = read_u32(character->raw_data, offset);
                    offset += 4;

                } else if (strcmp(key, "currentStats.experiencePoints") == 0) {
                    character->experience = read_u32(character->raw_data, offset);
                    offset += 4;

                } else if (strcmp(key, "numberOfKills") == 0) {
                    character->kills = read_u32(character->raw_data, offset);
                    offset += 4;

                } else if (strcmp(key, "numberOfDeaths") == 0) {
                    character->deaths = read_u32(character->raw_data, offset);
                    offset += 4;

                } else if (strcmp(key, "skillName") == 0) {
                    char *skill = read_string(character->raw_data, offset, &offset);
                    if (skill && strstr(skill, "Mastery.dbr")) {
                        if (!character->mastery1) character->mastery1 = strdup(skill);
                        else if (!character->mastery2) character->mastery2 = strdup(skill);
                    }
                    free(skill);

                /* ── Default: skip 4-byte value (or string) ─────────── */
                } else {
                    uint32_t val = read_u32(character->raw_data, offset);
                    if (val > 0 && val < 512 && offset + 4 + val <= (size_t)size)
                        offset += 4 + val;
                    else
                        offset += 4;
                }

                free(key);
                continue;
            }
        }
        offset++;
    }

    /* Clean up any dangling in-progress item */
    if (cur_inv_item) {
        vault_item_free_strings(cur_inv_item);
        free(cur_inv_item);
    }

    if (!character->character_name) character->character_name = strdup("Unknown");

    if (tqvc_debug) {
        printf("character_load: finished %s (level %u, inv_sacks=%d)\n",
               character->character_name, character->level, character->num_inv_sacks);
        printf("  inv_block: [%zu..%zu), equip_block: [%zu..%zu)\n",
               character->inv_block_start, character->inv_block_end,
               character->equip_block_start, character->equip_block_end);
        for (int s = 0; s < character->num_inv_sacks; s++)
            printf("  inv_sack[%d]: %d items\n", s, character->inv_sacks[s].num_items);
    }
    return character;
}

void character_free(TQCharacter *character) {
    if (!character) return;
    free(character->filepath);
    free(character->raw_data);
    free(character->character_name);
    free(character->class_name);
    free(character->mastery1);
    free(character->mastery2);
    for (int i = 0; i < 12; i++) {
        if (character->equipment[i]) {
            free(character->equipment[i]->base_name);
            free(character->equipment[i]->prefix_name);
            free(character->equipment[i]->suffix_name);
            free(character->equipment[i]->relic_name);
            free(character->equipment[i]->relic_bonus);
            free(character->equipment[i]->relic_name2);
            free(character->equipment[i]->relic_bonus2);
            free(character->equipment[i]);
        }
    }
    for (int s = 0; s < character->num_inv_sacks; s++) {
        for (int i = 0; i < character->inv_sacks[s].num_items; i++)
            vault_item_free_strings(&character->inv_sacks[s].items[i]);
        free(character->inv_sacks[s].items);
    }
    free(character);
}

int character_save(TQCharacter *character, const char *filepath) {
    if (!character || !character->raw_data) return -1;
    if (character->inv_block_start == 0 || character->inv_block_end == 0 ||
        character->equip_block_start == 0 || character->equip_block_end == 0) {
        fprintf(stderr, "character_save: boundary offsets not set, cannot splice\n");
        return -1;
    }
    if (character->inv_block_start >= character->inv_block_end ||
        character->inv_block_end > character->equip_block_start ||
        character->equip_block_start >= character->equip_block_end ||
        character->equip_block_end > character->data_size) {
        fprintf(stderr, "character_save: invalid boundary offsets\n");
        return -1;
    }

    /* Create backup on first save */
    char bak_path[1024];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", filepath);
    if (access(bak_path, F_OK) != 0) {
        FILE *bak = fopen(bak_path, "wb");
        if (bak) {
            fwrite(character->raw_data, 1, character->data_size, bak);
            fclose(bak);
            if (tqvc_debug)
                printf("character_save: backup created %s\n", bak_path);
        }
    }

    /* Encode new blobs */
    uint8_t *inv_blob = NULL, *equip_blob = NULL;
    size_t inv_size = 0, equip_size = 0;
    encode_inventory_blob(character, &inv_blob, &inv_size);
    encode_equipment_blob(character, &equip_blob, &equip_size);

    /* Splice: prefix + inv_blob + middle + equip_blob + suffix */
    size_t prefix_size = character->inv_block_start;
    size_t middle_size = character->equip_block_start - character->inv_block_end;
    size_t suffix_size = character->data_size - character->equip_block_end;

    size_t new_size = prefix_size + inv_size + middle_size + equip_size + suffix_size;
    uint8_t *new_data = malloc(new_size);
    if (!new_data) {
        free(inv_blob);
        free(equip_blob);
        return -1;
    }

    size_t pos = 0;
    /* Prefix: raw[0..inv_block_start) */
    memcpy(new_data + pos, character->raw_data, prefix_size);
    pos += prefix_size;

    /* New inventory blob */
    memcpy(new_data + pos, inv_blob, inv_size);
    pos += inv_size;

    /* Middle: raw[inv_block_end..equip_block_start) */
    memcpy(new_data + pos, character->raw_data + character->inv_block_end, middle_size);
    pos += middle_size;

    /* New equipment blob */
    memcpy(new_data + pos, equip_blob, equip_size);
    pos += equip_size;

    /* Suffix: raw[equip_block_end..EOF) */
    memcpy(new_data + pos, character->raw_data + character->equip_block_end, suffix_size);
    pos += suffix_size;

    free(inv_blob);
    free(equip_blob);

    /* Write to disk */
    FILE *file = fopen(filepath, "wb");
    if (!file) { free(new_data); return -1; }
    if (fwrite(new_data, 1, new_size, file) != new_size) {
        fclose(file);
        free(new_data);
        return -1;
    }
    fclose(file);

    /* Update raw_data and recalculate boundary offsets for subsequent saves */
    free(character->raw_data);
    character->raw_data = new_data;
    character->data_size = new_size;

    /* Recalculate boundaries from known structure */
    /* inv_block_start stays the same (prefix unchanged) */
    character->inv_block_end = character->inv_block_start + inv_size;
    character->equip_block_start = character->inv_block_end + middle_size;
    character->equip_block_end = character->equip_block_start + equip_size;

    if (tqvc_debug)
        printf("character_save: wrote %zu bytes to %s\n", new_size, filepath);

    return 0;
}
