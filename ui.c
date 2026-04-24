#include "ui.h"
#include "core.h"
#include "pinned.h"
#include <gio/gdesktopappinfo.h>
#include <sys/stat.h>

/* Forward declarations for static functions */
static void on_copy_ready(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void switch_view(GtkButton *button, gpointer user_data);
static void execute_app_detached(GAppInfo *appinfo);
static void on_app_clicked(GtkWidget *widget, gpointer user_data);
static void on_iconview_item_activated(GtkIconView *icon_view, GtkTreePath *path, gpointer user_data);
static void on_treeview_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data);
static void on_listbox_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void on_add_to_desktop(GtkMenuItem *item, gpointer user_data);
static void on_add_to_panel(GtkMenuItem *item, gpointer user_data);
static void on_pin_app(GtkMenuItem *item, gpointer user_data);
static void on_unpin_app(GtkMenuItem *item, gpointer user_data);
static void on_properties(GtkMenuItem *item, gpointer user_data);
static void on_menu_item_destroyed(gpointer data, GClosure *closure);
static void show_context_menu(GdkEventButton *event, GAppInfo *appinfo, GtkListStore *pinned_store);
static gboolean on_iconview_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean on_listbox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean on_treeview_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static void on_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data);
static void on_drag_data_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection_data, guint info, guint time, gpointer user_data);
static void on_choose_image_clicked(GtkButton *button, gpointer user_data);
static void on_user_profile_clicked(GtkButton *button, gpointer user_data);
static void on_app_icon_button_clicked(GtkButton *button, gpointer user_data);
static void on_open_path_folder_clicked(GtkButton *button, gpointer user_data);
static void show_app_properties_dialog(GAppInfo *app, GtkWindow *parent);

/* Async image loading support */
typedef struct {
    gchar *filename;
    gint width;
    gint height;
    GtkWidget *image_widget;
} AsyncImageData;

static void load_pixbuf_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void on_pixbuf_loaded(GObject *source_object, GAsyncResult *res, gpointer user_data);
static gboolean update_search_detail_icon_debounced(gpointer user_data);

/* Drag and Drop targets */
GtkTargetEntry dnd_targets[] = {
    { "text/uri-list", 0, 0 }
};

/* Função centralizada para garantir o isolamento do processo e acionamento do Polkit */
static void execute_app_detached(GAppInfo *appinfo) {
    track_recent_app(appinfo);

    if (G_IS_DESKTOP_APP_INFO(appinfo)) {
        /* 1. Tentativa Principal: gio launch (Cria uma sessão limpa para o Polkit) */
        const gchar *desktop_path = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(appinfo));
        if (desktop_path != NULL) {
            gchar *command = g_strdup_printf("gio launch \"%s\"", desktop_path);
            g_spawn_command_line_async(command, NULL);
            g_free(command);
            return; /* Sai da função com sucesso */
        }
        
        /* 2. Fallback: gtk-launch usando o ID do desktop */
        const gchar *app_id = g_app_info_get_id(appinfo);
        if (app_id != NULL) {
            gchar *cmd_fallback = g_strdup_printf("gtk-launch \"%s\"", app_id);
            g_spawn_command_line_async(cmd_fallback, NULL);
            g_free(cmd_fallback);
            return;
        }
    }
    
    /* 3. Último Recurso: API nativa do GTK (para binários sem arquivo .desktop) */
    GAppLaunchContext *launch_context = G_APP_LAUNCH_CONTEXT(launch_context_singleton);
    g_app_info_launch(appinfo, NULL, launch_context, NULL);
}

/* Callbacks para o GtkStack */
static void switch_view(GtkButton *button, gpointer user_data) {
    GtkStack *stack = GTK_STACK(g_object_get_qdata(G_OBJECT(button), quark_target_stack));
    const gchar *view_name = g_object_get_qdata(G_OBJECT(button), quark_target_view);
    gtk_stack_set_visible_child_name(stack, view_name);
    
    /* Se mudou para view_all, selecionar o primeiro item */
    if (g_strcmp0(view_name, "view_all") == 0 && app_treeview) {
        GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(app_treeview));
        if (model) {
            GtkTreeIter first_iter;
            if (gtk_tree_model_get_iter_first(model, &first_iter)) {
                GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app_treeview));
                gtk_tree_selection_select_iter(selection, &first_iter);
            }
        }
    }
}

static void on_app_clicked(GtkWidget *widget, gpointer user_data) {
    GAppInfo *appinfo = G_APP_INFO(user_data);
    execute_app_detached(appinfo);
}

/* Extração Segura em Nível C sem Máquina Virtual de Introspecção GValue */
static void on_iconview_item_activated(GtkIconView *icon_view, GtkTreePath *path, gpointer user_data) {
    GtkTreeModel *model = gtk_icon_view_get_model(icon_view);
    GtkTreeIter iter;

    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gpointer appinfo_ptr = NULL;
        gtk_tree_model_get(model, &iter, COL_APPINFO, &appinfo_ptr, -1);

        if (G_LIKELY(appinfo_ptr)) {
            on_app_clicked(NULL, (GAppInfo*)appinfo_ptr);
            g_object_unref(appinfo_ptr);
        }
    }
}

static void on_treeview_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data) {
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;

    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gpointer appinfo_ptr = NULL;
        gtk_tree_model_get(model, &iter, COL_APPINFO, &appinfo_ptr, -1);

        if (G_LIKELY(appinfo_ptr)) {
            on_app_clicked(NULL, (GAppInfo*)appinfo_ptr);
            g_object_unref(appinfo_ptr);
        }
    }
}

static void on_listbox_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    if (g_object_get_qdata(G_OBJECT(row), quark_is_header)) return;
    
    GtkListBoxRow *row_ptr = GTK_LIST_BOX_ROW(row);
    
    gpointer raw_ptr = g_object_get_qdata(G_OBJECT(row_ptr), quark_app_info);
    if (G_LIKELY(raw_ptr)) {
        GAppInfo *appinfo = (GAppInfo*)raw_ptr;
        on_app_clicked(GTK_WIDGET(row_ptr), appinfo);
    }
}

/* Ações do menu de contexto */
static void on_copy_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    gchar *dest_path = (gchar *)user_data;
    
    if (G_LIKELY(g_file_copy_finish(G_FILE(source_object), res, &error))) {
        chmod(dest_path, 0755);
    } else {
        g_printerr("Falha de I/O na cópia: %s\n", error->message);
        g_error_free(error);
    }
    g_free(dest_path);
}

