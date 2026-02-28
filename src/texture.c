#include "texture.h"
#include "config.h"
#include "asset_lookup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <MagickWand/MagickWand.h>

static char* normalize_path(const char *path) {
    char *res = strdup(path);
    for (int i = 0; res[i]; i++) {
        if (res[i] == '/') res[i] = '\\';
    }
    return res;
}

static GdkPixbuf* texture_load_from_data(uint8_t *raw_data, size_t raw_size) {
    if (tqvc_debug) printf("texture_load_from_data: size=%zu\n", raw_size);
    if (raw_size < 13) {
        free(raw_data);
        return NULL;
    }

    int header_size = 12;
    if (memcmp(raw_data, "TEX", 3) == 0) {
        if (raw_data[3] == 2) {
            header_size = 13;
        }
    }

    if (raw_size < (size_t)header_size + 4) {
        free(raw_data);
        return NULL;
    }

    // Skip TEX header to get to DDS
    uint8_t *dds_data = raw_data + header_size;
    size_t dds_size = raw_size - header_size;

    /* Fix up the DDS header so ImageMagick correctly interprets alpha.
     * Ported from TQVaultAE BitmapService.cs LoadFromTexMemory(). */
    if (dds_size >= 128) {
        uint32_t magic;
        memcpy(&magic, dds_data, 4);
        /* Accept both "DDS " (0x20534444) and "DDSR" (0x52534444) */
        if (magic == 0x52534444 || magic == 0x20534444) {
            /* Normalise magic to "DDS " */
            dds_data[0] = 'D'; dds_data[1] = 'D'; dds_data[2] = 'S'; dds_data[3] = ' ';

            uint32_t header_sz, pf_sz;
            memcpy(&header_sz, dds_data + 4, 4);
            memcpy(&pf_sz, dds_data + 76, 4);
            if (header_sz == 124 && pf_sz == 32) {
                int32_t bit_depth;
                memcpy(&bit_depth, dds_data + 88, 4);
                if (bit_depth >= 24) {
                    /* Set RGB pixel masks to A8R8G8B8 layout */
                    /* Red mask = 0x00FF0000 */
                    dds_data[92] = 0; dds_data[93] = 0; dds_data[94] = 0xFF; dds_data[95] = 0;
                    /* Green mask = 0x0000FF00 */
                    dds_data[96] = 0; dds_data[97] = 0xFF; dds_data[98] = 0; dds_data[99] = 0;
                    /* Blue mask = 0x000000FF */
                    dds_data[100] = 0xFF; dds_data[101] = 0; dds_data[102] = 0; dds_data[103] = 0;

                    if (bit_depth == 32) {
                        /* Enable DDPF_ALPHAPIXELS flag */
                        dds_data[80] |= 1;
                        /* Alpha mask = 0xFF000000 */
                        dds_data[104] = 0; dds_data[105] = 0; dds_data[106] = 0; dds_data[107] = 0xFF;
                    }
                }
                /* Set DDS caps flag */
                dds_data[109] |= 0x10;
            }
        }
    } else if (dds_size >= 4 && memcmp(dds_data, "DDSR", 4) == 0) {
        /* Short DDS: just fix the magic */
        dds_data[3] = ' ';
    }

    if (tqvc_debug) printf("  Initializing MagickWand...\n");
    MagickWand *wand = NewMagickWand();
    GdkPixbuf *pixbuf = NULL;

    if (tqvc_debug) printf("  Reading image blob (size %zu)...\n", dds_size);
    if (MagickReadImageBlob(wand, dds_data, dds_size) == MagickTrue) {
        if (tqvc_debug) printf("  Image read success. Converting to RGBA...\n");
        MagickSetImageFormat(wand, "RGBA");
        size_t width = MagickGetImageWidth(wand);
        size_t height = MagickGetImageHeight(wand);

        uint8_t *pixels = malloc(width * height * 4);
        MagickExportImagePixels(wand, 0, 0, width, height, "RGBA", CharPixel, pixels);

        pixbuf = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, TRUE, 8,
                                         (int)width, (int)height, (int)width * 4,
                                         (GdkPixbufDestroyNotify)free, NULL);
    } else {
        char *description;
        ExceptionType severity;
        description = MagickGetException(wand, &severity);
        fprintf(stderr, "MagickWand error: %s\n", description);
        MagickRelinquishMemory(description);
    }

    if (tqvc_debug) printf("  Destroying MagickWand...\n");
    DestroyMagickWand(wand);
    free(raw_data);

    return pixbuf;
}

GdkPixbuf* texture_load(const char *tex_path) {
    const TQAssetEntry *entry = asset_lookup(tex_path);
    if (!entry) return NULL;

    TQArcFile *arc = asset_get_arc(entry->file_id);
    if (!arc) return NULL;

    size_t raw_size;
    uint8_t *raw_data = arc_extract_file_at(arc, entry->offset, entry->size, entry->real_size, &raw_size);
    if (!raw_data) return NULL;

    return texture_load_from_data(raw_data, raw_size);
}

GdkPixbuf* texture_load_from_arc(TQArcFile *arc, const char *tex_path) {
    if (!arc) return NULL;

    char *target = normalize_path(tex_path);
    int entry_index = -1;
    
    for (uint32_t i = 0; i < arc->num_files; i++) {
        char *entry_norm = normalize_path(arc->entries[i].path);
        if (strcasecmp(entry_norm, target) == 0) {
            entry_index = (int)i;
            free(entry_norm);
            break;
        }
        free(entry_norm);
    }
    free(target);

    if (entry_index == -1) {
        if (tqvc_debug) printf("Texture not found in ARC: %s\n", tex_path);
        return NULL;
    }

    return texture_load_by_index(arc, (uint32_t)entry_index);
}

GdkPixbuf* texture_load_by_index(TQArcFile *arc, uint32_t index) {
    if (!arc || index >= arc->num_files) return NULL;

    size_t raw_size;
    uint8_t *raw_data = arc_extract_file(arc, index, &raw_size);
    if (!raw_data) return NULL;

    return texture_load_from_data(raw_data, raw_size);
}

GdkPixbuf* texture_create_with_number(GdkPixbuf *base, int number) {
    if (!base) return NULL;

    int width = gdk_pixbuf_get_width(base);
    int height = gdk_pixbuf_get_height(base);
    
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);

    gdk_cairo_set_source_pixbuf(cr, base, 0, 0);
    cairo_paint(cr);

    char text[4];
    snprintf(text, sizeof(text), "%d", number);
    
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 24);
    
    cairo_text_extents_t extents;
    cairo_text_extents(cr, text, &extents);
    double x = (width - extents.width) / 2 - extents.x_bearing;
    double y = (height - extents.height) / 2 - extents.y_bearing;

    // Outer glow/shadow
    cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            if (dx == 0 && dy == 0) continue;
            cairo_move_to(cr, x + dx, y + dy);
            cairo_show_text(cr, text);
        }
    }

    // White text
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);

    cairo_destroy(cr);

    GdkPixbuf *result = gdk_pixbuf_get_from_surface(surface, 0, 0, width, height);
    cairo_surface_destroy(surface);

    return result;
}
