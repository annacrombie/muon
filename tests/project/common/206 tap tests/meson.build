project('test features', 'c')

tester = executable('tester', 'tester.c')
cat = executable('cat', 'cat.c')
test('pass', tester, args: ['ok'], protocol: 'tap')
test('fail', tester, args: ['not ok'], should_fail: true, protocol: 'tap')
test('xfail', tester, args: ['not ok # todo'], protocol: 'tap')
test('xpass', tester, args: ['ok # todo'], should_fail: true, protocol: 'tap')
test('skip', tester, args: ['ok # skip'], protocol: 'tap')
test(
    'partially skipped',
    tester,
    args: ['ok 1\nok 2 # skip'],
    suite: ['verbose'],
    protocol: 'tap',
    verbose: true,
)
test(
    'partially skipped (real-world example)',
    cat,
    args: [files('issue7515.txt')],
    protocol: 'tap',
)
test(
    'skip comment',
    tester,
    args: ['ok # Skipped: with a comment'],
    protocol: 'tap',
)
test(
    'skip failure',
    tester,
    args: ['not ok # skip'],
    should_fail: true,
    protocol: 'tap',
)
test('no tests', tester, args: ['1..0 # skip'], protocol: 'tap')
