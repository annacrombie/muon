/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_LOG_H
#define MUON_LOG_H

#include "compat.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "lang/types.h"

extern const char *log_level_clr[log_level_count];
extern const char *log_level_name[log_level_count];
extern const char *log_level_shortname[log_level_count];

#define L(...) log_print(true, log_debug, __VA_ARGS__)
#define LOG_I(...) log_print(true, log_info, __VA_ARGS__)
#define LOG_W(...) log_print(true, log_warn, __VA_ARGS__)
#define LOG_E(...) log_print(true, log_error, __VA_ARGS__)

#define LL(...) log_print(false, log_debug, __VA_ARGS__)
#define LLOG_I(...) log_print(false, log_info, __VA_ARGS__)
#define LLOG_W(...) log_print(false, log_warn, __VA_ARGS__)
#define LLOG_E(...) log_print(false, log_error, __VA_ARGS__)

void log_init(void);
void log_set_file(FILE *log_file);
void log_set_lvl(enum log_level lvl);
void log_set_prefix(const char *prefix);
const char *log_get_prefix(void);

uint32_t log_print_prefix(enum log_level lvl, char *buf, uint32_t size);
void log_print(bool nl, enum log_level lvl, const char *fmt, ...)
MUON_ATTR_FORMAT(printf, 3, 4);
bool log_clr(void);
void log_plain(const char *fmt, ...)
MUON_ATTR_FORMAT(printf, 1, 2);
void log_plainv(const char *fmt, va_list ap);
FILE *log_file(void);
bool log_should_print(enum log_level lvl);
#endif
