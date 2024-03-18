/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: illiliti <illiliti@thunix.net>
 * SPDX-FileCopyrightText: Simon Zeni <simon@bl4ckb0ne.ca>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "backend/backend.h"
#include "backend/output.h"
#include "cmd_install.h"
#include "cmd_test.h"
#include "embedded.h"
#include "external/libarchive.h"
#include "external/libcurl.h"
#include "external/libpkgconf.h"
#include "external/samurai.h"
#include "functions/common.h"
#include "lang/analyze.h"
#include "lang/fmt.h"
#include "lang/serial.h"
#include "machine_file.h"
#include "meson_opts.h"
#include "options.h"
#include "opts.h"
#include "platform/init.h"
#include "platform/mem.h"
#include "platform/path.h"
#include "platform/run_cmd.h"
#include "tracy.h"
#include "version.h"
#include "wrap.h"

static bool
ensure_in_build_dir(void)
{
	if (!fs_dir_exists(output_path.private_dir)) {
		LOG_E("this subcommand must be run from a build directory");
		return false;
	}

	return true;
}

static bool
load_obj_from_serial_dump(struct workspace *wk, const char *path, obj *res)
{
	bool ret = false;
	FILE *f;
	if (!(f = fs_fopen(path, "rb"))) {
		return false;
	}

	if (!serial_load(wk, res, f)) {
		LOG_E("failed to load environment data");
		goto ret;
	}

	ret = true;
ret:
	if (!fs_fclose(f)) {
		ret = false;
	}
	return ret;
}

static bool
cmd_exe(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct {
		const char *feed;
		const char *capture;
		const char *environment;
		const char *args;
		const char *const *cmd;
		const char *remove_before_running;
	} opts = { 0 };

	OPTSTART("f:c:e:a:R:") {
		case 'f':
			opts.feed = optarg;
			break;
		case 'c':
			opts.capture = optarg;
			break;
		case 'e':
			opts.environment = optarg;
			break;
		case 'a':
			opts.args = optarg;
			break;
		case 'R':
			opts.remove_before_running = optarg;
			break;
	} OPTEND(argv[argi],
		" <cmd> [arg1[ arg2[...]]]",
		"  -f <file> - feed file to input\n"
		"  -c <file> - capture output to file\n"
		"  -e <file> - load environment from data file\n"
		"  -a <file> - load arguments from data file\n"
		"  -R <file> - remove file if it exists before executing the command\n",
		NULL, -1)

	if (argi >= argc && !opts.args) {
		LOG_E("missing command");
		return false;
	} else if (argi < argc && opts.args) {
		LOG_E("command cannot be specified by trailing arguments *and* -a");
		return false;
	}

	opts.cmd = (const char *const *)&argv[argi];
	++argi;

	if (opts.remove_before_running && fs_exists(opts.remove_before_running)) {
		if (!fs_remove(opts.remove_before_running)) {
			return false;
		}
	}

	bool ret = false;
	struct run_cmd_ctx ctx = { 0 };
	ctx.stdin_path = opts.feed;

	if (!opts.capture) {
		ctx.flags |= run_cmd_ctx_flag_dont_capture;
	}

	struct workspace wk;
	bool initialized_workspace = false,
	     allocated_argv = false;

	const char *envstr = NULL;
	uint32_t envc = 0;
	if (opts.environment) {
		initialized_workspace = true;
		workspace_init_bare(&wk);

		obj env;
		if (!load_obj_from_serial_dump(&wk, opts.environment, &env)) {
			goto ret;
		}

		env_to_envstr(&wk, &envstr, &envc, env);
	}

	if (opts.args) {
		if (!initialized_workspace) {
			initialized_workspace = true;
			workspace_init_bare(&wk);
		}

		obj args;
		if (!load_obj_from_serial_dump(&wk, opts.args, &args)) {
			goto ret;
		}

		const char *argstr;
		uint32_t argc;
		join_args_argstr(&wk, &argstr, &argc, args);

		argstr_to_argv(argstr, argc, NULL, (char *const **)&opts.cmd);
		allocated_argv = true;
	}

	if (!run_cmd_argv(&ctx, (char *const *)opts.cmd, envstr, envc)) {
		LOG_E("failed to run command: %s", ctx.err_msg);
		goto ret;
	}

	if (ctx.status != 0) {
		if (opts.capture) {
			fputs(ctx.err.buf, stderr);
		}
		goto ret;
	}

	if (opts.capture) {
		ret = fs_write(opts.capture, (uint8_t *)ctx.out.buf, ctx.out.len);
	} else {
		ret = true;
	}
ret:
	run_cmd_ctx_destroy(&ctx);
	if (initialized_workspace) {
		workspace_destroy_bare(&wk);
	}
	if (allocated_argv) {
		z_free((void *)opts.cmd);
	}
	return ret;
}

