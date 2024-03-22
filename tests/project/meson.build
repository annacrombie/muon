# SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
# SPDX-License-Identifier: GPL-3.0-only

if not host_machine.cpu_family().startswith('x86')
    fail_on_non_x86 = ['failing']
else
    fail_on_non_x86 = []
endif

tests = [
    # project tests created for muon
    ['muon/timeout', ['failing']],
    ['muon/sizeof_invalid'],
    ['muon/str'],
    ['muon/python', ['python']],
    ['muon/script_module'],

    # project tests imported from meson
    ['common/1 trivial'],
    ['common/2 cpp'],
    ['common/3 static'],
    ['common/4 shared'],
    ['common/5 linkstatic'],
    ['common/6 linkshared'],
    ['common/7 mixed'],
    ['common/8 install', ['python']],
    ['common/9 header install'],
    ['common/10 man install'],
    ['common/11 subdir'],
    ['common/12 data'],
    # requires pch :)
    # ['common/13 pch', ['python', 'failing']],
    ['common/14 configure file', ['python']],
    ['common/15 if'],
    ['common/16 comparison'],
    ['common/17 array'],
    ['common/18 includedir'],
    ['common/19 header in file list'],
    ['common/20 global arg'],
    ['common/21 target arg'],
    ['common/22 object extraction', ['python']],
    ['common/23 endian'],
    ['common/24 library versions'],
    ['common/25 config subdir'],
    ['common/26 find program', ['python']],
    ['common/27 multiline string'],
    ['common/28 try compile'],
    ['common/29 compiler id'],
    ['common/30 sizeof'],
    ['common/31 define10'],
    ['common/32 has header'],
    ['common/33 run program', ['python']],
    ['common/34 logic ops'],
    ['common/35 string operations'],
    ['common/36 has function'],
    ['common/37 has member'],
    ['common/38 alignment'],
    ['common/39 library chain'],
    ['common/40 options'],
    ['common/41 test args', ['python']],
    ['common/42 subproject'],
    ['common/43 subproject options'],
    ['common/44 pkgconfig-gen', ['python']],
    ['common/45 custom install dirs'],
    ['common/46 subproject subproject'],
    ['common/47 same file name'],
    ['common/48 file grabber'],
    ['common/49 custom target', ['python', 'skip_analyze']],
    ['common/50 custom target chain', ['python']],
    ['common/51 run target', ['python']],
    ['common/52 object generator', ['python']],
    ['common/53 install script', ['python']],
    ['common/54 custom target source output', ['python']],
    ['common/55 exe static shared'],
    ['common/56 array methods'],
    ['common/57 custom header generator', ['python']],
    ['common/58 multiple generators', ['python']],
    ['common/59 install subdir'],
    ['common/60 foreach'],
    ['common/61 number arithmetic'],
    ['common/62 string arithmetic'],
    ['common/63 array arithmetic'],
    ['common/64 arithmetic bidmas'],
    ['common/65 build always', ['python']],
    ['common/66 vcstag', ['python']],
    ['common/67 modules'],
    ['common/68 should fail'],
    ['common/69 configure file in custom target', ['python']],
    ['common/70 external test program', ['python']],
    ['common/71 ctarget dependency', ['python']],
    ['common/72 shared subproject'],
    ['common/73 shared subproject 2'],
    ['common/74 file object'],
    ['common/75 custom subproject dir'],
    ['common/76 has type'],
    ['common/77 extract from nested subdir'],
    ['common/78 internal dependency'],
    ['common/79 same basename'],
    ['common/80 declare dep'],
    ['common/81 extract all'],
    ['common/82 add language'],
    ['common/83 identical target name in subproject', ['python']],
    ['common/84 plusassign'],
    ['common/85 skip subdir'],
    ['common/86 private include', ['python']],
    ['common/87 default options'],
    ['common/88 dep fallback', ['python']],
    ['common/89 default library'],
    ['common/90 gen extra', ['python']],
    ['common/91 benchmark'],
    ['common/92 test workdir', ['python']],
    ['common/93 suites'],
    ['common/94 threads'],
    ['common/95 manygen', ['python']],
    ['common/96 stringdef'],
    ['common/97 find program path', ['python']],
    ['common/98 subproject subdir'],
    ['common/99 postconf', ['python']],
    ['common/100 postconf with args', ['python']],
    ['common/101 testframework options'],
    ['common/102 extract same name'],
    ['common/103 has header symbol'],
    ['common/104 has arg'],
    ['common/105 generatorcustom', ['python']],
    ['common/106 multiple dir configure file', ['python']],
    ['common/107 spaces backslash'],
    ['common/108 ternary'],
    ['common/109 custom target capture', ['python']],
    ['common/110 allgenerate', ['python']],
    ['common/111 pathjoin'],
    ['common/112 subdir subproject'],
    ['common/113 interpreter copy mutable var on assignment'],
    ['common/114 skip'],
    ['common/115 subproject project arguments'],
    ['common/116 test skip'],
    ['common/117 shared module'],
    ['common/118 llvm ir and assembly'],
    ['common/119 cpp and asm'],
    ['common/120 extract all shared library'],
    ['common/121 object only target', ['python']],
    ['common/122 no buildincdir'],
    ['common/123 custom target directory install', ['python']],
    ['common/124 dependency file generation'],
    ['common/125 configure file in generator', ['python']],
    ['common/126 generated llvm ir', ['python']],
    ['common/127 generated assembly', ['python']],
    ['common/128 build by default targets in tests', ['python']],
    ['common/129 build by default', ['python']],
    ['common/130 include order', ['python']],
    ['common/131 override options'],
    ['common/132 get define'],
    ['common/133 c cpp and asm'],
    ['common/134 compute int'],
    ['common/135 custom target object output', ['python']],
    ['common/136 empty build file'],
    ['common/137 whole archive'],
    ['common/138 C and CPP link'],
    # requires meson instrospect
    # ['common/139 mesonintrospect from scripts', ['python', 'failing']],
    ['common/140 custom target multiple outputs', ['python']],
    ['common/141 special characters', ['python']],
    ['common/142 nested links'],
    ['common/143 list of file sources', ['python']],
    ['common/144 link depends custom target', ['python']],
    ['common/145 recursive linking'],
    ['common/146 library at root'],
    # relies on unstable-simd module
    # ['common/147 simd', ['failing']],
    ['common/148 shared module resolving symbol in executable'],
    ['common/149 dotinclude'],
    ['common/150 reserved targets', ['python']],
    ['common/151 duplicate source names'],
    ['common/152 index customtarget', ['python']],
    ['common/153 wrap file should not failed', ['git_clean']],
    ['common/154 includedir subproj'],
    ['common/155 subproject dir name collision'],
    # relies on config-tool
    # ['common/156 config tool variable', ['failing']],
    ['common/157 custom target subdir depend files', ['python']],
    ['common/158 disabler', ['skip_analyze']],
    ['common/159 array option'],
    ['common/160 custom target template substitution', ['python']],
    ['common/161 not-found dependency'],
    ['common/162 subdir if_found'],
    ['common/163 default options prefix dependent defaults'],
    ['common/164 dependency factory'],
    ['common/165 get project license'],
    ['common/166 yield'],
    ['common/167 subproject nested subproject dirs'],
    ['common/168 preserve gendir', ['python']],
    ['common/169 source in dep', ['python']],
    ['common/170 generator link whole', ['python']],
    ['common/171 initial c_args'],
    ['common/172 identical target name in subproject flat layout'],
    ['common/173 as-needed'],
    ['common/174 ndebug if-release enabled'],
    ['common/175 ndebug if-release disabled'],
    ['common/176 subproject version'],
    ['common/177 subdir_done'],
    ['common/178 bothlibraries', ['python']],
    ['common/179 escape and unicode', ['python']],
    ['common/180 has link arg'],
    ['common/181 same target name flat layout'],
    ['common/182 find override', ['python']],
    ['common/183 partial dependency'],
    # requires dependency 'openmp'
    # ['common/184 openmp', ['failing']],
    ['common/185 same target name'],
    ['common/186 test depends', ['python']],
    ['common/187 args flattening'],
    ['common/188 dict'],
    ['common/189 check header'],
    ['common/190 install_mode'],
    ['common/191 subproject array version'],
    ['common/192 feature option'],
    ['common/193 feature option disabled'],
    ['common/194 static threads'],
    ['common/195 generator in subdir', ['python']],
    ['common/196 subproject with features'],
    ['common/197 function attributes'],
    ['common/198 broken subproject'],
    ['common/199 argument syntax'],
    ['common/200 install name_prefix name_suffix'],
    ['common/201 kwarg entry'],
    ['common/202 custom target build by default', ['python']],
    ['common/203 find_library and headers'],
    ['common/204 line continuation'],
    ['common/205 native file path override'],
    ['common/206 tap tests'],
    ['common/207 warning level 0'],
    ['common/208 link custom', ['python']],
    ['common/209 link custom_i single from multiple', ['python']],
    ['common/210 link custom_i multiple from multiple', ['python']],
    ['common/211 dependency get_variable method'],
    ['common/212 source set configuration_data'],
    ['common/213 source set dictionary'],
    ['common/214 source set custom target', ['python']],
    ['common/215 source set realistic example'],
    ['common/216 custom target input extracted objects', ['python']],
    ['common/217 test priorities', ['python']],
    ['common/218 include_dir dot'],
    ['common/219 include_type dependency'],
    ['common/220 fs module'],
    ['common/221 zlib'],
    # requires machine files
    # ['common/222 native prop', ['failing']],
    ['common/223 persubproject options'],
    ['common/224 arithmetic operators'],
    ['common/225 link language'],
    ['common/226 link depends indexed custom target', ['python']],
    # skip this test for now because it takes too long
    # ['common/227 very long command line', ['python']],
    ['common/228 custom_target source', ['python']],
    ['common/229 disabler array addition'],
    # requires unstable_external_project module
    # ['common/230 external project', ['failing']],
    ['common/231 subdir files'],
    ['common/232 dependency allow_fallback'],
    ['common/233 wrap case'],
    ['common/234 get_file_contents'],
    ['common/235 invalid standard overridden to valid'],
    ['common/236 proper args splitting', [], {'env': {'CFLAGS': '-DFOO -DBAR'}}],
    ['common/237 fstrings'],
    ['common/238 dependency include_type inconsistency'],
    ['common/239 includedir violation'],
    ['common/240 dependency native host == build'],
    ['common/241 set and get variable'],
    ['common/242 custom target feed', ['python']],
    ['common/243 escape++'],
    ['common/244 variable scope'],
    ['common/245 custom target index source', ['python']],
    ['common/246 dependency fallbacks'],
    ['common/247 deprecated option', ['python']],
    ['common/248 install_emptydir'],
    ['common/249 install_symlink'],
    ['common/250 system include dir'],
    ['common/251 add_project_dependencies'],
    ['common/252 install data structured', ['python']],
    ['common/253 subproject dependency variables'],
    ['common/254 long output'],
    # not relevant to muon
    # ['common/255 module warnings'],
    ['common/256 subproject extracted objects'],
    ['common/257 generated header dep'],
    ['common/258 subsubproject inplace'],
    ['common/259 preprocess'],
    ['common/260 declare_dependency objects'],
    # ['common/261 testcase clause'],

    # unit
    ['unit/65 alias target', ['python']],

    # native
    ['native/1 trivial'],
    ['native/2 global arg'],
    ['native/3 pipeline', ['python']],
    ['native/4 tryrun'],
    ['native/5 install script', ['python']],
    ['native/6 add language'],
    ['native/7 selfbuilt custom'],
    ['native/8 external program shebang parsing'],
    ['native/9 override with exe'],

    # nasm
    ['nasm/1 configure file'],
    ['nasm/2 asm language', fail_on_non_x86],
    ['nasm/3 nasm only'],

    # keyval
    ['keyval/1 basic'],
    ['keyval/2 subdir'],
    ['keyval/3 load_config files'],
    ['keyval/4 load_config builddir'],

    # frameworks
    ['frameworks/6 gettext'],
]

