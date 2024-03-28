/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_FORMATS_INI_H
#define MUON_FORMATS_INI_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "platform/filesystem.h"
#include "error.h"

typedef bool ((*inihcb)(void *ctx, struct source *src, const char *sect, const char *k, const char *v, struct source_location location));

bool ini_parse(const char *path, struct source *src, char **buf, inihcb cb, void *octx);
bool ini_reparse(const char *path, const struct source *src, char *buf, inihcb cb, void *octx);
bool keyval_parse(const char *path, struct source *src, char **buf, inihcb cb, void *octx);
#endif
