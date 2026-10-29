/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OS_UTIL_H
#define OS_UTIL_H
#include "common.h"
int strncasecmp_portable(const char *a, const char *b, size_t n);
uint32_t os_crc32(const void *data, size_t len);
void os_srand(uint32_t seed);
uint32_t os_rand(void);
int os_rand_range(int lo, int hi);
#endif
