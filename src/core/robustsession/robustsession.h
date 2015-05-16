#pragma once

#include <stdbool.h>

struct t_robustsession_ctx;

bool robustsession_init(void);
void robustsession_deinit(void);
struct t_robustsession_ctx *robustsession_connect(SERVER_REC *server);
void robustsession_send(struct t_robustsession_ctx *ctx, SERVER_REC *server, const char *buffer, int size_buf);
void robustsession_write_only(struct t_robustsession_ctx *ctx);
void robustsession_destroy(struct t_robustsession_ctx *ctx);
