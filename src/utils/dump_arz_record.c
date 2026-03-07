#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/arz.h"

#include "src/database_index.h"
#include "src/config.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <record_path>\n"
        "\n"
        "Dump all variables from a single DBR record in database.arz.\n"
        "Requires a game config with game_folder set (uses resource index).\n"
        "\n"
        "NOTE: Prefer tq-dbr-tool which works directly against .arz files\n"
        "without needing the resource index or game config.\n"
        "\n"
        "Examples:\n"
        "  %s records/xpack4/item/relics/x4_relic05.dbr\n",
        prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    config_init(NULL);
    TQResourceIndex *res_index = resource_index_init();
    resource_index_build(res_index, global_config.game_folder);

    const char *matched_key = NULL;
    const char *arz_path = resource_index_lookup(res_index, argv[1], &matched_key);
    if (!arz_path) {
        printf("Record not found: %s\n", argv[1]);
        return 1;
    }

    TQArzFile *arz = arz_load(arz_path);
    if (!arz) {
        printf("Failed to load ARZ: %s\n", arz_path);
        return 1;
    }

    TQArzRecordData *data = arz_read_record(arz, matched_key);
    if (!data) {
        printf("Failed to read record: %s\n", matched_key);
        return 1;
    }

    printf("Record: %s\n", matched_key);
    for (uint32_t i = 0; i < data->num_vars; i++) {
        TQVariable *v = &data->vars[i];
        printf("  %s: ", v->name);
        for (uint32_t j = 0; j < v->count; j++) {
            if (v->type == TQ_VAR_INT) printf("%d", v->value.i32[j]);
            else if (v->type == TQ_VAR_FLOAT) printf("%.4f", v->value.f32[j]);
            else if (v->type == TQ_VAR_STRING) printf("%s", v->value.str[j]);
            if (j < v->count - 1) printf(", ");
        }
        printf("\n");
    }

    arz_record_data_free(data);
    arz_free(arz);
    resource_index_free(res_index);
    config_free();
    return 0;
}
