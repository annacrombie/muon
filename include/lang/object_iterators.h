#ifndef LANG_OBJECT_ITERATORS_H
#define LANG_OBJECT_ITERATORS_H

#include <stdbool.h>
#include <stdint.h>

#include "preprocessor_helpers.h"

/******************************************************************************
 * obj_array_for
 ******************************************************************************/

#define obj_array_for(__wk, __arr, __val)                                      \
	for (struct obj_array *__a = get_obj_array(__wk, __arr); a->have_next; \
		__a = get_obj_array(wk, __a->next), __val = __a->val)

/******************************************************************************
 * obj_dict_for
 ******************************************************************************/

struct obj_dict_for_helper {
	struct obj_dict *d;
	struct hash *h;
	struct obj_dict_elem *e;
	void *k;
	uint64_t *v;
	uint32_t i;
	bool big;
};

#define obj_dict_for_get_kv_big(__iter, __key, __val)                                           \
	__iter.k = arr_get(&__iter.h->keys, __iter.i), __iter.v = hash_get(__iter.h, __iter.k), \
	__key = *__iter.v >> 32, __val = *__iter.v & 0xffffffff

#define obj_dict_for_get_kv(__iter, __key, __val) __key = __iter.e->key, __val = __iter.e->val

#define obj_dict_for_(__wk, __dict, __key, __val, __iter)                                            \
	struct obj_dict_for_helper __iter = {                                                        \
		.d = get_obj_dict(wk, __dict),                                                       \
	};                                                                                           \
	for (__iter.big = __iter.d->flags & obj_dict_flag_big,                                       \
	    __iter.h = __iter.big ? bucket_arr_get(&wk->vm.objects.dict_hashes, __iter.d->data) : 0, \
	    __iter.e = !__iter.big ? bucket_arr_get(&wk->vm.objects.dict_elems, __iter.d->data) : 0, \
	    __iter.big ? (obj_dict_for_get_kv_big(__iter, __key, __val)) :                           \
			 (obj_dict_for_get_kv(__iter, __key, __val));                                \
		__iter.big ? __iter.i < __iter.h->keys.len : __iter.e->next;                         \
		__iter.big ? (++__iter.i, obj_dict_for_get_kv_big(__iter, __key, __val)) :           \
			     (__iter.e = bucket_arr_get(&wk->vm.objects.dict_elems, __iter.e->next), \
			     obj_dict_for_get_kv(__iter, __key, __val)))

#define obj_dict_for(__wk, __dict, __key, __val) obj_dict_for_(__wk, __dict, __key, __val, CONCAT(__iter, __LINE__))

/******************************************************************************
 * obj_array_flat_for
 ******************************************************************************/

struct obj_array_flat_for_helper {
	struct workspace *wk;
	struct obj_array *a;
	uint32_t stack_base;
	bool empty;
};

#define obj_array_flat_for_begin(__wk, __arr, __val)                                  \
	{                                                                             \
		struct obj_array_flat_for_helper __flat_iter = {                      \
			.wk = __wk,                                                   \
			.a = get_obj_array(__wk, __arr),                              \
			.stack_base = __wk->stack.len,                                \
		};                                                                    \
		__flat_iter.empty = __flat_iter.a->len == 0;                          \
                                                                                      \
		while (true) {                                                        \
			__val = __flat_iter.a->val;                                   \
			if (get_obj_type(__flat_iter.wk, __val) == obj_array) {       \
				stack_push(&__flat_iter.wk->stack, __flat_iter.a);    \
				__flat_iter.a = get_obj_array(__flat_iter.wk, __val); \
				__flat_iter.empty = __flat_iter.a->len == 0;          \
				continue;                                             \
			}                                                             \
                                                                                      \
			if (!__flat_iter.empty)

#define obj_array_flat_for_end                                                              \
	}                                                                                   \
	if (__flat_iter.a->have_next) {                                                     \
		__flat_iter.a = get_obj_array(__flat_iter.wk, __flat_iter.a->next);         \
	} else if (__flat_iter.stack_base < __flat_iter.wk->stack.len) {                    \
		stack_pop(&__flat_iter.wk->stack, __flat_iter.a);                           \
		if (__flat_iter.a->have_next) {                                             \
			__flat_iter.a = get_obj_array(__flat_iter.wk, __flat_iter.a->next); \
		} else {                                                                    \
			break;                                                              \
		}                                                                           \
	} else {                                                                            \
		break;                                                                      \
	}                                                                                   \
	}

#endif