static bool
cmd_check(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct {
		const char *filename;
		bool print_ast;
		uint32_t parse_mode;
	} opts = { 0 };

	OPTSTART("pm:") {
		case 'p':
			opts.print_ast = true;
			break;
		case 'm':
			if (strcmp(optarg, "fmt") == 0) {
				opts.parse_mode |= pm_keep_formatting;
			} else if (strcmp(optarg, "functions") == 0) {
				opts.parse_mode |= pm_functions;
			} else {
				LOG_E("invalid parse mode '%s'", optarg);
				return false;
			}
			break;
	} OPTEND(argv[argi],
		" <filename>",
		"  -p - print parsed ast\n"
		"  -m <mode> - parse with parse mode <mode>\n",
		NULL, 1)

	opts.filename = argv[argi];

	bool ret = false;

	struct source src = { 0 };

	if (!fs_read_entire_file(opts.filename, &src)) {
		goto ret;
	}

	struct workspace wk;
	workspace_init_bare(&wk);

	struct node *n;
	if (!(n = parse(&wk, &src, &wk.vm.compiler_state.nodes))) {
		goto ret;
	}

	if (opts.print_ast) {
		print_ast(&wk, n);
	}

	ret = true;
ret:
	workspace_destroy(&wk);
	fs_source_destroy(&src);
	return ret;
}

static bool
cmd_analyze(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct analyze_opts opts = {
		.replay_opts = error_diagnostic_store_replay_include_sources,
		.enabled_diagnostics = analyze_diagnostic_unused_variable
				       | analyze_diagnostic_dead_code
				       | analyze_diagnostic_redirect_script_error,
	};

	OPTSTART("luqO:W:i:td:") {
		case 'i':
			opts.internal_file = optarg;
			break;
		case 'l':
			opts.subdir_error = true;
			opts.replay_opts &= ~error_diagnostic_store_replay_include_sources;
			break;
		case 'O':
			opts.file_override = optarg;
			break;
		case 'q':
			opts.replay_opts |= error_diagnostic_store_replay_errors_only;
			break;
		case 't':
			opts.eval_trace = true;
			break;
		case 'd':
			opts.get_definition_for = optarg;
			break;
		case 'W': {
			/* bool enable = true; */
			/* const char *name = optarg; */
			/* if (str_startswith(&WKSTR(optarg), &WKSTR("no-"))) { */
			/* 	enable = false; */
			/* 	name += 3; */
			/* } */

			/* if (strcmp(name, "list") == 0) { */
			/* 	analyze_print_diagnostic_names(); */
			/* 	return true; */
			/* } else if (strcmp(name, "error") == 0) { */
			/* 	opts.replay_opts |= error_diagnostic_store_replay_werror; */
			/* } else { */
			/* 	enum analyze_diagnostic d; */
			/* 	if (!analyze_diagnostic_name_to_enum(name, &d)) { */
			/* 		LOG_E("invalid diagnostic name '%s'", name); */
			/* 		return false; */
			/* 	} */

			/* 	if (enable) { */
			/* 		opts.enabled_diagnostics |= d; */
			/* 	} else { */
			/* 		opts.enabled_diagnostics &= ~d; */
			/* 	} */
			/* } */
			break;
		}
	} OPTEND(argv[argi], "",
		"  -l - optimize output for editor linter plugins\n"
		"  -q - only report errors\n"
		"  -O <path> - read project file with matching path from stdin\n"
		"  -i <path> - analyze the single file <path> in internal mode\n"
		"  -t - print a tree of all meson source files that are evaluated\n"
		"  -d <var> - print the location of the definition of <var>\n"
		"  -W [no-]<diagnostic> - enable or disable diagnostics\n"
		"  -W list - list available diagnostics\n"
		"  -W error - turn all warnings into errors\n"
		,
		NULL, 0)

	if (opts.internal_file && opts.file_override) {
		LOG_E("-i and -O are mutually exclusive");
		return false;
	}

	SBUF_manual(abs);
	if (opts.file_override) {
		path_make_absolute(NULL, &abs, opts.file_override);
		opts.file_override = abs.buf;
	}

	bool res;
	res = do_analyze(&opts);

	if (opts.file_override) {
		sbuf_destroy(&abs);
	}
	return res;
}

