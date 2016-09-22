// vim:ts=4:sw=4:et
// © 2015 Michael Stapelberg (see COPYING)

#include "common.h"
//#include "module.h"
#include "servers-setup.h"
#include "signals.h"
#include "module-formats.h"
#include "robustirc.h"

static void sig_server_add_fill(SERVER_SETUP_REC *rec, GHashTable *optlist) {
    char *value;

    if ((value = g_hash_table_lookup(optlist, "robustirc")) != NULL) {
        g_free_and_null(rec->chatnet);
        if (*value != '\0')
            rec->chatnet = g_strdup("robustirc");
    }
}

void fe_robustirc_init(void) {
    signal_add("server add fill", sig_server_add_fill);
    theme_register(fe_robustirc_formats);
    module_register(MODULE_NAME, "fe");
}

void fe_robustirc_deinit(void) {
    signal_remove("server add fill", sig_server_add_fill);
}

#ifdef IRSSI_ABI_VERSION
void fe_robustirc_abicheck(int *version) {
    *version = IRSSI_ABI_VERSION;
}
#endif
