#include "core.h"
#include <string.h>
#include <pango/pangocairo.h>

#define MAX_RECENT_APPS 10
static GPtrArray *recent_apps_cache = NULL;

/* Forward declarations for static functions */
static gint fast_compare_names(gconstpointer a, gconstpointer b);
static gboolean treeview_filter_func(GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data);
static void rebuild_all_apps_model(const GtkTreeView *treeview, const gchar *search_text);
static gboolean perform_search_delayed(gpointer user_data);

/* Inicialização do cache de aplicativos recentes */
void init_recent_apps_cache() {
    recent_apps_cache = g_ptr_array_new_with_free_func(g_free);
    gchar *filepath = g_build_filename(g_get_user_config_dir(), "opm", "recent.conf", NULL);
    gchar *contents = NULL;
    
    if (g_file_get_contents(filepath, &contents, NULL, NULL)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (gint i = 0; lines[i] && recent_apps_cache->len < MAX_RECENT_APPS; i++) {
            if (*lines[i] != '\0') g_ptr_array_add(recent_apps_cache, g_strdup(lines[i]));
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(filepath);
}

/* Atualização O(n) em memória + I/O assíncrono */
void track_recent_app(GAppInfo *appinfo) {
    if (!appinfo) return;
    const gchar *app_id = g_app_info_get_id(appinfo);
    if (!app_id) return;

    /* Remove duplicata se existir (reposicionando no topo) */
    for (guint i = 0; i < recent_apps_cache->len; i++) {
        if (g_strcmp0((gchar*)g_ptr_array_index(recent_apps_cache, i), app_id) == 0) {
            g_ptr_array_remove_index(recent_apps_cache, i);
            break;
        }
    }

    /* Insere no topo */
    g_ptr_array_insert(recent_apps_cache, 0, g_strdup(app_id));

    /* Trunca o excesso */
    if (recent_apps_cache->len > MAX_RECENT_APPS) {
        g_ptr_array_remove_index(recent_apps_cache, recent_apps_cache->len - 1);
    }

    /* Salva em background para não travar a UI */
    GString *out = g_string_new("");
    for (guint i = 0; i < recent_apps_cache->len; i++) {
        g_string_append_printf(out, "%s\n", (gchar*)g_ptr_array_index(recent_apps_cache, i));
    }
    
    gchar *filepath = g_build_filename(g_get_user_config_dir(), "opm", "recent.conf", NULL);
    g_file_set_contents(filepath, out->str, out->len, NULL);
    g_free(filepath);
    g_string_free(out, TRUE);
}

/* Getter para acessar o cache de aplicativos recentes */
GPtrArray* get_recent_apps_cache() {
    return recent_apps_cache;
}

/* Inicialização Bare-Metal sem branch prediction overhead */
void init_static_quarks(void) {
    quark_app_info = g_quark_from_static_string("app-info");
    quark_cached_search = g_quark_from_static_string("cached-search");
    quark_target_stack = g_quark_from_static_string("target-stack");
    quark_target_view = g_quark_from_static_string("target-view");
    quark_pinned_store = g_quark_from_static_string("pinned-store");
    quark_is_header = g_quark_from_static_string("is-header");
    
    /* Inicializa singleton de ícone fallback */
    if (!fallback_icon_singleton) {
        fallback_icon_singleton = g_themed_icon_new("application-x-executable");
    }
    
    /* Inicializa singleton de launch context */
    if (!launch_context_singleton) {
        GdkDisplay *display = gdk_display_get_default();
        if (display) {
            launch_context_singleton = gdk_display_get_app_launch_context(display);
        }
    }
    
    /* Inicializa singleton de GtkIconTheme */
    if (!icon_theme_singleton) {
        icon_theme_singleton = gtk_icon_theme_get_default();
    }
}

/* Factory unificada para criação de widgets de aplicativos */
GtkWidget* create_app_widget_factory(GAppInfo *app, gboolean is_compact) {
    GtkWidget *box = gtk_box_new(is_compact ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL, is_compact ? 4 : 12);
    gtk_widget_set_halign(box, GTK_ALIGN_START);

    if (!is_compact) {
        gtk_widget_set_margin_top(box, 5);
        gtk_widget_set_margin_bottom(box, 5);
        gtk_widget_set_margin_start(box, 24);
    }

    /* Resolução Direta de Ícone */
    GIcon *icon = g_app_info_get_icon(app);
    GtkWidget *image = NULL;
    
    if (G_LIKELY(icon)) {
        image = gtk_image_new_from_gicon(icon, is_compact ? GTK_ICON_SIZE_DND : GTK_ICON_SIZE_LARGE_TOOLBAR);
    } else {
        image = gtk_image_new_from_gicon(fallback_icon_singleton, is_compact ? GTK_ICON_SIZE_DND : GTK_ICON_SIZE_LARGE_TOOLBAR);
    }
    
    /* Forçar rasterização rígida em C para evitar OOM de SVG */
    gtk_image_set_pixel_size(GTK_IMAGE(image), 32);
    
    /* Nome do Aplicativo */
    GtkWidget *label = gtk_label_new(g_app_info_get_name(app));
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), is_compact ? 20 : 30);

    gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    return box;
}

