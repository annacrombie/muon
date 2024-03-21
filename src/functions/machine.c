/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "error.h"
#include "functions/common.h"
#include "functions/machine.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/uname.h"

static const char *known_cpu_families[] = {
	"aarch64",
	"alpha",
	"arc",
	"arm",
	"avr",
	"c2000",
	"csky",
	"dspic",
	"e2k",
	"ft32",
	"ia64",
	"loongarch64",
	"m68k",
	"microblaze",
	"mips",
	"mips64",
	"msp430",
	"parisc",
	"pic24",
	"ppc",
	"ppc64",
	"riscv32",
	"riscv64",
	"rl78",
	"rx",
	"s390",
	"s390x",
	"sh4",
	"sparc",
	"sparc64",
	"wasm32",
	"wasm64",
	"x86",
	"x86_64",
};

static const char *
machine_system_to_s(enum machine_system sys)
{
	switch (sys) {
	case machine_system_dragonfly:
		return "dragonfly";
	case machine_system_freebsd:
		return "freebsd";
	case machine_system_gnu:
		return "gnu";
	case machine_system_haiku:
		return "haiku";
	case machine_system_linux:
		return "linux";
	case machine_system_netbsd:
		return "netbsd";
	case machine_system_openbsd:
		return "openbsd";
	case machine_system_sunos:
		return "sunos";
	case machine_system_android:
		return "android";
	case machine_system_emscripten:
		return "emscripten";
	case machine_system_windows:
		return "windows";
	case machine_system_cygwin:
		return "cygwin";
	case machine_system_msys2:
		return "msys2";
	case machine_system_darwin:
		return "darwin";
	case machine_system_unknown:
		return "unknown";
	}

	UNREACHABLE_RETURN;
}

enum machine_system
machine_system(void)
{
	const char *sysname;
	if (!uname_sysname(&sysname)) {
		return machine_system_unknown;
	}

	// The Cygwin environment for Windows
	if (str_startswith(&WKSTR(sysname), &WKSTR("cygwin_nt"))) {
		return machine_system_cygwin;
	}

	// The MSYS2 environment for Windows
	if (str_startswith(&WKSTR(sysname), &WKSTR("msys_nt"))) {
		return machine_system_msys2;
	}

	const struct {
		const char *name;
		enum machine_system sys;
	} map[] = {
		{ "darwin", machine_system_darwin }, // Either OSX or iOS
		{ "dragonfly", machine_system_dragonfly }, // DragonFly BSD
		{ "freebsd", machine_system_freebsd }, // FreeBSD and its derivatives
		{ "gnu", machine_system_gnu }, // GNU Hurd
		{ "haiku", machine_system_haiku },
		{ "linux", machine_system_linux },
		{ "netbsd", machine_system_netbsd },
		{ "openbsd", machine_system_openbsd },
		{ "sunos", machine_system_sunos }, // illumos and Solaris

		// TODO: These probably need more than just a simple mapping
		{ "android", machine_system_android }, // By convention only, subject to change
		{ "emscripten", machine_system_emscripten }, // Emscripten's Javascript environment
		{ "windows", machine_system_windows }, // Any version of Windows
	};

	uint32_t i;
	for (i = 0; i < ARRAY_LEN(map); ++i) {
		if (strcmp(map[i].name, sysname) == 0) {
			return map[i].sys;
		}
	}

	return machine_system_unknown;
}

static bool
func_machine_system(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = make_str(wk, machine_system_to_s(machine_system()));
	return true;
}

static bool
func_machine_endian(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	enum endianness e;
	if (!uname_endian(&e)) {
		return false;
	}

	const char *s = NULL;
	switch (e) {
	case little_endian:
		s = "little";
		break;
	case big_endian:
		s = "big";
		break;
	}

	*res = make_str(wk, s);
	return true;
}

