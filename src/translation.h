#ifndef TRANSLATION_H
#define TRANSLATION_H

#include <glib.h>
#include <stdbool.h>

typedef struct {
  GHashTable *tags;
} TQTranslation;

// translation_init - create and initialize a new translation table
// returns: allocated TQTranslation, or NULL on failure
TQTranslation *translation_init(void);

// translation_free - free a translation table and all its entries
// t: translation table to free
void translation_free(TQTranslation *t);

// translation_load_from_arc - load translation strings from a Text_EN.arc file
// t: translation table to populate
// arc_path: filesystem path to the .arc file
// returns: true on success
bool translation_load_from_arc(TQTranslation *t, const char *arc_path);

// translation_get - look up a translation tag
// t: translation table
// tag: the tag to look up (case-insensitive)
// returns: translated string (internal pointer, do not free), or NULL
const char *translation_get(TQTranslation *t, const char *tag);

#endif
