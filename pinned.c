#include "pinned.h"
#include "core.h"
#include <gio/gdesktopappinfo.h>

/* Forward declarations for static functions */
static void read_pinned_apps_from_disk(void);
static void rebuild_pinned_cache(void);
static void reload_pinned_apps_ui_async(GtkListStore *pinned_store, gchar **pinned_list);
static gchar* get_normalized_app_id(GAppInfo *appinfo);
static void save_pinned_apps_async_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void save_pinned_apps_async_callback(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void reload_pinned_apps_ui_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void reload_pinned_apps_ui_callback(GObject *source_object, GAsyncResult *res, gpointer user_data);

typedef struct {
    gchar **pinned_list;
    gsize length;
} SavePinnedData;

typedef struct {
    GtkListStore *pinned_store;
    gchar **pinned_list;
} ReloadUIData;

/* Callback do timer para recarregamento debounced */
static gboolean on_reload_timer_expired(gpointer user_data) {
    /* Despacha a leitura assíncrona. A UI será atualizada pela callback. */
    read_pinned_apps_from_disk();
    
    reload_timer_id = 0;
    return G_SOURCE_REMOVE;
}

static gchar* get_normalized_app_id(GAppInfo *appinfo) {
    if (!appinfo) return NULL;
    
    /* 1. Tentar extração primária do ID nativo */
    const gchar *id = g_app_info_get_id(appinfo);
    if (id && id[0] != '\0') {
        return g_strdup(id);
    }
    
    /* 2. Fallback: Extração rigorosa do basename para instâncias geradas no painel Pinned */
    if (G_IS_DESKTOP_APP_INFO(appinfo)) {
        const gchar *filename = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(appinfo));
        if (filename) {
            return g_path_get_basename(filename);
        }
    }
    
    return NULL;
}

/* Callback chamado automaticamente pelo sistema operativo quando as apps mudam */
void on_app_info_changed(GAppInfoMonitor *monitor, gpointer user_data) {
    /* Cancela o timer anterior se existir (debouncing) */
    if (reload_timer_id > 0) {
        g_source_remove(reload_timer_id);
    }
    /* Cria um novo timer de 100ms */
    reload_timer_id = g_timeout_add(100, on_reload_timer_expired, NULL);
}

typedef struct {
    GPtrArray *apps_list;
} PinnedAppsResult;

static void read_pinned_apps_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    gchar *filepath = g_build_filename(g_get_user_config_dir(), "opm", "pinned.conf", NULL);
    
    PinnedAppsResult *result = g_new0(PinnedAppsResult, 1);
    
    GKeyFile *keyfile = g_key_file_new();
    if (g_key_file_load_from_file(keyfile, filepath, G_KEY_FILE_NONE, NULL)) {
        gsize pinned_length;
        gchar **pinned_list = g_key_file_get_string_list(keyfile, "Pinned", "Apps", &pinned_length, NULL);
        
        if (pinned_list) {
            result->apps_list = g_ptr_array_new_with_free_func(g_free);
            for (gsize i = 0; i < pinned_length; i++) {
                gchar *app_id = g_strstrip(pinned_list[i]);
                if (*app_id != '\0') {
                    g_ptr_array_add(result->apps_list, g_strdup(app_id));
                }
            }
            g_strfreev(pinned_list);
        }
    }
    
    g_key_file_free(keyfile);
    g_free(filepath);
    
    g_task_return_pointer(task, result, NULL);
}

static void read_pinned_apps_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    PinnedAppsResult *result = g_task_propagate_pointer(G_TASK(res), NULL);
    if (!result) return;
    
    g_rw_lock_writer_lock(&ctx.cache_rwlock);
    if (pinned_apps_list) {
        g_ptr_array_unref(pinned_apps_list);
    }
    pinned_apps_list = result->apps_list ? g_ptr_array_ref(result->apps_list) : NULL;
    g_rw_lock_writer_unlock(&ctx.cache_rwlock);
    
    if (result->apps_list) {
        g_ptr_array_unref(result->apps_list);
    }
    g_free(result);
    
    /* Reconstrói o cache e atualiza o modelo APÓS a leitura do disco concluir */
    rebuild_pinned_cache();
    init_all_apps_model(); /* Movido para cá para garantir sincronia de dados */
}

static void read_pinned_apps_from_disk(void) {
    GTask *task = g_task_new(NULL, ctx.global_async_cancellable, read_pinned_apps_callback, NULL);
    g_task_run_in_thread(task, read_pinned_apps_thread);
    g_object_unref(task);
}

