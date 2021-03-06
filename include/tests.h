#ifndef MUON_TESTS_H
#define MUON_TESTS_H
#include <stdbool.h>
#include <stdint.h>

#include "lang/object.h"

enum test_flag {
	test_flag_should_fail = 1 << 0,
};

#define MAX_CMDLINE_TEST_SUITES 64

enum test_display {
	test_display_auto,
	test_display_dots,
	test_display_bar,
};

struct test_options {
	const char *suites[MAX_CMDLINE_TEST_SUITES];
	uint32_t suites_len, workers, verbosity;
	enum test_display display;
	bool fail_fast, print_summary, no_rebuild;

	enum test_category cat;
};

bool tests_run(struct test_options *opts);
#endif
