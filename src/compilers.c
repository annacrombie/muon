/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: illiliti <illiliti@dimension.sh>
 * SPDX-FileCopyrightText: Owen Rafferty <owen@owenrafferty.com>
 * SPDX-FileCopyrightText: illiliti <illiliti@thunix.net>
 * SPDX-FileCopyrightText: Vincent Torri <vincent.torri@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "buf_size.h"
#include "compilers.h"
#include "error.h"
#include "functions/machine.h"
#include "guess.h"
#include "lang/workspace.h"
#include "log.h"
#include "options.h"
#include "platform/path.h"
#include "platform/run_cmd.h"

const char *
compiler_type_to_s(enum compiler_type t)
{
	switch (t) {
	case compiler_posix: return "posix";
	case compiler_gcc: return "gcc";
	case compiler_clang: return "clang";
	case compiler_apple_clang: return "clang";
	case compiler_clang_llvm_ir: return "clang";
	case compiler_clang_cl: return "clang-cl";
	case compiler_msvc: return "msvc";
	case compiler_nasm: return "nasm";
	case compiler_yasm: return "yasm";
	case compiler_type_count: UNREACHABLE;
	}

	UNREACHABLE_RETURN;
}

const char *
linker_type_to_s(enum linker_type t)
{
	switch (t) {
	case linker_posix: return "ld";
	case linker_gcc: return "ld.bfd";
	case linker_clang: return "lld";
	case linker_apple: return "ld64";
	case linker_lld_link: return "lld-link";
	case linker_msvc: return "link";
	case linker_type_count: UNREACHABLE;
	}

	UNREACHABLE_RETURN;
}

const char *
static_linker_type_to_s(enum static_linker_type t)
{
	switch (t) {
	case static_linker_ar_posix: return "ar";
	case static_linker_ar_gcc: return "ar (gcc)";
	case static_linker_msvc: return "lib";
	case static_linker_type_count: UNREACHABLE;
	}

	UNREACHABLE_RETURN;
}

static const char *compiler_language_names[compiler_language_count] = {
	[compiler_language_null] = "null",
	[compiler_language_c] = "c",
	[compiler_language_c_hdr] = "c_hdr",
	[compiler_language_cpp] = "cpp",
	[compiler_language_cpp_hdr] = "cpp_hdr",
	[compiler_language_c_obj] = "c_obj",
	[compiler_language_objc] = "objc",
	[compiler_language_assembly] = "assembly",
	[compiler_language_llvm_ir] = "llvm_ir",
	[compiler_language_nasm] = "nasm",
};

const char *
compiler_language_to_s(enum compiler_language l)
{
	assert(l < compiler_language_count);
	return compiler_language_names[l];
}

bool
s_to_compiler_language(const char *s, enum compiler_language *l)
{
	uint32_t i;
	for (i = 0; i < compiler_language_count; ++i) {
		if (strcmp(s, compiler_language_names[i]) == 0) {
			*l = i;
			return true;
		}
	}

	return false;
}

static const char *compiler_language_exts[compiler_language_count][10] = {
	[compiler_language_c] = { "c" },
	[compiler_language_c_hdr] = { "h" },
	[compiler_language_cpp] = { "cc", "cpp", "cxx", "C" },
	[compiler_language_cpp_hdr] = { "hh", "hpp", "hxx" },
	[compiler_language_c_obj] = { "o", "obj" },
	[compiler_language_objc] = { "m", "mm", "M" },
	[compiler_language_assembly] = { "S" },
	[compiler_language_llvm_ir] = { "ll" },
	[compiler_language_nasm] = { "asm" },
};

bool
filename_to_compiler_language(const char *str, enum compiler_language *l)
{
	uint32_t i, j;
	const char *ext;

	if (!(ext = strrchr(str, '.'))) {
		return false;
	}
	++ext;

	for (i = 0; i < compiler_language_count; ++i) {
		for (j = 0; compiler_language_exts[i][j]; ++j) {
			if (strcmp(ext, compiler_language_exts[i][j]) == 0) {
				*l = i;
				return true;
			}
		}
	}

	return false;
}

const char *
compiler_language_extension(enum compiler_language l)
{
	return compiler_language_exts[l][0];
}

enum compiler_language
coalesce_link_languages(enum compiler_language cur, enum compiler_language new)
{
	switch (new) {
	case compiler_language_null:
	case compiler_language_c_hdr:
	case compiler_language_cpp_hdr:
	case compiler_language_llvm_ir:
		break;
	case compiler_language_assembly:
		if (!cur) {
			return compiler_language_assembly;
		}
		break;
	case compiler_language_nasm:
	case compiler_language_c:
	case compiler_language_c_obj:
	case compiler_language_objc:
		if (!cur) {
			return compiler_language_c;
		}
		break;
	case compiler_language_cpp:
		if (!cur
		    || cur == compiler_language_c
		    || cur == compiler_language_assembly) {
			return compiler_language_cpp;
		}
		break;
	case compiler_language_count:
		UNREACHABLE;
	}

	return cur;
}

static bool
run_cmd_arr(struct workspace *wk, struct run_cmd_ctx *cmd_ctx, obj cmd_arr, const char *arg)
{
	obj args;
	obj_array_dup(wk, cmd_arr, &args);
	obj_array_push(wk, args, make_str(wk, arg));

	const char *argstr;
	uint32_t argc;
	join_args_argstr(wk, &argstr, &argc, args);

	if (!run_cmd(cmd_ctx, argstr, argc, NULL, 0)) {
		run_cmd_ctx_destroy(cmd_ctx);
		return false;
	}

	return true;
}

static const char *
guess_version_arg(struct workspace *wk, bool msvc_like)
{
	if (msvc_like) {
		return "/?";
	} else {
		return "--version";
	}
}