static bool
cmd_options(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct list_options_opts opts = { 0 };
	OPTSTART("am") {
		case 'a':
			opts.list_all = true;
			break;
		case 'm':
			opts.only_modified = true;
			break;
	} OPTEND(argv[argi], "",
		"  -a - list all options\n"
		"  -m - list only modified options\n"
		,
		NULL, 0)

	return list_options(&opts);
}

static bool
cmd_summary(uint32_t argc, uint32_t argi, char *const argv[])
{
	OPTSTART("") {
	} OPTEND(argv[argi], "", "", NULL, 0)

	if (!ensure_in_build_dir()) {
		return false;
	}

	SBUF_manual(path);
	path_join(0, &path, output_path.private_dir, output_path.summary);

	bool ret = false;
	struct source src = { 0 };
	if (!fs_read_entire_file(path.buf, &src)) {
		goto ret;
	}

	fwrite(src.src, 1, src.len, stdout);

	ret = true;
ret:
	sbuf_destroy(&path);
	fs_source_destroy(&src);
	return ret;
}

static bool
cmd_info(uint32_t argc, uint32_t argi, char *const argv[])
{
	LOG_W("the info subcommand has been deprecated, please use options / summary directly");

	static const struct command commands[] = {
		{ "options", cmd_options, "list project options" },
		{ "summary", cmd_summary, "print a configured project's summary" },
		0,
	};

	OPTSTART("") {
	} OPTEND(argv[argi], "", "", commands, -1);

	cmd_func cmd = NULL;;
	if (!find_cmd(commands, &cmd, argc, argi, argv, false)) {
		return false;
	}

	assert(cmd);
	return cmd(argc, argi, argv);
}

static const char *cmd_subprojects_subprojects_dir = "subprojects";

static bool
cmd_subprojects_check_wrap(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct {
		const char *filename;
	} opts = { 0 };

	OPTSTART("") {
	} OPTEND(argv[argi],
		" <filename>",
		"",
		NULL, 1)

	opts.filename = argv[argi];

	bool ret = false;

	struct wrap wrap = { 0 };
	if (!wrap_parse(opts.filename, &wrap)) {
		goto ret;
	}

	ret = true;
ret:
	wrap_destroy(&wrap);
	return ret;
}

struct cmd_subprojects_download_ctx {
	const char *subprojects;
};

static enum iteration_result
cmd_subprojects_download_iter(void *_ctx, const char *name)
{
	struct cmd_subprojects_download_ctx *ctx = _ctx;
	uint32_t len = strlen(name);
	SBUF_manual(path);

	if (len <= 5 || strcmp(&name[len - 5], ".wrap") != 0) {
		goto cont;
	}

	path_join(NULL, &path, ctx->subprojects, name);

	if (!fs_file_exists(path.buf)) {
		goto cont;
	}

	LOG_I("fetching %s", name);
	struct wrap wrap = { 0 };
	if (!wrap_handle(path.buf, ctx->subprojects, &wrap, true)) {
		goto cont;
	}

	wrap_destroy(&wrap);
cont:
	sbuf_destroy(&path);
	return ir_cont;
}

