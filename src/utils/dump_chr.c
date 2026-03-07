#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/character.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <chr_file>\n"
        "\n"
        "Load a .chr character save file and display the character name\n"
        "and equipment in all 12 slots (base, prefix, suffix, relics).\n"
        "Uses character_load() from the main codebase.\n"
        "\n"
        "NOTE: For more detailed character inspection, use tq-chr-tool\n"
        "which has dump, inv, equip, compare, validate, hex, and roundtrip\n"
        "commands.\n"
        "\n"
        "Examples:\n"
        "  %s testdata/saves/_soothie/Player.chr\n"
        "  %s testdata/saves/_kayana/Player.chr\n",
        prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return argc < 2 ? 1 : 0;
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
