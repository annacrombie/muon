project('custom-target-dir-install')

docgen = find_program('docgen.py')

custom_target(
    'docgen',
    output: 'html',
    command: [docgen, '@OUTPUT@'],
    install: true,
    build_by_default: false,
    install_dir: join_paths(get_option('datadir'), 'doc/testpkgname'),
)
