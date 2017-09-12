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
    GQueue *servers;
    GHashTable *backoff;
};

struct query {
    SERVER_REC *server;
    robustsession_network_resolved_cb callback;
    gpointer userdata;
    GCancellable *cancellable;
    gulong cancellable_handler;
};

static void resolve_cancelled(GCancellable *cancellable, gpointer user_data) {
    printtext(NULL, NULL, MSGLEVEL_CRAP, "resolve_cancelled()");
    g_free(user_data);
}

static void srv_resolved(GObject *obj, GAsyncResult *res, gpointer user_data) {
    struct query *query = user_data;

    GError *err = NULL;
    GResolver *resolver = (GResolver *)obj;
    GList *targets = g_resolver_lookup_service_finish(resolver, res, &err);
    if (g_cancellable_is_cancelled(query->cancellable)) {
        return;
    }
    if (err != NULL) {
        // TODO: is this how irssi’s retry works?
        robustsession_connect(query->server);
        return;
    }

    // Note that we do not shuffle the list of targets here, but call
    // robustsession_network_server() with random == TRUE for CreateSession
    // requests, achieving the same effect.
    GQueue *servers = g_queue_new();
    for (GList *r = targets; r != NULL; r = r->next) {
        GSrvTarget *target = r->data;
        gchar *server = g_strdup_printf(
            "%s:%d",
            g_srv_target_get_hostname(target),
            g_srv_target_get_port(target));
        if (server) {
            g_queue_push_tail(servers, server);
        }
    }

    struct network_ctx *ctx = g_new0(struct network_ctx, 1);
    ctx->servers = servers;
    ctx->backoff = g_hash_table_new(g_str_hash, g_str_equal);
    gchar *key = g_ascii_strdown(query->server->connrec->address, -1);
    g_hash_table_insert(networks, key, ctx);

    g_resolver_free_targets(targets);
    // TODO: here and below, signal resolving errors (g_list_length(servers) == 0)
    query->callback(query->server, query->userdata);
    g_cancellable_disconnect(query->cancellable, query->cancellable_handler);
    g_free(query);
}

bool robustsession_network_init(void) {
    srand(time(NULL));
    networks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return (networks != NULL);
}

void robustsession_network_resolve(
    SERVER_REC *server,
    GCancellable *cancellable,
    robustsession_network_resolved_cb callback,
    gpointer userdata) {
    // Skip resolving if we already resolved this network address.
    if (g_hash_table_lookup(networks, server->connrec->address)) {
        callback(server, userdata);
        return;
    }

    // For testing, a comma-separated list of targets skips resolving.
    gchar **targets = g_strsplit(server->connrec->address, ",", -1);
    guint len = g_strv_length(targets);
    if (len > 1) {
        struct network_ctx *ctx = g_new0(struct network_ctx, 1);
        ctx->servers = g_queue_new();
        ctx->backoff = g_hash_table_new(g_str_hash, g_str_equal);
        for (guint i = 0; i < len; i++) {
            gchar *server = g_strdup(targets[i]);
            if (server) {
                g_strstrip(server);
                if (strcmp(server, "") != 0) {
                    g_queue_push_tail(ctx->servers, server);
                } else {
                    g_free(server);
                }
            }
        }
        gchar *key = g_ascii_strdown(server->connrec->address, -1);
        g_hash_table_insert(networks, key, ctx);
        g_strfreev(targets);
        callback(server, userdata);
        return;
    }
    g_strfreev(targets);

    struct query *query = g_new0(struct query, 1);
    query->server = server;
    query->callback = callback;
    query->userdata = userdata;

    gulong cancellable_handler =
        g_cancellable_connect(cancellable, G_CALLBACK(resolve_cancelled), query, NULL);
    if (cancellable_handler == 0) {
        // g_cancellable_connect called g_free(query).
        return;
    }
    query->cancellable = cancellable;
    query->cancellable_handler = cancellable_handler;

    GResolver *resolver = g_resolver_get_default();
    g_resolver_lookup_service_async(
        resolver,
        "robustirc",
        "tcp",
        server->connrec->address,
        cancellable,
        srv_resolved,
        query);
    g_object_unref(resolver);
}

struct server_retry_ctx {
    char *address;
    gboolean random;
    robustsession_network_server_cb callback;
    gpointer userdata;
    guint timeout_id;
    GCancellable *cancellable;
    gulong cancellable_handler;
};

