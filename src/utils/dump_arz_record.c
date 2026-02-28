#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/arz.h"
#include "src/database_index.h"
#include "src/config.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <record_path>\n", argv[0]);
        return 1;
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