static void on_add_to_desktop(GtkMenuItem *item, gpointer user_data) {
    GAppInfo *appinfo = G_APP_INFO(user_data);
    if (!G_IS_DESKTOP_APP_INFO(appinfo)) return;

    const gchar *src_path = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(appinfo));
    const gchar *desktop_dir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    
    if (!src_path || !desktop_dir) return;

    const gchar *slash = strrchr(src_path, '/');
    const gchar *basename = slash ? (slash + 1) : src_path;
    
    gchar *dest_path = g_build_filename(desktop_dir, basename, NULL);

    GFile *src = g_file_new_for_path(src_path);
    GFile *dest = g_file_new_for_path(dest_path);

    if (!ctx.copy_cancellable) {
        ctx.copy_cancellable = g_cancellable_new();
    } else {
        g_cancellable_cancel(ctx.copy_cancellable);
        g_cancellable_reset(ctx.copy_cancellable);
    }

    g_file_copy_async(src, dest, G_FILE_COPY_OVERWRITE, G_PRIORITY_DEFAULT, ctx.copy_cancellable, NULL, NULL, on_copy_ready, g_strdup(dest_path));

    g_object_unref(src);
    g_object_unref(dest);
    g_free(dest_path);
}

static void on_add_to_panel(GtkMenuItem *item, gpointer user_data) {
    GAppInfo *appinfo = G_APP_INFO(user_data);
    if (!G_IS_DESKTOP_APP_INFO(appinfo)) return;

    const gchar *src_path = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(appinfo));
    if (!src_path) return;

    gchar *quoted_path = g_shell_quote(src_path);
    gchar *cmd = g_strdup_printf("xfce4-panel --add=launcher %s", quoted_path);
    
    g_spawn_command_line_async(cmd, NULL);
    
    g_free(cmd);
    g_free(quoted_path);
}

static void on_pin_app(GtkMenuItem *item, gpointer user_data) {
    AppContextData *data = (AppContextData *)user_data;
    pin_app(data->appinfo, data->pinned_store);
}

static void on_unpin_app(GtkMenuItem *item, gpointer user_data) {
    AppContextData *data = (AppContextData *)user_data;
    unpin_app(data->appinfo, data->pinned_store);
}

static void on_properties(GtkMenuItem *item, gpointer user_data) {
    GAppInfo *appinfo = G_APP_INFO(user_data);
    show_app_properties_dialog(appinfo, NULL);
}

/* Destruidor Transparente associado ao ciclo de vida de itens de menu */
static void on_menu_item_destroyed(gpointer data, GClosure *closure) {
    AppContextData *ctx_data = (AppContextData *)data;
    if (ctx_data) {
        g_clear_object(&ctx_data->appinfo);
        g_clear_pointer(&ctx_data, g_free);
    }
}

/* Async image loading thread function */
static void load_pixbuf_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    AsyncImageData *data = (AsyncImageData *)task_data;
    GError *error = NULL;
    
    if (!data || !data->filename) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid image data");
        return;
    }
    
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(data->filename, data->width, data->height, TRUE, &error);
    
    if (error) {
        g_task_return_error(task, error);
    } else {
        g_task_return_pointer(task, pixbuf, g_object_unref);
    }
}

/* Callback when pixbuf is loaded - runs on main thread */
static void on_pixbuf_loaded(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    AsyncImageData *data = (AsyncImageData *)user_data;
    GError *error = NULL;
    
    if (!data) {
        g_warning("AsyncImageData is NULL in on_pixbuf_loaded");
        return;
    }
    
    GdkPixbuf *pixbuf = g_task_propagate_pointer(G_TASK(res), &error);
    
    if (pixbuf) {
        if (GTK_IS_IMAGE(data->image_widget) && gtk_widget_get_realized(data->image_widget)) {
            gtk_image_set_from_pixbuf(GTK_IMAGE(data->image_widget), pixbuf);
            gtk_image_set_pixel_size(GTK_IMAGE(data->image_widget), data->width);
        }
        g_object_unref(pixbuf);
    } else if (error) {
        g_warning("Falha ao carregar imagem assíncrona: %s", error->message);
        g_error_free(error);
    }
    
    g_free(data->filename);
    g_free(data);
}

/* Callback para seleção na pesquisa - atualiza painel lateral com debouncing */
static guint search_detail_timeout_id = 0;

/* Debounced callback for search detail icon updates */
static gboolean update_search_detail_icon_debounced(gpointer user_data) {
    GtkTreeSelection *selection = GTK_TREE_SELECTION(user_data);
    GtkTreeIter iter;
    GtkTreeModel *model;

    search_detail_timeout_id = 0;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        GAppInfo *appinfo;
        gtk_tree_model_get(model, &iter, COL_APPINFO, &appinfo, -1);

        if (appinfo) {
            gtk_label_set_text(GTK_LABEL(global_search_ctx.detail_name), g_app_info_get_name(appinfo));
            
            GIcon *icon = g_app_info_get_icon(appinfo);
            if (icon) {
                gtk_image_set_from_gicon(GTK_IMAGE(global_search_ctx.detail_icon), icon, GTK_ICON_SIZE_DIALOG);
            }

            gtk_widget_show_all(global_search_ctx.detail_box);
            g_object_unref(appinfo);
        }
    } else {
        gtk_widget_hide(global_search_ctx.detail_box);
    }
    
    return G_SOURCE_REMOVE;
}

/* Callback para seleção na pesquisa - atualiza painel lateral com debouncing */
static void on_search_selection_changed(GtkTreeSelection *selection, gpointer user_data) {
    if (search_detail_timeout_id > 0) {
        g_source_remove(search_detail_timeout_id);
        search_detail_timeout_id = 0;
    }
    search_detail_timeout_id = g_timeout_add_full(G_PRIORITY_DEFAULT, 100, update_search_detail_icon_debounced, selection, NULL);
}

/* Callback para botão Abrir no painel lateral */
static void on_detail_open_clicked(GtkButton *button, gpointer user_data) {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(global_search_ctx.treeview));
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        GAppInfo *appinfo;
        gtk_tree_model_get(model, &iter, COL_APPINFO, &appinfo, -1);

        if (appinfo) {
            on_app_clicked(NULL, appinfo);
            g_object_unref(appinfo);
        }
    }
}

