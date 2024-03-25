/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: illiliti <illiliti@dimension.sh>
 * SPDX-FileCopyrightText: Owen Rafferty <owen@owenrafferty.com>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_COMPILERS_H
#define MUON_COMPILERS_H

#include <stdbool.h>
#include <stdint.h>

#include "lang/types.h"

struct workspace;

enum compiler_type {
	compiler_posix,
	compiler_gcc,
	compiler_clang,
	compiler_apple_clang,
	compiler_clang_llvm_ir,
	compiler_clang_cl,
	compiler_msvc,
	compiler_nasm,
	compiler_yasm,
	compiler_type_count,
};

enum linker_type {
	linker_posix,
	linker_gcc,
	linker_clang,
	linker_apple,
	linker_lld_link,
	linker_msvc,
	linker_type_count,
};

enum static_linker_type {
	static_linker_ar_posix,
	static_linker_ar_gcc,
	static_linker_msvc,
	static_linker_type_count
};

enum compiler_language {
	compiler_language_null,
	compiler_language_c,
	compiler_language_c_hdr,
	compiler_language_cpp,
	compiler_language_cpp_hdr,
	compiler_language_c_obj,
	compiler_language_objc,
	compiler_language_assembly,
	compiler_language_llvm_ir,
	compiler_language_nasm,
	compiler_language_count,
};

enum compiler_deps_type {
	compiler_deps_none,
	compiler_deps_gcc,
	compiler_deps_msvc,
};

enum compiler_optimization_lvl {
	compiler_optimization_lvl_none,
	compiler_optimization_lvl_0,
	compiler_optimization_lvl_1,
	compiler_optimization_lvl_2,
	compiler_optimization_lvl_3,
	compiler_optimization_lvl_g,
	compiler_optimization_lvl_s,
};

enum compiler_pgo_stage {
	compiler_pgo_generate,
	compiler_pgo_use,
};

enum compiler_warning_lvl {
	compiler_warning_lvl_0,
	compiler_warning_lvl_1,
	compiler_warning_lvl_2,
	compiler_warning_lvl_3,
	compiler_warning_lvl_everything,
};

enum compiler_visibility_type {
	compiler_visibility_default,
	compiler_visibility_hidden,
	compiler_visibility_internal,
	compiler_visibility_protected,
	compiler_visibility_inlineshidden,
};

typedef const struct args *((*compiler_get_arg_func_0)(void));
typedef const struct args *((*compiler_get_arg_func_1i)(uint32_t));
typedef const struct args *((*compiler_get_arg_func_1s)(const char *));
typedef const struct args *((*compiler_get_arg_func_2s)(const char *, const char *));

struct compiler {
	struct {
		compiler_get_arg_func_2s deps;
		compiler_get_arg_func_0 compile_only;
		compiler_get_arg_func_0 preprocess_only;
		compiler_get_arg_func_1s output;
		compiler_get_arg_func_1i optimization;
		compiler_get_arg_func_0 debug;
		compiler_get_arg_func_1i warning_lvl;
		compiler_get_arg_func_0 warn_everything;
		compiler_get_arg_func_0 werror;
		compiler_get_arg_func_1s set_std;
		compiler_get_arg_func_1s include;
		compiler_get_arg_func_1s include_system;
		compiler_get_arg_func_1i pgo;
		compiler_get_arg_func_0 pic;
		compiler_get_arg_func_0 pie;
		compiler_get_arg_func_1s sanitize;
		compiler_get_arg_func_1s define;
		compiler_get_arg_func_1i visibility;
		compiler_get_arg_func_1s specify_lang;
		compiler_get_arg_func_1s color_output;
		compiler_get_arg_func_0 enable_lto;
		compiler_get_arg_func_0 always;
	} args;
	enum compiler_deps_type deps;
	enum linker_type default_linker;
	enum static_linker_type default_static_linker;
	const char *object_ext;
};

struct linker {
	struct {
		compiler_get_arg_func_1s lib;
		compiler_get_arg_func_0 as_needed;
		compiler_get_arg_func_0 no_undefined;
		compiler_get_arg_func_0 start_group;
		compiler_get_arg_func_0 end_group;
		compiler_get_arg_func_0 shared;
		compiler_get_arg_func_1s soname;
		compiler_get_arg_func_1s rpath;
		compiler_get_arg_func_1i pgo;
		compiler_get_arg_func_1s sanitize;
		compiler_get_arg_func_0 allow_shlib_undefined;
		compiler_get_arg_func_0 export_dynamic;
		compiler_get_arg_func_0 fatal_warnings;
		compiler_get_arg_func_0 whole_archive;
		compiler_get_arg_func_0 no_whole_archive;
		compiler_get_arg_func_0 enable_lto;
		compiler_get_arg_func_2s input_output;
		compiler_get_arg_func_0 always;
	} args;
};

struct static_linker {
	struct {
		compiler_get_arg_func_0 base;
		compiler_get_arg_func_2s input_output;
	} args;
};

struct language {
	bool is_header;
	bool is_linkable;
};

extern struct compiler compilers[];
extern struct linker linkers[];
extern struct static_linker static_linkers[];
extern const struct language languages[];

const char *compiler_type_to_s(enum compiler_type t);
const char *linker_type_to_s(enum linker_type t);
const char *compiler_language_to_s(enum compiler_language l);
bool s_to_compiler_language(const char *s, enum compiler_language *l);
bool filename_to_compiler_language(const char *str, enum compiler_language *l);
const char *compiler_language_extension(enum compiler_language l);
enum compiler_language coalesce_link_languages(enum compiler_language cur, enum compiler_language new);

bool toolchain_detect(struct workspace *wk, obj *comp, enum compiler_language lang);
void compilers_init(void);
#endif