static bool
compiler_detect_c_or_cpp(struct workspace *wk, obj cmd_arr, obj comp_id)
{
	bool msvc_like = obj_array_in(wk, cmd_arr, make_str(wk, "cl"));

	// helpful: mesonbuild/compilers/detect.py:350
	struct run_cmd_ctx cmd_ctx = { 0 };
	if (!run_cmd_arr(wk, &cmd_ctx, cmd_arr, guess_version_arg(wk, msvc_like))) {
		run_cmd_ctx_destroy(&cmd_ctx);
		return false;
	}

	enum compiler_type type;
	bool unknown = true;
	obj ver;

	if (cmd_ctx.status != 0) {
		goto detection_over;
	}

	if (strstr(cmd_ctx.out.buf, "Apple") && strstr(cmd_ctx.out.buf, "clang")) {
		type = compiler_apple_clang;
	} else if (strstr(cmd_ctx.out.buf, "clang") || strstr(cmd_ctx.out.buf, "Clang")) {
		if (strstr(cmd_ctx.out.buf, "msvc")) {
			type = compiler_clang_cl;
		} else {
			type = compiler_clang;
		}
	} else if (strstr(cmd_ctx.out.buf, "Free Software Foundation")) {
		type = compiler_gcc;
	} else if (strstr(cmd_ctx.out.buf, "Microsoft")) {
		type = compiler_msvc;
	} else {
		goto detection_over;
	}

	if (!guess_version(wk, (type == compiler_msvc) ? cmd_ctx.err.buf : cmd_ctx.out.buf, &ver)) {
		ver = make_str(wk, "unknown");
	}

	unknown = false;

detection_over:
	if (unknown) {
		LOG_W("unable to detect compiler type, falling back on posix compiler");
		type = compiler_posix;
		ver = make_str(wk, "unknown");
	}

	struct obj_compiler *comp = get_obj_compiler(wk, comp_id);
	comp->cmd_arr = cmd_arr;
	comp->type = type;
	comp->ver = ver;

	run_cmd_ctx_destroy(&cmd_ctx);
	return true;
}

static bool
compiler_detect_nasm(struct workspace *wk, obj cmd_arr, obj comp_id)
{
	struct run_cmd_ctx cmd_ctx = { 0 };
	if (!run_cmd_arr(wk, &cmd_ctx, cmd_arr, "--version")) {
		run_cmd_ctx_destroy(&cmd_ctx);
		return false;
	}

	enum compiler_type type;
	obj ver;

	if (strstr(cmd_ctx.out.buf, "NASM")) {
		type = compiler_nasm;
	} else if (strstr(cmd_ctx.out.buf, "yasm")) {
		type = compiler_yasm;
	} else {
		// Just assume it is nasm
		type = compiler_nasm;
	}

	if (!guess_version(wk, cmd_ctx.out.buf, &ver)) {
		ver = make_str(wk, "unknown");
	}

	obj new_cmd;
	obj_array_dup(wk, cmd_arr, &new_cmd);

	{
		uint32_t addr_bits = machine_cpu_address_bits();
		enum machine_system sys = machine_system();

		const char *plat;
		SBUF(define);

		if (sys == machine_system_windows || sys == machine_system_cygwin) {
			plat = "win";
			sbuf_pushf(wk, &define, "WIN%d", addr_bits);
		} else if (sys == machine_system_darwin) {
			plat = "macho";
			sbuf_pushs(wk, &define, "MACHO");
		} else {
			plat = "elf";
			sbuf_pushs(wk, &define, "ELF");
		}

		obj_array_push(wk, new_cmd, make_strf(wk, "-f%s%d", plat, addr_bits));
		obj_array_push(wk, new_cmd, make_strf(wk, "-D%s", define.buf));
		if (addr_bits == 64) {
			obj_array_push(wk, new_cmd, make_str(wk, "-D__x86_64__"));
		}
	}

	struct obj_compiler *comp = get_obj_compiler(wk, comp_id);
	comp->cmd_arr = new_cmd;
	comp->type = type;
	comp->ver = ver;
	comp->lang = compiler_language_nasm;

	run_cmd_ctx_destroy(&cmd_ctx);
	return true;
}

static bool
compiler_get_libdirs(struct workspace *wk, struct obj_compiler *comp)
{
	struct run_cmd_ctx cmd_ctx = { 0 };
	if (!run_cmd_arr(wk, &cmd_ctx, comp->cmd_arr, "-print-search-dirs")
	    || cmd_ctx.status) {
		goto done;
	}

	const char *key = "libraries: ";
	char *s, *e;
	bool beginning_of_line = true;
	for (s = cmd_ctx.out.buf; *s; ++s) {
		if (beginning_of_line && strncmp(s, key, strlen(key)) == 0) {
			s += strlen(key);
			if (*s == '=') {
				++s;
			}

			e = strchr(s, '\n');

			struct str str = {
				.s = s,
				.len = e ? (uint32_t)(e - s) : strlen(s),
			};

			comp->libdirs = str_split(wk, &str, &WKSTR(ENV_PATH_SEP_STR));
			goto done;
		}

		beginning_of_line = *s == '\n';
	}

done:
	run_cmd_ctx_destroy(&cmd_ctx);

	if (!comp->libdirs) {
		const char *libdirs[] = {
			"/usr/lib",
			"/usr/local/lib",
			"/lib",
			NULL
		};

		make_obj(wk, &comp->libdirs, obj_array);

		uint32_t i;
		for (i = 0; libdirs[i]; ++i) {
			obj_array_push(wk, comp->libdirs, make_str(wk, libdirs[i]));
		}
	}

	return true;
}

static bool
compiler_detect_cmd_arr(struct workspace *wk, obj comp, enum compiler_language lang, obj cmd_arr)
{
	if (log_should_print(log_debug)) {
		obj_fprintf(wk, log_file(), "checking compiler %o\n", cmd_arr);
	}

	switch (lang) {
	case compiler_language_c:
	case compiler_language_cpp:
	case compiler_language_objc:
		if (!compiler_detect_c_or_cpp(wk, cmd_arr, comp)) {
			return false;
		}

		struct obj_compiler *compiler = get_obj_compiler(wk, comp);
		compiler_get_libdirs(wk, compiler);
		compiler->lang = lang;
		return true;
	case compiler_language_nasm:
		if (!compiler_detect_nasm(wk, cmd_arr, comp)) {
			return false;
		}
		return true;
	default:
		LOG_E("tried to get a compiler for unsupported language '%s'", compiler_language_to_s(lang));
		return false;
	}
}