/* Construtor do menu de contexto */
static void show_context_menu(GdkEventButton *event, GAppInfo *appinfo, GtkListStore *pinned_store) {
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *item_desktop = gtk_menu_item_new_with_label("Adicionar à área de Trabalho");
    g_signal_connect_data(item_desktop, "activate", G_CALLBACK(on_add_to_desktop), 
                          g_object_ref(appinfo), (GClosureNotify)g_object_unref, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_desktop);

    GtkWidget *item_panel = gtk_menu_item_new_with_label("Adicionar ao painel (XFCE)");
    g_signal_connect_data(item_panel, "activate", G_CALLBACK(on_add_to_panel), 
                          g_object_ref(appinfo), (GClosureNotify)g_object_unref, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_panel);

    GtkWidget *separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);

    gboolean pinned = is_app_pinned(appinfo);
    const gchar *pin_label = pinned ? "Desfixar" : "Fixar";
    GtkWidget *item_pin = gtk_menu_item_new_with_label(pin_label);
    
    AppContextData *pin_data = g_new(AppContextData, 1);
    pin_data->appinfo = g_object_ref(appinfo);
    pin_data->pinned_store = pinned_store;
    
    if (pinned) {
        g_signal_connect_data(item_pin, "activate", G_CALLBACK(on_unpin_app), 
                              pin_data, on_menu_item_destroyed, 0);
    } else {
        g_signal_connect_data(item_pin, "activate", G_CALLBACK(on_pin_app), 
                              pin_data, on_menu_item_destroyed, 0);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_pin);

    GtkWidget *separator2 = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator2);

    GtkWidget *item_properties = gtk_menu_item_new_with_label("Propriedades");
    g_signal_connect_data(item_properties, "activate", G_CALLBACK(on_properties),
                          g_object_ref(appinfo), (GClosureNotify)g_object_unref, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_properties);

    gtk_widget_show_all(menu);
    
    g_signal_connect_after(menu, "destroy", G_CALLBACK(gtk_widget_destroyed), &menu);
    
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)event);
}

static gboolean on_iconview_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    if (event->button == 3 && event->type == GDK_BUTTON_PRESS) {
        GtkIconView *icon_view = GTK_ICON_VIEW(widget);
        GtkTreePath *path = gtk_icon_view_get_path_at_pos(icon_view, event->x, event->y);
        
        if (path) {
            gtk_icon_view_select_path(icon_view, path);
            
            GtkTreeModel *model = gtk_icon_view_get_model(icon_view);
            GtkTreeModel *child_model = model;
            GtkTreePath *child_path = path;
            
            if (GTK_IS_TREE_MODEL_FILTER(model)) {
                child_path = gtk_tree_model_filter_convert_path_to_child_path(
                    GTK_TREE_MODEL_FILTER(model), path);
                child_model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
            }
            
            if (child_path) {
                GtkTreeIter iter;
                if (gtk_tree_model_get_iter(child_model, &iter, child_path)) {
                    GAppInfo *appinfo = NULL;
                    gtk_tree_model_get(child_model, &iter, COL_APPINFO, &appinfo, -1);
                    if (appinfo) {
                        GtkListStore *pinned_store = GTK_LIST_STORE(g_object_get_qdata(G_OBJECT(widget), quark_pinned_store));
                        show_context_menu(event, appinfo, pinned_store);
                        g_object_unref(appinfo);
                    }
                }
                if (child_path != path) {
                    gtk_tree_path_free(child_path);
                }
            }
            gtk_tree_path_free(path);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean on_listbox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    if (event->button == 3 && event->type == GDK_BUTTON_PRESS) {
        GtkListBox *list_box = GTK_LIST_BOX(widget);
        GtkListBoxRow *row = gtk_list_box_get_row_at_y(list_box, event->y);

        if (row && !g_object_get_qdata(G_OBJECT(row), quark_is_header)) {
            gtk_list_box_select_row(list_box, row);

            GAppInfo *appinfo = G_APP_INFO(g_object_get_qdata(G_OBJECT(row), quark_app_info));
            if (appinfo) {
                GtkListStore *pinned_store = GTK_LIST_STORE(g_object_get_qdata(G_OBJECT(widget), quark_pinned_store));
                show_context_menu(event, appinfo, pinned_store);
                return TRUE;
            }
        }
    }
    return FALSE;
}

static gboolean on_treeview_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    if (event->button == 3 && event->type == GDK_BUTTON_PRESS) {
        GtkTreeView *tree_view = GTK_TREE_VIEW(widget);
        GtkTreePath *path = NULL;

        if (gtk_tree_view_get_path_at_pos(tree_view, (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL)) {
            GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
            GtkTreeIter iter;

            if (gtk_tree_model_get_iter(model, &iter, path)) {
                gpointer appinfo_ptr = NULL;
                gtk_tree_model_get(model, &iter, COL_APPINFO, &appinfo_ptr, -1);

                if (G_LIKELY(appinfo_ptr)) {
                    GtkListStore *pinned_store = GTK_LIST_STORE(g_object_get_qdata(G_OBJECT(widget), quark_pinned_store));
                    
                    GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);
                    gtk_tree_selection_select_iter(selection, &iter);
                    
                    show_context_menu(event, G_APP_INFO(appinfo_ptr), pinned_store);
                    g_object_unref(appinfo_ptr);
                    gtk_tree_path_free(path);
                    return TRUE;
                }
            }
            gtk_tree_path_free(path);
        }
    }
    return FALSE;
}

static void on_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data) {
    GAppInfo *appinfo = NULL;

    GtkTreeView *tree_view = GTK_TREE_VIEW(widget);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gpointer appinfo_ptr = NULL;
        gtk_tree_model_get(model, &iter, COL_APPINFO, &appinfo_ptr, -1);
        if (appinfo_ptr) {
            appinfo = G_APP_INFO(appinfo_ptr);
        }
    }
    
    if (appinfo) {
        GIcon *icon = g_app_info_get_icon(appinfo);
        if (icon) {
            gtk_drag_source_set_icon_gicon(widget, icon);
            g_object_unref(icon);
        } else {
            gtk_drag_source_set_icon_name(widget, "application-x-executable");
        }
    } else {
        gtk_drag_source_set_icon_name(widget, "application-x-executable");
    }
}