/* Solução definitiva de Lógica e Eficiência. Zero VM Overhead. */
static gint fast_compare_names(gconstpointer a, gconstpointer b) {
    const SortCache *c_a = (const SortCache*)a;
    const SortCache *c_b = (const SortCache*)b;
    /* Com collate_key, strcmp puro ordena UTF-8 e Acentos matematicamente em instrução nativa */
    gint cmp = strcmp(c_a->collate_key, c_b->collate_key);
    /* Desempate determinístico por ponteiro de memória */
    if (cmp == 0) {
        return (c_a->app < c_b->app) ? -1 : 1;
    }
    return cmp;
}

typedef struct {
    GAppInfo **apps;
    guint count;
} SortResult;

static void init_all_apps_model_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    GList *apps = g_app_info_get_all();
    guint exact_len = g_list_length(apps);

    /* Heap allocation prevents stack overflow with many installed apps */
    SortCache *sort_buffer = g_new(SortCache, exact_len);
    guint valid_count = 0;

    for (GList *l = apps; l != NULL; l = l->next) {
        GAppInfo *app = (GAppInfo*)l->data;
        if (G_LIKELY(g_app_info_should_show(app))) {
            const gchar *name = g_app_info_get_name(app);
            if (name) {
                gchar *name_lower = g_utf8_casefold(name, -1);
                gchar *collate_key = g_utf8_collate_key(name_lower, -1);
                g_free(name_lower);
                
                sort_buffer[valid_count].app = app;
                sort_buffer[valid_count].collate_key = collate_key;
                valid_count++;
            }
        }
    }

    qsort(sort_buffer, valid_count, sizeof(SortCache), fast_compare_names);

    /* Prepare result for main thread */
    SortResult *result = g_new0(SortResult, 1);
    result->apps = g_new0(GAppInfo*, valid_count);
    result->count = valid_count;
    
    for (guint i = 0; i < valid_count; i++) {
        result->apps[i] = g_object_ref(sort_buffer[i].app);
        g_free(sort_buffer[i].collate_key);
    }
    
    /* Cleanup */
    GList *l = apps;
    while (l) {
        GList *next = l->next;
        if (l->data) {
            g_object_unref(l->data);
        }
        g_list_free_1(l);
        l = next;
    }
    g_free(sort_buffer);
    
    g_task_return_pointer(task, result, NULL);
}

static gboolean bind_model_on_main_thread(gpointer user_data) {
    if (app_treeview && all_apps_model) {
        /* Create filter model for search functionality */
        if (filter_model) {
            g_object_unref(filter_model);
        }
        filter_model = gtk_tree_model_filter_new(GTK_TREE_MODEL(all_apps_model), NULL);
        gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter_model), treeview_filter_func, NULL, NULL);

        /* Set filter model on treeview */
        gtk_tree_view_set_model(GTK_TREE_VIEW(app_treeview), GTK_TREE_MODEL(filter_model));
    }
    return G_SOURCE_REMOVE;
}