static bool
static_linker_detect(struct workspace *wk, obj comp, enum compiler_language lang, obj cmd_arr)
{
	struct obj_compiler *compiler = get_obj_compiler(wk, comp);

	struct run_cmd_ctx cmd_ctx = { 0 };
	if (!run_cmd_arr(wk, &cmd_ctx, cmd_arr, guess_version_arg(wk, compiler->type == compiler_msvc))) {
		run_cmd_ctx_destroy(&cmd_ctx);
		return false;
	}

	enum static_linker_type type = compiler->type == compiler_msvc ? static_linker_msvc : static_linker_ar_posix;

	if (cmd_ctx.status == 0 && strstr(cmd_ctx.out.buf, "Free Software Foundation")) {
		type = static_linker_ar_gcc;
	}

	run_cmd_ctx_destroy(&cmd_ctx);

	get_obj_compiler(wk, comp)->static_linker_cmd_arr = cmd_arr;
	get_obj_compiler(wk, comp)->static_linker_type = type;
	return true;
}

static bool
linker_detect(struct workspace *wk, obj comp, enum compiler_language lang, obj cmd_arr)
{
	struct obj_compiler *compiler = get_obj_compiler(wk, comp);

	struct run_cmd_ctx cmd_ctx = { 0 };
	if (!run_cmd_arr(wk, &cmd_ctx, cmd_arr, guess_version_arg(wk, compiler->type == compiler_msvc))) {
		run_cmd_ctx_destroy(&cmd_ctx);
		return false;
	}

	enum linker_type type = compilers[compiler->type].default_linker;

	// TODO: do something with command output?

	run_cmd_ctx_destroy(&cmd_ctx);

	get_obj_compiler(wk, comp)->linker_cmd_arr = cmd_arr;
	get_obj_compiler(wk, comp)->linker_type = type;
	return true;
}

typedef bool ((*toolchain_detect_cmd_arr_cb)(struct workspace *wk, obj comp, enum compiler_language lang, obj cmd_arr));

bool
toolchain_exe_detect(struct workspace *wk,
	const char *toolchain_exe_option_name,
	const char *exe_list[],
	obj comp, enum compiler_language lang, toolchain_detect_cmd_arr_cb cb)
{
	if (!toolchain_exe_option_name) {
		return false;
	}

	obj cmd_arr_opt;
	get_option(wk, NULL, &WKSTR(toolchain_exe_option_name), &cmd_arr_opt);
	struct obj_option *cmd_arr = get_obj_option(wk, cmd_arr_opt);

	if (cmd_arr->source > option_value_source_default) {
		return cb(wk, comp, lang, cmd_arr->val);
	}

	uint32_t i;
	for (i = 0; exe_list[i]; ++i) {
		obj cmd_arr;
		make_obj(wk, &cmd_arr, obj_array);
		obj_array_push(wk, cmd_arr, make_str(wk, exe_list[i]));

		if (cb(wk, comp, lang, cmd_arr)) {
			return true;
		}
	}

	return false;
}

static bool
toolchain_linker_detect(struct workspace *wk, obj comp, enum compiler_language lang)
{
	struct obj_compiler *compiler = get_obj_compiler(wk, comp);

	const char *exe_list[] = {
		linker_type_to_s(compilers[compiler->type].default_linker),
		"ld",
		NULL
	};

	return toolchain_exe_detect(wk, "env.LD", exe_list, comp, lang, linker_detect);
}

static bool
toolchain_static_linker_detect(struct workspace *wk, obj comp, enum compiler_language lang)
{
	const char **exe_list = NULL;

	struct obj_compiler *compiler = get_obj_compiler(wk, comp);

	if (compiler->type == compiler_msvc) {
		static const char *msvc_list[] = { "lib", NULL };
		exe_list = msvc_list;
	} else {
		static const char *default_list[] = { "ar", NULL };
		exe_list = default_list;
	}

	return toolchain_exe_detect(wk, "env.AR", exe_list, comp, lang, static_linker_detect);
}

static bool
toolchain_compiler_detect(struct workspace *wk, obj comp, enum compiler_language lang)
{
	static const char *compiler_option[compiler_language_count] = {
		[compiler_language_c] = "env.CC",
		[compiler_language_cpp] = "env.CXX",
		[compiler_language_objc] = "env.OBJC",
		[compiler_language_nasm] = "env.NASM",
	};

	const char **exe_list = NULL;

	if (machine_system() == machine_system_windows) {
		static const char *default_executables[][compiler_language_count] = {
			[compiler_language_c] = { "cl", "cc", "gcc", "clang", "clang-cl", NULL },
			[compiler_language_cpp] = { "cl", "c++", "g++", "clang++", "clang-cl", NULL },
			[compiler_language_objc] = { "cc", "gcc", NULL },
			[compiler_language_nasm] = { "nasm", "yasm", NULL },
		};

		exe_list = default_executables[lang];
	} else {
		static const char *default_executables[][compiler_language_count] = {
			[compiler_language_c] = { "cc", "gcc", "clang", NULL },
			[compiler_language_cpp] = { "c++", "g++", "clang++", NULL },
			[compiler_language_objc] = { "cc", "gcc", "clang", NULL },
			[compiler_language_nasm] = { "nasm", "yasm", NULL },
		};

		exe_list = default_executables[lang];
	}

	return toolchain_exe_detect(wk, compiler_option[lang], exe_list, comp, lang, compiler_detect_cmd_arr);
}

