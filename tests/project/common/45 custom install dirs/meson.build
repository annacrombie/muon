project('custom install dirs', 'c')
executable('prog', 'prog.c', install: true, install_dir: 'dib/dab/dub')
executable(
    'prog2',
    'prog.c',
    install: true,
    install_dir: get_option('prefix') + '/dib/dab/dub2',
)
install_headers('sample.h', install_dir: 'some/dir')
install_headers('sample.h', install_dir: get_option('prefix') + '/some/dir2')
install_man('prog.1', install_dir: 'woman')
install_man('prog.1', install_dir: get_option('prefix') + '/woman2')
install_data('datafile.cat', install_dir: 'meow')
install_data('datafile.cat', install_dir: get_option('prefix') + '/meow2')
install_subdir('subdir', install_dir: 'woof')
install_subdir('subdir', install_dir: get_option('prefix') + '/woof2')
