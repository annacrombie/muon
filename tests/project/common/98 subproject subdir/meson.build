project('proj', 'c', default_options: ['default_library=shared'])
subproject('sub')
libSub = dependency('sub', fallback: ['sub', 'libSub'])

exe = executable('prog', 'prog.c', dependencies: libSub)
test('subproject subdir', exe)

# Verify the subproject has placed dependency override.
dependency('sub-1.0')

# Verify we can now take 'sub' dependency without fallback, but only version 1.0.
dependency('sub')
d = dependency('sub', version: '>=2.0', required: false)
assert(not d.found(), 'version should not match')

# Verify that not-found does not get cached, we can still fallback afterward.
dependency('sub2', required: false)
d = dependency('sub2', fallback: ['sub', 'libSub'])
assert(d.found(), 'Should fallback even if a previous call returned not-found')

# Verify we can get a fallback dependency without specifying the variable name,
# because the subproject overridden 'sub-novar'.
dependency('sub-novar', fallback: 'sub_novar')

# Verify a subproject can force a dependency to be not-found
d = dependency('sub-notfound', fallback: 'sub_novar', required: false)
assert(not d.found(), 'Dependency should be not-found')

# Verify that implicit fallback works because subprojects/sub_implicit directory exists
d = dependency('sub_implicit', default_options: 'opt=overridden')
assert(d.found(), 'Should implicitly fallback')

# Verify that implicit fallback works because sub_implicit.wrap has
# `dependency_names=sub_implicit_provide1` and the subproject overrides sub_implicit_provide1.
d = dependency('sub_implicit_provide1')
assert(d.found(), 'Should implicitly fallback')

# Verify that implicit fallback works because sub_implicit.wrap has
# `sub_implicit_provide2=sub_implicit_provide2_dep` and does not override
# sub_implicit_provide2.
d = dependency('sub_implicit_provide2')
assert(d.found(), 'Should implicitly fallback')

# sub_implicit.wrap provides glib-2.0 and we already configured that subproject,
# so we must not return the system dependency here. Using glib-2.0 here because
# some CI runners have it installed.
d = dependency('glib-2.0', required: false)
assert(d.found())
assert(d.type_name() == 'internal')

# sub_implicit.wrap provides gobject-2.0 and we already configured that subproject,
# so we must not return the system dependency here. But since the subproject did
# not override that dependency and its not required, not-found should be returned.
# Using gobject-2.0 here because some CI runners have it installed.
d = dependency('gobject-2.0', required: false)
assert(not d.found())

# different than meson: subprojects do not promote
# # Verify that implicit fallback works because subprojects/sub_implicit/subprojects/subsub
# # directory exists.
# d = dependency('subsub')
# assert(d.found(), 'Should be able to fallback to sub-subproject')

# # Verify that implicit fallback works because
# # subprojects/sub_implicit/subprojects/subsub/subprojects/subsubsub.wrap
# # file exists.
# d = dependency('subsubsub')
# assert(d.found(), 'Should be able to fallback to sub-sub-subproject')

# Verify that `static: true` implies 'default_library=static'.
d = dependency('sub_static', static: true)
assert(d.found())
# Verify that when not specifying static kwarg we can still get fallback dep.
d = dependency('sub_static')
assert(d.found())
# But when asking for shared library explicitly, it is not found.
d = dependency('sub_static', static: false, required: false)
assert(not d.found())
# The subproject also overrides sub_static2 with `static: true`
d = dependency('sub_static2')
assert(d.found())
d = dependency('sub_static2', static: true)
assert(d.found())
d = dependency('sub_static2', static: false, required: false)
assert(not d.found())
# sub_static3 is overridden twice with `static: true` and `static: false`
d = dependency('sub_static3')
assert(d.found())
assert(d.get_variable('static') == 'true')
d = dependency('sub_static3', static: true)
assert(d.found())
assert(d.get_variable('static') == 'true')
d = dependency('sub_static3', static: false)
assert(d.found())
assert(d.get_variable('static') == 'false')
