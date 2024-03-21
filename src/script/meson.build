# SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
# SPDX-License-Identifier: GPL-3.0-only

scripts_input = []
scripts_cmdline = []

foreach s : [
    'copyfile.meson',
    'global_options.meson',
    'per_project_options.meson',
    'vcs_tagger.meson',

    'modules/_test.meson',
    'modules/gnome.meson',
]
    scripts_input += s
    scripts_cmdline += files(s) + [s]
endforeach

script_sources = custom_target(
    'embedded files',
    input: scripts_input,
    output: 'embedded_files.h',
    capture: true,
    command: [embedder, scripts_cmdline],
)
