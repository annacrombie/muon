# SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
# SPDX-License-Identifier: GPL-3.0-only

tests = [
    ['array.meson'],
    ['badnum.meson', {'should_fail': true}],
    ['configuration_data.meson'],
    ['dict.meson'],
    ['disabler.meson'],
    ['environment.meson', {'env': 'inherited=secret'}],
    ['fstring.meson'],
    ['join.meson'],
    ['join_paths.meson'],
    ['katie.meson'],
    ['multiline.meson'],
    ['range.meson'],
    ['run_command.meson'],
    ['string_format_escape.meson'],
    ['strings.meson'],
    ['ternary.meson'],
    ['unicode.meson'],
    ['version_compare.meson'],
]

foreach t : tests
    kwargs = t.length() == 2 ? t[1] : {}

    args = ['internal', 'eval'] + files(t[0])

    test(t[0], muon, args: args, kwargs: kwargs, suite: 'lang')
endforeach