if dep_dict['samurai']
    ninja = '@0@ samu'.format(muon.full_path())
else
    ninja = find_program('ninja', 'samu', required: false)
    if ninja.found()
        ninja = ninja.full_path()
    else
        warning('ninja/samu not found, skipping project tests')
        subdir_done()
    endif
endif

test_dir = meson.current_build_dir()

runner = find_program('runner.sh')
is_git_repo = fs.is_dir(meson.global_source_root() / '.git')

foreach t : tests
    # The exit code to use when a test is skipped
    test = t[0]
    extra = t.get(1, [])
    kwargs = t.get(2, {})

    skip_exit_code = 77
    if 'failing' in extra
        skip_exit_code = 1
        kwargs += {'should_fail': true}
    endif

    skip_analyze = 'skip_analyze' in extra ? 1 : 0
    git_clean = (is_git_repo and 'git_clean' in extra) ? 1 : 0

    suites = ['project', test.split('/')[0]]

    if 'python' in extra
        if not python3.found()
            continue
        endif

        suites += 'requires_python'
    endif

    test(
        test,
        runner,
        args: [
            muon,
            ninja,
            meson.current_source_dir() / test,
            test_dir / test,
            skip_exit_code.to_string(),
            skip_analyze.to_string(),
            git_clean.to_string(),
        ],
        suite: suites,
        kwargs: kwargs,
    )
endforeach
