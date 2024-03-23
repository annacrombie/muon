#ifndef MUON_LANG_SOURCE_H
/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define MUON_LANG_SOURCE_H

#include <stdint.h>

enum source_reopen_type {
	source_reopen_type_none,
	source_reopen_type_embedded,
	source_reopen_type_file,
};

struct source {
	const char *label;
	const char *src;
	uint64_t len;
	//
	// only necessary if src is NULL.  If so, this source will be re-read
	// on error to fetch appropriate context lines.
	enum source_reopen_type reopen_type;
};

struct source_location {
	uint32_t off, len;
};

#endif