bool
toolchain_detect(struct workspace *wk, obj *comp, enum compiler_language lang)
{
	make_obj(wk, comp, obj_compiler);

	if (!toolchain_compiler_detect(wk, *comp, lang)) {
		return false;
	}

	if (!toolchain_linker_detect(wk, *comp, lang)) {
		return false;
	}

	if (!toolchain_static_linker_detect(wk, *comp, lang)) {
		return false;
	}

	struct obj_compiler *compiler = get_obj_compiler(wk, *comp);

	LLOG_I("detected compiler %s ", compiler_type_to_s(compiler->type));
	obj_fprintf(wk, log_file(), "%o (%o), linker: %s (%o), static_linker: %s (%o)\n",
		compiler->ver, compiler->cmd_arr,
		linker_type_to_s(compiler->linker_type), compiler->linker_cmd_arr,
		static_linker_type_to_s(compiler->static_linker_type), compiler->static_linker_cmd_arr
		);

	return true;
}

#define COMPILER_ARGS(...) \
	static const char *argv[] = __VA_ARGS__; \
	static struct args args = { \
		.args = argv, \
		.len = ARRAY_LEN(argv) \
	};

/* posix compilers */

static const struct args *
compiler_posix_args_compile_only(void)
{
	COMPILER_ARGS({ "-c" });

	return &args;
}

static const struct args *
compiler_posix_args_preprocess_only(void)
{
	COMPILER_ARGS({ "-E" });

	return &args;
}

static const struct args *
compiler_posix_args_output(const char *f)
{
	COMPILER_ARGS({ "-o", NULL });

	argv[1] = f;

	return &args;
}

static const struct args *
compiler_posix_args_optimization(uint32_t lvl)
{
	COMPILER_ARGS({ NULL });

	switch ((enum compiler_optimization_lvl)lvl) {
	case compiler_optimization_lvl_0:
		argv[0] = "-O0";
		break;
	case compiler_optimization_lvl_1:
	case compiler_optimization_lvl_2:
	case compiler_optimization_lvl_3:
		argv[0] = "-O1";
		break;
	case compiler_optimization_lvl_none:
	case compiler_optimization_lvl_g:
	case compiler_optimization_lvl_s:
		args.len = 0;
		break;
	}

	return &args;
}

static const struct args *
compiler_posix_args_debug(void)
{
	COMPILER_ARGS({ "-g" });

	return &args;
}

static const struct args *
compiler_posix_args_include(const char *dir)
{
	COMPILER_ARGS({ "-I", NULL });

	argv[1] = dir;

	return &args;
}

static const struct args *
compiler_posix_args_define(const char *define)
{
	COMPILER_ARGS({ "-D", NULL });

	argv[1] = define;

	return &args;
}

/* gcc compilers */

static const struct args *
compiler_gcc_args_preprocess_only(void)
{
	COMPILER_ARGS({ "-E", "-P" });

	return &args;
}

static const struct args *
compiler_gcc_args_include_system(const char *dir)
{
	COMPILER_ARGS({ "-isystem", NULL });

	argv[1] = dir;

	return &args;
}

static const struct args *
compiler_gcc_args_deps(const char *out_target, const char *out_file)
{
	COMPILER_ARGS({ "-MD", "-MQ", NULL, "-MF", NULL });

	argv[2] = out_target;
	argv[4] = out_file;

	return &args;
}

static const struct args *
compiler_gcc_args_optimization(uint32_t lvl)
{
	COMPILER_ARGS({ NULL });

	switch ((enum compiler_optimization_lvl)lvl) {
	case compiler_optimization_lvl_none:
		args.len = 0;
		break;
	case compiler_optimization_lvl_0:
		argv[0] = "-O0";
		break;
	case compiler_optimization_lvl_1:
		argv[0] = "-O1";
		break;
	case compiler_optimization_lvl_2:
		argv[0] = "-O2";
		break;
	case compiler_optimization_lvl_3:
		argv[0] = "-O3";
		break;
	case compiler_optimization_lvl_g:
		argv[0] = "-Og";
		break;
	case compiler_optimization_lvl_s:
		argv[0] = "-Os";
		break;
	}

	return &args;
}

static const struct args *
compiler_gcc_args_warning_lvl(uint32_t lvl)
{
	COMPILER_ARGS({ NULL, NULL, NULL });

	args.len = 0;

	switch ((enum compiler_warning_lvl)lvl) {
	case compiler_warning_lvl_everything:
		UNREACHABLE;
		break;
	case compiler_warning_lvl_3:
		argv[args.len] = "-Wpedantic";
		++args.len;
	/* fallthrough */
	case compiler_warning_lvl_2:
		argv[args.len] = "-Wextra";
		++args.len;
	/* fallthrough */
	case compiler_warning_lvl_1:
		argv[args.len] = "-Wall";
		++args.len;
	/* fallthrough */
	case compiler_warning_lvl_0:
		break;
	}

	return &args;
}

static const struct args *
compiler_gcc_args_werror(void)
{
	COMPILER_ARGS({ "-Werror" });
	return &args;
}

