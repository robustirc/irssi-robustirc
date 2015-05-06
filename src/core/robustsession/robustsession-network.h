#pragma once

// stdlib includes
#include <stdbool.h>

// irssi includes
#include "irc.h"
#include "irc-servers.h"

typedef void (*robustsession_network_resolved_cb)(SERVER_REC *server,
                                                  gpointer userdata);
typedef void (*robustsession_network_server_cb)(const char *target,
                                                gpointer userdata);

bool robustsession_network_init(void);

void robustsession_network_resolve(
    SERVER_REC *server,
    GCancellable *cancellable,
    robustsession_network_resolved_cb callback,
    gpointer userdata);

gboolean robustsession_network_server(
    const char *address,
    gboolean random,
    GCancellable *cancellable,
    robustsession_network_server_cb callback,
    gpointer userdata);

void robustsession_network_failed(const char *address, const char *target);

void robustsession_network_succeeded(const char *address, const char *target);

void robustsession_network_update_servers(const char *address, GList *servers);
