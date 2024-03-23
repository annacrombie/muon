/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "datastructures/bucket_arr.h"
#include "log.h"
#include "platform/mem.h"

void
init_bucket(struct bucket_arr *ba, struct bucket *b)
{
	b->mem = z_calloc(ba->item_size, ba->bucket_size);
}

uint64_t
bucket_arr_size(struct bucket_arr *ba)
{
	return ba->buckets.len * ba->item_size * ba->bucket_size;
}

void
bucket_arr_init(struct bucket_arr *ba, uint32_t bucket_size, uint32_t item_size)
{
	assert(item_size > 0);

	*ba = (struct bucket_arr){
		.item_size = item_size,
		.bucket_size = bucket_size,
	};

	arr_init(&ba->buckets, 1, sizeof(struct bucket));

	arr_push(&ba->buckets, &(struct bucket){ 0 });
	init_bucket(ba, arr_get(&ba->buckets, 0));
}

void
bucket_arr_clear(struct bucket_arr *ba)
{
	uint32_t i;
	struct bucket *b;

	for (i = 0; i < ba->buckets.len; ++i) {
		b = arr_get(&ba->buckets, i);
		b->len = 0;
	}

	ba->buckets.len = 1;
	ba->len = 0;
}

void
bucket_arr_save(const struct bucket_arr *ba, struct bucket_arr_save *save)
{
	struct bucket *b;
	b = arr_get(&ba->buckets, ba->tail_bucket);

	save->tail_bucket = ba->tail_bucket;
	save->tail_bucket_len = b->len;
}

void
bucket_arr_restore(struct bucket_arr *ba, const struct bucket_arr_save *save)
{
	struct bucket *b;

	b = arr_get(&ba->buckets, save->tail_bucket);
	assert(save->tail_bucket_len <= b->len);
	ba->len -= b->len - save->tail_bucket_len;
	b->len = save->tail_bucket_len;
	memset(&b->mem[b->len * ba->item_size], 0, (ba->bucket_size - b->len) * ba->item_size);

	uint32_t bi;
	for (bi = save->tail_bucket + 1; bi < ba->buckets.len; ++bi) {
		b = arr_get(&ba->buckets, bi);
		memset(b->mem, 0, b->len * ba->item_size);
		ba->len -= b->len;
		b->len = 0;
	}

	ba->tail_bucket = save->tail_bucket;
}

void *
bucket_arr_pushn(struct bucket_arr *ba, const void *data, uint32_t data_len, uint32_t reserve)
{
	void *dest;
	struct bucket *b;

	assert(reserve >= data_len);
	assert(reserve <= ba->bucket_size);

	b = arr_get(&ba->buckets, ba->tail_bucket);

	if (b->len + reserve > ba->bucket_size) {
		if (ba->tail_bucket >= ba->buckets.len - 1) {
			arr_push(&ba->buckets, &(struct bucket){ 0 });
			++ba->tail_bucket;
			b = arr_get(&ba->buckets, ba->tail_bucket);
			init_bucket(ba, b);
		} else {
			++ba->tail_bucket;
			b = arr_get(&ba->buckets, ba->tail_bucket);
			assert(b->mem);
			assert(b->len == 0);
		}
	}

	dest = b->mem + (b->len * ba->item_size);
	if (data) {
		memcpy(dest, data, ba->item_size * data_len);
	}
	b->len += reserve;
	ba->len += reserve;

	return dest;
}

void *
bucket_arr_push(struct bucket_arr *ba, const void *item)
{
	return bucket_arr_pushn(ba, item, 1, 1);
}

void *
bucket_arr_get(const struct bucket_arr *ba, uint32_t i)
{
	struct bucket *b;
	uint32_t bucket_i = i % ba->bucket_size;

	b = arr_get(&ba->buckets, i / ba->bucket_size);
	assert(bucket_i < b->len);

	return b->mem + (bucket_i * ba->item_size);
}

void
bucket_arr_destroy(struct bucket_arr *ba)
{
	uint32_t i;

	struct bucket *b;

	for (i = 0; i < ba->buckets.len; ++i) {
		b = arr_get(&ba->buckets, i);

		z_free(b->mem);
	}

	arr_destroy(&ba->buckets);
}

bool
bucket_arr_lookup_pointer(struct bucket_arr *ba, const uint8_t *p, uint64_t *ret)
{
	uint32_t i;
	for (i = 0; i < ba->buckets.len; ++i) {
		struct bucket *b = arr_get(&ba->buckets, i);

		if (b->mem <= p && p < b->mem + (b->len * ba->item_size)) {
			*ret = i * ba->bucket_size + (p - b->mem) / ba->item_size;
			return true;
		}
	}

	return false;
}