static void
machine_cpu_normalize_base(const char **machine_cstr, const char **norm)
{
	const struct str *machine;

	*norm = NULL;

	if (!uname_machine(machine_cstr)) {
		LOG_E("unable to determine cpu information");
		*machine_cstr = "unknown";
		return;
	}

	machine = &WKSTR(*machine_cstr);

	if (str_startswith(machine, &WKSTR("aarch64"))) {
		*norm = "aarch64";
	} else if (str_startswith(machine, &WKSTR("earm"))) {
		*norm = "arm";
	} else if (str_startswith(machine, &WKSTR("mips"))) {
		if (strstr(machine->s, "64")) {
			*norm = "mips64";
		} else {
			*norm = "mips";
		}
	} else {
		const char *map[][2] = {
			{ "amd64", "x86_64" },
			{ "x64", "x86_64" },
			{ "i86pc", "x86_64" },
			0
		};

		uint32_t i;
		for (i = 0; map[i][0]; ++i) {
			if (str_eql(&WKSTR(map[i][0]), machine)) {
				*norm = map[i][1];
				break;
			}
		}
	}
}

static const char *
machine_cpu_family(void)
{
	const char *machine_cstr, *norm;
	const struct str *machine;

	machine_cpu_normalize_base(&machine_cstr, &norm);
	machine = &WKSTR(machine_cstr);

	if (norm) {
		goto done;
	}

	if (machine->s[0] == 'i' && str_endswith(machine, &WKSTR("86"))) {
		norm = "x86";
	} else if (str_startswith(machine, &WKSTR("arm"))) {
		norm = "arm";
	} else if (str_startswith(machine, &WKSTR("powerpc64"))
		   || str_startswith(machine, &WKSTR("ppc64"))) {
		norm = "ppc64";
	} else if (str_startswith(machine, &WKSTR("powerpc"))
		   || str_startswith(machine, &WKSTR("ppc"))) {
		norm = "ppc";
	} else {
		const char *map[][2] = {
			{ "bepc", "x86" },
			{ "arm64", "aarch64" },
			{ "macppc", "ppc" },
			{ "power macintosh", "ppc" },
			{ "amd64", "x86_64" },
			{ "x64", "x86_64" },
			{ "i86pc", "x86_64" },
			{ "sun4u", "sparc64" },
			{ "sun4v", "sparc64" },
			{ "ip30", "mpis64" },
			{ "ip35", "mpis64" },
			0
		};

		uint32_t i;
		for (i = 0; map[i][0]; ++i) {
			if (str_eql(&WKSTR(map[i][0]), machine)) {
				norm = map[i][1];
				break;
			}
		}
	}

done:
	if (!norm) {
		norm = machine->s;
	}

	uint32_t i;
	for (i = 0; i < ARRAY_LEN(known_cpu_families); ++i) {
		if (strcmp(norm, known_cpu_families[i]) == 0) {
			return norm;
		}
	}

	LOG_W("returning unknown cpu family '%s'", machine->s);
	return norm;
}

static bool
func_machine_cpu_family(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = make_str(wk, machine_cpu_family());
	return true;
}

uint32_t
machine_cpu_address_bits(void)
{
	const char *is_64_bit[] = {
		"aarch64",
		"alpha",
		"ia64",
		"loongarch64",
		"mips64",
		"ppc64",
		"riscv64",
		"s390x",
		"sparc64",
		"wasm64",
		"x86_64",
	};

	const char *fam = machine_cpu_family();

	uint32_t i;
	for (i = 0; i < ARRAY_LEN(is_64_bit); ++i) {
		if (strcmp(fam, is_64_bit[i]) == 0) {
			return 64;
		}
	}

	return 32;
}


static bool
func_machine_cpu(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	const char *machine_cstr, *norm;

	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	machine_cpu_normalize_base(&machine_cstr, &norm);
	if (!norm) {
		norm = machine_cstr;
	}

	*res = make_str(wk, norm);
	return true;
}

const struct func_impl impl_tbl_machine[] = {
	{ "cpu", func_machine_cpu, tc_string },
	{ "cpu_family", func_machine_cpu_family, tc_string },
	{ "endian", func_machine_endian, tc_string },
	{ "system", func_machine_system, tc_string },
	{ NULL, NULL },
};
