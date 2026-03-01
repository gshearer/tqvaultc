/* Quick test: load Player_working.chr, save it, compare output */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Minimal stubs - we only need character.c and vault_item_free_strings */
#include "src/character.h"

/* Stub for vault_item_free_strings */
void vault_item_free_strings(TQVaultItem *item) {
    if (!item) return;
    free(item->base_name);
    free(item->prefix_name);
    free(item->suffix_name);
    free(item->relic_name);
    free(item->relic_bonus);
    free(item->relic_name2);
    free(item->relic_bonus2);
}

int tqvc_debug = 1;

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "Player_working.chr";
    TQCharacter *chr = character_load(path);
    if (!chr) { fprintf(stderr, "Failed to load\n"); return 1; }
    
    printf("\n=== Character loaded ===\n");
    printf("Name: %s, Level: %u, Sacks: %d\n", chr->character_name, chr->level, chr->num_inv_sacks);
    printf("inv_block: [%zu..%zu) = %zu bytes\n",
           chr->inv_block_start, chr->inv_block_end, chr->inv_block_end - chr->inv_block_start);
    printf("equip_block: [%zu..%zu) = %zu bytes\n",
           chr->equip_block_start, chr->equip_block_end, chr->equip_block_end - chr->equip_block_start);
    printf("middle: [%zu..%zu) = %zu bytes\n",
           chr->inv_block_end, chr->equip_block_start, chr->equip_block_start - chr->inv_block_end);
    
    for (int s = 0; s < chr->num_inv_sacks; s++) {
        int expanded = 0;
        for (int i = 0; i < chr->inv_sacks[s].num_items; i++) {
            int ss = chr->inv_sacks[s].items[i].stack_size;
            expanded += ss > 1 ? ss : 1;
        }
        printf("sack[%d]: %d merged items, %d expanded\n", s, chr->inv_sacks[s].num_items, expanded);
    }
    
    /* Save */
    int ret = character_save(chr, "/tmp/test_chr_resave.chr");
    printf("\nSave returned: %d\n", ret);
    
    if (ret == 0) {
        /* Read the saved file and dump structural keys */
        FILE *f = fopen("/tmp/test_chr_resave.chr", "rb");
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *data = malloc(sz);
        fread(data, 1, sz, f);
        fclose(f);
        
        printf("\nOriginal file: %zu bytes\n", chr->data_size);
        printf("Saved file: %ld bytes\n", sz);
        printf("Difference: %ld bytes\n", sz - (long)chr->data_size);
        
        /* Scan for structural keys in saved file's inventory region */
        printf("\n=== Structural keys in saved inventory section ===\n");
        size_t off = 0;
        int found_inv = 0;
        while (off + 4 <= (size_t)sz) {
            uint32_t klen;
            memcpy(&klen, data + off, 4);
            if (klen > 0 && klen < 256 && off + 4 + klen <= (size_t)sz) {
                int printable = 1;
                for (uint32_t i = 0; i < klen; i++) {
                    if (data[off+4+i] < 32 || data[off+4+i] > 126) { printable = 0; break; }
                }
                if (printable) {
                    char key[256];
                    memcpy(key, data + off + 4, klen);
                    key[klen] = '\0';
                    size_t voff = off + 4 + klen;
                    uint32_t val;
                    memcpy(&val, data + voff, 4);
                    
                    if (strcmp(key, "itemPositionsSavedAsGridCoords") == 0) found_inv = 1;
                    
                    if (found_inv && (
                        strcmp(key, "begin_block") == 0 || strcmp(key, "end_block") == 0 ||
                        strcmp(key, "numberOfSacks") == 0 || strcmp(key, "tempBool") == 0 ||
                        strcmp(key, "size") == 0 || strcmp(key, "currentlyFocusedSackNumber") == 0 ||
                        strcmp(key, "currentlySelectedSackNumber") == 0 ||
                        strcmp(key, "itemPositionsSavedAsGridCoords") == 0)) {
                        printf("  @%zu: %s = 0x%08X (%d)\n", off, key, val, (int)val);
                    }
                    
                    if (strcmp(key, "useAlternate") == 0) {
                        printf("  @%zu: %s = 0x%08X\n", off, key, val);
                        break;
                    }
                    
                    off = voff;
                    /* String-valued keys */
                    if (strcmp(key, "baseName") == 0 || strcmp(key, "prefixName") == 0 ||
                        strcmp(key, "suffixName") == 0 || strcmp(key, "relicName") == 0 ||
                        strcmp(key, "relicBonus") == 0 || strcmp(key, "relicName2") == 0 ||
                        strcmp(key, "relicBonus2") == 0 || strcmp(key, "myPlayerName") == 0 ||
                        strcmp(key, "playerCharacterClass") == 0 || strcmp(key, "skillName") == 0) {
                        if (val > 0 && val < 1024 && off + 4 + val <= (size_t)sz)
                            off += 4 + val;
                        else
                            off += 4;
                    } else {
                        off += 4;
                    }
                    continue;
                }
            }
            off++;
        }
        free(data);
    }
    
    character_free(chr);
    return 0;
}