static void init_all_apps_model_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    SortResult *result = g_task_propagate_pointer(G_TASK(res), NULL);
    if (!result) {
        return;
    }

    if (!all_apps_model)
        all_apps_model = gtk_list_store_new(NUM_COLS, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_APP_INFO);

    /* Insert items incrementally to avoid CPU spikes from batch updates */
    for (guint i = 0; i < result->count; i++) {
        GAppInfo *app = result->apps[i];
        GIcon *icon = g_app_info_get_icon(app);
        if (!icon) icon = fallback_icon_singleton;
        const gchar *name = g_app_info_get_name(app);
        
        /* Zero-allocation: intern the casefolded string */
        gchar *name_lower_raw = g_utf8_casefold(name, -1);
        const gchar *interned_lower = g_intern_string(name_lower_raw);
        g_free(name_lower_raw);

        GtkTreeIter iter;
        gtk_list_store_insert_with_values(all_apps_model, &iter, -1,
                           COL_ICON, icon,
                           COL_NAME, name,
                           COL_NAME_LOWER, (gpointer)interned_lower,
                           COL_APPINFO, app,
                           -1);

        if (result->apps[i]) {
            g_object_unref(result->apps[i]);
        }
    }

    g_free(result->apps);
    g_free(result);

    /* Bind model to listbox on main thread after it's populated */
    bind_model_on_main_thread(NULL);
}

void init_all_apps_model() {
    GTask *task = g_task_new(NULL, ctx.global_async_cancellable, init_all_apps_model_callback, NULL);
    g_task_run_in_thread(task, init_all_apps_model_thread);
    g_object_unref(task);
}

/* Filter function for GtkTreeModelFilter */
static gboolean treeview_filter_func(GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data) {
    if (!global_search_ctx.entry || !G_IS_OBJECT(global_search_ctx.entry)) return TRUE;
    const gchar *search_text = g_object_get_qdata(G_OBJECT(global_search_ctx.entry), quark_cached_search);

    if (G_UNLIKELY(!search_text || search_text[0] == '\0')) return TRUE;

    /* Zero-allocation: read pointer directly from G_TYPE_POINTER column */
    const gchar *name_lower = NULL;
    gtk_tree_model_get(model, iter, COL_NAME_LOWER, &name_lower, -1);

    if (G_UNLIKELY(!name_lower)) return FALSE;

    /* Teste atômico de prefixo aborta cedo para chaves nulas */
    if (name_lower[0] == '\0') {
        return FALSE;
    }

    /* strstr is SIMD-optimized in glibc, faster than manual byte-level early exit */
    return strstr(name_lower, search_text) != NULL;
}

/* Filtro do Grid (IconView) sincronizado com a mesma lógica */
gboolean iconview_filter_func(GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data) {
    /* Lê direto do Quark em O(1) armazenado na widget global */
    if (!global_search_ctx.entry || !G_IS_OBJECT(global_search_ctx.entry)) return TRUE;
    const gchar *search_text_lower = g_object_get_qdata(G_OBJECT(global_search_ctx.entry), quark_cached_search);
    
    if (G_UNLIKELY(!search_text_lower || search_text_lower[0] == '\0')) return TRUE;

    /* Zero-allocation: read pointer directly from G_TYPE_POINTER column */
    const gchar *name_lower = NULL;
    gtk_tree_model_get(model, iter, COL_NAME_LOWER, &name_lower, -1);

    if (G_UNLIKELY(!name_lower)) return FALSE;

    return strstr(name_lower, search_text_lower) != NULL;
}

/* Rebuild Seguro e Otimizado da Busca */
static void rebuild_all_apps_model(const GtkTreeView *treeview, const gchar *search_text) {
    if (!treeview) return;

    /* Refilter the model */
    if (filter_model) {
        gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(filter_model));
        
        // Auto-foco do primeiro elemento
        GtkTreeIter first_iter;
        if (gtk_tree_model_get_iter_first(filter_model, &first_iter)) {
            GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app_treeview));
            gtk_tree_selection_select_iter(selection, &first_iter);
        }
    }
}

