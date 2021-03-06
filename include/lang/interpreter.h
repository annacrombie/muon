#ifndef MUON_LANG_INTERPRETER_H
#define MUON_LANG_INTERPRETER_H

#include <stdbool.h>
#include <stddef.h>

#include "object.h"
#include "parser.h"
#include "workspace.h"

bool interp_node(struct workspace *wk, uint32_t n_id, obj *res);
bool interp_arithmetic(struct workspace *wk, uint32_t err_node,
	enum arithmetic_type type, bool plusassign, uint32_t nl, uint32_t nr,
	obj *res);

void interp_error(struct workspace *wk, uint32_t n_id, const char *fmt, ...)
__attribute__ ((format(printf, 3, 4)));
void interp_warning(struct workspace *wk, uint32_t n_id, const char *fmt, ...)
__attribute__ ((format(printf, 3, 4)));

bool typecheck_custom(struct workspace *wk, uint32_t n_id, obj obj_id, enum obj_type type, const char *fmt);
const char *typechecking_type_to_s(struct workspace *wk, enum obj_typechecking_type t);
bool typecheck_simple_err(struct workspace *wk, obj o, enum obj_type type);
bool typecheck_array(struct workspace *wk, uint32_t n_id, obj arr, enum obj_type type);
bool typecheck_dict(struct workspace *wk, uint32_t n_id, obj dict, enum obj_type type);
bool typecheck(struct workspace *wk, uint32_t n_id, obj obj_id, enum obj_type type);
bool boundscheck(struct workspace *wk, uint32_t n_id, uint32_t len, int64_t *i);
bool bounds_adjust(struct workspace *wk, uint32_t len, int64_t *i);
bool rangecheck(struct workspace *wk, uint32_t n_id, int64_t min, int64_t max, int64_t n);

void assign_variable(struct workspace *wk, const char *name, obj o, uint32_t n_id);
void unassign_variable(struct workspace *wk, const char *name);

void interpreter_init(void);
#endif
