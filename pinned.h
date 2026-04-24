#ifndef PINNED_H
#define PINNED_H

#include "types.h"

/* Inicialização do cache de pinned apps */
void init_pinned_cache(void);

/* Leitura de pinned apps do disco - static, não exportada */
/* Reconstrução do cache - static, não exportada */

/* Verificação se app está fixado */
gboolean is_app_pinned(GAppInfo *appinfo);

/* Fixar/desfixar app */
void pin_app(GAppInfo *appinfo, GtkListStore *pinned_store);
void unpin_app(GAppInfo *appinfo, GtkListStore *pinned_store);

/* Recarregar UI de pinned apps - static, não exportada */

/* Callback de mudança de apps */
void on_app_info_changed(GAppInfoMonitor *monitor, gpointer user_data);

#endif /* PINNED_H */
