/*
 * SPDX-FileCopyrightText: Michael Forney <mforney@mforney.org>
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: MIT
 */

#include "compat.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "buf_size.h"
#include "external/samurai/ctx.h"
#include "lang/string.h"
#include "platform/path.h"

#include "external/samurai/arg.h"
#include "external/samurai/env.h"
#include "external/samurai/graph.h"
#include "external/samurai/parse.h"
#include "external/samurai/tool.h"
#include "external/samurai/util.h"

static int
samu_cleanpath(struct samu_ctx *ctx, struct samu_string *path)
{
	if (path && fs_exists(path->s)) {
		if (fs_remove(path->s)) {
			samu_printf(ctx, "remove %s\n", path->s);
		} else {
			return -1;
		}
	}

	return 0;
}

static int
samu_cleanedge(struct samu_ctx *ctx, struct samu_edge *e)
{
	int ret = 0;
	size_t i;

	for (i = 0; i < e->nout; ++i) {
		if (samu_cleanpath(ctx, e->out[i]->path) < 0)
			ret = -1;
	}
	if (samu_cleanpath(ctx, samu_edgevar(ctx, e, "rspfile", false)) < 0)
		ret = -1;
	if (samu_cleanpath(ctx, samu_edgevar(ctx, e, "depfile", false)) < 0)
		ret = -1;

	return ret;
}

static int
samu_cleantarget(struct samu_ctx *ctx, struct samu_node *n)
{
	int ret = 0;
	size_t i;

	if (!n->gen || n->gen->rule == &ctx->phonyrule)
		return 0;
	if (samu_cleanpath(ctx, n->path) < 0)
		ret = -1;
	for (i = 0; i < n->gen->nin; ++i) {
		if (samu_cleantarget(ctx, n->gen->in[i]) < 0)
			ret = -1;
	}

	return ret;
}

static int
samu_clean(struct samu_ctx *ctx, int argc, char *argv[])
{
	int ret = 0;
	bool cleangen = false, cleanrule = false;
	struct samu_edge *e;
	struct samu_node *n;
	struct samu_rule *r;

	SAMU_ARGBEGIN
	{
	case 'g': cleangen = true; break;
	case 'r': cleanrule = true; break;
	default: fprintf(stderr, "usage: %s ... -t clean [-gr] [targets...]\n", ctx->argv0); return 2;
	}
	SAMU_ARGEND

	if (cleanrule) {
		if (!argc)
			samu_fatal("expected a rule to clean");
		for (; *argv; ++argv) {
			r = samu_envrule(ctx->env.rootenv, *argv);
			if (!r) {
				samu_warn("unknown rule '%s'", *argv);
				ret = 1;
				continue;
			}
			for (e = ctx->graph.alledges; e; e = e->allnext) {
				if (e->rule != r)
					continue;
				if (samu_cleanedge(ctx, e) < 0)
					ret = 1;
			}
		}
	} else if (argc > 0) {
		for (; *argv; ++argv) {
			n = samu_nodeget(ctx, *argv, 0);
			if (!n) {
				samu_warn("unknown target '%s'", *argv);
				ret = 1;
				continue;
			}
			if (samu_cleantarget(ctx, n) < 0)
				ret = 1;
		}
	} else {
		for (e = ctx->graph.alledges; e; e = e->allnext) {
			if (e->rule == &ctx->phonyrule)
				continue;
			if (!cleangen && samu_edgevar(ctx, e, "generator", true))
				continue;
			if (samu_cleanedge(ctx, e) < 0)
				ret = 1;
		}
	}

	return ret;
}

/* depth-first traversal */
static void
samu_targetcommands(struct samu_ctx *ctx, struct samu_node *n)
{
	struct samu_edge *e = n->gen;
	struct samu_string *command;
	size_t i;

	if (!e || (e->flags & FLAG_WORK))
		return;
	e->flags |= FLAG_WORK;
	for (i = 0; i < e->nin; ++i)
		samu_targetcommands(ctx, e->in[i]);
	command = samu_edgevar(ctx, e, "command", true);
	if (command && command->n)
		samu_puts(ctx, command->s);
}

static int
samu_commands(struct samu_ctx *ctx, int argc, char *argv[])
{
	struct samu_node *n;

	if (argc > 1) {
		while (*++argv) {
			n = samu_nodeget(ctx, *argv, 0);
			if (!n)
				samu_fatal("unknown target '%s'", *argv);
			samu_targetcommands(ctx, n);
		}
	} else {
		samu_defaultnodes(ctx, samu_targetcommands);
	}

	if (fflush(stdout) || ferror(stdout))
		samu_fatal("write failed");

	return 0;
}

