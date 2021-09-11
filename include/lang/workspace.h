#ifndef MUON_LANG_WORKSPACE_H
#define MUON_LANG_WORKSPACE_H

#include "posix.h"

#include <limits.h>

#include "data/bucket_array.h"
#include "data/darr.h"
#include "data/hash.h"
#include "lang/eval.h"
#include "lang/parser.h"

typedef uint32_t str;

#include "lang/object.h"

struct project {
	struct hash scope;

	str source_root, cwd, build_dir, subproject_name;
	obj opts, compilers, targets, tests;

	struct {
		uint32_t name;
		uint32_t version;
		uint32_t license;
		uint32_t meson_version;
		uint32_t args;
	} cfg;
};

enum loop_ctl {
	loop_norm,
	loop_breaking,
	loop_continuing,
};

struct option_override {
	uint32_t proj, name, val;
	bool obj_value;
};

struct workspace {
	char argv0[PATH_MAX],
	     source_root[PATH_MAX],
	     build_root[PATH_MAX],
	     muon_private[PATH_MAX];

	/* obj_array that tracks each source file eval'd */
	uint32_t sources;
	/* TODO host machine dict */
	uint32_t host_machine;
	/* TODO binaries dict */
	uint32_t binaries;
	uint32_t install;
	uint32_t subprojects;

	struct darr strs;
	struct bucket_array objs;

	struct darr projects;
	struct darr option_overrides;
	struct darr source_data;

	struct hash scope;

	uint32_t stack_depth, loop_depth;
	enum loop_ctl loop_ctl;

	uint32_t cur_project;
	/* ast of current file */
	struct ast *ast;
	/* source of current file */
	struct source *src;

	enum language_mode lang_mode;

	char *strbuf;
	uint32_t strbuf_cap;
	uint32_t strbuf_len;
};

struct obj *make_obj(struct workspace *wk, uint32_t *id, enum obj_type type);
uint32_t make_str(struct workspace *wk, const char *str);
struct obj *get_obj(struct workspace *wk, uint32_t id);
bool get_obj_id(struct workspace *wk, const char *name, uint32_t *id, uint32_t proj_id);

uint32_t wk_str_pushf(struct workspace *wk, const char *fmt, ...)  __attribute__ ((format(printf, 2, 3)));
char *wk_str(struct workspace *wk, uint32_t id);
void wk_str_appf(struct workspace *wk, uint32_t *id, const char *fmt, ...)  __attribute__ ((format(printf, 3, 4)));
void wk_str_app(struct workspace *wk, uint32_t *id, const char *str);
void wk_str_appn(struct workspace *wk, uint32_t *id, const char *str, uint32_t n);
uint32_t wk_str_push(struct workspace *wk, const char *str);
uint32_t wk_str_pushn(struct workspace *wk, const char *str, uint32_t n);
char *wk_objstr(struct workspace *wk, uint32_t id);
char *wk_file_path(struct workspace *wk, uint32_t id);
uint32_t wk_str_push_stripped(struct workspace *wk, const char *s);
uint32_t wk_str_split(struct workspace *wk, const char *s, const char *sep);

void workspace_init_bare(struct workspace *wk);
void workspace_init(struct workspace *wk);
void workspace_destroy_bare(struct workspace *wk);
void workspace_destroy(struct workspace *wk);
bool workspace_setup_dirs(struct workspace *wk, const char *build, const char *argv0, bool mkdir);

void push_install_target(struct workspace *wk, uint32_t base_path, uint32_t filename,
	uint32_t install_dir, uint32_t install_mode);
bool push_install_targets(struct workspace *wk, uint32_t base_path, uint32_t filenames,
	uint32_t install_dirs, uint32_t install_mode);

struct project *make_project(struct workspace *wk, uint32_t *id, const char *subproject_name,
	const char *cwd, const char *build_dir);
struct project *current_project(struct workspace *wk);
#endif
