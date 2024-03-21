/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_FUNCTIONS_BUILD_TARGET_H
#define MUON_FUNCTIONS_BUILD_TARGET_H
#include "functions/common.h"

bool tgt_src_to_object_path(struct workspace *wk, const struct obj_build_target *tgt, obj src_file, bool relative, struct sbuf *res);

bool build_target_extract_all_objects(struct workspace *wk, uint32_t err_node, obj self, obj *res, bool recursive);

extern const struct func_impl impl_tbl_build_target[8];
#endif
