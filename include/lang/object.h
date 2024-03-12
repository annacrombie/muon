/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: illiliti <illiliti@dimension.sh>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_LANG_OBJECT_H
#define MUON_LANG_OBJECT_H

#include "compat.h"

#include <stdarg.h>
#include <stdio.h>

#include "compilers.h"
#include "datastructures/bucket_arr.h"
#include "datastructures/hash.h"
#include "datastructures/iterator.h"
#include "lang/types.h"

enum language_mode {
	language_external,
	language_internal,
	language_opts,
	language_mode_count,

	language_extended,
};

enum obj_type {
	/* singleton object types */
	obj_null,
	obj_meson,
	obj_disabler,
	obj_machine, // this won't be a singleton object when cross compilaton is implemented

	/* simple object types */
	obj_bool,
	obj_file,
	obj_feature_opt,

	/* complex object types */
	_obj_aos_start,
	obj_number = _obj_aos_start,
	obj_string,
	obj_array,
	obj_dict,
	obj_compiler,
	obj_build_target,
	obj_custom_target,
	obj_subproject,
	obj_dependency,
	obj_external_program,
	obj_python_installation,
	obj_run_result,
	obj_configuration_data,
	obj_test,
	obj_module,
	obj_install_target,
	obj_environment,
	obj_include_directory,
	obj_option,
	obj_generator,
	obj_generated_list,
	obj_alias_target,
	obj_both_libs,
	obj_source_set,
	obj_source_configuration,
	obj_iterator,

	/* muon-specific objects */
	obj_func,
	obj_typeinfo,

	obj_type_count,
};

/* start of object structs */

struct obj_typeinfo {
	type_tag type, subtype;
};

struct obj_func {
	const char *name;
	struct ast *ast;
	enum language_mode lang_mode;
	uint32_t args_id, block_id, nargs, nkwargs;
	obj kwarg_defaults, src, scope_stack;
	type_tag return_type;
};

enum tgt_type {
	tgt_executable = 1 << 0,
	tgt_static_library = 1 << 1,
	tgt_dynamic_library = 1 << 2,
	tgt_shared_module = 1 << 3,
};
enum tgt_type_count { tgt_type_count = 4, }; // keep in sync

enum feature_opt_state {
	feature_opt_auto,
	feature_opt_enabled,
	feature_opt_disabled,
};

enum module {
	module_fs,
	module_keyval,
	module_pkgconfig,
	module_python,
	module_python3,
	module_sourceset,

	// unimplemented
	module_unimplemented_separator,
	module_cmake = module_unimplemented_separator,
	module_dlang,
	module_gnome,
	module_hotdoc,
	module_i18n,
	module_java,
	module_modtest,
	module_qt,
	module_qt4,
	module_qt5,
	module_qt6,
	module_unstable_cuda,
	module_unstable_external_project,
	module_unstable_icestorm,
	module_unstable_rust,
	module_unstable_simd,
	module_unstable_wayland,
	module_windows,

	module_count,
};

enum str_flags {
	str_flag_big = 1 << 0,
	str_flag_mutable = 1 << 1,
};

struct str {
	const char *s;
	uint32_t len;
	enum str_flags flags;
};

struct obj_internal {
	enum obj_type t;
	uint32_t val;
};

struct obj_subproject {
	uint32_t id;
	bool found;
};

struct obj_module {
	enum module module;
	bool found, has_impl;
	obj exports;
};

struct obj_array {
	obj val; // any
	obj next; // obj_array
	obj tail; // obj_array
	uint32_t len;
	bool have_next;
};

enum obj_dict_flags {
	obj_dict_flag_big = 1 << 0,
	obj_dict_flag_int_key = 1 << 1,
	obj_dict_flag_dont_expand = 1 << 2,
};

struct obj_dict_elem {
	uint32_t next;
	obj key, val;
};

struct obj_dict {
	uint32_t data, len;
	obj tail;
	enum obj_dict_flags flags;
};

enum build_tgt_flags {
	build_tgt_flag_export_dynamic = 1 << 0,
	build_tgt_flag_pic = 1 << 1,
	build_tgt_generated_include = 1 << 2,
	build_tgt_flag_build_by_default = 1 << 3,
	build_tgt_flag_visibility = 1 << 4,
	build_tgt_flag_installed = 1 << 5,
	build_tgt_flag_pie = 1 << 6,
};

struct build_dep {
	enum compiler_language link_language;

	obj link_whole; // obj_array

	obj link_with; // obj_array
	obj link_with_not_found; // obj_array

	obj link_args; // obj_array
	obj compile_args; // obj_array

	obj include_directories; // obj_array