static const struct args *
compiler_gcc_args_warn_everything(void)
{
	COMPILER_ARGS({ "-Wall", "-Winvalid-pch", "-Wextra", "-Wpedantic",
			"-Wcast-qual", "-Wconversion", "-Wfloat-equal",
			"-Wformat=2", "-Winline", "-Wmissing-declarations",
			"-Wredundant-decls", "-Wshadow", "-Wundef",
			"-Wuninitialized", "-Wwrite-strings",
			"-Wdisabled-optimization", "-Wpacked", "-Wpadded",
			"-Wmultichar", "-Wswitch-default", "-Wswitch-enum",
			"-Wunused-macros", "-Wmissing-include-dirs",
			"-Wunsafe-loop-optimizations", "-Wstack-protector",
			"-Wstrict-overflow=5", "-Warray-bounds=2",
			"-Wlogical-op", "-Wstrict-aliasing=3", "-Wvla",
			"-Wdouble-promotion", "-Wsuggest-attribute=const",
			"-Wsuggest-attribute=noreturn",
			"-Wsuggest-attribute=pure", "-Wtrampolines",
			"-Wvector-operation-performance",
			"-Wsuggest-attribute=format", "-Wdate-time",
			"-Wformat-signedness", "-Wnormalized=nfc",
			"-Wduplicated-cond", "-Wnull-dereference",
			"-Wshift-negative-value", "-Wshift-overflow=2",
			"-Wunused-const-variable=2", "-Walloca",
			"-Walloc-zero", "-Wformat-overflow=2",
			"-Wformat-truncation=2", "-Wstringop-overflow=3",
			"-Wduplicated-branches", "-Wattribute-alias=2",
			"-Wcast-align=strict", "-Wsuggest-attribute=cold",
			"-Wsuggest-attribute=malloc", "-Wanalyzer-too-complex",
			"-Warith-conversion", "-Wbidi-chars=ucn",
			"-Wopenacc-parallelism", "-Wtrivial-auto-var-init",
			"-Wbad-function-cast", "-Wmissing-prototypes",
			"-Wnested-externs", "-Wstrict-prototypes",
			"-Wold-style-definition", "-Winit-self",
			"-Wc++-compat", "-Wunsuffixed-float-constants" });
	return &args;
}

static const struct args *
compiler_clang_args_warn_everything(void)
{
	COMPILER_ARGS({ "-Weverything" });
	return &args;
}

static const struct args *
compiler_gcc_args_set_std(const char *std)
{
	static char buf[BUF_SIZE_S];
	COMPILER_ARGS({ buf });

	snprintf(buf, BUF_SIZE_S, "-std=%s", std);

	return &args;
}

static const struct args *
compiler_gcc_args_pgo(uint32_t stage)
{
	COMPILER_ARGS({ NULL, NULL });

	args.len = 1;

	switch ((enum compiler_pgo_stage)stage) {
	case compiler_pgo_generate:
		argv[0] = "-fprofile-generate";
		break;
	case compiler_pgo_use:
		argv[1] = "-fprofile-correction";
		++args.len;
		argv[0] = "-fprofile-use";
		break;
	}

	return &args;
}

static const struct args *
compiler_gcc_args_pic(void)
{
	COMPILER_ARGS({ "-fpic" });
	return &args;
}

static const struct args *
compiler_gcc_args_pie(void)
{
	COMPILER_ARGS({ "-fpie" });
	return &args;
}

static const struct args *
compiler_gcc_args_sanitize(const char *sanitizers)
{
	static char buf[BUF_SIZE_S];
	COMPILER_ARGS({ buf });

	snprintf(buf, BUF_SIZE_S, "-fsanitize=%s", sanitizers);

	return &args;
}

static const struct args *
compiler_gcc_args_visibility(uint32_t type)
{
	COMPILER_ARGS({ NULL, NULL });

	args.len = 1;

	switch ((enum compiler_visibility_type)type) {
	case compiler_visibility_default:
		argv[0] = "-fvisibility=default";
		break;
	case compiler_visibility_internal:
		argv[0] = "-fvisibility=internal";
		break;
	case compiler_visibility_protected:
		argv[0] = "-fvisibility=protected";
		break;
	case compiler_visibility_inlineshidden:
		argv[1] = "-fvisibility-inlines-hidden";
		++args.len;
	// fallthrough
	case compiler_visibility_hidden:
		argv[0] = "-fvisibility=hidden";
		break;
	default:
		assert(false && "unreachable");
	}

	return &args;
}

static const struct args *
compiler_gcc_args_specify_lang(const char *language)
{
	COMPILER_ARGS({ "-x", NULL, });

	argv[1] = language;

	return &args;
}

static const struct args *
compiler_gcc_args_color_output(const char *when)
{
	static char buf[BUF_SIZE_S];
	COMPILER_ARGS({ buf });

	snprintf(buf, BUF_SIZE_S, "-fdiagnostics-color=%s", when);

	return &args;
}

static const struct args *
compiler_gcc_args_lto(void)
{
	COMPILER_ARGS({ "-flto" });

	return &args;
}

/* cl compilers
 * see mesonbuild/compilers/mixins/visualstudio.py for reference
 */

static const struct args *
compiler_cl_args_always(void)
{
	COMPILER_ARGS({ "/nologo" });

	return &args;
}

static const struct args *
compiler_cl_args_deps(const char *out_target, const char *out_file)
{
	static char buf[BUF_SIZE_S];
	COMPILER_ARGS({ "/showIncludes", "/MD", buf });

	snprintf(buf, BUF_SIZE_S, "/Fd%s.pdb", out_target);

	return &args;
}

static const struct args *
compiler_cl_args_compile_only(void)
{
	COMPILER_ARGS({ "/c" });

	return &args;
}

static const struct args *
compiler_cl_args_preprocess_only(void)
{
	COMPILER_ARGS({ "/EP" });

	return &args;
}

static const struct args *
compiler_cl_args_output(const char *f)
{
	static char buf[BUF_SIZE_S];
	COMPILER_ARGS({ buf });

	if (str_endswithi(&WKSTR(f), &WKSTR(".exe"))) {
		snprintf(buf, BUF_SIZE_S, "/Fe%s", f);
	} else {
		snprintf(buf, BUF_SIZE_S, "/Fo%s", f);
	}

	return &args;
}

static const struct args *
compiler_cl_args_optimization(uint32_t lvl)
{
	COMPILER_ARGS({ NULL, NULL });
	switch ((enum compiler_optimization_lvl)lvl) {
	case compiler_optimization_lvl_none:
	case compiler_optimization_lvl_g:
		args.len = 0;
		break;
	case compiler_optimization_lvl_0:
		args.len = 1;
		argv[0] = "/Od";
		break;
	case compiler_optimization_lvl_1:
		args.len = 1;
		argv[0] = "/O1";
		break;
	case compiler_optimization_lvl_2:
		args.len = 1;
		argv[0] = "/O2";
		break;
	case compiler_optimization_lvl_3:
		args.len = 2;
		argv[0] = "/O2";
		argv[1] = "/Gw";
		break;
	case compiler_optimization_lvl_s:
		args.len = 2;
		argv[0] = "/O1";
		argv[1] = "/Gw";
		break;
	}

	return &args;
}

