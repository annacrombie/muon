# SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
# SPDX-License-Identifier: GPL-3.0-only

scdoc = find_program('scdoc', required: get_option('docs'))

if not scdoc.found()
    subdir_done()
endif

man_pages = {
    'muon': 1,
    'meson.build': 5,
}

man_page_targets = {}

foreach page, section : man_pages
    name = f'@page@.@section@'
    input = f'@name@.scd'
    tgt = custom_target(
        input,
        input: files(input),
        output: name,
        capture: true,
        feed: true,
        command: scdoc,
        install: true,
        install_dir: get_option('mandir') / 'man@0@'.format(section),
    )

    man_page_targets += {name: tgt}
endforeach

meson_proj = dependency('', required: false)
if python3.found()
    meson_proj = subproject('meson-docs', required: false)
endif
if meson_proj.found()
    man_page_targets += {
        'meson-reference.3': meson_proj.get_variable('meson_reference_3'),
    }
else
    warning('meson-docs not found, meson-reference.3 will not be built')
endif

if get_option('website')
    subdir('website')
endif

summary('docs', true)
