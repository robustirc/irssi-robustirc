// vim:ts=4:sw=4:et
// © 2015 Michael Stapelberg (see COPYING)

// stdlib includes
#include <stdbool.h>
#include <limits.h>
#include <math.h>

// external library includes
#include <gio/gio.h>
#include <glib.h>
#include <glib/gprintf.h>

// irssi includes
#include "common.h"
#include "irc.h"
#include "irc-servers.h"
#include "levels.h"
#include "printtext.h"

// module includes
#include "robustsession-network.h"

// Hash table, keyed by lowercase network address (e.g. “robustirc.net”),
// holding the resolved host:port targets and their current backoff state.
static GHashTable *networks = NULL;

struct backoff_state {
    int exponent;
    time_t next;
};

struct network_ctx {
    GList *servers;
    GHashTable *backoff;
};

struct query {
    SERVER_REC *server;
    robustsession_network_resolved_cb callback;
};

static void srv_resolved(GObject *obj, GAsyncResult *res, gpointer user_data) {
    struct query *query = user_data;

    GError *err = NULL;
    GResolver *resolver = (GResolver *)obj;
    GList *targets = g_resolver_lookup_service_finish(resolver, res, &err);
    if (err != NULL) {
        // TODO: is this how irssi’s retry works?
        robustsession_connect(query->server);
        return;
    }

    // Note that we do not shuffle the list of targets here, but call
    // robustsession_network_server() with random == TRUE for CreateSession
    // requests, achieving the same effect.
    GList *servers = NULL;
    for (GList *r = targets; r != NULL; r = r->next) {
        GSrvTarget *target = r->data;
        gchar *server = g_strdup_printf(
            "%s:%d",
            g_srv_target_get_hostname(target),
            g_srv_target_get_port(target));
        if (server) {
            servers = g_list_append(servers, server);
        }
    }

    struct network_ctx *ctx = g_new0(struct network_ctx, 1);
    ctx->servers = servers;
    ctx->backoff = g_hash_table_new(g_str_hash, g_str_equal);
    gchar *key = g_ascii_strdown(query->server->connrec->address, -1);
    g_hash_table_insert(networks, key, ctx);

    g_resolver_free_targets(targets);
    // TODO: here and below, signal resolving errors (g_list_length(servers) == 0)
    query->callback(query->server);
    g_free(query);
}

bool robustsession_network_init(void) {
    srand(time(NULL));
    networks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return (networks != NULL);
}

void robustsession_network_resolve(SERVER_REC *server, robustsession_network_resolved_cb callback) {
    // Skip resolving if we already resolved this network address.
    if (g_hash_table_lookup(networks, server->connrec->address)) {
        callback(server);
        return;
    }

    // For testing, a comma-separated list of targets skips resolving.
    gchar **targets = g_strsplit(server->connrec->address, ",", -1);
    guint len = g_strv_length(targets);
    if (len > 1) {
        struct network_ctx *ctx = g_new0(struct network_ctx, 1);
        ctx->backoff = g_hash_table_new(g_str_hash, g_str_equal);
        for (guint i = 0; i < len; i++) {
            gchar *server = g_strdup(targets[i]);
            if (server) {
                g_strstrip(server);
                if (strcmp(server, "") != 0) {
                    ctx->servers = g_list_append(ctx->servers, server);
                } else {
                    g_free(server);
                }
            }
        }
        gchar *key = g_ascii_strdown(server->connrec->address, -1);
        g_hash_table_insert(networks, key, ctx);
        g_strfreev(targets);
        callback(server);
        return;
    }
    g_strfreev(targets);

    struct query *query = g_new0(struct query, 1);
    query->server = server;
    query->callback = callback;

    GResolver *resolver = g_resolver_get_default();
    g_resolver_lookup_service_async(
        resolver,
        "robustirc",
        "tcp",
        server->connrec->address,
        NULL,  // TODO: do we want to cancel this on timeout?
        srv_resolved,
        query);
    g_object_unref(resolver);
}

struct server_retry_ctx {
    char *address;
    gboolean random;
    robustsession_network_server_cb callback;
    gpointer userdata;
};

static gboolean robustsession_network_server_retry_cb(gpointer user_data) {
    struct server_retry_ctx *ctx = user_data;
    robustsession_network_server(
        ctx->address, ctx->random, ctx->callback, ctx->userdata);
    free(ctx->address);
    free(ctx);
    return FALSE;
}

