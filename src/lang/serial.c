/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "backend/output.h"
#include "buf_size.h"
#include "lang/serial.h"
#include "lang/workspace.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/mem.h"
#include "platform/path.h"

#define SERIAL_MAGIC_LEN 8
static const char serial_magic[SERIAL_MAGIC_LEN] = "muondump";
static const uint32_t serial_version = 7;

static bool
corrupted_dump(void)
{
	LOG_E("unable to load corrupted serial dump");
	return false;
}


static bool
dump_uint32(uint32_t v, FILE *f)
{
	return fs_fwrite(&v, sizeof(uint32_t), f);
}

static bool
dump_uint64(uint64_t v, FILE *f)
{
	return fs_fwrite(&v, sizeof(uint64_t), f);
}

static bool
load_uint32(uint32_t *v, FILE *f)
{
	return fs_fread(v, sizeof(uint32_t), f);
}

static bool
load_uint64(uint64_t *v, FILE *f)
{
	return fs_fread(v, sizeof(uint64_t), f);
}

static bool
dump_bucket_arr(const struct bucket_arr *ba, FILE *f)
{
	uint32_t i;

	if (!dump_uint32(ba->buckets.len, f)) {
		return false;
	}

	for (i = 0; i < ba->buckets.len; ++i) {
		struct bucket *b = arr_get(&ba->buckets, i);

		if (!dump_uint32(b->len, f)) {
			return false;
		}

		if (!fs_fwrite(b->mem, ba->item_size * b->len, f)) {
			return false;
		}
	}

	return true;
}

static bool
load_bucket_arr(struct bucket_arr *ba, FILE *f)
{
	uint32_t buckets_len;
	uint32_t i;
	struct bucket b = { 0 };

	assert(ba->len == 0);

	if (!load_uint32(&buckets_len, f)) {
		return false;
	}

	z_free(((struct bucket *)arr_get(&ba->buckets, 0))->mem);
	arr_clear(&ba->buckets);

	for (i = 0; i < buckets_len; ++i) {
		init_bucket(ba, &b);

		if (!load_uint32(&b.len, f)) {
			goto done;
		}

		if (b.len > ba->bucket_size) {
			corrupted_dump();
			goto done;
		}

		ba->len += b.len;

		if (!fs_fread(b.mem, ba->item_size * b.len, f)) {
			goto done;
		}

		arr_push(&ba->buckets, &b);
		continue;
done:
		z_free(b.mem);
		return corrupted_dump();
	}

	return true;
}

static bool
dump_serial_header(FILE *f)
{
	return fs_fwrite(serial_magic, SERIAL_MAGIC_LEN, f)
	       && dump_uint32(serial_version, f);
}

static bool
load_serial_header(FILE *f)
{
	char buf[SERIAL_MAGIC_LEN] = { 0 };

	if (!fs_fread(buf, SERIAL_MAGIC_LEN, f)) {
		return false;
	}

	if (memcmp(buf, serial_magic, SERIAL_MAGIC_LEN) != 0) {
		LOG_E("invalid file (missing magic)");
		return false;
	}

	uint32_t v;
	if (!load_uint32(&v, f)) {
		return false;
	}

	if (v != serial_version) {
		LOG_E("unable to load data file created by a different version of muon (%d != %d)", v, serial_version);
		return false;
	}

	return true;
}

static bool
dump_big_strings(struct workspace *wk, struct arr *offsets, FILE *f)
{
	uint64_t len = 0;
	uint64_t start, end;

	// dump a placeholder at the start
	if (!fs_ftell(f, &start)) {
		return false;
	} else if (!dump_uint64(0, f)) {
		return false;
	}

	uint32_t i;
	struct bucket_arr *str_ba = &wk->vm.objects.obj_aos[obj_string - _obj_aos_start];
	for (i = 0; i < str_ba->len; ++i) {
		struct str *ss = bucket_arr_get(str_ba, i);

		if (!(ss->flags & str_flag_big)) {
			continue;
		}

		if (!fs_fwrite(ss->s, ss->len + 1, f)) {
			return false;
		}

		arr_push(offsets, &len);
		len += ss->len + 1;
	}

	// jump back to the beginning, write the length, and then return
	if (!fs_ftell(f, &end)) {
		return false;
	} else if (!fs_fseek(f, start)) {
		return false;
	} else if (!dump_uint64(len, f)) {
		return false;
	} else if (!fs_fseek(f, end)) {
		return false;
	}

	return true;
}

