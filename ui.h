#ifndef UI_H
#define UI_H

#include "types.h"

/* Forward declarations para funções de D-Bus (implementadas em main.c) */
void action_suspend(GtkButton const *button, gpointer user_data);
void action_reboot(GtkButton const *button, gpointer user_data);
void action_poweroff(GtkButton const *button, gpointer user_data);
void action_open_settings(GtkButton const *button, gpointer user_data);

/* Forward declarations para callbacks de janela (implementadas em main.c) */
gboolean on_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_window_destroy(GtkWidget *widget, gpointer data);

/* Construção da UI principal */
void activate(GApplication *application, gpointer user_data);

/* Funções estáticas internas - não exportadas */

/* Drag and Drop */
extern GtkTargetEntry dnd_targets[];

#endif /* UI_H */
