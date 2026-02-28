#include <gtk/gtk.h>
#include "ui.h"
#include "config.h"
#include "arc.h"
#include "arz.h"
#include "texture.h"
#include "asset_lookup.h"
#include "affix_table.h"
#include "item_stats.h"
#include "prefetch.h"
#include <MagickWand/MagickWand.h>

static int g_saved_argc;
static char **g_saved_argv;

static void dump_dbr(const char *record_path) {
    TQArzRecordData *data = asset_get_dbr(record_path);
    if (!data) { printf("DBR not found: %s\n", record_path); return; }
    printf("\n--- DBR Dump: %s ---\n", record_path);
    for (uint32_t i = 0; i < data->num_vars; i++) {
        TQVariable *v = &data->vars[i];
        if (!v->name) continue;
        if (v->type == TQ_VAR_STRING && v->count > 0 && v->value.str) {
            printf("  %s =", v->name);
            for (uint32_t j = 0; j < v->count; j++)
                printf(" \"%s\"", v->value.str[j] ? v->value.str[j] : "(null)");
            printf("\n");
        } else if (v->type == TQ_VAR_INT && v->count > 0 && v->value.i32) {
            printf("  %s =", v->name);
            for (uint32_t j = 0; j < v->count; j++) printf(" %d", v->value.i32[j]);
            printf("\n");
        } else if (v->type == TQ_VAR_FLOAT && v->count > 0 && v->value.f32) {
            printf("  %s =", v->name);
            for (uint32_t j = 0; j < v->count; j++) printf(" %.4f", v->value.f32[j]);
            printf("\n");
        } else {
            printf("  %s = (type=%d count=%u)\n", v->name, v->type, v->count);
        }
    }
}

static void debug_run_tests(int argc, char **argv) {
    printf("--- TQVaultC Debug Tests ---\n");
    printf("Game Folder: %s\n", global_config.game_folder ? global_config.game_folder : "NOT SET");
    printf("Save Folder: %s\n", global_config.save_folder ? global_config.save_folder : "NOT SET");

    if (!global_config.game_folder) return;

    const char *test_asset = "records\\items\\geararmor\\torso\\t_plate01.dbr";
    const TQAssetEntry *entry = asset_lookup(test_asset);
    if (entry) {
        printf("SUCCESS: Found %s in %s at offset %u\n", test_asset, asset_get_file_path(entry->file_id), entry->offset);
    } else {
        printf("FAILURE: Could not find %s in index (this is expected if index is dummy)\n", test_asset);
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) continue;
        dump_dbr(argv[i]);
    }

    printf("\n--- Debug Tests Complete ---\n");
}

/* ── GTK activate callback ───────────────────────────────────────── */

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    /* Initialize asset manager (loads index + pre-loads ARZ mmaps) */
    if (global_config.game_folder) {
        if (tqvc_debug) printf("Main: Initializing asset manager...\n");
        asset_manager_init(global_config.game_folder);
        if (tqvc_debug) printf("Main: Asset manager initialized.\n");

        arz_intern_init();
        item_stats_init();
        if (tqvc_debug) printf("Main: String intern + item stats initialized.\n");

        affix_table_init(NULL);
        if (tqvc_debug) printf("Main: Affix table initialized.\n");
    }

    if (tqvc_debug) {
        debug_run_tests(g_saved_argc, g_saved_argv);
    }

    /* Build the main UI, or show first-run setup if no config exists */
    if (config_is_first_run())
        ui_first_run_setup(app);
    else
        ui_app_activate(app, NULL);
}

int main(int argc, char **argv) {
    const char *config_override = NULL;
    bool debug_mode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = true;
        } else {
            config_override = argv[i];
        }
    }

    tqvc_debug = debug_mode;
    config_init(config_override);

    g_saved_argc = argc;
    g_saved_argv = argv;

    /* Strip our custom flags so GTK doesn't see them */
    int gtk_argc = 0;
    char **gtk_argv = malloc(sizeof(char *) * (argc + 1));
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) continue;
        gtk_argv[gtk_argc++] = argv[i];
    }
    gtk_argv[gtk_argc] = NULL;

    if (tqvc_debug) printf("Main: Creating GTK application...\n");
    GtkApplication *app = gtk_application_new("org.tqvaultinc.tqvaultc", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    if (tqvc_debug) printf("Main: Running GTK application...\n");
    int status = g_application_run(G_APPLICATION(app), gtk_argc, gtk_argv);
    free(gtk_argv);
    if (tqvc_debug) printf("Main: GTK application finished with status %d.\n", status);
    prefetch_free();
    item_stats_free();
    affix_table_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    g_object_unref(app);
    return status;
}