/* Reconstrói o cache na RAM a partir da lista em memória */
static void rebuild_pinned_cache(void) {
    GHashTable *new_cache = g_hash_table_new(g_direct_hash, g_direct_equal);

    g_rw_lock_writer_lock(&ctx.cache_rwlock);
    if (pinned_apps_list) {
        for (gsize i = 0; i < pinned_apps_list->len; i++) {
            const gchar *app_id = g_ptr_array_index(pinned_apps_list, i);
            if (*app_id != '\0') {
                GQuark quark = g_quark_from_string(app_id);
                g_hash_table_add(new_cache, GUINT_TO_POINTER(quark));
            }
        }
    }
    
    /* Troca atômica e segura do ponteiro */
    GHashTable *old_cache = ctx.pinned_cache;
    ctx.pinned_cache = new_cache;
    g_rw_lock_writer_unlock(&ctx.cache_rwlock);
    
    if (old_cache) {
        g_hash_table_destroy(old_cache);
    }
}

/* Inicialização segura do cache de pinned apps */
void init_pinned_cache(void) {
    g_rw_lock_writer_lock(&ctx.cache_rwlock);
    if (ctx.pinned_cache == NULL) {
        ctx.pinned_cache = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    
    if (pinned_apps_list == NULL) {
        /* Para o arranque inicial da aplicação, a leitura DEVE ser síncrona 
           para garantir que o cache está pronto antes de renderizar a UI. */
        gchar *filepath = g_build_filename(g_get_user_config_dir(), "opm", "pinned.conf", NULL);
        GKeyFile *keyfile = g_key_file_new();
        
        pinned_apps_list = g_ptr_array_new_with_free_func(g_free);
        
        if (g_key_file_load_from_file(keyfile, filepath, G_KEY_FILE_NONE, NULL)) {
            gsize pinned_length;
            gchar **pinned_list = g_key_file_get_string_list(keyfile, "Pinned", "Apps", &pinned_length, NULL);
            if (pinned_list) {
                for (gsize i = 0; i < pinned_length; i++) {
                    gchar *app_id = g_strstrip(pinned_list[i]);
                    if (*app_id != '\0') {
                        g_ptr_array_add(pinned_apps_list, g_strdup(app_id));
                    }
                }
                g_strfreev(pinned_list);
            }
        }
        g_key_file_free(keyfile);
        g_free(filepath);
        
        /* Como já temos o Writer Lock, podemos chamar a lógica do rebuild diretamente */
        for (gsize i = 0; i < pinned_apps_list->len; i++) {
            const gchar *app_id = g_ptr_array_index(pinned_apps_list, i);
            GQuark quark = g_quark_from_string(app_id);
            g_hash_table_add(ctx.pinned_cache, GUINT_TO_POINTER(quark));
        }
    }
    g_rw_lock_writer_unlock(&ctx.cache_rwlock);
}

gboolean is_app_pinned(GAppInfo *appinfo) {
    gchar *id = get_normalized_app_id(appinfo);
    if (!id) return FALSE;
    
    const gchar *id_without_ext = id;
    gchar id_with_ext_stack[256];
    gchar *id_with_ext = id_with_ext_stack;
    
    gsize id_len = strlen(id);
    if (g_str_has_suffix(id, ".desktop")) {
        /* Usa ponteiro direto para parte sem extensão (sem alocação) */
        id_without_ext = id;
    } else {
        /* Constrói versão com extensão em buffer stack-allocated */
        if (id_len + 9 < sizeof(id_with_ext_stack)) {
            memcpy(id_with_ext_stack, id, id_len);
            memcpy(id_with_ext_stack + id_len, ".desktop", 9);
            id_with_ext = id_with_ext_stack;
        } else {
            /* Fallback para heap apenas se string for muito longa */
            id_with_ext = g_strdup_printf("%s.desktop", id);
        }
    }

    GQuark quark_with_ext = g_quark_from_string(id_with_ext);
    GQuark quark_without_ext = g_quark_from_string(id_without_ext);

    g_free(id);
    if (id_with_ext != id_with_ext_stack) {
        g_free(id_with_ext);
    }

    if (quark_with_ext == 0 && quark_without_ext == 0) return FALSE;

    g_rw_lock_reader_lock(&ctx.cache_rwlock);
    gboolean is_pinned = FALSE;
    if (ctx.pinned_cache) {
        /* Valida qualquer uma das variantes via GQuark */
        if (quark_with_ext != 0) {
            is_pinned = g_hash_table_contains(ctx.pinned_cache, GUINT_TO_POINTER(quark_with_ext));
        }
        if (!is_pinned && quark_without_ext != 0) {
            is_pinned = g_hash_table_contains(ctx.pinned_cache, GUINT_TO_POINTER(quark_without_ext));
        }
    }
    g_rw_lock_reader_unlock(&ctx.cache_rwlock);
    
    return is_pinned;
}

void pin_app(GAppInfo *appinfo, GtkListStore *pinned_store) {
    if (is_app_pinned(appinfo)) return;

    gchar *id = get_normalized_app_id(appinfo);
    if (!id) return;

    /* Forçar a padronização no vetor de memória */
    gchar *id_with_ext = NULL;
    if (g_str_has_suffix(id, ".desktop")) {
        id_with_ext = g_strdup(id);
    } else {
        id_with_ext = g_strdup_printf("%s.desktop", id);
    }

    /* Atualiza a lista em memória atomicamente */
    g_rw_lock_writer_lock(&ctx.cache_rwlock);
    if (!pinned_apps_list) {
        pinned_apps_list = g_ptr_array_new_with_free_func(g_free);
    }
    g_ptr_array_add(pinned_apps_list, g_strdup(id_with_ext));
    g_rw_lock_writer_unlock(&ctx.cache_rwlock);

    g_free(id);
    
    /* Garantir a sincronização do cache de pesquisa antes do recarregamento da UI */
    rebuild_pinned_cache();
    
    /* Cria array temporário para a UI */
    g_rw_lock_reader_lock(&ctx.cache_rwlock);
    gchar **ui_list = NULL;
    if (pinned_apps_list && pinned_apps_list->len > 0) {
        ui_list = g_new0(gchar *, pinned_apps_list->len + 1);
        for (gsize i = 0; i < pinned_apps_list->len; i++) {
            ui_list[i] = g_strdup(g_ptr_array_index(pinned_apps_list, i));
        }
    }
    g_rw_lock_reader_unlock(&ctx.cache_rwlock);
    
    reload_pinned_apps_ui_async(pinned_store, ui_list);
    
    /* Cria uma cópia independente para a thread de salvamento */
    gchar **save_list = g_strdupv(ui_list);
    
    /* Despacha a gravação assíncrona para thread separada */
    SavePinnedData *save_data = g_new0(SavePinnedData, 1);
    save_data->pinned_list = save_list;
    save_data->length = pinned_apps_list->len;
    
    GTask *task = g_task_new(NULL, ctx.global_async_cancellable, save_pinned_apps_async_callback, NULL);
    g_task_set_task_data(task, save_data, NULL);
    g_task_run_in_thread(task, save_pinned_apps_async_thread);
    g_object_unref(task);
}

void unpin_app(GAppInfo *appinfo, GtkListStore *pinned_store) {
    gchar *id = get_normalized_app_id(appinfo);
    if (!id) return;
    
    gchar *id_without_ext = NULL;
    if (g_str_has_suffix(id, ".desktop")) {
        id_without_ext = g_strndup(id, strlen(id) - 8);
    } else {
        id_without_ext = g_strdup(id);
    }
    gchar *id_with_ext = g_strdup_printf("%s.desktop", id_without_ext);

    /* Remove o item diretamente do GtkListStore (UI) antes de atualizar memória */
    GtkTreeModel *model = GTK_TREE_MODEL(pinned_store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    
    while (valid) {
        GAppInfo *row_appinfo = NULL;
        gtk_tree_model_get(model, &iter, COL_APPINFO, &row_appinfo, -1);
        
        if (row_appinfo) {
            gchar *row_id = get_normalized_app_id(row_appinfo);
            if (row_id) {
                gchar *row_id_without_ext = NULL;
                if (g_str_has_suffix(row_id, ".desktop")) {
                    row_id_without_ext = g_strndup(row_id, strlen(row_id) - 8);
                } else {
                    row_id_without_ext = g_strdup(row_id);
                }
                gchar *row_id_with_ext = g_strdup_printf("%s.desktop", row_id_without_ext);
                
                gboolean match = (g_strcmp0(row_id_with_ext, id_with_ext) == 0 || 
                                 g_strcmp0(row_id_with_ext, id_without_ext) == 0 ||
                                 g_strcmp0(row_id_without_ext, id_with_ext) == 0 ||
                                 g_strcmp0(row_id_without_ext, id_without_ext) == 0);
                
                g_free(row_id);
                g_free(row_id_without_ext);
                g_free(row_id_with_ext);
                
                if (match) {
                    valid = gtk_list_store_remove(GTK_LIST_STORE(model), &iter);
                    g_object_unref(row_appinfo);
                    continue;
                }
            }
            g_object_unref(row_appinfo);
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }

    /* Atualiza a lista em memória atomicamente */
    g_rw_lock_writer_lock(&ctx.cache_rwlock);
    if (pinned_apps_list) {
        /* Iteração inversa para remoção segura e total de duplicados */
        for (gint i = pinned_apps_list->len - 1; i >= 0; i--) {
            const gchar *app_id = g_ptr_array_index(pinned_apps_list, i);
            if (g_strcmp0(app_id, id_with_ext) == 0 || g_strcmp0(app_id, id_without_ext) == 0) {
                g_ptr_array_remove_index(pinned_apps_list, i);
                /* Omissão propositada do 'break' para limpar corrupções de memória */
            }
        }
    }
    g_rw_lock_writer_unlock(&ctx.cache_rwlock);
    
    g_free(id);
    g_free(id_without_ext);
    g_free(id_with_ext);
    
    /* Reconstruir o cache de memória */
    rebuild_pinned_cache();
    
    /* Cria lista para salvamento assíncrono (sem recarregar UI) */
    g_rw_lock_reader_lock(&ctx.cache_rwlock);
    gchar **save_list = NULL;
    if (pinned_apps_list && pinned_apps_list->len > 0) {
        save_list = g_new0(gchar *, pinned_apps_list->len + 1);
        for (gsize i = 0; i < pinned_apps_list->len; i++) {
            save_list[i] = g_strdup(g_ptr_array_index(pinned_apps_list, i));
        }
    }
    g_rw_lock_reader_unlock(&ctx.cache_rwlock);
    
    /* Despacha a gravação assíncrona para thread separada */
    SavePinnedData *save_data = g_new0(SavePinnedData, 1);
    save_data->pinned_list = save_list;
    save_data->length = pinned_apps_list ? pinned_apps_list->len : 0;
    
    GTask *task = g_task_new(NULL, ctx.global_async_cancellable, save_pinned_apps_async_callback, NULL);
    g_task_set_task_data(task, save_data, NULL);
    g_task_run_in_thread(task, save_pinned_apps_async_thread);
    g_object_unref(task);
}

static void save_pinned_apps_async_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    SavePinnedData *data = (SavePinnedData*)task_data;
    
    gchar *config_dir = g_build_filename(g_get_user_config_dir(), "opm", NULL);
    gchar *config_file = g_build_filename(config_dir, "pinned.conf", NULL);
    
    g_mkdir_with_parents(config_dir, 0755);
    
    GKeyFile *keyfile = g_key_file_new();
    /* Não precisa carregar arquivo existente - vamos sobrescrever completamente */
    
    g_key_file_set_string_list(keyfile, "Pinned", "Apps", (const gchar * const *)data->pinned_list, data->length);
    g_key_file_save_to_file(keyfile, config_file, NULL);
    
    g_key_file_free(keyfile);
    g_free(config_dir);
    g_free(config_file);
    
    g_task_return_pointer(task, data, NULL);
}

static void save_pinned_apps_async_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    SavePinnedData *data = g_task_propagate_pointer(G_TASK(res), NULL);
    if (data) {
        g_strfreev(data->pinned_list);
        g_free(data);
    }
}

static void reload_pinned_apps_ui_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    ReloadUIData *data = (ReloadUIData*)task_data;
    
    if (!data) {
        g_task_return_pointer(task, g_ptr_array_new(), NULL);
        return;
    }
    
    GPtrArray *apps_array = g_ptr_array_new();
    
    if (data->pinned_list) {
        for (gsize i = 0; data->pinned_list[i] != NULL; i++) {
            GDesktopAppInfo *app = g_desktop_app_info_new(data->pinned_list[i]);
            if (app) {
                g_ptr_array_add(apps_array, app);
            }
        }
    }
    
    g_task_return_pointer(task, apps_array, NULL);
}

