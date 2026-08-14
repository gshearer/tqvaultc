#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include <gtk/gtk.h>

// Callbacks defined in ui.c and wired up by the window builders in ui_build.c.
// Not part of the module's public API -- ui.h is what the rest of the app uses.

void on_context_menu_closed(GtkPopover *popover, gpointer user_data);
void on_search_changed(GtkSearchEntry *entry, gpointer user_data);
void on_search_stop(GtkSearchEntry *entry, gpointer user_data);
gboolean on_search_key(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                       GdkModifierType state, gpointer user_data);
gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                        GdkModifierType state, gpointer user_data);
void on_quit_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
gboolean on_close_request(GtkWindow *window, gpointer user_data);
void on_settings_btn_clicked(GtkButton *btn, gpointer user_data);
void on_overlay_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data);

#endif
