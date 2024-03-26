# SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
# SPDX-License-Identifier: GPL-3.0-only

add_test_setup('valgrind', exclude_suites: 'project', exe_wrapper: ['valgrind'])
add_test_setup('no_python', exclude_suites: 'requires_python')

# subdir('fmt')
subdir('fuzz')
subdir('lang')
subdir('project')
