// vim:ts=4:sw=4:et
// © 2015 Michael Stapelberg (see COPYING)

#include <assert.h>

#include "common.h"
#include "modules.h"
#include "signals.h"
#include "channels.h"
#include "channels-setup.h"
#include "chat-protocols.h"
#include "chatnets.h"
#include "servers-setup.h"
#include "settings.h"
#include "queries.h"
#include "commands.h"
#include "rawlog.h"
#include "irc.h"
#include "irc-servers.h"
#include "irc-queries.h"
#include "net-sendbuffer.h"
#include "printtext.h"
#include "levels.h"

#include "robustirc.h"
#include "robustio.h"
#include "robustsession.h"

static GHashTable *connrecs = NULL;

static CHATNET_REC *create_chatnet(void) {
    return g_malloc0(sizeof(CHATNET_REC));
}

static SERVER_SETUP_REC *create_server_setup(void) {
    return g_malloc0(sizeof(SERVER_SETUP_REC));
}

static CHANNEL_SETUP_REC *create_channel_setup(void) {
    return g_malloc0(sizeof(CHANNEL_SETUP_REC));
}

static SERVER_CONNECT_REC *create_server_connect(void) {
    return g_malloc0(sizeof(IRC_SERVER_CONNECT_REC));
}

static void destroy_server_connect(IRC_SERVER_CONNECT_REC *conn) {
    (void)conn;
}

SERVER_REC *robustirc_server_init_connect(SERVER_CONNECT_REC *connrec) {
    SERVER_REC *server;

    printtext(NULL, NULL, MSGLEVEL_CRAP, "robustirc_server_init_connect");

    g_hash_table_insert(connrecs, connrec, NULL);

    connrec->chat_type = IRC_PROTOCOL;
    server = irc_server_init_connect(connrec);
    GIOChannel *handle = robust_io_channel_new(server);
    RobustIOChannel *io = (RobustIOChannel *)handle;
    io->robustsession = robustsession_connect(server);
    server->handle = net_sendbuffer_create(handle, 0);
    // TODO: how many of these are necessary?
    server->connrec->no_connect = TRUE;
    server->connect_pid = -1;
    server->connect_tag = 1;
    return server;
}

static void robustirc_server_connect_copy(SERVER_CONNECT_REC **dest, IRC_SERVER_CONNECT_REC *src) {
    g_return_if_fail(dest != NULL);
    if (!IS_IRC_SERVER_CONNECT(src))
        return;

    // *dest == NULL likely means the sig_server_connect_copy() in
    // irssi/src/irc/core/irc-servers-reconnect.c did not run.
    // Perhaps the irssi code structure has changed?
    assert(*dest != NULL);

    if (g_hash_table_lookup_extended(connrecs, src, NULL, NULL)) {
        // So that robustirc_server_init_connect is called on reconnects.
        (*dest)->chat_type = ROBUSTIRC_PROTOCOL;
        g_hash_table_remove(connrecs, src);
    }
}

void robustirc_server_connect(IRC_SERVER_REC *server) {
    if (!IS_IRC_SERVER(server)) {
        return;
    }

    gchar *m = g_strdup_printf("server = %p, server->connrec = %p", server, server->connrec);
    printtext(NULL, NULL, MSGLEVEL_CRAP, "connect. server = %s", m);
    g_free(m);

    //err:
    //    server->connection_lost = TRUE;
    //    if (error != NULL) {
    //        server_connect_failed(SERVER(server), error->message);
    //        g_error_free(error);
    //    } else
    //        server_connect_failed(SERVER(server), err_msg);
}

QUERY_REC *query_create(const char *server_tag,
                        const char *nick, int automatic) {
    QUERY_REC *rec;

    g_return_val_if_fail(nick != NULL, NULL);

    rec = g_new0(QUERY_REC, 1);
    rec->chat_type = IRC_PROTOCOL;
    rec->name = g_strdup(nick);
    rec->server_tag = g_strdup(server_tag);
    query_init(rec, automatic);
    return rec;
}

void robustirc_core_init(void) {
    CHAT_PROTOCOL_REC *rec;
    rec = g_new0(CHAT_PROTOCOL_REC, 1);
    rec->name = ROBUSTIRC_PROTOCOL_NAME;
    rec->fullname = "RobustIRC";
    rec->chatnet = "robustirc";
    rec->create_chatnet = create_chatnet;
    rec->create_server_setup = create_server_setup;
    rec->create_server_connect = create_server_connect;
    rec->create_channel_setup = create_channel_setup;
    rec->destroy_server_connect =
        (void (*)(SERVER_CONNECT_REC *))destroy_server_connect;
    rec->server_init_connect = robustirc_server_init_connect;
    rec->server_connect = (void (*)(SERVER_REC *))robustirc_server_connect;
    rec->channel_create =
        (CHANNEL_REC * (*)(SERVER_REC *, const char *,
                           const char *, int))
        irc_channel_create;

    rec->query_create = irc_query_create;
    chat_protocol_register(rec);
    g_free(rec);

    command_set_options("connect", "robustirc");

    signal_add_last("server connect copy", (SIGNAL_FUNC)robustirc_server_connect_copy);

    connrecs = g_hash_table_new(NULL, NULL);

    robustsession_init();

    module_register(MODULE_NAME, "core");
}

void robustirc_core_deinit(void) {
    robustsession_deinit();

    g_hash_table_destroy(connrecs);
}