	obj sources; // obj_array
	obj objects; // obj_array

	obj order_deps; // obj_array
	obj rpath; // obj_array

	struct {
		obj deps;
		obj link_with;
		obj link_whole;
	} raw;
};

struct obj_build_target {
	obj name; // obj_string
	obj build_name; // obj_string
	obj build_path; // obj_string
	obj private_path; // obj_string
	obj cwd; // obj_string
	obj build_dir; // obj_string
	obj soname; // obj_string
	obj src; // obj_array
	obj objects; // obj_array
	obj args; // obj_dict
	obj link_depends; // obj_array
	obj generated_pc; // obj_string
	obj override_options; // obj_array
	obj required_compilers; // obj_dict

	struct build_dep dep;
	struct build_dep dep_internal;

	enum compiler_visibility_type visibility;
	enum build_tgt_flags flags;
	enum tgt_type type;
};

struct obj_both_libs {
	obj static_lib; // obj_build_target
	obj dynamic_lib; // obj_build_target
};

enum custom_target_flags {
	custom_target_capture = 1 << 0,
	custom_target_build_always_stale = 1 << 1,
	custom_target_build_by_default = 1 << 2,
	custom_target_feed = 1 << 3,
	custom_target_console = 1 << 4,
};

struct obj_custom_target {
	obj name; // obj_string
	obj args; // obj_array
	obj input; // obj_array
	obj output; // obj_array
	obj depends; // obj_array
	obj private_path; // obj_string
	obj env; // str | list[str] | dict[str] | env
	obj depfile; // str
	enum custom_target_flags flags;
};

struct  obj_alias_target {
	obj name; // obj_string
	obj depends; // obj_array
};

enum dependency_type {
	dependency_type_declared,
	dependency_type_pkgconf,
	dependency_type_threads,
	dependency_type_external_library,
	dependency_type_appleframeworks,
	dependency_type_not_found,
};

enum dep_flags {
	dep_flag_found = 1 << 0,
};

enum include_type {
	include_type_preserve,
	include_type_system,
	include_type_non_system,
};

struct obj_dependency {
	obj name; // obj_string
	obj version; // obj_string
	obj variables; // obj_dict

	struct build_dep dep;

	enum dep_flags flags;
	enum dependency_type type;
	enum include_type include_type;
};

struct obj_external_program {
	bool found, guessed_ver;
	obj cmd_array;
	obj ver;
};

struct obj_python_installation {
	obj prog;

	obj language_version;
	obj sysconfig_paths;
	obj sysconfig_vars;
};

enum run_result_flags {
	run_result_flag_from_compile = 1 << 0,
	run_result_flag_compile_ok = 1 << 1,
};

struct obj_run_result {
	obj out;
	obj err;
	int32_t status;
	enum run_result_flags flags;
};

struct obj_configuration_data {
	obj dict; // obj_dict
};

enum test_category {
	test_category_test,
	test_category_benchmark,
};

enum test_protocol {
	test_protocol_exitcode,
	test_protocol_tap,
	test_protocol_gtest,
	test_protocol_rust,
};

struct obj_test {
	obj name; // obj_string
	obj exe; // obj_string
	obj args; // obj_array
	obj env; // obj_array
	obj suites; // obj_array
	obj workdir; // obj_string
	obj depends; // obj_array of obj_string
	obj timeout; // obj_number
	obj priority; // obj_number
	bool should_fail, is_parallel, verbose;
	enum test_category category;
	enum test_protocol protocol;
};

struct obj_compiler {
	obj cmd_arr;
	obj linker_cmd_arr;
	obj static_linker_cmd_arr;
	obj ver;
	obj libdirs;
	enum compiler_type type;
	enum linker_type linker_type;
	enum static_linker_type static_linker_type;
	enum compiler_language lang;
};

enum install_target_type {
	install_target_default,
	install_target_subdir,
	install_target_symlink,
	install_target_emptydir,
};

struct obj_install_target {
	obj src;
	obj dest;
	bool has_perm;
	uint32_t perm;
	obj exclude_directories; // obj_array of obj_string
	obj exclude_files; // obj_array of obj_string
	enum install_target_type type;
	bool build_target;
};

struct obj_environment {
	obj actions; // array
};

struct obj_include_directory {
	obj path;
	bool is_system;
};

enum build_option_type {
	op_string,
	op_boolean,
	op_combo,
	op_integer,
	op_array,
	op_feature,
	build_option_type_count,
};

enum build_option_kind {
	build_option_kind_default,
	build_option_kind_prefixed_dir,
};