static void on_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                     GtkSelectionData *selection_data, guint info,
                     guint time, gpointer user_data) {
    GAppInfo *appinfo = NULL;

    if (GTK_IS_ICON_VIEW(widget)) {
        GList *paths = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(widget));
        if (paths) {
            GtkTreeModel *model = gtk_icon_view_get_model(GTK_ICON_VIEW(widget));
            GtkTreeIter iter;
            gtk_tree_model_get_iter(model, &iter, (GtkTreePath*)paths->data);
            gtk_tree_model_get(model, &iter, COL_APPINFO, &appinfo, -1);
            g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
        }
    } else if (GTK_IS_TREE_VIEW(widget)) {
        GtkTreeView *tree_view = GTK_TREE_VIEW(widget);
        GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);
        GtkTreeModel *model;
        GtkTreeIter iter;

        if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
            gtk_tree_model_get(model, &iter, COL_APPINFO, &appinfo, -1);
            if (appinfo) g_object_ref(appinfo);
        }
    } else if (GTK_IS_LIST_BOX(widget)) {
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(widget));
        if (row && !g_object_get_qdata(G_OBJECT(row), quark_is_header)) {
            appinfo = G_APP_INFO(g_object_get_qdata(G_OBJECT(row), quark_app_info));
            if (appinfo) g_object_ref(appinfo);
        }
    } else if (GTK_IS_LIST_BOX_ROW(widget)) {
        GtkListBoxRow *row = GTK_LIST_BOX_ROW(widget);
        if (!g_object_get_qdata(G_OBJECT(row), quark_is_header)) {
            appinfo = G_APP_INFO(g_object_get_qdata(G_OBJECT(row), quark_app_info));
            if (appinfo) g_object_ref(appinfo);
        }
    } else {
        GtkWidget *ancestor = gtk_widget_get_ancestor(widget, GTK_TYPE_LIST_BOX_ROW);
        if (ancestor && GTK_IS_LIST_BOX_ROW(ancestor)) {
            GtkListBoxRow *row = GTK_LIST_BOX_ROW(ancestor);
            if (!g_object_get_qdata(G_OBJECT(row), quark_is_header)) {
                appinfo = G_APP_INFO(g_object_get_qdata(G_OBJECT(row), quark_app_info));
                if (appinfo) g_object_ref(appinfo);
            }
        }
    }

    if (appinfo && G_IS_DESKTOP_APP_INFO(appinfo)) {
        const gchar *path = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(appinfo));
        if (path) {
            gchar *uri = g_filename_to_uri(path, NULL, NULL);
            gchar *uris[] = { uri, NULL };
            gtk_selection_data_set_uris(selection_data, uris);
            g_free(uri);
        }
        g_object_unref(appinfo);
    }
}

/* Diálogos */
static void on_choose_image_clicked(GtkButton *button, gpointer user_data) {
    ProfileDialogData *data = (ProfileDialogData *)user_data;
    
    GtkWidget *chooser = gtk_file_chooser_dialog_new("Escolher imagem do avatar",
                                                     NULL,
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Cancelar",
                                                     GTK_RESPONSE_CANCEL,
                                                     "Abrir",
                                                     GTK_RESPONSE_ACCEPT,
                                                     NULL);
    
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Imagens");
    gtk_file_filter_add_mime_type(filter, "image/png");
    gtk_file_filter_add_mime_type(filter, "image/jpeg");
    gtk_file_filter_add_mime_type(filter, "image/jpg");
    gtk_file_filter_add_mime_type(filter, "image/svg+xml");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    
    gint result = gtk_dialog_run(GTK_DIALOG(chooser));
    if (result == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (filename) {
            AsyncImageData *async_data = g_new(AsyncImageData, 1);
            async_data->filename = g_strdup(filename);
            async_data->width = 128;
            async_data->height = 128;
            async_data->image_widget = data->image_preview;

            GTask *task = g_task_new(NULL, ctx.global_async_cancellable, on_pixbuf_loaded, async_data);
            g_task_run_in_thread(task, load_pixbuf_thread);
            g_object_unref(task);

            g_free(data->selected_image_path);
            data->selected_image_path = filename;
        }
    }
    
    gtk_widget_destroy(chooser);
}

static void on_user_profile_clicked(GtkButton *button, gpointer user_data) {
    ProfileDialogData *main_data = (ProfileDialogData *)user_data;
    
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Configurar Perfil",
                                                     NULL,
                                                     GTK_DIALOG_MODAL,
                                                     "Cancelar",
                                                     GTK_RESPONSE_CANCEL,
                                                     "Salvar",
                                                     GTK_RESPONSE_ACCEPT,
                                                     NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 300);
    
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 15);
    
    GtkWidget *label_name = gtk_label_new("Nome de exibição:");
    gtk_widget_set_halign(label_name, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), label_name, FALSE, FALSE, 0);
    
    GtkWidget *entry_name = gtk_entry_new();
    
    gchar *current_display_name = NULL;
    gchar *current_image_path = NULL;
    load_profile_config(&current_display_name, &current_image_path);
    
    gtk_entry_set_text(GTK_ENTRY(entry_name), current_display_name ? current_display_name : g_get_user_name());
    gtk_box_pack_start(GTK_BOX(box), entry_name, FALSE, FALSE, 0);
    
    GtkWidget *label_image = gtk_label_new("Imagem do avatar:");
    gtk_widget_set_halign(label_image, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), label_image, FALSE, FALSE, 0);
    
    GtkWidget *image_preview = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_halign(image_preview, GTK_ALIGN_CENTER);
    
    if (current_image_path) {
        AsyncImageData *async_data = g_new(AsyncImageData, 1);
        async_data->filename = g_strdup(current_image_path);
        async_data->width = 128;
        async_data->height = 128;
        async_data->image_widget = image_preview;
        
        GTask *task = g_task_new(NULL, ctx.global_async_cancellable, on_pixbuf_loaded, async_data);
        g_task_run_in_thread(task, load_pixbuf_thread);
        g_object_unref(task);
    }
    
    gtk_box_pack_start(GTK_BOX(box), image_preview, FALSE, FALSE, 0);
    
    GtkWidget *btn_choose_image = gtk_button_new_with_label("Escolher imagem...");
    gtk_box_pack_start(GTK_BOX(box), btn_choose_image, FALSE, FALSE, 0);
    
    ProfileDialogData *dialog_data = g_new(ProfileDialogData, 1);
    dialog_data->image_preview = image_preview;
    dialog_data->selected_image_path = current_image_path ? g_strdup(current_image_path) : NULL;
    dialog_data->user_avatar = main_data->user_avatar;
    dialog_data->user_name = main_data->user_name;
    
    g_signal_connect(btn_choose_image, "clicked", G_CALLBACK(on_choose_image_clicked), dialog_data);
    
    gtk_container_add(GTK_CONTAINER(content_area), box);
    gtk_widget_show_all(box);
    
    gint result = gtk_dialog_run(GTK_DIALOG(dialog));
    if (result == GTK_RESPONSE_ACCEPT) {
        const gchar *new_name = gtk_entry_get_text(GTK_ENTRY(entry_name));
        save_profile_config(new_name, dialog_data->selected_image_path);
        
        gtk_label_set_text(GTK_LABEL(dialog_data->user_name), new_name);
        if (dialog_data->selected_image_path) {
            AsyncImageData *async_data = g_new(AsyncImageData, 1);
            async_data->filename = g_strdup(dialog_data->selected_image_path);
            async_data->width = 32;
            async_data->height = 32;
            async_data->image_widget = dialog_data->user_avatar;
            
            GTask *task = g_task_new(NULL, ctx.global_async_cancellable, on_pixbuf_loaded, async_data);
            g_task_run_in_thread(task, load_pixbuf_thread);
            g_object_unref(task);
        }
        
    }
    
    g_free(dialog_data->selected_image_path);
    g_free(current_display_name);
    g_free(current_image_path);
    g_free(dialog_data);
    gtk_widget_destroy(dialog);
}

