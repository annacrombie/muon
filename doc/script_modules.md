<!--
SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
SPDX-License-Identifier: GPL-3.0-only
-->

# Script Modules

With muon, you can write modules in an extended form of the meson dsl.  This is
for a number of reasons:

- Many meson modules are effectively reusable chunks of meson code (calls to
  dependency, custom\_target, etc.), so it is not necessary to access things
  that a native module has access to like the interpreter state.
- Lowering the bar for module contributions (this is important because they
  often involve specific tools I have no experience with/interest in).
- Potential to some day share module code with other meson implementations.

That last point is a bit pie in the sky, but at least this makes it technically
possible from muon's point of view and proves that it has value.

## Writing a new script module

All script modules live at `src/script/modules/<module_name>.meson`.  After
adding a new module, you must modify `src/script/meson.build` to register it.
muon will embed the module source text as a char[] in the built executable, and
when the module is import()ed that source will be interpreted.

## Module structure

A script module consists of functions that perform the various module
operations, with a return statement at the bottom of the file that exports those
functions.

```
func func1()
endfunc

func func2()
endfunc

return {
    'func1': func1,
    'func2': func2,
}
```

The final `return` at module scope is what "exports" the functions to module
consumers.  The returned object must be of type `dict[func]`.

## Functions

Function definition is started with the `func` keyword followed by an
identifier, and a list of arguments enclosed in parenthesis.  If the function
returns a value it must specify the type of that value by adding `-> <type>`
after the argument list.

Positional and keyword arguments are supported in the argument list.  Positional
arguments must come first.  Keyword arguments are specified by adding a colon
(:) to the end of the argument followed by an optional default value.
All arguments must also specify their type by following the argument name with a
type specifier.

Examples:

```
func a() -> int
    return 1
endfunc

func b(arg1 int) -> int
    return arg1
endfunc

func b(arg1 int, kw str:, kw_with_default str: 'default')
    message(arg1)
    message(kw)
    message(kw_with_default)
endfunc
```

## Scope

Scope generally works like other interpreted languages.

Modules have their own scope and cannot access any external variables (save
builtins such as meson, build\_machine, etc.).

Functions also get their own scope, but can see variables that have been
declared in parent scopes prior to the function definition.  Function
definitions also capture their scope.

Example:

```
func a() -> func
    i = 0
    func b() -> int
        i += 1
        return i
    endfunc

    return b
endfunc

counter = a()
counter() #=> 1
counter() #=> 2
new_counter = a()
new_counter() #=> 1
new_counter() #=> 2
counter() #=> 3
```

## Type specifiers

Type specifiers take the form of the ones found in the meson docs.  The
following types are allowed:

- `void`
- `compiler`
- `dep`
- `meson`
- `str`
- `int`
- `list`
- `dict`
- `bool`
- `file`
- `build_tgt`
- `subproject`
- `build_machine`
- `feature`
- `external_program`
- `python_installation`
- `runresult`
- `cfg_data`
- `custom_tgt`
- `test`
- `module`
- `install_tgt`
- `env`
- `inc`
- `option`
- `disabler`
- `generator`
- `generated_list`
- `alias_tgt`
- `both_libs`
- `typeinfo`
- `func`
- `source_set`
- `source_configuration`

In addition, a value that can take one of many types can be specified by writing
multiple types separated with a `|`.

```
int|str

dict|list|str
```

There are a few preset combinations of types:

- `any` - all of the above types except `void`
- `exe` - `str|file|external_program|python_installation|build_tgt|custom_tgt`

`dict` and `list` should also be followed by a sub-type enclosed in square
brackets.

```
dict[str]

dict[str|int]
```

Finally, you can wrap the entire type in `glob[<type>]` or `listify[<type>]` to
get the special argument handling detailed in doc/contributing.md (search for
`TYPE_TAG_{GLOB,LISTIFY}`).

## Additional built-in functions

Various additional builtin functions are avaliable:

- `serial_load(path str) -> any` - load a serialized object from a file
- `serial_dump(path str, obj any)` - serialize and save an object to a file
- `is_void(arg any) -> bool` - check if arg is the value void.  This can be used
  to check if a kwarg with no default value has been set.
- `typeof(arg any) -> str` - returns the type of an object.
- `list.delete(index int)` - delete the element from list at the specefied index
- `dict.delete(key str)` - delete key from dict
- `meson.argv0() -> str` - gets the command used to invoke muon (useful for
  creating targets which invoke scripts).

The `fs` module also has additonal functions:

- `copy(src str|file, dest str)` - copy a file from src to dest
- `write(dest str|file, data str)` - write `data` to `dest`
- `cwd() -> str` - returns muon's current working directory
- `mkdir(path str)` - make the directory at `path`
- `rmdir(path str, recursive bool)` - delete the directory at `path`. `recursive`
  defaults to false. If set to true the contents of `path` will be recursively
  deleted before `path` itself.
- `is_basename(path str) -> bool` - true if str contains no path separators
- `is_subpath(base str, sub str) -> bool` - true if `sub` is a subpath of `base`
- `add_suffix(path str, suff str) -> str` - add the suffix `suff` to `path`
- `make_absolute(path str) -> str` - prepend cwd to `path` if path is relative
- `relative_to(base str, sub str) -> str` - return path `sub` relative to `base`
- `without_ext(path str) -> str` - return path with extension removed
  to it, otherwise return path
- `executable(path str) -> str` - if path has no path separators, prepend './'


## Semantic differences between regular and extended Meson

- In regular meson functions which create targets, the `output` parameter is not
  allowed to contain file separators. This has the effect that targets must only
  produce outputs which live in the current build directory. In extended meson
  file separators are permitted in these cases and so extended meson functions
  can create targets whose outputs may exist in other build directories.