static void retry_cancelled(GCancellable *cancellable, gpointer user_data) {
    struct server_retry_ctx *ctx = user_data;
    g_source_remove(ctx->timeout_id);
    g_free(ctx->address);
    g_free(ctx);
}

static gboolean robustsession_network_server_retry_cb(gpointer user_data) {
    struct server_retry_ctx *ctx = user_data;
    robustsession_network_server(
        ctx->address, ctx->random, ctx->cancellable, ctx->callback, ctx->userdata);
    g_cancellable_disconnect(ctx->cancellable, ctx->cancellable_handler);
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
    GCancellable *cancellable,
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
    gchar *server = g_queue_pop_nth(ctx->servers, 0);

    struct backoff_state *backoff =
        g_hash_table_lookup(ctx->backoff, server);

#if 0
    printtext(NULL, NULL, MSGLEVEL_CRAP, "backoff = %s for *%s*", (backoff ? "yes" : "no"), server);
    if (backoff)
        printtext(NULL, NULL, MSGLEVEL_CRAP, "current backoff = %d, next = %d for *%s*, time = %d", backoff->exponent, backoff->next, server, time(NULL));
#endif
    if (!backoff || backoff->next <= time(NULL)) {
        // Retry this server next.
        g_queue_push_head(ctx->servers, server);
        callback(server, userdata);
        return TRUE;
    }
    // Retry this server last.
    g_queue_push_tail(ctx->servers, server);

    time_t soonest = LONG_MAX;
    for (guint i = 0; i < g_queue_get_length(ctx->servers); i++) {
        gchar *s = g_queue_peek_nth(ctx->servers, i);
        struct backoff_state *backoff =
            g_hash_table_lookup(ctx->backoff, s);

#if 0
        printtext(NULL, NULL, MSGLEVEL_CRAP, "backoff = %s for s->data=*%s*", (backoff ? "yes" : "no"), s);
        if (backoff)
            printtext(NULL, NULL, MSGLEVEL_CRAP, "current backoff = %d, next = %d for *%s*, time = %d", backoff->exponent, backoff->next, s, time(NULL));
#endif

        if (!backoff || backoff->next <= time(NULL)) {
            // Retry this server next.
            s = g_queue_pop_nth(ctx->servers, i);
            g_queue_push_head(ctx->servers, s);
            callback(s, userdata);
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
    retry_ctx->timeout_id = g_timeout_add_seconds(
        soonest, robustsession_network_server_retry_cb, retry_ctx);

    gulong cancellable_handler =
        g_cancellable_connect(cancellable, G_CALLBACK(retry_cancelled), retry_ctx, NULL);
    if (cancellable_handler == 0) {
        // g_cancellable_connect called g_free(retry_ctx).
        return;
    }
    retry_ctx->cancellable = cancellable;
    retry_ctx->cancellable_handler = cancellable_handler;

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
    backoff->next = time(NULL) +
                    pow(2, backoff->exponent) +
                    (rand() % (backoff->exponent + 1));
#if 0
    printtext(NULL, NULL, MSGLEVEL_CRAP, "set backoff = %d, next = %d for *%s*", backoff->exponent, backoff->next, target);
#endif
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

static gint gcharcmp(gconstpointer a, gconstpointer b) {
    gchar *s1 = a;
    gchar *s2 = b;
    if (strlen(s1) != strlen(s2)) {
        return 1;
    }
    return g_ascii_strncasecmp(s1, s2, strlen(s1));
}

void robustsession_network_update_servers(const char *address, GQueue *servers) {
    gchar *key = g_ascii_strdown(address, -1);
    struct network_ctx *ctx = g_hash_table_lookup(networks, key);
    g_free(key);
    if (!ctx) {
        return;
    }

    // Skip the update if both queues contain the same entries so that our retry
    // order within the queue is kept. The algorithm is quadratic, but only used
    // for very small n=3.
    gboolean equal = TRUE;
    for (guint i = 0; i < g_queue_get_length(servers); i++) {
        if (g_queue_find_custom(ctx->servers, g_queue_peek_nth(servers, i), gcharcmp) == NULL) {
            equal = FALSE;
            break;
        }
    }
    if (equal) {
        g_queue_free_full(servers, g_free);
        return;
    }

    g_queue_free_full(ctx->servers, g_free);
    ctx->servers = servers;

    // TODO: delete entries in ctx->backoff which now no longer have a corresponding server
}
