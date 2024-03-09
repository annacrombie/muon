/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_DATA_ARR_H
#define MUON_DATA_ARR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct arr {
	size_t len;
	size_t cap;
	size_t item_size;
	uint8_t *e;
};

void arr_init(struct arr *arr, size_t initial, size_t item_size);
void arr_destroy(struct arr *arr);
size_t arr_push(struct arr *arr, const void *item);
void arr_pop(struct arr *arr, void *e);
void *arr_get(const struct arr *arr, size_t i);
void arr_del(struct arr *arr, size_t i);
void arr_clear(struct arr *arr);
void arr_grow_by(struct arr *arr, size_t size);

typedef int32_t (*sort_func)(const void *a, const void *b, void *ctx);
void arr_sort(struct arr *arr, void *ctx, sort_func func);
#endif
