/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_LANG_TYPECHECK_H
#define MUON_LANG_TYPECHECK_H

#include "lang/object.h"

enum complex_type {
	complex_type_or,
	complex_type_nested,
};

#define ARG_TYPE_NULL (obj_type_count + 1)

#define TYPE_TAG_ALLOW_VOID       (((type_tag)1) << 59)
#define TYPE_TAG_COMPLEX          (((type_tag)1) << 60)
#define TYPE_TAG_GLOB             (((type_tag)1) << 61)
#define TYPE_TAG_LISTIFY          (((type_tag)1) << 62)
#define obj_typechecking_type_tag (((type_tag)1) << 63)

#define TYPE_TAG_MASK \
	(TYPE_TAG_ALLOW_VOID | TYPE_TAG_COMPLEX \
	 | TYPE_TAG_GLOB | TYPE_TAG_LISTIFY \
	 | obj_typechecking_type_tag)

/* complex types look like this:
 *
 * 32 bits -> index into obj_typeinfo bucket array
 * 16 bits -> unused
 *  8 bits -> enum complex_type type (e.g. complex_type_or or complex_type_nested)
 *  4 bits -> tags (should be ARG_TYPE_COMPLEX |
 *  obj_typechecking_type_tag (and potentially also
 *  TYPE_TAG_GLOB/TYPE_TAG_LISTIFY))
 */
#define COMPLEX_TYPE(index, t) (((uint64_t)index) | (((uint64_t)t) << 48) | TYPE_TAG_COMPLEX | obj_typechecking_type_tag);
#define COMPLEX_TYPE_INDEX(t) (t & 0xffffffff)
#define COMPLEX_TYPE_TYPE(t) ((t >> 48) & 0xff)

#define tc_meson                (obj_typechecking_type_tag | (((type_tag)1) << 0))
#define tc_disabler             (obj_typechecking_type_tag | (((type_tag)1) << 1))
#define tc_machine              (obj_typechecking_type_tag | (((type_tag)1) << 2))
#define tc_bool                 (obj_typechecking_type_tag | (((type_tag)1) << 3))
#define tc_file                 (obj_typechecking_type_tag | (((type_tag)1) << 4))
#define tc_feature_opt          (obj_typechecking_type_tag | (((type_tag)1) << 5))
#define tc_number               (obj_typechecking_type_tag | (((type_tag)1) << 6))
#define tc_string               (obj_typechecking_type_tag | (((type_tag)1) << 7))
#define tc_array                (obj_typechecking_type_tag | (((type_tag)1) << 8))
#define tc_dict                 (obj_typechecking_type_tag | (((type_tag)1) << 9))
#define tc_compiler             (obj_typechecking_type_tag | (((type_tag)1) << 10))
#define tc_build_target         (obj_typechecking_type_tag | (((type_tag)1) << 11))
#define tc_custom_target        (obj_typechecking_type_tag | (((type_tag)1) << 12))
#define tc_subproject           (obj_typechecking_type_tag | (((type_tag)1) << 13))
#define tc_dependency           (obj_typechecking_type_tag | (((type_tag)1) << 14))
#define tc_external_program     (obj_typechecking_type_tag | (((type_tag)1) << 15))
#define tc_python_installation  (obj_typechecking_type_tag | (((type_tag)1) << 16))
#define tc_run_result           (obj_typechecking_type_tag | (((type_tag)1) << 17))
#define tc_configuration_data   (obj_typechecking_type_tag | (((type_tag)1) << 18))
#define tc_test                 (obj_typechecking_type_tag | (((type_tag)1) << 19))
#define tc_module               (obj_typechecking_type_tag | (((type_tag)1) << 20))
#define tc_install_target       (obj_typechecking_type_tag | (((type_tag)1) << 21))
#define tc_environment          (obj_typechecking_type_tag | (((type_tag)1) << 22))
#define tc_include_directory    (obj_typechecking_type_tag | (((type_tag)1) << 23))
#define tc_option               (obj_typechecking_type_tag | (((type_tag)1) << 24))
#define tc_generator            (obj_typechecking_type_tag | (((type_tag)1) << 25))
#define tc_generated_list       (obj_typechecking_type_tag | (((type_tag)1) << 26))
#define tc_alias_target         (obj_typechecking_type_tag | (((type_tag)1) << 27))
#define tc_both_libs            (obj_typechecking_type_tag | (((type_tag)1) << 28))
#define tc_source_set           (obj_typechecking_type_tag | (((type_tag)1) << 29))
#define tc_source_configuration (obj_typechecking_type_tag | (((type_tag)1) << 30))
#define tc_iterator             (obj_typechecking_type_tag | (((type_tag)1) << 31))
#define tc_func                 (obj_typechecking_type_tag | (((type_tag)1) << 32))
#define tc_type_count         33

#define tc_any                (tc_bool | tc_file | tc_number | tc_string | tc_array | tc_dict \
			       | tc_compiler | tc_build_target | tc_custom_target \
			       | tc_subproject | tc_dependency | tc_feature_opt \
			       | tc_external_program | tc_python_installation | tc_run_result \
			       | tc_configuration_data | tc_test | tc_module \
			       | tc_install_target | tc_environment | tc_include_directory \
			       | tc_option | tc_generator | tc_generated_list \
			       | tc_alias_target | tc_both_libs | tc_disabler \
			       | tc_meson | tc_machine | tc_source_set | tc_source_configuration | tc_func \
			       | tc_iterator \
			       )

#define tc_exe                (tc_string | tc_file | tc_external_program | tc_python_installation \
			       | tc_build_target | tc_custom_target | tc_both_libs)

#define tc_coercible_env      (tc_environment | tc_string | tc_array | tc_dict)
#define tc_coercible_files    (tc_string | tc_custom_target | tc_build_target | tc_file | tc_both_libs)
#define tc_coercible_inc      (tc_string | tc_include_directory)
#define tc_command_array      (TYPE_TAG_LISTIFY | tc_exe)
#define tc_depends_kw         (TYPE_TAG_LISTIFY | tc_build_target | tc_custom_target | tc_both_libs)
#define tc_install_mode_kw    (TYPE_TAG_LISTIFY | tc_string | tc_number | tc_bool)
#define tc_required_kw        (tc_bool | tc_feature_opt)
/* XXX: tc_file should not really be in tc_link_with_kw, however this is
 * how muon represents custom_target outputs, which are valid link_with
 * arguments...
 */
#define tc_link_with_kw       (TYPE_TAG_LISTIFY | tc_build_target | tc_custom_target | tc_file | tc_both_libs)
#define tc_message            (TYPE_TAG_GLOB | tc_string | tc_bool | tc_number | tc_array | tc_dict) // doesn't handle nested types

struct obj_typechecking_type_to_obj_type {
	enum obj_type type;
	type_tag tc;
};


bool typecheck(struct workspace *wk, uint32_t n_id, obj obj_id, type_tag type);
bool typecheck_custom(struct workspace *wk, uint32_t n_id, obj obj_id, type_tag type, const char *fmt);
bool typecheck_simple_err(struct workspace *wk, obj o, type_tag type);
const char *typechecking_type_to_s(struct workspace *wk, type_tag t);
obj typechecking_type_to_arr(struct workspace *wk, type_tag t);
type_tag make_complex_type(struct workspace *wk, enum complex_type t, type_tag type, type_tag subtype);

type_tag obj_type_to_tc_type(enum obj_type t);
#endif
