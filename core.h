#ifndef CORE_H
#define CORE_H

#include "types.h"

/* Inicialização de quarks estáticos */
void init_static_quarks(void);

/* Funções de metadados de app - static, não exportadas */

/* Factory para widgets de app */
GtkWidget* create_app_widget_factory(GAppInfo *app, gboolean is_compact);

/* Funções de ordenação - static, não exportadas */

/* Inicialização do modelo de apps */
void init_all_apps_model(void);

/* Funções de filtragem */
gboolean iconview_filter_func(GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data);

/* Funções de busca */
void on_search_changed(GtkSearchEntry *entry, gpointer user_data);
void on_search_activated(GtkEntry *entry, gpointer user_data);

/* Funções de configuração de perfil */
void save_profile_config(const gchar *display_name, const gchar *image_path);
void load_profile_config(gchar **display_name, gchar **image_path);

/* Funções de cache de aplicativos recentes (LRU) */
void init_recent_apps_cache(void);
void track_recent_app(GAppInfo *appinfo);
GPtrArray* get_recent_apps_cache(void);

/* Função para adicionar app ao store */
void add_app_to_store(GtkListStore *store, GAppInfo *app);

#endif /* CORE_H */