struct big_string_table {
	uint8_t *data;
	uint64_t len;
};

static bool
load_big_strings(struct workspace *wk, struct big_string_table *bst, FILE *f)
{
	uint64_t len;
	uint8_t *buf = NULL;
	if (!load_uint64(&len, f)) {
		return false;
	}

	if (len) {
		if (len > UINT32_MAX) {
			return corrupted_dump();
		}

		buf = z_calloc(1, len);
		if (!fs_fread(buf, len, f)) {
			return false;
		}
	}

	*bst = (struct big_string_table){
		.data = buf,
		.len = len,
	};

	return true;
}

struct serial_str {
	uint64_t s, len;
	enum str_flags flags;
};

static bool
get_big_string(struct workspace *wk, const struct big_string_table *bst, struct serial_str *src, struct str *res)
{
	assert(src->flags & str_flag_big);

	if (src->s >= bst->len) {
		return corrupted_dump();
	}

	char *buf = z_calloc(1, src->len + 1);
	memcpy(buf, &bst->data[src->s], src->len);

	*res = (struct str) {
		.flags = src->flags,
		.len = src->len,
		.s = buf,
	};

	return true;
}

static bool
dump_objs(struct workspace *wk, struct arr *big_string_offsets, FILE *f)
{
	if (!dump_uint32(wk->vm.objects.objs.len - 1, f)) {
		return false;
	}

	struct serial_str ser_s;
	// memsan is upset about uninitialized padding bytes in this struct
	// when we try to write it out.  A better solution would be to not
	// write the padding bytes to disk, but that would complicate the code.
	memset(&ser_s, 0, sizeof(struct serial_str));

	void *data;
	size_t len;
	uint8_t type_tag;

	assert(obj_type_count < UINT8_MAX && "increase size of type tag");

	uint32_t i, big_string_i = 0;
	for (i = 1; i < wk->vm.objects.objs.len; ++i) {
		struct obj_internal *o = bucket_arr_get(&wk->vm.objects.objs, i);
		type_tag = o->t;

		if (!fs_fwrite(&type_tag, sizeof(uint8_t), f)) {
			return false;
		}

		if (o->t == obj_string) {
			const struct str *ss =
				bucket_arr_get(&wk->vm.objects.obj_aos[obj_string - _obj_aos_start], o->val);

			ser_s = (struct serial_str) {
				.len = ss->len,
				.flags = ss->flags,
			};

			if (ss->flags & str_flag_big) {
				ser_s.s = *(uint64_t *)arr_get(big_string_offsets, big_string_i);
				++big_string_i;
			} else {
				if (!bucket_arr_lookup_pointer(&wk->vm.objects.chrs, (uint8_t *)ss->s, &ser_s.s)) {
					assert(false && "pointer not found");
				}
			}

			data = &ser_s;
			len = sizeof(struct serial_str);
		} else if (o->t < _obj_aos_start) {
			data = &o->val;
			len = sizeof(uint32_t);
		} else {
			struct bucket_arr *ba = &wk->vm.objects.obj_aos[o->t - _obj_aos_start];
			data = bucket_arr_get(ba, o->val);
			len = ba->item_size;
		}

		if (!fs_fwrite(data, len, f)) {
			return false;
		}
	}

	return true;
}

