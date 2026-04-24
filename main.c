#include "types.h"
#include "ui.h"
#include <pango/pangocairo.h>

/* Definição das variáveis globais */
RuntimeContext ctx = {
    .pinned_cache = NULL,
    .app_monitor = NULL,
    .copy_cancellable = NULL,
    .initialized = 0
};

SearchContext global_search_ctx;
gboolean global_debug_mode = FALSE;

GDBusConnection *system_bus_conn = NULL;
GtkCssProvider *css_provider = NULL;
GtkTreeModel *filter_model = NULL;
guint search_timeout_id = 0;
GQuark quark_app_info = 0;
GQuark quark_cached_search = 0;
GQuark quark_target_stack = 0;
GQuark quark_target_view = 0;
GQuark quark_pinned_store = 0;
GQuark quark_is_header = 0;
GIcon *fallback_icon_singleton = NULL;
GdkAppLaunchContext *launch_context_singleton = NULL;
GtkIconTheme *icon_theme_singleton = NULL;
GtkListStore *all_apps_model = NULL;
GtkWidget *app_treeview = NULL;
GPtrArray *pinned_apps_list = NULL;
guint reload_timer_id = 0;

/* Handler customizado para capturar 100% dos eventos de debug e erros */
static void custom_log_handler(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data) {
    /* Se não estiver em modo debug, ignora mensagens de info e debug */
    if (!global_debug_mode && log_level >= G_LOG_LEVEL_INFO) {
        return;
    }

    GDateTime *now = g_date_time_new_now_local();
    gchar *time_str = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    
    const gchar *level_str = "LOG";
    if (log_level & G_LOG_LEVEL_ERROR) level_str = "ERROR";
    else if (log_level & G_LOG_LEVEL_CRITICAL) level_str = "CRITICAL";
    else if (log_level & G_LOG_LEVEL_WARNING) level_str = "WARNING";
    else if (log_level & G_LOG_LEVEL_DEBUG) level_str = "DEBUG";

    gchar *log_msg = g_strdup_printf("[%s] [%s] %s: %s\n", time_str, level_str, log_domain ? log_domain : "APP", message);

    /* Imprime no console (stderr) sempre */
    g_printerr("%s", log_msg);

    /* Se o debug estiver ativo, grava 100% das capturas no arquivo */
    if (global_debug_mode) {
        FILE *f = fopen("/tmp/opm_debug.log", "a");
        if (f) {
            fprintf(f, "%s", log_msg);
            fclose(f);
        }
    }

    g_free(log_msg);
    g_free(time_str);
    g_date_time_unref(now);
}

/* Callbacks de D-Bus para energia */
static void on_bus_acquired(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    system_bus_conn = g_bus_get_finish(res, &error);
    
    if (G_UNLIKELY(error)) {
        g_warning("Falha silenciosa ao conectar no D-Bus: %s", error->message);
        g_error_free(error);
    }
}

static void on_logind_method_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), res, &error);
    const gchar *action_name = (const gchar *)user_data;

    if (G_UNLIKELY(error)) {
        g_warning("Erro D-Bus (%s): %s", action_name ? action_name : "Desconhecido", error->message);
    }
}

static void call_logind_method(const gchar *method_name, const gchar *action_display_name) {
    if (G_UNLIKELY(!system_bus_conn)) return;

    g_dbus_connection_call(system_bus_conn, DBUS_LOGIN1_SERVICE, DBUS_LOGIN1_PATH, 
                           DBUS_LOGIN1_IFACE, method_name,
                           g_variant_new("(b)", TRUE), 
                           NULL, G_DBUS_CALL_FLAGS_NONE, G_MAXINT, NULL, 
                           on_logind_method_ready, (gpointer)action_display_name);
}

void action_suspend(GtkButton const * button G_GNUC_UNUSED, gpointer user_data) {
    call_logind_method("Suspend", "Suspender");
}

