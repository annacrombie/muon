#include "posix.h"

#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "buf_size.h"
#include "compilers.h"
#include "error.h"
#include "guess.h"
#include "lang/workspace.h"
#include "log.h"
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
	case linker_apple: return "ld64";
	case linker_type_count: UNREACHABLE;
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
	case compiler_language_c:
	case compiler_language_c_obj:
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
	case compiler_language_objc:
	case compiler_language_count:
		UNREACHABLE;
	}

	return cur;
}

static bool
compiler_detect_c_or_cpp(struct workspace *wk, const char *cc, obj *comp_id)
{
	// helpful: mesonbuild/compilers/detect.py:350
	struct run_cmd_ctx cmd_ctx = { 0 };
	if (!run_cmd_argv(&cmd_ctx, cc, (char *const []){
		(char *)cc, "--version", NULL,
	}, NULL)) {
		run_cmd_ctx_destroy(&cmd_ctx);
		return false;
	}

	if (cmd_ctx.status != 0) {
		cmd_ctx = (struct run_cmd_ctx) { 0 };
		if (!run_cmd_argv(&cmd_ctx, cc, (char *const []){
			(char *)cc, "-v", NULL,
		}, NULL)) {
			run_cmd_ctx_destroy(&cmd_ctx);
			return false;
		}
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
		type = compiler_clang;
	} else if (strstr(cmd_ctx.out.buf, "Free Software Foundation")) {
		type = compiler_gcc;
	} else {
		goto detection_over;
	}

	char *p;
	if ((p = strchr(cmd_ctx.out.buf, '\n'))) {
		*p = 0;
	}

	if (!guess_version(wk, cmd_ctx.out.buf, &ver)) {
		ver = make_str(wk, "unknown");
	}

	unknown = false;
	LOG_I("detected compiler %s %s (%s), linker %s", compiler_type_to_s(type),
		get_cstr(wk, ver), cc, linker_type_to_s(compilers[type].linker));

detection_over:
	if (unknown) {
		LOG_W("unable to detect compiler type, falling back on posix compiler");
		type = compiler_posix;
		ver = make_str(wk, "unknown");
	}

	make_obj(wk, comp_id, obj_compiler);
	struct obj_compiler *comp = get_obj_compiler(wk, *comp_id);
	comp->name = make_str(wk, cc);
	comp->type = type;
	comp->ver = ver;

	run_cmd_ctx_destroy(&cmd_ctx);
	return true;
}

static bool
compiler_get_libdirs(struct workspace *wk, struct obj_compiler *comp)
{
	struct run_cmd_ctx cmd_ctx = { 0 };
	if (!run_cmd_argv(&cmd_ctx, get_cstr(wk, comp->name), (char *const []){
		(char *)get_cstr(wk, comp->name), "--print-search-dirs", NULL,
	}, NULL) || cmd_ctx.status) {
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

			comp->libdirs = str_split(wk, &str, &WKSTR(":"));
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

bool
compiler_detect(struct workspace *wk, obj *comp, enum compiler_language lang)
{
	const char *cmd;

	static const char *compiler_env_override[] = {
		[compiler_language_c] = "CC",
		[compiler_language_cpp] = "CXX",
	};

	static const char *compiler_default[] = {
		[compiler_language_c] = "cc",
		[compiler_language_cpp] = "c++",
	};

	if (!(cmd = getenv(compiler_env_override[lang]))) {
		cmd = compiler_default[lang];
	}

	switch (lang) {
	case compiler_language_c:
	case compiler_language_cpp:
		if (!compiler_detect_c_or_cpp(wk, cmd, comp)) {
			return false;
		}

		struct obj_compiler *compiler = get_obj_compiler(wk, *comp);
		compiler_get_libdirs(wk, compiler);
		compiler->lang = lang;
		return true;
	default:
		LOG_E("tried to get a compiler for unsupported language '%s'", compiler_language_to_s(lang));
		return false;
	}
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

compiler_get_arg_func_0 as_needed;
compiler_get_arg_func_0 no_undefined;
compiler_get_arg_func_0 start_group;
compiler_get_arg_func_0 end_group;

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
		}
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
	posix.linker = linker_posix;

	struct compiler gcc = posix;
	gcc.args.preprocess_only = compiler_gcc_args_preprocess_only;
	gcc.args.deps = compiler_gcc_args_deps;
	gcc.args.optimization = compiler_gcc_args_optimization;
	gcc.args.warning_lvl = compiler_gcc_args_warning_lvl;
	gcc.args.werror = compiler_gcc_args_werror;
	gcc.args.set_std = compiler_gcc_args_set_std;
	gcc.args.include_system = compiler_gcc_args_include_system;
	gcc.args.pgo = compiler_gcc_args_pgo;
	gcc.args.pic = compiler_gcc_args_pic;
	gcc.args.pie = compiler_gcc_args_pie;
	gcc.args.sanitize = compiler_gcc_args_sanitize;
	gcc.args.visibility = compiler_gcc_args_visibility;
	gcc.deps = compiler_deps_gcc;
	gcc.linker = linker_gcc;

	struct compiler apple_clang = gcc;
	apple_clang.linker = linker_apple;

	compilers[compiler_posix] = posix;
	compilers[compiler_gcc] = gcc;
	compilers[compiler_clang] = gcc;
	compilers[compiler_apple_clang] = apple_clang;
	compilers[compiler_clang_llvm_ir] = clang_llvm_ir;
}

static const struct args *
linker_posix_args_lib(const char *s)
{
	COMPILER_ARGS({ "-l", NULL });
	argv[1] = s;

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
		}
	};

	struct linker posix = empty;
	posix.args.lib = linker_posix_args_lib;
	posix.args.shared = linker_posix_args_shared;

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

	struct linker apple = posix;

	linkers[linker_posix] = posix;
	linkers[linker_gcc] = gcc;
	linkers[linker_apple] = apple;
}

void
compilers_init(void)
{
	build_compilers();
	build_linkers();
}
