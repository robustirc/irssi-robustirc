#pragma once

typedef struct _RobustIOChannel RobustIOChannel;

struct _RobustIOChannel {
    GIOChannel channel;
    SERVER_REC *server;
    struct t_robustsession_ctx *robustsession;
};

GIOChannel *robust_io_channel_new(SERVER_REC *server);