// Returns TRUE and calls |callback| as soon as a connection to a server for
// network |address| is possible. Connections might be blocked due to
// exponential backoff.
//
// Returns FALSE when |address| was not yet resolved using
// robustsession_network_resolve().
gboolean robustsession_network_server(
    const char *address,
    gboolean random,
    robustsession_network_server_cb callback,
    gpointer userdata) {
    gchar *key = g_ascii_strdown(address, -1);
    struct network_ctx *ctx = g_hash_table_lookup(networks, key);
    g_free(key);
    if (!ctx) {
        return FALSE;
    }

#if 0
    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, ctx->backoff);
    printtext(NULL, NULL, MSGLEVEL_CRAP, "dumping ht");
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        printtext(NULL, NULL, MSGLEVEL_CRAP, "entry for key=%s", k);
    }
    printtext(NULL, NULL, MSGLEVEL_CRAP, "end dumping ht");
#endif

    // Try to use a random server, but fall back to using the next
    // available server in case the randomly picked server is unhealthy.
    if (random) {
        gchar *server = g_list_nth_data(
            ctx->servers, rand() % g_list_length(ctx->servers));
        struct backoff_state *backoff =
            g_hash_table_lookup(ctx->backoff, server);
#if 0
        printtext(NULL, NULL, MSGLEVEL_CRAP, "backoff = %s for *%s*", (backoff ? "yes" : "no"), server);
        if (backoff)
            printtext(NULL, NULL, MSGLEVEL_CRAP, "current backoff = %d, next = %d for *%s*, time = %d", backoff->exponent, backoff->next, server, time(NULL));
#endif
        if (!backoff || backoff->next <= time(NULL)) {
            callback(server, userdata);
            return TRUE;
        }
    }

    time_t soonest = LONG_MAX;
    for (GList *s = ctx->servers; s != NULL; s = s->next) {
        struct backoff_state *backoff =
            g_hash_table_lookup(ctx->backoff, s->data);
#if 0
        printtext(NULL, NULL, MSGLEVEL_CRAP, "backoff = %s for s->data=*%s*", (backoff ? "yes" : "no"), s->data);
        if (backoff)
            printtext(NULL, NULL, MSGLEVEL_CRAP, "current backoff = %d, next = %d for *%s*, time = %d", backoff->exponent, backoff->next, s->data, time(NULL));
#endif
        if (!backoff || backoff->next <= time(NULL)) {
            callback(s->data, userdata);
            return TRUE;
        }
        const time_t wait = backoff->next - time(NULL);
        if (wait < soonest) {
            soonest = wait;
        }
    }

    struct server_retry_ctx *retry_ctx = g_new0(struct server_retry_ctx, 1);
    retry_ctx->address = g_strdup(address);
    retry_ctx->random = random;
    retry_ctx->callback = callback;
    retry_ctx->userdata = userdata;
    g_timeout_add_seconds(
        soonest, robustsession_network_server_retry_cb, retry_ctx);
    return TRUE;
}

// Correspondingly adjusts exponential backoff state after |target| failed.
void robustsession_network_failed(const char *address, const char *target) {
    gchar *key = g_ascii_strdown(address, -1);
    struct network_ctx *ctx = g_hash_table_lookup(networks, key);
    g_free(key);
    if (!ctx) {
        return;
    }

    struct backoff_state *backoff = g_hash_table_lookup(ctx->backoff, target);
    if (!backoff) {
        backoff = g_new0(struct backoff_state, 1);
    }
    // Cap the exponential backoff at 2^6 = 64 seconds. In that region, we run
    // into danger of the client disconnecting due to ping timeout.
    if (backoff->exponent < 6) {
        backoff->exponent++;
    }
    backoff->next = time(NULL) + pow(2, backoff->exponent);
#if 0
    printtext(NULL, NULL, MSGLEVEL_CRAP, "set backoff = %d, next = %d for *%s*", backoff->exponent, backoff->next, target);
#endif
    // TODO: jitter
    g_hash_table_replace(ctx->backoff, (gpointer)g_strdup(target), backoff);
}

void robustsession_network_succeeded(const char *address, const char *target) {
    gchar *key = g_ascii_strdown(address, -1);
    struct network_ctx *ctx = g_hash_table_lookup(networks, key);
    g_free(key);
    if (!ctx) {
        return;
    }
    g_hash_table_remove(ctx->backoff, target);
}

void robustsession_network_update_servers(const char *address, GList *servers) {
    gchar *key = g_ascii_strdown(address, -1);
    struct network_ctx *ctx = g_hash_table_lookup(networks, key);
    g_free(key);
    if (!ctx) {
        return;
    }
    g_list_free_full(ctx->servers, g_free);
    ctx->servers = servers;

    // TODO: delete entries in ctx->backoff which now no longer have a corresponding server
}