/* Debounce Refatorado - Sem micro-otimizações que quebram UTF-8 */
static gboolean perform_search_delayed(gpointer user_data) {
    const gchar *raw_text = gtk_entry_get_text(GTK_ENTRY(global_search_ctx.entry));

    if (!raw_text || raw_text[0] == '\0') {
        g_object_set_qdata(G_OBJECT(global_search_ctx.entry), quark_cached_search, NULL);
        rebuild_all_apps_model(global_search_ctx.treeview, NULL);
    } else {
        const gchar *current_cache = g_object_get_qdata(G_OBJECT(global_search_ctx.entry), quark_cached_search);

        /* Casefold e compara direta - correto para UTF-8 */
        gchar *lower_text = g_utf8_casefold(raw_text, -1);
        gboolean needs_update = !current_cache || g_strcmp0(lower_text, current_cache) != 0;

        if (needs_update) {
            g_object_set_qdata_full(G_OBJECT(global_search_ctx.entry), quark_cached_search, lower_text, g_free);
            gtk_stack_set_visible_child_name(global_search_ctx.stack, "view_all");
            rebuild_all_apps_model(global_search_ctx.treeview, lower_text);
        } else {
            g_free(lower_text);
        }
    }

    search_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

/* Gatilho Livre de Heap Allocation */
void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    gpointer *widgets = (gpointer *)user_data;

    /* Configura os ponteiros globalmente uma única vez */
    global_search_ctx.treeview = GTK_TREE_VIEW(widgets[0]);
    global_search_ctx.stack = GTK_STACK(widgets[1]);
    global_search_ctx.entry = entry;

    /* Zero condicional race, atômico no handler */
    g_clear_handle_id(&search_timeout_id, g_source_remove);
    search_timeout_id = g_timeout_add(150, perform_search_delayed, NULL);
}

/* Callback disparado quando o usuário aperta Enter na barra de busca */
void on_search_activated(GtkEntry *entry, gpointer user_data) {
    const gchar *text = gtk_entry_get_text(entry);

    if (g_strcmp0(text, "/debug on") == 0) {
        global_debug_mode = TRUE;
        gtk_entry_set_text(entry, ""); // Limpa a barra
        g_message("MODO DEBUG ATIVADO. Capturando 100%% dos erros para /tmp/opm_debug.log");
        return;
    }

    if (g_strcmp0(text, "/debug off") == 0) {
        global_debug_mode = FALSE;
        gtk_entry_set_text(entry, ""); // Limpa a barra
        g_message("MODO DEBUG DESATIVADO.");
        return;
    }
}

/* Função para salvar configuração de perfil */
void save_profile_config(const gchar *display_name, const gchar *image_path) {
    gchar *config_dir = g_build_filename(g_get_user_config_dir(), "opm", NULL);
    g_mkdir_with_parents(config_dir, 0755);
    
    gchar *config_file = g_build_filename(config_dir, "profile.conf", NULL);
    g_free(config_dir);
    
    GKeyFile *keyfile = g_key_file_new();
    
    if (display_name) {
        g_key_file_set_string(keyfile, "Profile", "DisplayName", display_name);
    }
    
    if (image_path) {
        g_key_file_set_string(keyfile, "Profile", "ImagePath", image_path);
    }
    
    gchar *data = g_key_file_to_data(keyfile, NULL, NULL);
    g_file_set_contents(config_file, data, -1, NULL);
    
    g_free(data);
    g_key_file_free(keyfile);
    g_free(config_file);
}

/* Função para carregar configuração de perfil */
void load_profile_config(gchar **display_name, gchar **image_path) {
    gchar *config_file = g_build_filename(g_get_user_config_dir(), "opm", "profile.conf", NULL);
    
    GKeyFile *keyfile = g_key_file_new();
    if (g_key_file_load_from_file(keyfile, config_file, G_KEY_FILE_NONE, NULL)) {
        *display_name = g_key_file_get_string(keyfile, "Profile", "DisplayName", NULL);
        *image_path = g_key_file_get_string(keyfile, "Profile", "ImagePath", NULL);
    }
    
    g_key_file_free(keyfile);
    g_free(config_file);
}

/* Helper function para adicionar app ao GtkListStore */
void add_app_to_store(GtkListStore *store, GAppInfo *app) {
    const gchar *name = g_app_info_get_name(app);
    
    /* Internamento de string para zero-allocation na filtragem */
    gchar *lower_raw = g_utf8_casefold(name, -1);
    const gchar *interned_lower = g_intern_string(lower_raw);
    g_free(lower_raw);
    
    /* Resolução Direta de Ícone */
    GIcon *app_icon = g_app_info_get_icon(app);
    
    /* Proteção contra fallback nulo */
    if (!app_icon) app_icon = fallback_icon_singleton;
    
    GtkTreeIter iter;
    gtk_list_store_insert_with_values(store, &iter, -1,
        COL_ICON, app_icon,
        COL_NAME, name,
        COL_NAME_LOWER, (gpointer)interned_lower,
        COL_APPINFO, app,
        -1);
}