static const struct args *
compiler_cl_args_debug(void)
{
	COMPILER_ARGS({ "/Zi" });

	return &args;
}

static const struct args *
compiler_cl_args_warning_lvl(uint32_t lvl)
{
	/* meson uses nothing instead of /W0, but it's the same warning level
	 * see: https://mesonbuild.com/Builtin-options.html#details-for-warning_level
	 */

	COMPILER_ARGS({ NULL });

	switch ((enum compiler_warning_lvl)lvl) {
	case compiler_warning_lvl_0:
		args.len = 0;
		break;
	case compiler_warning_lvl_1:
		argv[0] = "/W2";
		break;
	case compiler_warning_lvl_2:
		argv[0] = "/W3";
		break;
	case compiler_warning_lvl_everything:
	case compiler_warning_lvl_3:
		argv[0] = "/W4";
		break;
	}

	return &args;
}

static const struct args *
compiler_cl_args_warn_everything(void)
{
	COMPILER_ARGS({ "/Wall" });
	return &args;
}

static const struct args *
compiler_cl_args_werror(void)
{
	COMPILER_ARGS({ "/WX" });
	return &args;
}

static const struct args *
compiler_cl_args_set_std(const char *std)
{
	static char buf[BUF_SIZE_S];
	COMPILER_ARGS({ buf });
	snprintf(buf, BUF_SIZE_S, "/std:%s", std);

	return &args;
}

static const struct args *
compiler_cl_args_include(const char *dir)
{
	COMPILER_ARGS({ "/I", NULL });

	argv[1] = dir;

	return &args;
}

static const struct args *
compiler_cl_args_sanitize(const char *sanitizers)
{
	static char buf[BUF_SIZE_S];
	COMPILER_ARGS({ buf });
	snprintf(buf, BUF_SIZE_S, "-fsanitize=%s", sanitizers);

	return &args;
}

static const struct args *
compiler_cl_args_define(const char *define)
{
	COMPILER_ARGS({ "/D", NULL });

	argv[1] = define;

	return &args;
}

static const struct args *
compiler_clang_cl_args_color_output(const char *when)
{
	COMPILER_ARGS({ "-fcolor-diagnostics" });

	return &args;
	(void)when;
}

static const struct args *
compiler_clang_cl_args_lto(void)
{
	COMPILER_ARGS({ "-flto" });

	return &args;
}

/* empty functions */

static const struct args *
compiler_arg_empty_0(void)
{
	COMPILER_ARGS({ NULL });
	args.len = 0;
	return &args;
}

static const struct args *
compiler_arg_empty_1i(uint32_t _)
{
	COMPILER_ARGS({ NULL });
	args.len = 0;
	return &args;
}

static const struct args *
compiler_arg_empty_1s(const char * _)
{
	COMPILER_ARGS({ NULL });
	args.len = 0;
	return &args;
}

static const struct args *
compiler_arg_empty_2s(const char *_, const char *__)
{
	COMPILER_ARGS({ NULL });
	args.len = 0;
	return &args;
}

struct compiler compilers[compiler_type_count];
struct linker linkers[linker_type_count];
struct static_linker static_linkers[static_linker_type_count];

const struct language languages[compiler_language_count] = {
	[compiler_language_null] = { 0 },
	[compiler_language_c] = { .is_header = false },
	[compiler_language_c_hdr] = { .is_header = true },
	[compiler_language_cpp] = { .is_header = false },
	[compiler_language_cpp_hdr] = { .is_header = true },
	[compiler_language_c_obj] = { .is_linkable = true },
	[compiler_language_assembly] = { 0 },
	[compiler_language_llvm_ir] = { 0 },
};