static bool
load_objs(struct workspace *wk, const struct big_string_table *bst, FILE *f)
{
	uint32_t len;
	if (!load_uint32(&len, f)) {
		return false;
	}

	if (!len) {
		return true;
	}

	uint8_t type_tag;
	struct serial_str ser_s;
	struct bucket_arr *ba;

	uint32_t i;
	for (i = 0; i < len; ++i) {
		if (!fs_fread(&type_tag, sizeof(uint8_t), f)) {
			return false;
		}

		bucket_arr_pushn(&wk->vm.objects.objs, NULL, 0, 1);
		struct obj_internal *o = bucket_arr_get(&wk->vm.objects.objs, wk->vm.objects.objs.len - 1);

		*o = (struct obj_internal) { .t = type_tag, };

		if (type_tag < _obj_aos_start) {
			if (!fs_fread(&o->val, sizeof(uint32_t), f)) {
				return false;
			}
			continue;
		}

		if (type_tag >= obj_type_count) {
			return corrupted_dump();
		}

		ba = &wk->vm.objects.obj_aos[type_tag - _obj_aos_start];
		o->val = ba->len;
		bucket_arr_pushn(ba, NULL, 0, 1);

		if (type_tag == obj_string) {
			if (!fs_fread(&ser_s, sizeof(struct serial_str), f)) {
				return false;
			}

			struct str *ss = bucket_arr_get(ba, o->val);

			if (ser_s.flags & str_flag_big) {
				if (!get_big_string(wk, bst, &ser_s, ss)) {
					return false;
				}
			} else {
				uint32_t bucket_i = ser_s.s % wk->vm.objects.chrs.bucket_size,
					 buckets_i = ser_s.s / wk->vm.objects.chrs.bucket_size;
				if (buckets_i > wk->vm.objects.chrs.buckets.len
				    || bucket_i > ((struct bucket *)(arr_get(&wk->vm.objects.chrs.buckets, buckets_i)))->len) {
					return corrupted_dump();
				}

				*ss = (struct str){
					.s = bucket_arr_get(&wk->vm.objects.chrs, ser_s.s),
					.len = ser_s.len,
					.flags = ser_s.flags,
				};
			}
		} else {
			if (!fs_fread(bucket_arr_get(ba, o->val), ba->item_size, f)) {
				return false;
			}
		}
	}

	return true;
}

bool
serial_dump(struct workspace *wk_src, obj o, FILE *f)
{
	bool ret = false;
	struct workspace wk_dest = { 0 };
	workspace_init_bare(&wk_dest);

	struct arr big_string_offsets;
	arr_init(&big_string_offsets, 32, sizeof(uint64_t));

	obj obj_dest;
	if (!obj_clone(wk_src, &wk_dest, o, &obj_dest)) {
		goto ret;
	}

	/* obj_fprintf(&wk_dest, log_file(), "saving %o\n", obj_dest); */

	if (!(dump_serial_header(f)
	      && dump_uint32(obj_dest, f)
	      && dump_bucket_arr(&wk_dest.vm.objects.chrs, f)
	      && dump_big_strings(&wk_dest, &big_string_offsets, f)
	      && dump_objs(&wk_dest, &big_string_offsets, f)
	      && dump_bucket_arr(&wk_dest.vm.objects.dict_elems, f))) {
		goto ret;
	}

	ret = true;
ret:
	workspace_destroy_bare(&wk_dest);
	arr_destroy(&big_string_offsets);
	return ret;
}

bool
serial_load(struct workspace *wk, obj *res, FILE *f)
{
	bool ret = false;
	struct workspace wk_src = { 0 };
	workspace_init_bare(&wk_src);
	bucket_arr_clear(&wk_src.vm.objects.dict_elems); // remove null dict_elem

	struct big_string_table bst = { 0 };

	obj obj_src;
	if (!(load_serial_header(f)
	      && load_uint32(&obj_src, f)
	      && load_bucket_arr(&wk_src.vm.objects.chrs, f)
	      && load_big_strings(&wk_src, &bst, f)
	      && load_objs(&wk_src, &bst, f)
	      && load_bucket_arr(&wk_src.vm.objects.dict_elems, f))) {
		goto ret;
	}

	/* obj_fprintf(&wk_src, log_file(), "loaded %o\n", obj_src); */

	if (!obj_clone(&wk_src, wk, obj_src, res)) {
		corrupted_dump();
		goto ret;
	}

	ret = true;
ret:
	if (bst.data) {
		z_free(bst.data);
	}
	workspace_destroy_bare(&wk_src);
	return ret;
}

bool
serial_load_from_private_dir(struct workspace *wk, obj *res, const char *file)
{
	SBUF(path);
	path_join(wk, &path, output_path.private_dir, file);

	if (!fs_file_exists(path.buf)) {
		return false;
	}

	FILE *f;
	if (!(f = fs_fopen(path.buf, "rb"))) {
		return false;
	}

	bool ret = serial_load(wk, res, f);

	if (!fs_fclose(f)) {
		ret = false;
	}

	return ret;
}
