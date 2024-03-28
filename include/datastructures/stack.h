/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_DATASTRUCTURES_STACK_H
#define MUON_DATASTRUCTURES_STACK_H

#include <stdbool.h>
#include <stdint.h>

#include "preprocessor_helpers.h"

struct stack_tag;

typedef void (*stack_print_cb)(void *ctx, void *mem, struct stack_tag *tag);

struct stack {
	char *mem;
	uint32_t len, cap;

	const char *name;
	bool log;
	stack_print_cb cb;
	void *ctx;
};

void stack_init(struct stack *stack, uint32_t cap);

void stack_print(struct stack *_stack);
void stack_push_sized(struct stack *stack, const void *mem, uint32_t size, const char *name);
void stack_pop_sized(struct stack *stack, void *mem, uint32_t size);
void stack_peek_sized(struct stack *stack, void *mem, uint32_t size);

#define stack_push(__stack, __it, __nv) \
	stack_push_sized((__stack), &(__it), (sizeof(__it)), __FILE__ ":" LINE_STRING " " #__it), __it = __nv
#define stack_pop(__stack, __it) stack_pop_sized((__stack), &(__it), (sizeof(__it)))
#define stack_peek(__stack, __it) stack_peek_sized((__stack), &(__it), (sizeof(__it)))

#endif