static void
build_compilers(void)
{
	struct compiler empty = {
		.args = {
			.deps            = compiler_arg_empty_2s,
			.compile_only    = compiler_arg_empty_0,
			.preprocess_only = compiler_arg_empty_0,
			.output          = compiler_arg_empty_1s,
			.optimization    = compiler_arg_empty_1i,
			.debug           = compiler_arg_empty_0,
			.warning_lvl     = compiler_arg_empty_1i,
			.warn_everything = compiler_arg_empty_0,
			.werror          = compiler_arg_empty_0,
			.set_std         = compiler_arg_empty_1s,
			.include         = compiler_arg_empty_1s,
			.include_system  = compiler_arg_empty_1s,
			.pgo             = compiler_arg_empty_1i,
			.pic             = compiler_arg_empty_0,
			.pie             = compiler_arg_empty_0,
			.sanitize        = compiler_arg_empty_1s,
			.define          = compiler_arg_empty_1s,
			.visibility      = compiler_arg_empty_1i,
			.specify_lang    = compiler_arg_empty_1s,
			.color_output    = compiler_arg_empty_1s,
			.enable_lto      = compiler_arg_empty_0,
			.always          = compiler_arg_empty_0,
		},
		.object_ext = ".o",
	};

	struct compiler clang_llvm_ir = empty;
	clang_llvm_ir.args.compile_only = compiler_posix_args_compile_only;
	clang_llvm_ir.args.output = compiler_posix_args_output;

	struct compiler posix = empty;
	posix.args.compile_only = compiler_posix_args_compile_only;
	posix.args.preprocess_only = compiler_posix_args_preprocess_only;
	posix.args.output = compiler_posix_args_output;
	posix.args.optimization = compiler_posix_args_optimization;
	posix.args.debug = compiler_posix_args_debug;
	posix.args.include = compiler_posix_args_include;
	posix.args.include_system = compiler_posix_args_include;
	posix.args.define = compiler_posix_args_define;
	posix.default_linker = linker_posix;
	posix.default_static_linker = static_linker_ar_posix;

	struct compiler gcc = posix;
	gcc.args.preprocess_only = compiler_gcc_args_preprocess_only;
	gcc.args.deps = compiler_gcc_args_deps;
	gcc.args.optimization = compiler_gcc_args_optimization;
	gcc.args.warning_lvl = compiler_gcc_args_warning_lvl;
	gcc.args.warn_everything = compiler_gcc_args_warn_everything;
	gcc.args.werror = compiler_gcc_args_werror;
	gcc.args.set_std = compiler_gcc_args_set_std;
	gcc.args.include_system = compiler_gcc_args_include_system;
	gcc.args.pgo = compiler_gcc_args_pgo;
	gcc.args.pic = compiler_gcc_args_pic;
	gcc.args.pie = compiler_gcc_args_pie;
	gcc.args.sanitize = compiler_gcc_args_sanitize;
	gcc.args.visibility = compiler_gcc_args_visibility;
	gcc.args.specify_lang = compiler_gcc_args_specify_lang;
	gcc.args.color_output = compiler_gcc_args_color_output;
	gcc.args.enable_lto = compiler_gcc_args_lto;
	gcc.deps = compiler_deps_gcc;
	gcc.default_linker = linker_gcc;
	gcc.default_static_linker = static_linker_ar_gcc;

	struct compiler clang = gcc;
	clang.args.warn_everything = compiler_clang_args_warn_everything;
	clang.default_linker = linker_clang;

	struct compiler apple_clang = clang;
	apple_clang.default_linker = linker_apple;
	apple_clang.default_static_linker = static_linker_ar_posix;

	struct compiler msvc = empty;
	msvc.args.deps = compiler_cl_args_deps;
	msvc.args.compile_only = compiler_cl_args_compile_only;
	msvc.args.preprocess_only = compiler_cl_args_preprocess_only;
	msvc.args.output = compiler_cl_args_output;
	msvc.args.optimization = compiler_cl_args_optimization;
	msvc.args.debug = compiler_cl_args_debug;
	msvc.args.warning_lvl = compiler_cl_args_warning_lvl;
	msvc.args.warn_everything = compiler_cl_args_warn_everything;
	msvc.args.werror = compiler_cl_args_werror;
	msvc.args.set_std = compiler_cl_args_set_std;
	msvc.args.include = compiler_cl_args_include;
	msvc.args.sanitize = compiler_cl_args_sanitize;
	msvc.args.define = compiler_cl_args_define;
	msvc.args.always = compiler_cl_args_always;
	msvc.default_linker = linker_msvc;
	msvc.default_static_linker = static_linker_msvc;
	msvc.deps = compiler_deps_msvc;
	msvc.object_ext = ".obj";

	struct compiler clang_cl = msvc;
	clang_cl.args.color_output = compiler_clang_cl_args_color_output;
	clang_cl.args.enable_lto = compiler_clang_cl_args_lto;
	clang_cl.default_linker = linker_lld_link;

	compilers[compiler_posix] = posix;
	compilers[compiler_gcc] = gcc;
	compilers[compiler_clang] = clang;
	compilers[compiler_apple_clang] = apple_clang;
	compilers[compiler_clang_llvm_ir] = clang_llvm_ir;
	compilers[compiler_clang_cl] = clang_cl;
	compilers[compiler_msvc] = msvc;

	struct compiler nasm = empty;
	nasm.args.output = compiler_posix_args_output;
	nasm.args.optimization = compiler_posix_args_optimization;
	nasm.args.debug = compiler_posix_args_debug;
	nasm.args.include = compiler_posix_args_include;
	nasm.args.include_system = compiler_posix_args_include;
	nasm.args.define = compiler_posix_args_define;
	nasm.default_linker = linker_posix;
	nasm.default_static_linker = static_linker_ar_posix;

	compilers[compiler_nasm] = nasm;
	compilers[compiler_yasm] = nasm;
}

static const struct args *
linker_posix_args_lib(const char *s)
{
	COMPILER_ARGS({ "-l", NULL });
	argv[1] = s;

	return &args;
}


static const struct args *
linker_posix_args_input_output(const char *in, const char *out)
{
	COMPILER_ARGS({ NULL, NULL });
	argv[0] = out;
	argv[1] = in;
	return &args;
}

/* technically not a posix linker argument, but include it here since it is so
 * common
 */
static const struct args *
linker_posix_args_shared(void)
{
	COMPILER_ARGS({ "-shared" });
	return &args;
}

static const struct args *
linker_gcc_args_as_needed(void)
{
	COMPILER_ARGS({ "-Wl,--as-needed" });
	return &args;
}

static const struct args *
linker_gcc_args_no_undefined(void)
{
	COMPILER_ARGS({ "-Wl,--no-undefined" });
	return &args;
}

static const struct args *
linker_gcc_args_start_group(void)
{
	COMPILER_ARGS({ "-Wl,--start-group" });
	return &args;
}

static const struct args *
linker_gcc_args_end_group(void)
{
	COMPILER_ARGS({ "-Wl,--end-group" });
	return &args;
}

static const struct args *
linker_gcc_args_soname(const char *soname)
{
	static char buf[BUF_SIZE_S];
	COMPILER_ARGS({ buf });

	snprintf(buf, BUF_SIZE_S, "-Wl,-soname,%s", soname);

	return &args;
}

static const struct args *
linker_gcc_args_rpath(const char *rpath)
{
	static char buf[BUF_SIZE_S];
	COMPILER_ARGS({ buf });

	snprintf(buf, BUF_SIZE_S, "-Wl,-rpath,%s", rpath);

	return &args;
}

static const struct args *
linker_gcc_args_allow_shlib_undefined(void)
{
	COMPILER_ARGS({ "-Wl,--allow-shlib-undefined" });
	return &args;
}

static const struct args *
linker_gcc_args_export_dynamic(void)
{
	COMPILER_ARGS({ "-Wl,-export-dynamic" });
	return &args;
}

