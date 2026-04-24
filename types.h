#ifndef TYPES_H
#define TYPES_H

#include <gtk/gtk.h>
#include <gio/gio.h>

/* D-Bus Constants for logind */
#define DBUS_LOGIN1_SERVICE "org.freedesktop.login1"
#define DBUS_LOGIN1_PATH    "/org/freedesktop/login1"
#define DBUS_LOGIN1_IFACE   "org.freedesktop.login1.Manager"

/* Colunas do GtkListStore para GtkIconView */
enum {
    COL_ICON,
    COL_NAME,
    COL_NAME_LOWER,
    COL_APPINFO,
    NUM_COLS
};

/* Singleton State para Cache de Sistema */
typedef struct {
    GHashTable *pinned_cache;
    GAppInfoMonitor *app_monitor;
    GRWLock cache_rwlock;
    GCancellable *copy_cancellable;
    GCancellable *global_async_cancellable;
    guint8 initialized;
    guint8 __padding[63];
} __attribute__((aligned(64))) RuntimeContext;

extern RuntimeContext ctx;

/* Estrutura para contexto de busca */
typedef struct {
    GtkSearchEntry *entry;
    GtkTreeView *treeview;
    GtkStack *stack;
    /* Novos nós para o painel lateral */
    GtkWidget *detail_box;
    GtkWidget *detail_icon;
    GtkWidget *detail_name;
    GtkWidget *detail_action_open;
} SearchContext;

extern SearchContext global_search_ctx;


/* Estrutura para cache de ordenação */
typedef struct {
    GAppInfo *app; // cppcheck-suppress unusedStructMember
    gchar *collate_key; // cppcheck-suppress unusedStructMember
} SortCache;

/* Estrutura para dados de diálogo de perfil */
typedef struct {
    GtkWidget *image_preview; // cppcheck-suppress unusedStructMember
    gchar *selected_image_path; // cppcheck-suppress unusedStructMember
    GtkWidget *user_avatar; // cppcheck-suppress unusedStructMember
    GtkWidget *user_name; // cppcheck-suppress unusedStructMember
} ProfileDialogData;

/* Estrutura para contexto de app em callbacks */
typedef struct {
    GAppInfo *appinfo; // cppcheck-suppress unusedStructMember
    GtkListStore *pinned_store; // cppcheck-suppress unusedStructMember
} AppContextData;

/* Estado Global de Debug */
extern gboolean global_debug_mode;

/* Variáveis globais */
extern GDBusConnection *system_bus_conn;
extern GtkCssProvider *css_provider;
extern GtkTreeModel *filter_model;
extern guint search_timeout_id;
extern GQuark quark_app_info;
extern GQuark quark_cached_search;
extern GQuark quark_target_stack;
extern GQuark quark_target_view;
extern GQuark quark_pinned_store;
extern GQuark quark_is_header;
extern GIcon *fallback_icon_singleton;
extern GdkAppLaunchContext *launch_context_singleton;
extern GtkIconTheme *icon_theme_singleton;
extern GtkListStore *all_apps_model;
extern GtkWidget *app_treeview;
extern GPtrArray *pinned_apps_list;
extern guint reload_timer_id;

#endif /* TYPES_H */