static void on_app_icon_button_clicked(GtkButton *button, gpointer user_data) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Escolher Novo Ícone",
                                                    GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "Cancelar", GTK_RESPONSE_CANCEL,
                                                    "Abrir", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Imagens");
    gtk_file_filter_add_mime_type(filter, "image/png");
    gtk_file_filter_add_mime_type(filter, "image/jpeg");
    gtk_file_filter_add_mime_type(filter, "image/svg+xml");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void on_open_path_folder_clicked(GtkButton *button, gpointer user_data) {
    GtkEntry *path_entry = GTK_ENTRY(user_data);
    const gchar *path = gtk_entry_get_text(path_entry);
    
    gchar *dir = g_path_get_dirname(path);
    if (dir) {
        gchar *uri = g_filename_to_uri(dir, NULL, NULL);
        if (uri) {
            g_app_info_launch_default_for_uri(uri, NULL, NULL);
            g_free(uri);
        }
        g_free(dir);
    }
}

static void show_app_properties_dialog(GAppInfo *app, GtkWindow *parent) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Propriedades do Aplicativo", 
                                                    parent, 
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, 
                                                    "OK", GTK_RESPONSE_OK,
                                                    "Cancelar", GTK_RESPONSE_CANCEL,
                                                    "Aplicar", GTK_RESPONSE_APPLY,
                                                    NULL);

    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_DIALOG);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_widget_set_margin_start(content_area, 16);
    gtk_widget_set_margin_end(content_area, 16);
    gtk_widget_set_margin_top(content_area, 16);
    gtk_widget_set_margin_bottom(content_area, 16);
    gtk_box_set_spacing(GTK_BOX(content_area), 12);

    GtkWidget *header_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    
    GtkWidget *icon_button = gtk_button_new();
    GtkWidget *icon_image = NULL;
    if (app && g_app_info_get_icon(app)) {
        icon_image = gtk_image_new_from_gicon(g_app_info_get_icon(app), GTK_ICON_SIZE_DIALOG);
    } else {
        icon_image = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
    }
    gtk_image_set_pixel_size(GTK_IMAGE(icon_image), 48);
    gtk_container_add(GTK_CONTAINER(icon_button), icon_image);
    gtk_widget_set_tooltip_text(icon_button, "Clique para alterar o ícone");
    g_signal_connect(icon_button, "clicked", G_CALLBACK(on_app_icon_button_clicked), NULL);

    GtkWidget *name_entry = gtk_entry_new();
    gtk_widget_set_hexpand(name_entry, TRUE);
    gtk_widget_set_valign(name_entry, GTK_ALIGN_CENTER);
    if (app) {
        gtk_entry_set_text(GTK_ENTRY(name_entry), g_app_info_get_name(app));
    }

    gtk_box_pack_start(GTK_BOX(header_hbox), icon_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_hbox), name_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content_area), header_hbox, FALSE, FALSE, 0);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(content_area), separator, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);

    GtkWidget *path_label = gtk_label_new("Caminho:");
    gtk_widget_set_halign(path_label, GTK_ALIGN_END);
    
    GtkWidget *path_entry = gtk_entry_new();
    gtk_widget_set_hexpand(path_entry, TRUE);
    
    if (app && G_IS_DESKTOP_APP_INFO(app)) {
        const gchar *desktop_path = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app));
        if (desktop_path) {
            gtk_entry_set_text(GTK_ENTRY(path_entry), desktop_path);
        }
    }
    
    GtkWidget *path_btn = gtk_button_new_from_icon_name("folder-open-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(path_btn, "Abrir pasta do arquivo");
    g_signal_connect(path_btn, "clicked", G_CALLBACK(on_open_path_folder_clicked), path_entry);

    gtk_grid_attach(GTK_GRID(grid), path_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), path_entry, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), path_btn,   2, 0, 1, 1);

    GtkWidget *pkg_label = gtk_label_new("Pacote:");
    gtk_widget_set_halign(pkg_label, GTK_ALIGN_END);
    
    GtkWidget *pkg_entry = gtk_entry_new();
    gtk_widget_set_hexpand(pkg_entry, TRUE);

    gtk_grid_attach(GTK_GRID(grid), pkg_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), pkg_entry, 1, 1, 2, 1);

    GtkWidget *cmd_label = gtk_label_new("Comando:");
    gtk_widget_set_halign(cmd_label, GTK_ALIGN_END);
    
    GtkWidget *cmd_entry = gtk_entry_new();
    gtk_widget_set_hexpand(cmd_entry, TRUE);
    if (app && g_app_info_get_commandline(app)) {
        gtk_entry_set_text(GTK_ENTRY(cmd_entry), g_app_info_get_commandline(app));
    }

    gtk_grid_attach(GTK_GRID(grid), cmd_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cmd_entry, 1, 2, 2, 1);

    gtk_box_pack_start(GTK_BOX(content_area), grid, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);

    g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
}