static const struct args *
linker_gcc_args_fatal_warnings(void)
{
	COMPILER_ARGS({ "-Wl,--fatal-warnings" });
	return &args;
}

static const struct args *
linker_gcc_args_whole_archive(void)
{
	COMPILER_ARGS({ "-Wl,--whole-archive" });
	return &args;
}

static const struct args *
linker_gcc_args_no_whole_archive(void)
{
	COMPILER_ARGS({ "-Wl,--no-whole-archive" });
	return &args;
}

/* cl linkers */

static const struct args *
linker_link_args_always(void)
{
	COMPILER_ARGS({ "/nologo" });

	return &args;
}

static const struct args *
linker_link_args_lib(const char *s)
{
	COMPILER_ARGS({ NULL });
	argv[0] = s;

	return &args;
}
static const struct args *
linker_link_args_shared(void)
{
	COMPILER_ARGS({ "/DYNAMICBASE" });
	return &args;
}

static const struct args *
linker_link_args_soname(const char *soname)
{
	static char buf[BUF_SIZE_S];
	COMPILER_ARGS({ buf });

	snprintf(buf, BUF_SIZE_S, "/OUT:%s", soname);

	return &args;
}

static const struct args *
linker_link_args_input_output(const char *in, const char *out)
{
	static char buf[BUF_SIZE_S];
	COMPILER_ARGS({ buf, NULL });

	snprintf(buf, BUF_SIZE_S, "/out:%s", out);

	argv[1] = in;

	return &args;
}

static const struct args *
linker_lld_link_args_whole_archive(void)
{
	COMPILER_ARGS({ "/whole-archive" });
	return &args;
}

static void
build_linkers(void)
{
	/* linkers */
	struct linker empty = {
		.args = {
			.lib          = compiler_arg_empty_1s,
			.as_needed    = compiler_arg_empty_0,
			.no_undefined = compiler_arg_empty_0,
			.start_group  = compiler_arg_empty_0,
			.end_group    = compiler_arg_empty_0,
			.shared       = compiler_arg_empty_0,
			.soname       = compiler_arg_empty_1s,
			.rpath        = compiler_arg_empty_1s,
			.pgo          = compiler_arg_empty_1i,
			.sanitize     = compiler_arg_empty_1s,
			.allow_shlib_undefined = compiler_arg_empty_0,
			.export_dynamic = compiler_arg_empty_0,
			.fatal_warnings = compiler_arg_empty_0,
			.whole_archive = compiler_arg_empty_0,
			.no_whole_archive = compiler_arg_empty_0,
			.enable_lto = compiler_arg_empty_0,
			.input_output = compiler_arg_empty_2s,
			.always       = compiler_arg_empty_0,
		}
	};

	struct linker posix = empty;
	posix.args.lib = linker_posix_args_lib;
	posix.args.shared = linker_posix_args_shared;
	posix.args.input_output = linker_posix_args_input_output;

	struct linker gcc = posix;
	gcc.args.as_needed = linker_gcc_args_as_needed;
	gcc.args.no_undefined = linker_gcc_args_no_undefined;
	gcc.args.start_group = linker_gcc_args_start_group;
	gcc.args.end_group = linker_gcc_args_end_group;
	gcc.args.soname = linker_gcc_args_soname;
	gcc.args.rpath = linker_gcc_args_rpath;
	gcc.args.pgo = compiler_gcc_args_pgo;
	gcc.args.sanitize = compiler_gcc_args_sanitize;
	gcc.args.allow_shlib_undefined = linker_gcc_args_allow_shlib_undefined;
	gcc.args.export_dynamic = linker_gcc_args_export_dynamic;
	gcc.args.fatal_warnings = linker_gcc_args_fatal_warnings;
	gcc.args.whole_archive = linker_gcc_args_whole_archive;
	gcc.args.no_whole_archive = linker_gcc_args_no_whole_archive;
	gcc.args.enable_lto = compiler_gcc_args_lto;

	struct linker lld = gcc;

	struct linker apple = posix;
	apple.args.sanitize = compiler_gcc_args_sanitize;
	apple.args.enable_lto = compiler_gcc_args_lto;

	struct linker link = empty;
	link.args.lib = linker_link_args_lib;
	link.args.shared = linker_link_args_shared;
	link.args.soname = linker_link_args_soname;
	link.args.input_output = linker_link_args_input_output;
	link.args.always = linker_link_args_always;

	struct linker lld_link = link;
	lld_link.args.whole_archive = linker_lld_link_args_whole_archive;

	linkers[linker_posix] = posix;
	linkers[linker_gcc] = gcc;
	linkers[linker_clang] = lld;
	linkers[linker_apple] = apple;
	linkers[linker_lld_link] = lld_link;
	linkers[linker_msvc] = link;
}

/* static linkers */

static const struct args *
static_linker_ar_posix_args_base(void)
{
	COMPILER_ARGS({ "csr" });
	return &args;
}

static const struct args *
static_linker_ar_gcc_args_base(void)
{
	COMPILER_ARGS({ "csrD" });
	return &args;
}

static void
build_static_linkers(void)
{
	struct static_linker empty = {
		.args = {
			.base          = compiler_arg_empty_0,
			.input_output  = compiler_arg_empty_2s,
		}
	};

	struct static_linker posix = empty;
	posix.args.base = static_linker_ar_posix_args_base;
	posix.args.input_output = linker_posix_args_input_output;

	struct static_linker gcc = posix;
	gcc.args.base = static_linker_ar_gcc_args_base;

	struct static_linker msvc = empty;
	msvc.args.input_output = linker_link_args_input_output;

	static_linkers[static_linker_ar_posix] = posix;
	static_linkers[static_linker_ar_gcc] = gcc;
	static_linkers[static_linker_msvc] = msvc;
}

void
compilers_init(void)
{
	build_compilers();
	build_linkers();
	build_static_linkers();
}