enum option_value_source {
	option_value_source_unset,
	option_value_source_default,
	option_value_source_environment,
	option_value_source_yield,
	option_value_source_default_options,
	option_value_source_subproject_default_options,
	option_value_source_override_options,
	option_value_source_deprecated_rename,
	option_value_source_commandline,
};

struct obj_option {
	obj name;
	obj val;
	obj choices;
	obj max;
	obj min;
	obj deprecated;
	obj description;
	enum option_value_source source;
	enum build_option_type type;
	enum build_option_kind kind;
	bool yield, builtin;
};

struct obj_generator {
	obj output;
	obj raw_command;
	obj depfile;
	obj depends;
	bool capture;
	bool feed;
};

struct obj_generated_list {
	obj generator; // obj_generator
	obj input; // obj_array of obj_file
	obj extra_arguments; // obj_array of obj_string
	obj preserve_path_from; // obj_string
};

struct obj_source_set {
	obj rules;
	bool frozen;
};

struct obj_source_configuration {
	obj sources, dependencies;
};

enum obj_iterator_type {
	obj_iterator_type_array,
	obj_iterator_type_dict_small,
	obj_iterator_type_dict_big,
	obj_iterator_type_range,
};

struct range_params {
	uint32_t start, stop, step;
};

struct obj_iterator {
	enum obj_iterator_type type;
	union {
		struct obj_array *array;
		struct obj_dict_elem *dict_small;
		struct {
			struct obj_dict *d;
			uint32_t i;
		} dict_big;
		struct range_params range;
	} data;
};

/* end of object structs */

struct obj_clear_mark {
	uint32_t obji;
	struct bucket_arr_save objs, chrs;
	struct bucket_arr_save obj_aos[obj_type_count - _obj_aos_start];
};

void make_obj(struct workspace *wk, obj *id, enum obj_type type);
enum obj_type get_obj_type(struct workspace *wk, obj id);

void obj_set_clear_mark(struct workspace *wk, struct obj_clear_mark *mk);
void obj_clear(struct workspace *wk, const struct obj_clear_mark *mk);

bool get_obj_bool(struct workspace *wk, obj o);
void set_obj_bool(struct workspace *wk, obj o, bool v);
int64_t get_obj_number(struct workspace *wk, obj o);
void set_obj_number(struct workspace *wk, obj o, int64_t v);
obj *get_obj_file(struct workspace *wk, obj o);
const char *get_file_path(struct workspace *wk, obj o);
const struct str *get_str(struct workspace *wk, obj s);
enum feature_opt_state get_obj_feature_opt(struct workspace *wk, obj fo);
void set_obj_feature_opt(struct workspace *wk, obj fo, enum feature_opt_state state);

#define OBJ_GETTER(type) struct type *get_ ## type(struct workspace *wk, obj o)

OBJ_GETTER(obj_array);
OBJ_GETTER(obj_dict);
OBJ_GETTER(obj_compiler);
OBJ_GETTER(obj_build_target);
OBJ_GETTER(obj_custom_target);
OBJ_GETTER(obj_subproject);
OBJ_GETTER(obj_dependency);
OBJ_GETTER(obj_external_program);
OBJ_GETTER(obj_python_installation);
OBJ_GETTER(obj_run_result);
OBJ_GETTER(obj_configuration_data);
OBJ_GETTER(obj_test);
OBJ_GETTER(obj_module);
OBJ_GETTER(obj_install_target);
OBJ_GETTER(obj_environment);
OBJ_GETTER(obj_include_directory);
OBJ_GETTER(obj_option);
OBJ_GETTER(obj_generator);
OBJ_GETTER(obj_generated_list);
OBJ_GETTER(obj_alias_target);
OBJ_GETTER(obj_both_libs);
OBJ_GETTER(obj_typeinfo);
OBJ_GETTER(obj_func);
OBJ_GETTER(obj_source_set);
OBJ_GETTER(obj_source_configuration);
OBJ_GETTER(obj_iterator);

#undef OBJ_GETTER

struct sbuf;

const char *obj_type_to_s(enum obj_type t);
bool s_to_type_tag(const char *s, type_tag *t);
void obj_to_s(struct workspace *wk, obj o, struct sbuf *sb);
bool obj_equal(struct workspace *wk, obj left, obj right);
bool obj_clone(struct workspace *wk_src, struct workspace *wk_dest, obj val, obj *ret);

#define LO(...) if (log_should_print(log_debug)) { char buf[4096]; log_print_prefix(log_debug, buf, ARRAY_LEN(buf)); log_plain("%s", buf); obj_fprintf(wk, log_file(), __VA_ARGS__); }
#define LOBJ(object_id) LO("%s: %o\n", #object_id, object_id)