/* Função principal de construção da UI */
void activate(GApplication *application, gpointer user_data) {
    init_static_quarks();
    init_pinned_cache();
    init_recent_apps_cache();

    ctx.app_monitor = g_app_info_monitor_get();
    g_signal_connect(ctx.app_monitor, "changed", G_CALLBACK(on_app_info_changed), NULL);

    GtkWidget *window = gtk_application_window_new(GTK_APPLICATION(application));
    gtk_window_set_title(GTK_WINDOW(window), "Optimenum");
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);

    gint win_width = 480;
    gint win_height = 720;
    gtk_window_set_default_size(GTK_WINDOW(window), win_width, win_height);

    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    GdkRectangle workarea;
    gdk_monitor_get_workarea(monitor, &workarea);

    gint margin_y = 10;
    gint pos_x = workarea.x + (workarea.width / 2) - (win_width / 2);
    gint pos_y = workarea.y + workarea.height - win_height - margin_y;

    gtk_window_move(GTK_WINDOW(window), pos_x, pos_y);

    g_signal_connect(window, "focus-out-event", G_CALLBACK(on_focus_out), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);

    GtkWidget *header_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(header_hbox), "bottom-panel");
    
    GtkWidget *search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), "Search for apps, settings, and documents");
    gtk_widget_set_margin_start(search_entry, 24);
    gtk_widget_set_margin_end(search_entry, 24);
    gtk_widget_set_margin_top(search_entry, 12);
    gtk_widget_set_margin_bottom(search_entry, 12);
    gtk_box_pack_start(GTK_BOX(header_hbox), search_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_vbox), header_hbox, FALSE, FALSE, 0);

    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 250);
    gtk_widget_set_margin_start(stack, 24);
    gtk_widget_set_margin_end(stack, 24);
    gtk_box_pack_start(GTK_BOX(main_vbox), stack, TRUE, TRUE, 0);

    /* PÁGINA 1: PINNED */
    GtkWidget *page_pinned = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    
    GtkWidget *pinned_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *pinned_label = gtk_label_new("Pinned");
    gtk_widget_set_halign(pinned_label, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(pinned_label), "section-title");
    
    GtkWidget *all_apps_btn = gtk_button_new_with_label("All apps >");
    gtk_button_set_relief(GTK_BUTTON(all_apps_btn), GTK_RELIEF_NONE);
    gtk_widget_set_halign(all_apps_btn, GTK_ALIGN_END);
    g_object_set_qdata(G_OBJECT(all_apps_btn), quark_target_stack, stack);
    g_object_set_qdata(G_OBJECT(all_apps_btn), quark_target_view, (gpointer)"view_all");
    g_signal_connect(all_apps_btn, "clicked", G_CALLBACK(switch_view), NULL);

    gtk_box_pack_start(GTK_BOX(pinned_hbox), pinned_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(pinned_hbox), all_apps_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page_pinned), pinned_hbox, FALSE, FALSE, 0);

    GtkWidget *scroll_pinned = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_pinned), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    
    GtkListStore *pinned_store = gtk_list_store_new(NUM_COLS, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_APP_INFO);
    
    filter_model = gtk_tree_model_filter_new(GTK_TREE_MODEL(pinned_store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter_model),
                                          iconview_filter_func,
                                          NULL, NULL);
    
    GtkWidget *app_iconview = gtk_icon_view_new_with_model(filter_model);
    
    GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
    g_object_set(renderer, "stock-size", GTK_ICON_SIZE_DND, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(app_iconview), renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(app_iconview), renderer,
                                   "gicon", COL_ICON, NULL);
    
    GtkCellRenderer *text_renderer = gtk_cell_renderer_text_new();
    g_object_set(text_renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    g_object_set(text_renderer, "max-width-chars", 15, NULL);
    g_object_set(text_renderer, "xalign", 0.5, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(app_iconview), text_renderer, FALSE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(app_iconview), text_renderer,
                                   "text", COL_NAME, NULL);
    
    g_object_set_qdata(G_OBJECT(app_iconview), quark_pinned_store, pinned_store);
    
    gtk_icon_view_set_columns(GTK_ICON_VIEW(app_iconview), 6);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(app_iconview), 100);
    gtk_icon_view_set_item_padding(GTK_ICON_VIEW(app_iconview), 0);
    gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(app_iconview), 5);
    gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(app_iconview), 5);
    
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(app_iconview), GTK_SELECTION_SINGLE);
    gtk_icon_view_set_activate_on_single_click(GTK_ICON_VIEW(app_iconview), TRUE);
    gtk_icon_view_enable_model_drag_source(GTK_ICON_VIEW(app_iconview),
                                           GDK_BUTTON1_MASK,
                                           dnd_targets, 1,
                                           GDK_ACTION_COPY);
    g_signal_connect(app_iconview, "drag-data-get", G_CALLBACK(on_drag_data_get), NULL);
    g_signal_connect(app_iconview, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(app_iconview, "item-activated", G_CALLBACK(on_iconview_item_activated), pinned_store);
    g_signal_connect(app_iconview, "button-press-event", G_CALLBACK(on_iconview_button_press), NULL);
    
    gtk_container_add(GTK_CONTAINER(scroll_pinned), app_iconview);
    gtk_box_pack_start(GTK_BOX(page_pinned), scroll_pinned, TRUE, TRUE, 0);

    /* SEÇÃO RECOMENDADOS */
    GtkWidget *lbl_recomendados = gtk_label_new("Recomendados");
    gtk_widget_set_halign(lbl_recomendados, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_recomendados), "section-title");
    gtk_box_pack_start(GTK_BOX(page_pinned), lbl_recomendados, FALSE, FALSE, 0);

    GtkWidget *recomendados_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    
    GtkWidget *recomendados_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(recomendados_list), GTK_SELECTION_SINGLE);
    g_object_set_qdata(G_OBJECT(recomendados_list), quark_pinned_store, pinned_store);
    gtk_drag_source_set(recomendados_list, GDK_BUTTON1_MASK, dnd_targets, 1, GDK_ACTION_COPY);
    g_signal_connect(recomendados_list, "drag-data-get", G_CALLBACK(on_drag_data_get), NULL);
    g_signal_connect(recomendados_list, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(recomendados_list, "row-activated", G_CALLBACK(on_listbox_row_activated), NULL);
    g_signal_connect(recomendados_list, "button-press-event", G_CALLBACK(on_listbox_button_press), NULL);
    gtk_box_pack_start(GTK_BOX(recomendados_hbox), recomendados_list, TRUE, TRUE, 0);
    
    GtkWidget *recomendados_list2 = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(recomendados_list2), GTK_SELECTION_SINGLE);
    g_object_set_qdata(G_OBJECT(recomendados_list2), quark_pinned_store, pinned_store);
    gtk_drag_source_set(recomendados_list2, GDK_BUTTON1_MASK, dnd_targets, 1, GDK_ACTION_COPY);
    g_signal_connect(recomendados_list2, "drag-data-get", G_CALLBACK(on_drag_data_get), NULL);
    g_signal_connect(recomendados_list2, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(recomendados_list2, "row-activated", G_CALLBACK(on_listbox_row_activated), NULL);
    g_signal_connect(recomendados_list2, "button-press-event", G_CALLBACK(on_listbox_button_press), NULL);
    gtk_box_pack_start(GTK_BOX(recomendados_hbox), recomendados_list2, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page_pinned), recomendados_hbox, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(stack), page_pinned, "view_pinned");

    /* PÁGINA 2: ALL APPS */
    GtkWidget *page_all = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_name(page_all, "page_all");
    
    GtkWidget *all_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *all_label = gtk_label_new("All apps");
    gtk_widget_set_halign(all_label, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(all_label), "section-title");
    
    GtkWidget *back_btn = gtk_button_new_with_label("< Back");
    gtk_button_set_relief(GTK_BUTTON(back_btn), GTK_RELIEF_NONE);
    gtk_widget_set_halign(back_btn, GTK_ALIGN_END);
    g_object_set_qdata(G_OBJECT(back_btn), quark_target_stack, stack);
    g_object_set_qdata(G_OBJECT(back_btn), quark_target_view, (gpointer)"view_pinned");
    g_signal_connect(back_btn, "clicked", G_CALLBACK(switch_view), NULL);

    gtk_box_pack_start(GTK_BOX(all_hbox), all_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(all_hbox), back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page_all), all_hbox, FALSE, FALSE, 0);

    // 1. Criar container principal horizontal para a aba de pesquisa
    GtkWidget *search_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    
    // 2. Lado Esquerdo: Lista de resultados (Treeview já existente)
    GtkWidget *scroll_all = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_all), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll_all, 300, -1); // Definir largura fixa para a lista
    
    app_treeview = gtk_tree_view_new();
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app_treeview), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(app_treeview), TRUE);
    gtk_tree_view_set_rubber_banding(GTK_TREE_VIEW(app_treeview), TRUE);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app_treeview));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);

    GtkCellRenderer *renderer_icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(renderer_icon, "stock-size", GTK_ICON_SIZE_DND, NULL);
    GtkTreeViewColumn *col_icon = gtk_tree_view_column_new_with_attributes("", renderer_icon, "gicon", COL_ICON, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app_treeview), col_icon);

    GtkCellRenderer *renderer_text = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col_text = gtk_tree_view_column_new_with_attributes("", renderer_text, "text", COL_NAME, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app_treeview), col_text);

    g_object_set_qdata(G_OBJECT(app_treeview), quark_pinned_store, pinned_store);

    gtk_drag_source_set(app_treeview, GDK_BUTTON1_MASK, dnd_targets, 1, GDK_ACTION_COPY);
    g_signal_connect(app_treeview, "drag-data-get", G_CALLBACK(on_drag_data_get), NULL);
    g_signal_connect(app_treeview, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(app_treeview, "row-activated", G_CALLBACK(on_treeview_row_activated), NULL);
    g_signal_connect(app_treeview, "button-press-event", G_CALLBACK(on_treeview_button_press), NULL);
    gtk_container_add(GTK_CONTAINER(scroll_all), app_treeview);
    gtk_box_pack_start(GTK_BOX(search_hbox), scroll_all, FALSE, FALSE, 0);

    // 3. Lado Direito: Painel de Detalhes
    global_search_ctx.detail_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(global_search_ctx.detail_box, TRUE);
    gtk_widget_hide(global_search_ctx.detail_box); // Ocultar inicialmente
    gtk_style_context_add_class(gtk_widget_get_style_context(global_search_ctx.detail_box), "search-side-panel");
    
    global_search_ctx.detail_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(global_search_ctx.detail_icon), 96); // Ícone grande
    
    global_search_ctx.detail_name = gtk_label_new("");
    gtk_widget_set_halign(global_search_ctx.detail_name, GTK_ALIGN_CENTER);

    global_search_ctx.detail_action_open = gtk_button_new_with_label("Abrir");
    gtk_widget_set_halign(global_search_ctx.detail_action_open, GTK_ALIGN_CENTER);
    g_signal_connect(global_search_ctx.detail_action_open, "clicked", G_CALLBACK(on_detail_open_clicked), NULL);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(separator, 8);
    gtk_widget_set_margin_bottom(separator, 8);

    gtk_box_pack_start(GTK_BOX(global_search_ctx.detail_box), global_search_ctx.detail_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(global_search_ctx.detail_box), global_search_ctx.detail_name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(global_search_ctx.detail_box), separator, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(global_search_ctx.detail_box), global_search_ctx.detail_action_open, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(search_hbox), global_search_ctx.detail_box, TRUE, TRUE, 0);

    // 4. Conectar sinal de seleção para reatividade
    g_signal_connect(selection, "changed", G_CALLBACK(on_search_selection_changed), NULL);

    gtk_box_pack_start(GTK_BOX(page_all), search_hbox, TRUE, TRUE, 0);

    gtk_stack_add_named(GTK_STACK(stack), page_all, "view_all");

    /* PINNED APPS */
    gchar *config_dir = g_build_filename(g_get_user_config_dir(), "opm", NULL);
    gchar *config_file = g_build_filename(config_dir, "pinned.conf", NULL);

    g_mkdir_with_parents(config_dir, 0755);

    GKeyFile *keyfile = g_key_file_new();

    if (!g_key_file_load_from_file(keyfile, config_file, G_KEY_FILE_NONE, NULL)) {
        const gchar *default_pinned[] = {
            "firefox.desktop",
            "xfce4-terminal.desktop",
            "thunar.desktop",
            NULL
        };
        g_key_file_set_string_list(keyfile, "Pinned", "Apps", default_pinned, g_strv_length((gchar **)default_pinned));
        g_key_file_save_to_file(keyfile, config_file, NULL);
    }

    gsize pinned_length;
    gchar **pinned_list = g_key_file_get_string_list(keyfile, "Pinned", "Apps", &pinned_length, NULL);

    for (gsize i = 0; i < pinned_length; i++) {
        GDesktopAppInfo *app = g_desktop_app_info_new(pinned_list[i]);
        
        if (app) {
            add_app_to_store(pinned_store, G_APP_INFO(app));
            g_object_unref(app);
        }
    }

    g_strfreev(pinned_list);
    g_key_file_free(keyfile);
    g_free(config_file);
    g_free(config_dir);

    /* RECOMMENDED APPS - usando cache LRU de aplicativos recentes */
    GPtrArray *recent_cache = get_recent_apps_cache();
    if (recent_cache && recent_cache->len > 0) {
        for (guint i = 0; i < recent_cache->len; i++) {
            const gchar *app_id = (const gchar*)g_ptr_array_index(recent_cache, i);
            GDesktopAppInfo *app = g_desktop_app_info_new(app_id);
            if (app) {
                GtkWidget *row = gtk_list_box_row_new();
                GtkWidget *content = create_app_widget_factory(G_APP_INFO(app), FALSE);
                
                gtk_container_add(GTK_CONTAINER(row), content);

                g_object_set_qdata_full(G_OBJECT(row), quark_app_info, g_object_ref(app), g_object_unref);

                if (i % 2 == 0) {
                    gtk_list_box_insert(GTK_LIST_BOX(recomendados_list), row, -1);
                } else {
                    gtk_list_box_insert(GTK_LIST_BOX(recomendados_list2), row, -1);
                }
                g_object_unref(app);
            }
        }
    } else {
        /* Fallback para apps padrão se o cache estiver vazio */
        const gchar *fallback_apps[] = {
            "org.gnome.Nautilus.desktop",
            "org.gnome.Settings.desktop",
            "firefox.desktop",
            "libreoffice-writer.desktop",
            NULL
        };

        for (gsize i = 0; fallback_apps[i] != NULL; i++) {
            GDesktopAppInfo *app = g_desktop_app_info_new(fallback_apps[i]);
            if (app) {
                GtkWidget *row = gtk_list_box_row_new();
                GtkWidget *content = create_app_widget_factory(G_APP_INFO(app), FALSE);
                
                gtk_container_add(GTK_CONTAINER(row), content);

                g_object_set_qdata_full(G_OBJECT(row), quark_app_info, g_object_ref(app), g_object_unref);

                if (i % 2 == 0) {
                    gtk_list_box_insert(GTK_LIST_BOX(recomendados_list), row, -1);
                } else {
                    gtk_list_box_insert(GTK_LIST_BOX(recomendados_list2), row, -1);
                }
                g_object_unref(app);
            }
        }
    }

    /* ALL APPS */
    init_all_apps_model();

    /* Configuração de Pesquisa */
    gpointer *search_widgets = g_new(gpointer, 2);
    search_widgets[0] = app_treeview;
    search_widgets[1] = stack;
    g_signal_connect_data(search_entry, "search-changed", G_CALLBACK(on_search_changed), search_widgets, (GClosureNotify)g_free, 0);

    /* Conecta o evento da tecla Enter (activate) na barra de pesquisa */
    g_signal_connect(search_entry, "activate", G_CALLBACK(on_search_activated), NULL);

    /* RODAPÉ */
    GtkWidget *footer_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(footer_hbox), "bottom-panel");
    GtkWidget *user_profile_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(user_profile_btn), GTK_RELIEF_NONE);
    gtk_widget_set_hexpand(user_profile_btn, FALSE);
    gtk_widget_set_margin_start(user_profile_btn, 48);
    
    GtkWidget *user_profile_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    
    gchar *saved_display_name = NULL;
    gchar *saved_image_path = NULL;
    load_profile_config(&saved_display_name, &saved_image_path);
    
    GtkWidget *user_avatar;
    if (saved_image_path) {
        GError *error = NULL;
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(saved_image_path, 32, 32, TRUE, &error);
        if (G_LIKELY(pixbuf != NULL)) {
            user_avatar = gtk_image_new_from_pixbuf(pixbuf);
            gtk_image_set_pixel_size(GTK_IMAGE(user_avatar), 32);
            g_object_unref(pixbuf);
        } else {
            g_warning("Falha ao processar imagem de perfil: %s", error->message);
            g_error_free(error);
            user_avatar = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_LARGE_TOOLBAR);
        }
    } else {
        user_avatar = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_LARGE_TOOLBAR);
    }
    
    GtkWidget *user_name = gtk_label_new(saved_display_name ? saved_display_name : g_get_user_name());
    gtk_widget_set_halign(user_name, GTK_ALIGN_START);
    
    gtk_box_pack_start(GTK_BOX(user_profile_box), user_avatar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(user_profile_box), user_name, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(user_profile_btn), user_profile_box);
    
    ProfileDialogData *profile_data = g_new(ProfileDialogData, 1);
    profile_data->user_avatar = user_avatar;
    profile_data->user_name = user_name;
    profile_data->image_preview = NULL;
    profile_data->selected_image_path = NULL;
    
    g_signal_connect(user_profile_btn, "clicked", G_CALLBACK(on_user_profile_clicked), profile_data);
    
    g_free(saved_display_name);
    g_free(saved_image_path);
    
    /* Botão Menu de Energia */
    GtkWidget *power_btn = gtk_menu_button_new();
    GtkWidget *power_icon = gtk_image_new_from_icon_name("system-shutdown", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_button_set_image(GTK_BUTTON(power_btn), power_icon);
    gtk_button_set_relief(GTK_BUTTON(power_btn), GTK_RELIEF_NONE);
    gtk_widget_set_halign(power_btn, GTK_ALIGN_END);
    gtk_widget_set_margin_end(power_btn, 24);

    /* Botão de Configurações */
    GtkWidget *settings_btn = gtk_button_new();
    GtkWidget *settings_icon = gtk_image_new_from_icon_name("preferences-system-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_button_set_image(GTK_BUTTON(settings_btn), settings_icon);
    gtk_button_set_relief(GTK_BUTTON(settings_btn), GTK_RELIEF_NONE);
    g_signal_connect(settings_btn, "clicked", (GCallback)action_open_settings, NULL);

    /* Popover */
    GtkWidget *power_popover = gtk_popover_new(power_btn);
    GtkWidget *popover_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(popover_vbox), 10);
    
    GtkWidget *btn_suspend = gtk_button_new_with_label("Suspender");
    GtkWidget *btn_reboot = gtk_button_new_with_label("Reiniciar");
    GtkWidget *btn_poweroff = gtk_button_new_with_label("Desligar");
    
    gtk_button_set_relief(GTK_BUTTON(btn_suspend), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(btn_reboot), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(btn_poweroff), GTK_RELIEF_NONE);
    
    g_signal_connect(btn_suspend, "clicked", (GCallback)action_suspend, NULL);
    g_signal_connect(btn_reboot, "clicked", (GCallback)action_reboot, NULL);
    g_signal_connect(btn_poweroff, "clicked", (GCallback)action_poweroff, NULL);
    
    gtk_box_pack_start(GTK_BOX(popover_vbox), btn_suspend, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(popover_vbox), btn_reboot, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(popover_vbox), btn_poweroff, FALSE, FALSE, 0);
    
    gtk_container_add(GTK_CONTAINER(power_popover), popover_vbox);
    gtk_widget_show_all(popover_vbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(power_btn), power_popover);

    gtk_box_pack_start(GTK_BOX(footer_hbox), user_profile_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer_hbox), gtk_label_new(""), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(footer_hbox), settings_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer_hbox), power_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(main_vbox), footer_hbox, FALSE, FALSE, 0);

    /* CSS */
    css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider,
        "window { background-color: #f3f3f3; border-radius: 0px; box-shadow: 0 4px 15px rgba(0,0,0,0.2); } "
        "scrolledwindow, viewport, iconview, list { background-color: transparent; } "
        "#page_all { background-color: white; } "
        "iconview { padding: 10px; } "
        "iconview.cell:hover { background-color: #e5e5e5; border-radius: 6px; } "
        "iconview:selected { background-color: #e5e5e5; border-radius: 6px; } "
        "list row { padding: 4px; border-radius: 6px; border: none; background-color: transparent; } "
        "list row:selected { background-color: #e5e5e5; } "
        ".search-side-panel { background-color: #e8e8e8; border: none; margin: 0; padding: 20px; } "
        ".bottom-panel { background-color: #e8e8e8; border: none; margin: 0; padding: 12px 0px; } "
        ".section-title { font-weight: bold; font-size: 14px; padding-left: 24px; margin-top: 16px; margin-bottom: 8px; color: #202020; }",
        -1, NULL);
    
    GdkScreen *screen = gdk_screen_get_default();
    
    if (screen) {
        gtk_style_context_add_provider_for_screen(
            screen,
            GTK_STYLE_PROVIDER(css_provider), 
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
    } else {
        g_warning("Falha: Nenhum GdkScreen ativo encontrado (Servidor gráfico indisponível).");
    }

    gtk_widget_show_all(window);
}