static void
samu_printjson(struct samu_ctx *ctx, const char *s, size_t n, bool join)
{
	size_t i;
	char c;

	for (i = 0; i < n; ++i) {
		c = s[i];
		switch (c) {
		case '"':
		case '\\': samu_putchar(ctx, '\\'); break;
		case '\n':
			if (join)
				c = ' ';
			break;
		case '\0': return;
		}
		samu_putchar(ctx, c);
	}
}

static int
samu_compdb(struct samu_ctx *ctx, int argc, char *argv[])
{
	char *p;
	struct samu_edge *e;
	struct samu_string *cmd, *rspfile, *content;
	bool expandrsp = false, first = true;
	int i;
	size_t off;
	bool all = false;

	SAMU_ARGBEGIN
	{
	case 'x': expandrsp = true; break;
	case 'a': all = true; break;
	default: fprintf(stderr, "usage: %s ... -t compdb [-xa] [rules...]\n", ctx->argv0); return 2;
	}
	SAMU_ARGEND

	SBUF_manual(dir);
	path_copy_cwd(0, &dir);

	samu_putchar(ctx, '[');
	for (e = ctx->graph.alledges; e; e = e->allnext) {
		if (e->nin == 0)
			continue;
		if (e->rule == &ctx->phonyrule) {
			continue;
		}

		if (!all) {
			for (i = 0; i < argc; ++i) {
				if (strcmp(e->rule->name, argv[i]) == 0) {
					break;
				}
			}

			if (i == argc) {
				continue;
			}
		}

		if (first)
			first = false;
		else
			samu_putchar(ctx, ',');

		samu_printf(ctx, "\n  {\n    \"directory\": \"");
		samu_printjson(ctx, dir.buf, -1, false);

		samu_printf(ctx, "\",\n    \"command\": \"");
		cmd = samu_edgevar(ctx, e, "command", true);
		rspfile = expandrsp ? samu_edgevar(ctx, e, "rspfile", true) : NULL;
		p = rspfile ? strstr(cmd->s, rspfile->s) : NULL;
		if (!p || p == cmd->s || p[-1] != '@') {
			samu_printjson(ctx, cmd->s, cmd->n, false);
		} else {
			off = p - cmd->s;
			samu_printjson(ctx, cmd->s, off - 1, false);
			content = samu_edgevar(ctx, e, "rspfile_content", true);
			samu_printjson(ctx, content->s, content->n, true);
			off += rspfile->n;
			samu_printjson(ctx, cmd->s + off, cmd->n - off, false);
		}

		samu_printf(ctx, "\",\n    \"file\": \"");
		samu_printjson(ctx, e->in[0]->path->s, -1, false);

		samu_printf(ctx, "\",\n    \"output\": \"");
		samu_printjson(ctx, e->out[0]->path->s, -1, false);

		samu_printf(ctx, "\"\n  }");
	}
	samu_puts(ctx, "\n]");

	if (fflush(stdout) || ferror(stdout))
		samu_fatal("write failed");

	sbuf_destroy(&dir);
	return 0;
}

static void
samu_graphnode(struct samu_ctx *ctx, struct samu_node *n)
{
	struct samu_edge *e = n->gen;
	size_t i;
	const char *style;

	samu_printf(ctx, "\"%p\" [label=\"%s\"]\n", (void *)n, n->path->s);

	if (!e || (e->flags & FLAG_WORK))
		return;
	e->flags |= FLAG_WORK;

	for (i = 0; i < e->nin; ++i)
		samu_graphnode(ctx, e->in[i]);

	if (e->nin == 1 && e->nout == 1) {
		samu_printf(
			ctx, "\"%p\" -> \"%p\" [label=\"%s\"]\n", (void *)e->in[0], (void *)e->out[0], e->rule->name);
	} else {
		samu_printf(ctx, "\"%p\" [label=\"%s\", shape=ellipse]\n", (void *)e, e->rule->name);
		for (i = 0; i < e->nout; ++i)
			samu_printf(ctx, "\"%p\" -> \"%p\"\n", (void *)e, (void *)e->out[i]);
		for (i = 0; i < e->nin; ++i) {
			style = i >= e->inorderidx ? " style=dotted" : "";
			samu_printf(ctx, "\"%p\" -> \"%p\" [arrowhead=none%s]\n", (void *)e->in[i], (void *)e, style);
		}
	}
}

static int
samu_graph(struct samu_ctx *ctx, int argc, char *argv[])
{
	struct samu_node *n;

	samu_puts(ctx, "digraph ninja {");
	samu_puts(ctx, "rankdir=\"LR\"");
	samu_puts(ctx, "node [fontsize=10, shape=box, height=0.25]");
	samu_puts(ctx, "edge [fontsize=10]");

	if (argc > 1) {
		while (*++argv) {
			n = samu_nodeget(ctx, *argv, 0);
			if (!n)
				samu_fatal("unknown target '%s'", *argv);
			samu_graphnode(ctx, n);
		}
	} else {
		samu_defaultnodes(ctx, samu_graphnode);
	}

	samu_puts(ctx, "}");

	if (fflush(stdout) || ferror(stdout))
		samu_fatal("write failed");

	return 0;
}

