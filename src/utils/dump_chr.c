#include <stdio.h>
#include <stdlib.h>
#include "src/character.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <chr_file>\n", argv[0]);
        return 1;
    }
    TQCharacter *character = character_load(argv[1]);
    if (!character) {
        printf("Failed to load %s\n", argv[1]);
        return 1;
    }

    printf("Character Name: %s\n", character->character_name);
    for (int i = 0; i < 12; i++) {
        if (character->equipment[i]) {
            printf("Slot %d: %s (Seed: %u)\n", i, character->equipment[i]->base_name, character->equipment[i]->seed);
            if (character->equipment[i]->prefix_name) printf("  Prefix: %s\n", character->equipment[i]->prefix_name);
            if (character->equipment[i]->suffix_name) printf("  Suffix: %s\n", character->equipment[i]->suffix_name);
            if (character->equipment[i]->relic_name) printf("  Relic: %s\n", character->equipment[i]->relic_name);
            if (character->equipment[i]->relic_bonus) printf("  Relic Bonus: %s\n", character->equipment[i]->relic_bonus);
            if (character->equipment[i]->relic_name2) printf("  Relic2: %s\n", character->equipment[i]->relic_name2);
            if (character->equipment[i]->relic_bonus2) printf("  Relic2 Bonus: %s\n", character->equipment[i]->relic_bonus2);
        } else {
            printf("Slot %d: Empty\n", i);
        }
    }

    character_free(character);
    return 0;
}
