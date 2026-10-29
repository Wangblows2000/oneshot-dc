/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host-only test API (not compiled into the Dreamcast build). */
#ifndef OS_HOST_H
#define OS_HOST_H
#include "../common.h"
typedef enum { HOST_STORE_OK, HOST_STORE_NONE, HOST_STORE_FULL, HOST_STORE_TORN } host_store_mode_t;
void host_set_data_root(const char *p);
void host_set_buttons(uint32_t b);
void host_set_store(const char *dir, host_store_mode_t mode, int free_bytes);
const uint32_t *host_framebuffer(void);
const char *host_bgm(void);
#endif