static int
samu_query(struct samu_ctx *ctx, int argc, char *argv[])
{
	struct samu_node *n;
	struct samu_edge *e;
	char *path;
	int i;
	size_t j, k;

	if (argc == 1) {
		fprintf(stderr, "usage: %s ... -t query target...\n", ctx->argv0);
		exit(2);
	}
	for (i = 1; i < argc; ++i) {
		path = argv[i];
		n = samu_nodeget(ctx, path, 0);
		if (!n)
			samu_fatal("unknown target '%s'", path);
		samu_printf(ctx, "%s:\n", argv[i]);
		e = n->gen;
		if (e) {
			samu_printf(ctx, "  input: %s\n", e->rule->name);
			for (j = 0; j < e->nin; ++j)
				samu_printf(ctx, "    %s\n", e->in[j]->path->s);
		}
		samu_puts(ctx, "  outputs:");
		for (j = 0; j < n->nuse; ++j) {
			e = n->use[j];
			for (k = 0; k < e->nout; ++k)
				samu_printf(ctx, "    %s\n", e->out[k]->path->s);
		}
	}

	return 0;
}

static void
samu_targetsdepth(struct samu_ctx *ctx, struct samu_node *n, size_t depth, size_t indent)
{
	struct samu_edge *e = n->gen;
	size_t i;

	for (i = 0; i < indent; ++i)
		samu_printf(ctx, "  ");
	if (e) {
		samu_printf(ctx, "%s: %s\n", n->path->s, e->rule->name);
		if (depth != 1) {
			for (i = 0; i < e->nin; ++i)
				samu_targetsdepth(ctx, e->in[i], depth - 1, indent + 1);
		}
	} else {
		samu_puts(ctx, n->path->s);
	}
}

static void
samu_targetsusage(struct samu_ctx *ctx)
{
	fprintf(stderr,
		"usage: %s ... -t targets [depth [maxdepth]]\n"
		"       %s ... -t targets rule [rulename]\n"
		"       %s ... -t targets all\n",
		ctx->argv0,
		ctx->argv0,
		ctx->argv0);
	exit(2);
}

static int
samu_targets(struct samu_ctx *ctx, int argc, char *argv[])
{
	struct samu_edge *e;
	size_t depth = 1, i;
	char *end, *mode, *name;

	if (argc > 3)
		samu_targetsusage(ctx);
	mode = argv[1];
	if (!mode || strcmp(mode, "depth") == 0) {
		if (argc == 3) {
			depth = strtol(argv[2], &end, 10);
			if (*end)
				samu_targetsusage(ctx);
		}
		for (e = ctx->graph.alledges; e; e = e->allnext) {
			for (i = 0; i < e->nout; ++i) {
				if (e->out[i]->nuse == 0)
					samu_targetsdepth(ctx, e->out[i], depth, 0);
			}
		}
	} else if (strcmp(mode, "rule") == 0) {
		name = argv[2];
		for (e = ctx->graph.alledges; e; e = e->allnext) {
			if (!name) {
				for (i = 0; i < e->nin; ++i) {
					if (!e->in[i]->gen)
						samu_puts(ctx, e->in[i]->path->s);
				}
			} else if (strcmp(e->rule->name, name) == 0) {
				for (i = 0; i < e->nout; ++i)
					samu_puts(ctx, e->out[i]->path->s);
			}
		}
	} else if (strcmp(mode, "all") == 0 && argc == 2) {
		for (e = ctx->graph.alledges; e; e = e->allnext) {
			for (i = 0; i < e->nout; ++i)
				samu_printf(ctx, "%s: %s\n", e->out[i]->path->s, e->rule->name);
		}
	} else {
		samu_targetsusage(ctx);
	}

	if (fflush(stdout) || ferror(stdout))
		samu_fatal("write failed");

	return 0;
}

const struct samu_tool *
samu_toolget(const char *name)
{
	static const struct samu_tool tools[] = {
		{ "clean", samu_clean },
		{ "commands", samu_commands },
		{ "compdb", samu_compdb },
		{ "graph", samu_graph },
		{ "query", samu_query },
		{ "targets", samu_targets },
	};

	const struct samu_tool *t;
	size_t i;

	t = NULL;
	for (i = 0; i < ARRAY_LEN(tools); ++i) {
		if (strcmp(name, tools[i].name) == 0) {
			t = &tools[i];
			break;
		}
	}
	if (!t)
		samu_fatal("unknown tool '%s'", name);

	return t;
}