static bool
cmd_subprojects_download(uint32_t argc, uint32_t argi, char *const argv[])
{
	bool res = false;

	OPTSTART("") {
	} OPTEND(argv[argi], " <list of subprojects>", "", NULL, -1)

	SBUF_manual(path);
	path_make_absolute(NULL, &path, cmd_subprojects_subprojects_dir);

	struct cmd_subprojects_download_ctx ctx = {
		.subprojects = path.buf,
	};

	if (argc > argi) {
		SBUF_manual(wrap_file);

		for (; argc > argi; ++argi) {
			path_join(NULL, &wrap_file, ctx.subprojects, argv[argi]);

			sbuf_pushs(NULL, &wrap_file, ".wrap");

			if (!fs_file_exists(wrap_file.buf)) {
				LOG_E("wrap file for '%s' not found", argv[argi]);
				goto ret;
			}

			if (cmd_subprojects_download_iter(&ctx, wrap_file.buf) == ir_err) {
				goto ret;
			}
		}

		sbuf_destroy(&wrap_file);
		res = true;
	} else {
		res = fs_dir_foreach(path.buf, &ctx, cmd_subprojects_download_iter);
	}

ret:
	sbuf_destroy(&path);
	return res;
}

static bool
cmd_subprojects(uint32_t argc, uint32_t argi, char *const argv[])
{
	static const struct command commands[] = {
		{ "check-wrap", cmd_subprojects_check_wrap, "check if a wrap file is valid" },
		{ "download", cmd_subprojects_download, "download subprojects" },
		{ 0 },
	};

	OPTSTART("d:") {
		case 'd':
			cmd_subprojects_subprojects_dir = optarg;
			break;
	} OPTEND(argv[0], "",
		"  -d <directory> - use an alternative subprojects directory\n",
		commands, -1)

	cmd_func cmd;
	if (!find_cmd(commands, &cmd, argc, argi, argv, false)) {
		return false;
	}

	return cmd(argc, argi, argv);
}

static bool
eval_internal(const char *filename, bool embedded, const char *argv0, char *const argv[], uint32_t argc)
{
	bool ret = false;

	struct source src = { 0 };
	bool src_allocd = false;

	struct workspace wk;
	workspace_init_bare(&wk);

	wk.vm.lang_mode = language_internal;

	if (embedded) {
		if (!(src.src = embedded_get(filename))) {
			LOG_E("failed to find '%s' in embedded sources", filename);
			goto ret;
		}

		src.len = strlen(src.src);
		src.label = filename;
	} else {
		if (!fs_read_entire_file(filename, &src)) {
			goto ret;
		}
		src_allocd = true;
	}

	{ // populate argv array
		obj argv_obj;
		make_obj(&wk, &argv_obj, obj_array);
		wk.vm.behavior.assign_variable(&wk, "argv", argv_obj, 0, assign_local);

		uint32_t i;
		for (i = 0; i < argc; ++i) {
			obj_array_push(&wk, argv_obj, make_str(&wk, argv[i]));
		}
	}

	obj res;
	if (!eval(&wk, &src, eval_mode_default, &res)) {
		goto ret;
	}

	ret = true;
ret:
	if (src_allocd) {
		fs_source_destroy(&src);
	}
	workspace_destroy(&wk);
	return ret;
}

static bool
cmd_eval(uint32_t argc, uint32_t argi, char *const argv[])
{
	const char *filename;
	bool embedded = false;

	OPTSTART("es") {
		case 'e':
			embedded = true;
			break;
		case 's':
			disable_fuzz_unsafe_functions = true;
			break;
	} OPTEND(argv[argi], " <filename> [args]",
		"  -e - lookup <filename> as an embedded script\n"
		"  -s - disable functions that are unsafe to be called at random\n",
		NULL, -1)

	if (argi >= argc) {
		LOG_E("missing required filename argument");
		return false;
	}

	filename = argv[argi];

	return eval_internal(filename, embedded, argv[0], &argv[argi], argc - argi);
}

static bool
cmd_repl(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct workspace wk;
	workspace_init(&wk);
	wk.vm.lang_mode = language_internal;

	obj id;
	make_project(&wk, &id, "dummy", wk.source_root, wk.build_root);

	repl(&wk, false);

	workspace_destroy(&wk);
	return true;
}