bool obj_vasprintf(struct workspace *wk, struct sbuf *sb, const char *fmt, va_list ap);
bool obj_asprintf(struct workspace *wk, struct sbuf *sb, const char *fmt, ...)
MUON_ATTR_FORMAT(printf, 3, 4);
bool obj_vfprintf(struct workspace *wk, FILE *f, const char *fmt, va_list ap)
MUON_ATTR_FORMAT(printf, 3, 0);
bool obj_fprintf(struct workspace *wk, FILE *f, const char *fmt, ...)
MUON_ATTR_FORMAT(printf, 3, 4);
bool obj_printf(struct workspace *wk, const char *fmt, ...)
MUON_ATTR_FORMAT(printf, 2, 3);
uint32_t obj_vsnprintf(struct workspace *wk, char *buf, uint32_t len, const char *fmt, va_list ap);
uint32_t obj_snprintf(struct workspace *wk, char *buf, uint32_t len, const char *fmt, ...)
MUON_ATTR_FORMAT(printf, 4, 5);
void obj_inspect(struct workspace *wk, FILE *out, obj val);

typedef enum iteration_result (*obj_array_iterator)(struct workspace *wk, void *ctx, obj val);
void obj_array_push(struct workspace *wk, obj arr, obj child);
void obj_array_prepend(struct workspace *wk, obj *arr, obj val);
bool obj_array_foreach(struct workspace *wk, obj arr, void *ctx, obj_array_iterator cb);
bool obj_array_foreach_flat(struct workspace *wk, obj arr, void *usr_ctx, obj_array_iterator cb);
bool obj_array_in(struct workspace *wk, obj arr, obj val);
bool obj_array_index_of(struct workspace *wk, obj arr, obj val, uint32_t *idx);
void obj_array_index(struct workspace *wk, obj arr, int64_t i, obj *res);
void obj_array_extend(struct workspace *wk, obj arr, obj arr2);
void obj_array_extend_nodup(struct workspace *wk, obj arr, obj arr2);
void obj_array_dup(struct workspace *wk, obj arr, obj *res);
bool obj_array_join(struct workspace *wk, bool flat, obj arr, obj join, obj *res);
void obj_array_tail(struct workspace *wk, obj arr, obj *res);
void obj_array_set(struct workspace *wk, obj arr, int64_t i, obj v);
void obj_array_del(struct workspace *wk, obj arr, int64_t i);
void obj_array_dedup(struct workspace *wk, obj arr, obj *res);
void obj_array_dedup_in_place(struct workspace *wk, obj *arr);
bool obj_array_flatten_one(struct workspace *wk, obj val, obj *res);
typedef int32_t (*obj_array_sort_func)(struct workspace *wk, void *_ctx, obj a, obj b);
int32_t obj_array_sort_by_str(struct workspace *wk, void *_ctx, obj a, obj b);
void obj_array_sort(struct workspace *wk, void *usr_ctx, obj arr, obj_array_sort_func func, obj *res);
obj obj_array_slice(struct workspace *wk, obj arr, int64_t i0, int64_t i1);
obj obj_array_get_tail(struct workspace *wk, obj arr);
obj obj_array_pop(struct workspace *wk, obj arr);

typedef enum iteration_result (*obj_dict_iterator)(struct workspace *wk, void *ctx, obj key, obj val);
bool obj_dict_foreach(struct workspace *wk, obj dict, void *ctx, obj_dict_iterator cb);
bool obj_dict_in(struct workspace *wk, obj dict, obj key);
bool obj_dict_index(struct workspace *wk, obj dict, obj key, obj *res);
bool obj_dict_index_strn(struct workspace *wk, obj dict, const char *str, uint32_t len, obj *res);
bool obj_dict_index_str(struct workspace *wk, obj dict, const char *str, obj *res);
void obj_dict_set(struct workspace *wk, obj dict, obj key, obj val);
void obj_dict_dup(struct workspace *wk, obj dict, obj *res);
void obj_dict_merge(struct workspace *wk, obj dict, obj dict2, obj *res);
void obj_dict_merge_nodup(struct workspace *wk, obj dict, obj dict2);
void obj_dict_seti(struct workspace *wk, obj dict, uint32_t key, obj val);
bool obj_dict_geti(struct workspace *wk, obj dict, uint32_t key, obj *val);
void obj_dict_del(struct workspace *wk, obj dict, obj key);
void obj_dict_del_str(struct workspace *wk, obj dict, const char *str);
void obj_dict_del_strn(struct workspace *wk, obj dict, const char *str, uint32_t len);

bool obj_iterable_foreach(struct workspace *wk, obj dict_or_array, void *ctx, obj_dict_iterator cb);
#endif
