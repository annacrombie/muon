#ifndef MUON_FUNCTIONS_BUILD_TARGET_H
#define MUON_FUNCTIONS_BUILD_TARGET_H
#include "functions/common.h"

bool tgt_src_to_object_path(struct workspace *wk, const struct obj_build_target *tgt,
	obj src_file, bool relative, char res[PATH_MAX]);

bool build_target_extract_all_objects(struct workspace *wk, uint32_t err_node, obj rcvr, obj *res, bool recursive);

extern const struct func_impl_name impl_tbl_build_target[8];
#endif
