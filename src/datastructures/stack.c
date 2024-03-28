/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <assert.h>
#include <string.h>

#include "datastructures/stack.h"
#include "log.h"
#include "platform/mem.h"

/******************************************************************************
 * stack
 ******************************************************************************/

void
stack_init(struct stack *stack, uint32_t cap)
{
	*stack = (struct stack){
		.mem = z_malloc(cap),
		.cap = cap,
	};
}

void
stack_destroy(struct stack *stack)
{
	z_free(stack->mem);
}

struct stack_tag {
	const char *name;
	uint32_t size;
};

static void
stack_push_raw(struct stack *stack, const void *mem, uint32_t size)
{
	assert(stack->len + size < stack->cap);
	memcpy(stack->mem + stack->len, mem, size);
	stack->len += size;
}

static void
stack_pop_raw(struct stack *stack, void *mem, uint32_t size)
{
	assert(stack->len >= size);
	stack->len -= size;
	memcpy(mem, stack->mem + stack->len, size);
}

static void
stack_peek_raw(struct stack *stack, void *mem, uint32_t size, uint32_t *off)
{
	assert(*off >= size);
	*off -= size;
	memcpy(mem, stack->mem + *off, size);
}

void
stack_print(struct stack *_stack)
{
	struct stack_tag tag;
	struct stack stack = *_stack;
	while (stack.len) {
		stack_pop_raw(&stack, &tag, sizeof(tag));
		printf("  - %04d - %s", tag.size, tag.name);

		assert(stack.len >= tag.size);
		stack.len -= tag.size;
		void *mem = stack.mem + stack.len;

		stack.cb(stack.ctx, mem, &tag);

		printf("\n");
	}
}

void
stack_push_sized(struct stack *stack, const void *mem, uint32_t size, const char *name)
{
	stack_push_raw(stack, mem, size);
	stack_push_raw(stack, &(struct stack_tag){ name, size }, sizeof(struct stack_tag));

	if (stack->log) {
		L("\033[33mstack\033[0m %05d %s pushed %s (%d)", stack->len, stack->name, name, size);
		/* stack_print(stack); */
	}
}

void
stack_pop_sized(struct stack *stack, void *mem, uint32_t size)
{
	struct stack_tag tag;
	stack_pop_raw(stack, &tag, sizeof(tag));

	assert(size == tag.size);

	stack_pop_raw(stack, mem, size);

	if (stack->log) {
		L("\033[33mstack\033[0m %05d %s popped %s (%d)", stack->len, stack->name, tag.name, tag.size);
		/* stack_print(stack); */
	}
}

void
stack_peek_sized(struct stack *stack, void *mem, uint32_t size)
{
	uint32_t off = stack->len;
	struct stack_tag tag;
	stack_peek_raw(stack, &tag, sizeof(tag), &off);

	assert(size == tag.size);

	stack_peek_raw(stack, mem, size, &off);
}
