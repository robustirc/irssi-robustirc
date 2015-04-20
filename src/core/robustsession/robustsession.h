#pragma once

#include <stdbool.h>

bool robustsession_init(void);
void robustsession_deinit(void);
void robustsession_connect(SERVER_REC *server);
void robustsession_send(SERVER_REC *server, const char *buffer, int size_buf);
void robustsession_destroy(SERVER_REC *server);
