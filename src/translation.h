#ifndef TRANSLATION_H
#define TRANSLATION_H

#include <glib.h>
#include <stdbool.h>

typedef struct {
    GHashTable *tags;
} TQTranslation;

TQTranslation* translation_init();
void translation_free(TQTranslation *t);
bool translation_load_from_arc(TQTranslation *t, const char *arc_path);
const char* translation_get(TQTranslation *t, const char *tag);

#endif