static bool
cmd_dump_signatures(uint32_t argc, uint32_t argi, char *const argv[])
{
	OPTSTART("") {
	} OPTEND(argv[argi], "", "", NULL, 0)

	struct workspace wk;
	workspace_init(&wk);

	obj id;
	make_project(&wk, &id, "dummy", wk.source_root, wk.build_root);
	if (!setup_project_options(&wk, NULL)) {
		UNREACHABLE;
	}

	dump_function_signatures(&wk);

	workspace_destroy(&wk);
	return true;
}

static bool
cmd_internal(uint32_t argc, uint32_t argi, char *const argv[])
{
	static const struct command commands[] = {
		{ "eval", cmd_eval, "evaluate a file" },
		{ "exe", cmd_exe, "run an external command" },
		{ "repl", cmd_repl, "start a meson language repl" },
		{ "dump_funcs", cmd_dump_signatures, "output all supported functions and arguments" },
		0,
	};

	OPTSTART("") {
	} OPTEND(argv[argi], "", "", commands, -1);

	cmd_func cmd = NULL;;
	if (!find_cmd(commands, &cmd, argc, argi, argv, false)) {
		return false;
	}

	assert(cmd);
	return cmd(argc, argi, argv);
}

static bool
cmd_samu(uint32_t argc, uint32_t argi, char *const argv[])
{
	return samu_main(argc - argi, (char **)&argv[argi], 0);
}

static bool
cmd_test(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct test_options test_opts = { 0 };

	if (strcmp(argv[argi], "benchmark") == 0) {
		test_opts.cat = test_category_benchmark;
		test_opts.print_summary = true;
	}

	OPTSTART("s:d:Sfj:lvRe:") {
		case 'l':
			test_opts.list = true;
			break;
		case 'e':
			test_opts.setup = optarg;
			break;
		case 's':
			if (test_opts.suites_len > MAX_CMDLINE_TEST_SUITES) {
				LOG_E("too many -s options (max: %d)", MAX_CMDLINE_TEST_SUITES);
				return false;
			}
			test_opts.suites[test_opts.suites_len] = optarg;
			++test_opts.suites_len;
			break;
		case 'd':
			if (strcmp(optarg, "auto") == 0) {
				test_opts.display = test_display_auto;
			} else if (strcmp(optarg, "dots") == 0) {
				test_opts.display = test_display_dots;
			} else if (strcmp(optarg, "bar") == 0) {
				test_opts.display = test_display_bar;
			} else {
				LOG_E("invalid progress display mode '%s'", optarg);
				return false;
			}
			break;
		case 'f':
			test_opts.fail_fast = true;
			break;
		case 'S':
			test_opts.print_summary = true;
			break;
		case 'j': {
			char *endptr;
			unsigned long n = strtoul(optarg, &endptr, 10);

			if (n > UINT32_MAX || !*optarg || *endptr) {
				LOG_E("invalid number of jobs: %s", optarg);
				return false;
			}

			test_opts.jobs = n;
			break;
		}
		case 'v':
			++test_opts.verbosity;
			break;
		case 'R':
			test_opts.no_rebuild = true;
			break;
	} OPTEND(argv[argi], " [test [test [...]]]",
		"  -d <mode> - change progress display mode (auto|dots|bar)\n"
		"  -e <setup> - use test setup <setup>\n"
		"  -f - fail fast; exit after first failure\n"
		"  -j <jobs> - set the number of test workers\n"
		"  -l - list tests that would be run\n"
		"  -R - disable automatic rebuild\n"
		"  -S - print a summary with elapsed time\n"
		"  -s <suite> - only run items in <suite>, may be passed multiple times\n"
		"  -v - increase verbosity, may be passed twice\n",
		NULL, -1)

	if (!ensure_in_build_dir()) {
		return false;
	}

	test_opts.tests = &argv[argi];
	test_opts.tests_len = argc - argi;

	return tests_run(&test_opts, argv[0]);
}

static bool
cmd_install(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct install_options opts = {
		.destdir = getenv("DESTDIR"),
	};

	OPTSTART("nd:") {
		case 'n':
			opts.dry_run = true;
			break;
		case 'd':
			opts.destdir = optarg;
			break;
	} OPTEND(argv[argi], "",
		"  -n - dry run\n"
		"  -d <destdir> - set destdir\n",
		NULL, 0)

	if (!ensure_in_build_dir()) {
		return false;
	}

	return install_run(&opts);
}