void action_reboot(GtkButton const * button G_GNUC_UNUSED, gpointer user_data) {
    call_logind_method("Reboot", "Reiniciar");
}

void action_poweroff(GtkButton const * button G_GNUC_UNUSED, gpointer user_data) {
    call_logind_method("PowerOff", "Desligar");
}

void action_open_settings(GtkButton const * button G_GNUC_UNUSED, gpointer user_data) {
    GError *error = NULL;
    if (!g_spawn_command_line_async("xfce4-settings-manager", &error)) {
        g_warning("Falha ao abrir xfce4-settings-manager: %s", error->message);
        g_error_free(error);
    }
}

/* Callback para fechar janela ao perder foco */
gboolean on_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data) {
    gtk_window_close(GTK_WINDOW(widget));
    return FALSE;
}

/* Callback para destruição da janela */
void on_window_destroy(GtkWidget *widget, gpointer data) {
    if (ctx.copy_cancellable) {
        g_cancellable_cancel(ctx.copy_cancellable);
        g_cancellable_reset(ctx.copy_cancellable);
    }
}

/* Callback de shutdown para limpeza de recursos */
static void on_application_shutdown(GApplication *application, gpointer user_data) {

    if (reload_timer_id > 0) {
        g_source_remove(reload_timer_id);
        reload_timer_id = 0;
    }

    if (search_timeout_id > 0) {
        g_source_remove(search_timeout_id);
        search_timeout_id = 0;
    }


    g_rw_lock_clear(&ctx.cache_rwlock);

    if (ctx.app_monitor) {
        g_object_unref(ctx.app_monitor);
        ctx.app_monitor = NULL;
    }

    if (ctx.pinned_cache) {
        g_hash_table_destroy(ctx.pinned_cache);
        ctx.pinned_cache = NULL;
    }

    if (ctx.copy_cancellable) {
        g_cancellable_cancel(ctx.copy_cancellable);
        g_cancellable_reset(ctx.copy_cancellable);
    }

    if (ctx.global_async_cancellable) {
        g_cancellable_cancel(ctx.global_async_cancellable);
        g_clear_object(&ctx.global_async_cancellable);
    }

    if (all_apps_model != NULL) {
        g_object_unref(all_apps_model);
        all_apps_model = NULL;
    }

    if (filter_model != NULL) {
        g_object_unref(filter_model);
        filter_model = NULL;
    }

    if (pinned_apps_list != NULL) {
        g_ptr_array_unref(pinned_apps_list);
        pinned_apps_list = NULL;
    }

    if (system_bus_conn != NULL) {
        g_object_unref(system_bus_conn);
        system_bus_conn = NULL;
    }

    if (css_provider) {
        GdkScreen *screen = gdk_screen_get_default();
        if (screen) {
            gtk_style_context_remove_provider_for_screen(
                screen,
                GTK_STYLE_PROVIDER(css_provider)
            );
        }
        g_object_unref(css_provider);
        css_provider = NULL;
    }

    /* PangoFontMap singleton is managed by the system, do not unref */

    if (fallback_icon_singleton) {
        g_object_unref(fallback_icon_singleton);
        fallback_icon_singleton = NULL;
    }

    if (launch_context_singleton) {
        g_object_unref(launch_context_singleton);
        launch_context_singleton = NULL;
    }

    /* icon_theme_singleton is a singleton from gtk_icon_theme_get_default(), do not unref */

}

int main(int argc, char **argv) {
    /* Força o GLib a passar todas as mensagens pelo nosso interceptador */
    g_log_set_default_handler(custom_log_handler, NULL);
    
    g_rw_lock_init(&ctx.cache_rwlock);
    ctx.global_async_cancellable = g_cancellable_new();
    
    GtkApplication *app = gtk_application_new("org.xfce.optimenum", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_application_shutdown), NULL);
    
    g_bus_get(G_BUS_TYPE_SYSTEM, NULL, on_bus_acquired, NULL);
    
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
