// vim:ts=4:sw=4:et
// © 2015 Michael Stapelberg (see COPYING)

#include <glib.h>

#include "common.h"
#include "printtext.h"
#include "levels.h"

#include "robustio.h"
#include "robustsession.h"

static GIOStatus robust_io_read(GIOChannel *channel,
                                gchar *buf,
                                gsize count,
                                gsize *bytes_read,
                                GError **err);

static GIOStatus robust_io_write(GIOChannel *channel,
                                 const gchar *buf,
                                 gsize count,
                                 gsize *bytes_written,
                                 GError **err);

static GIOStatus robust_io_close(GIOChannel *channel,
                                 GError **err);

static void robust_io_free(GIOChannel *channel);

static GSource *robust_io_create_watch(GIOChannel *channel,
                                       GIOCondition condition);

static GIOStatus robust_io_set_flags(GIOChannel *channel,
                                     GIOFlags flags,
                                     GError **err);

static GIOFlags robust_io_get_flags(GIOChannel *channel);

static GIOFuncs robust_channel_funcs = {
    .io_read = robust_io_read,
    .io_write = robust_io_write,
    .io_seek = NULL,
    .io_close = robust_io_close,
    .io_create_watch = robust_io_create_watch,
    .io_free = robust_io_free,
    .io_set_flags = robust_io_set_flags,
    .io_get_flags = robust_io_get_flags,
};

gboolean robust_io_is_robustio_channel(GIOChannel *channel) {
    return (channel->funcs == &robust_channel_funcs);
}

GIOChannel *robust_io_channel_new(SERVER_REC *server) {
    RobustIOChannel *channel;
    GIOChannel *iochannel;

    printtext(NULL, NULL, MSGLEVEL_CRAP, "robust_io_channel_new");

    channel = g_new0(RobustIOChannel, 1);
    iochannel = (GIOChannel *)channel;

    channel->server = server;

    iochannel->is_readable = FALSE;
    iochannel->is_seekable = FALSE;
    iochannel->is_writeable = TRUE;

    g_io_channel_init(iochannel);
    iochannel->use_buffer = FALSE;
    iochannel->close_on_unref = TRUE;
    iochannel->funcs = &robust_channel_funcs;

    return iochannel;
}

static GIOStatus robust_io_read(GIOChannel *channel,
                                gchar *buf,
                                gsize count,
                                gsize *bytes_read,
                                GError **err) {
    (void)channel;
    (void)buf;
    (void)count;
    (void)err;
    // This function should never be called because we never signal readiness
    // for reading, so return EOF if it does get called nevertheless.
    *bytes_read = 0;
    return G_IO_STATUS_EOF;
}

static GIOStatus robust_io_write(GIOChannel *channel,
                                 const gchar *buf,
                                 gsize count,
                                 gsize *bytes_written,
                                 GError **err) {
    (void)err;
    RobustIOChannel *robust_channel = (RobustIOChannel *)channel;

    robustsession_send(robust_channel->robustsession, robust_channel->server, buf, count);
    *bytes_written = count;

    return G_IO_STATUS_NORMAL;
}

static GIOStatus robust_io_close(GIOChannel *channel, GError **err) {
    (void)err;
    RobustIOChannel *robust_channel = (RobustIOChannel *)channel;
    robustsession_destroy(robust_channel->robustsession);
    return G_IO_STATUS_NORMAL;
}

static void robust_io_free(GIOChannel *channel) {
    g_free(channel);
}

static gboolean never_prepare(GSource *source, gint *timeout_);
static gboolean never_check(GSource *source);
static gboolean never_dispatch(GSource *source, GSourceFunc callback, gpointer user_data);

static GSourceFuncs never_source_funcs = {
    .prepare = never_prepare,
    .check = never_check,
    .dispatch = never_dispatch,
};

static gboolean never_prepare(GSource *source, gint *timeout_) {
    (void)source;
    *timeout_ = -1;
    return FALSE;
}

static gboolean never_check(GSource *source) {
    (void)source;
    return FALSE;
}

static gboolean never_dispatch(GSource *source, GSourceFunc callback, gpointer user_data) {
    (void)source;
    (void)callback;
    (void)user_data;
    return TRUE;
}

static GSource *robust_io_create_watch(GIOChannel *channel,
                                       GIOCondition condition) {
    (void)channel;
    (void)condition;
    return g_source_new(&never_source_funcs, sizeof(GSource));
}

static GIOStatus robust_io_set_flags(GIOChannel *channel,
                                     GIOFlags flags,
                                     GError **err) {
    (void)channel;
    (void)flags;
    (void)err;
    return G_IO_STATUS_NORMAL;
}

static GIOFlags robust_io_get_flags(GIOChannel *channel) {
    (void)channel;
    return 0;
}