static bool
cmd_setup(uint32_t argc, uint32_t argi, char *const argv[])
{
	TracyCZoneAutoS;
	bool res = false;
	struct workspace wk;
	workspace_init(&wk);

	uint32_t original_argi = argi + 1;

	OPTSTART("D:c:b") {
		case 'D':
			if (!parse_and_set_cmdline_option(&wk, optarg)) {
				goto ret;
			}
			break;
		case 'c': {
			FILE *f;
			if (!(f = fs_fopen(optarg, "rb"))) {
				goto ret;
			} else if (!serial_load(&wk, &wk.compiler_check_cache, f)) {
				LOG_E("failed to load compiler check cache");
				goto ret;
			} else if (!fs_fclose(f)) {
				goto ret;
			}
			break;
		}
		case 'b':
			wk.vm.dbg_state.break_on_err = true;
			break;
	} OPTEND(argv[argi],
		" <build dir>",
		"  -D <option>=<value> - set project options\n"
		"  -c <compiler_check_cache.dat> - path to compiler check cache dump\n"
		"  -b - break on errors\n",
		NULL, 1)

	const char *build = argv[argi];
	++argi;

	if (!workspace_setup_paths(&wk, build, argv[0],
		argc - original_argi,
		&argv[original_argi])) {
		goto ret;
	}

	uint32_t project_id;
	if (!eval_project(&wk, NULL, wk.source_root, wk.build_root, &project_id)) {
		goto ret;
	}

	log_plain("\n");

	if (!backend_output(&wk)) {
		goto ret;
	}

	workspace_print_summaries(&wk, log_file());

	LOG_I("setup complete");

	res = true;
ret:
	workspace_destroy(&wk);
	TracyCZoneAutoE;
	return res;
}

static bool
cmd_format(uint32_t argc, uint32_t argi, char *const argv[])
{
	if (strcmp(argv[argi], "fmt_unstable") == 0) {
		LOG_W("the subcommand name fmt_unstable is deprecated, please use fmt instead");
	}

	struct {
		char *const *filenames;
		const char *cfg_path;
		bool in_place, check_only, editorconfig;
	} opts = { 0 };

	OPTSTART("ic:qe") {
		case 'i':
			opts.in_place = true;
			break;
		case 'c':
			opts.cfg_path = optarg;
			break;
		case 'q':
			opts.check_only = true;
			break;
		case 'e':
			opts.editorconfig = true;
			break;
	} OPTEND(argv[argi], " <file>[ <file>[...]]",
		"  -q - exit with 1 if files would be modified by muon fmt\n"
		"  -i - format files in-place\n"
		"  -c <muon_fmt.ini> - read configuration from muon_fmt.ini\n"
		"  -e - try to read configuration from .editorconfig\n",
		NULL, -1)

	if (opts.in_place && opts.check_only) {
		LOG_E("-q and -i are mutually exclusive");
		return false;
	}

	opts.filenames = &argv[argi];
	const uint32_t num_files = argc - argi;

	bool ret = true;
	bool opened_out;
	FILE *out;
	uint32_t i;
	for (i = 0; i < num_files; ++i) {
		bool fmt_ret = true;
		opened_out = false;

		struct source src = { 0 };
		if (!fs_read_entire_file(opts.filenames[i], &src)) {
			ret = false;
			goto cont;
		}

		if (opts.in_place) {
			if (!(out = fs_fopen(opts.filenames[i], "wb"))) {
				ret = false;
				goto cont;
			}
			opened_out = true;
		} else if (opts.check_only) {
			out = NULL;
		} else {
			out = stdout;
		}

		fmt_ret = fmt(&src, out, opts.cfg_path, opts.check_only, opts.editorconfig);
cont:
		if (opened_out) {
			fs_fclose(out);

			if (!fmt_ret) {
				fs_write(opts.filenames[i], (const uint8_t *)src.src, src.len);
			}
		}
		fs_source_destroy(&src);
		ret &= fmt_ret;
	}

	return ret;
}

