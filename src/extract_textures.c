#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <gtk/gtk.h>
#include "arc.h"
#include "texture.h"

static void make_path(const char *path) {
    char tmp[1024];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    }
    mkdir(tmp, S_IRWXU);
}

static char* normalize_to_forward_slashes(const char *path) {
    char *res = strdup(path);
    for (int i = 0; res[i]; i++) {
        if (res[i] == '\\') res[i] = '/';
    }
    return res;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <arc_file> [output_dir]\n", argv[0]);
        return 1;
    }

    const char *arc_path = argv[1];
    const char *out_base = (argc > 2) ? argv[2] : "extracted_textures";

    // Initialize GLib/GObject (required for GdkPixbuf in some environments)
    // No need for full gtk_init if we only use pixbufs.

    TQArcFile *arc = arc_load(arc_path);
    if (!arc) {
        fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
        return 1;
    }

    mkdir(out_base, S_IRWXU);

    for (uint32_t i = 0; i < arc->num_files; i++) {
        const char *entry_path = arc->entries[i].path;
        if (strstr(entry_path, ".tex") || strstr(entry_path, ".TEX")) {
            printf("Extracting [%u/%u]: %s\n", i + 1, arc->num_files, entry_path);
            
            GdkPixbuf *pb = texture_load_by_index(arc, i);
            if (pb) {
                char *norm_path = normalize_to_forward_slashes(entry_path);
                char out_path[1024];
                snprintf(out_path, sizeof(out_path), "%s/%s", out_base, norm_path);
                
                // Change extension to .png
                char *dot = strrchr(out_path, '.');
                if (dot) strcpy(dot, ".png");

                // Ensure directory exists
                char dir_path[1024];
                strcpy(dir_path, out_path);
                char *last_slash = strrchr(dir_path, '/');
                if (last_slash) {
                    *last_slash = '\0';
                    make_path(dir_path);
                }

                GError *error = NULL;
                if (!gdk_pixbuf_save(pb, out_path, "png", &error, NULL)) {
                    fprintf(stderr, "Failed to save %s: %s\n", out_path, error ? error->message : "Unknown error");
                    if (error) g_error_free(error);
                } else {
                    // printf("Saved to %s\n", out_path);
                }
                
                free(norm_path);
                g_object_unref(pb);
            } else {
                fprintf(stderr, "Failed to load texture at index %u: %s\n", i, entry_path);
            }
        }
    }

    arc_free(arc);
    printf("Extraction complete.\n");
    return 0;
}
