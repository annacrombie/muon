#include "posix.h"

#include "args.h"
#include "backend/ninja/custom_target.h"
#include "lang/workspace.h"
#include "log.h"
#include "platform/path.h"

static enum iteration_result
relativize_paths_iter(struct workspace *wk, void *_ctx, obj val)
{
	uint32_t *dest = _ctx;
	if (get_obj_type(wk, val) == obj_string) {
		obj_array_push(wk, *dest, val);
		return ir_cont;
	}

	char buf[PATH_MAX];

	if (!path_relative_to(buf, PATH_MAX, wk->build_root, get_file_path(wk, val))) {
		return ir_err;
	}

	obj_array_push(wk, *dest, make_str(wk, buf));
	return ir_cont;
}

bool
ninja_write_custom_tgt(struct workspace *wk, const struct project *proj, obj tgt_id, FILE *out)
{
	struct obj_custom_target *tgt = get_obj_custom_target(wk, tgt_id);
	LOG_I("writing rules for custom target '%s'", get_cstr(wk, tgt->name));

	uint32_t outputs, inputs, cmdline;

	make_obj(wk, &inputs, obj_array);
	if (!obj_array_foreach(wk, tgt->input, &inputs, relativize_paths_iter)) {
		return ir_err;
	}

	make_obj(wk, &outputs, obj_array);
	if (!obj_array_foreach(wk, tgt->output, &outputs, relativize_paths_iter)) {
		return ir_err;
	}

	make_obj(wk, &cmdline, obj_array);
	obj_array_push(wk, cmdline, make_str(wk, wk->argv0));
	obj_array_push(wk, cmdline, make_str(wk, "internal"));
	obj_array_push(wk, cmdline, make_str(wk, "exe"));

	if (tgt->flags & custom_target_capture) {
		obj_array_push(wk, cmdline, make_str(wk, "-c"));

		obj elem;
		obj_array_index(wk, tgt->output, 0, &elem);

		if (relativize_paths_iter(wk, &cmdline, elem) == ir_err) {
			return ir_err;
		}
	}

	obj_array_push(wk, cmdline, make_str(wk, "--"));

	uint32_t tgt_args;
	if (!arr_to_args(wk, 0, tgt->args, &tgt_args)) {
		return ir_err;
	}

	obj depends_rel;
	make_obj(wk, &depends_rel, obj_array);
	if (!obj_array_foreach(wk, tgt->depends, &depends_rel, relativize_paths_iter)) {
		return ir_err;
	}

	obj depends = join_args_ninja(wk, depends_rel);

	obj_array_extend(wk, cmdline, tgt_args);

	outputs = join_args_ninja(wk, outputs);
	inputs = join_args_ninja(wk, inputs);
	cmdline = join_args_shell_ninja(wk, cmdline);

	fprintf(out, "build %s: CUSTOM_COMMAND %s | %s\n"
		" COMMAND = %s\n"
		" DESCRIPTION = %s\n\n",
		get_cstr(wk, outputs),
		get_cstr(wk, inputs),
		get_cstr(wk, depends),
		get_cstr(wk, cmdline),
		get_cstr(wk, cmdline)
		);

	return ir_cont;
}