static bool
cmd_version(uint32_t argc, uint32_t argi, char *const argv[])
{
	printf("muon %s%s%s\nmeson compatibility version %s\nenabled features:\n",
		muon_version.version,
		*muon_version.vcs_tag ? "-" : "",
		muon_version.vcs_tag,
		muon_version.meson_compat
		);

	const struct {
		const char *name;
		bool enabled;
	} feature_names[] = {
		{ "libcurl", have_libcurl },
		{ "libpkgconf", have_libpkgconf },
		{ "libarchive", have_libarchive },
		{ "samurai", have_samurai },
#ifdef TRACY_ENABLE
		{ "tracy", true },
#endif
	};

	uint32_t i;
	for (i = 0; i < ARRAY_LEN(feature_names); ++i) {
		if (feature_names[i].enabled) {
			printf("  %s\n", feature_names[i].name);
		}
	}

	return true;
}

static bool cmd_main(uint32_t argc, uint32_t argi, char *argv[]);

static bool
cmd_meson(uint32_t argc, uint32_t argi, char *const argv[])
{
	++argi;

	struct workspace wk;
	char **new_argv;
	uint32_t new_argc, new_argi;
	workspace_init_bare(&wk);
	if (!translate_meson_opts(&wk, argc, argi, (char **)argv, &new_argc, &new_argi, &new_argv)) {
		return false;
	}

	argi = new_argi;
	argc = new_argc;
	argv = new_argv;

	bool res = cmd_main(argc, argi, (char **)argv);

	workspace_destroy(&wk);
	z_free(new_argv);

	return res;
}

static bool
cmd_main(uint32_t argc, uint32_t argi, char *argv[])
{
	const struct command commands[] = {
		{ "analyze", cmd_analyze, "run a static analyzer on the current project." },
		{ "benchmark", cmd_test, "run benchmarks" },
		{ "check", cmd_check, "check if a meson file parses" },
		{ "fmt", cmd_format, "format meson source file" },
		{ "fmt_unstable", cmd_format, NULL },
		{ "info", cmd_info, NULL },
		{ "install", cmd_install, "install project" },
		{ "internal", cmd_internal, "internal subcommands" },
		{ "meson", cmd_meson, "meson compatible cli proxy" },
		{ "options", cmd_options, "list project options" },
		{ "samu", cmd_samu, have_samurai ? "run samurai" : NULL },
		{ "setup", cmd_setup, "setup a build directory" },
		{ "subprojects", cmd_subprojects, "manage subprojects" },
		{ "summary", cmd_summary, "print a configured project's summary" },
		{ "test", cmd_test, "run tests" },
		{ "version", cmd_version, "print version information" },
		{ 0 },
	};

	bool res = false;
	SBUF_manual(argv0);

	OPTSTART("vC:") {
		case 'v':
			log_set_lvl(log_debug);
			break;
		case 'C': {
			// fix argv0 here since if it is a relative path it will be
			// wrong after chdir
			if (!path_is_basename(argv[0])) {
				path_make_absolute(NULL, &argv0, argv[0]);
				argv[0] = argv0.buf;
			}

			if (!path_chdir(optarg)) {
				return false;
			}
			break;
		}
	} OPTEND(argv[0], "",
		"  -v - turn on debug messages\n"
		"  -C <path> - chdir to path\n",
		commands, -1)

	cmd_func cmd;
	if (!find_cmd(commands, &cmd, argc, argi, argv, false)) {
		goto ret;
	}

	res = cmd(argc, argi, argv);

ret:
	sbuf_destroy(&argv0);
	return res;
}

int
main(int argc, char *argv[])
{
	platform_init();

	log_init();
	log_set_lvl(log_info);

	path_init();

	compilers_init();

	bool res;
	bool meson_compat = false;

	{
		SBUF(basename);
		path_basename(NULL, &basename, argv[0]);
		meson_compat = strcmp(basename.buf, "meson") == 0 && strcmp(argv[1], "internal") != 0;
		sbuf_destroy(&basename);
	}

	if (meson_compat) {
		res = cmd_meson(argc, 0, argv);
	} else {
		res = cmd_main(argc, 0, argv);
	}

	int ret = res ? 0 : 1;

	path_deinit();
	return ret;
}