static void reload_pinned_apps_ui_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    ReloadUIData *data = (ReloadUIData*)user_data;
    
    if (!data) {
        g_warning("ReloadUIData is NULL in reload_pinned_apps_ui_callback");
        return;
    }
    
    GPtrArray *apps_array = g_task_propagate_pointer(G_TASK(res), NULL);
    
    if (apps_array) {
        gtk_list_store_clear(data->pinned_store);
        
        for (gsize i = 0; i < apps_array->len; i++) {
            GDesktopAppInfo *app = g_ptr_array_index(apps_array, i);
            add_app_to_store(data->pinned_store, G_APP_INFO(app));
            g_object_unref(app);
        }
        
        g_ptr_array_free(apps_array, TRUE);
    }
    
    if (data->pinned_list) {
        g_strfreev(data->pinned_list);
    }
    g_free(data);
}

static void reload_pinned_apps_ui_async(GtkListStore *pinned_store, gchar **pinned_list) {
    ReloadUIData *data = g_new0(ReloadUIData, 1);
    data->pinned_store = pinned_store;
    data->pinned_list = pinned_list;
    
    GTask *task = g_task_new(NULL, ctx.global_async_cancellable, reload_pinned_apps_ui_callback, data);
    g_task_run_in_thread(task, reload_pinned_apps_ui_thread);
    g_object_unref(task);
}
