---
layout: document
title: Slate grammar
---

# Slate grammar

This reference describes the public CLI and the configuration accepted by the
generator. The [guide](guide.md) shows how to generate, build, and run tests.

The notation uses EBNF for tokens. `record { … }` describes an unordered JSON
object. A field marked `?` may be omitted. `list(T)` is a JSON array and
`map(K, V)` is a JSON object whose keys and values have the given types.
The `+` operator combines the fields of two records.
Both accept empty collections unless constrained below. `String`, `Boolean`,
`Number`, `Value`, and `null` have their JSON meanings. A terminal inside a
record is a JSON string value. Other quoted terminals denote literal tokens.

## CLI

```ebnf
Command     = "slate", (Help | "--version" | Generate | Build | Run | Test | Install | Tool | Doctor | Graph) ;
Help        = "--help" | "help" ;
Generate    = "gen", { Common | Arg("--explain", TargetName) } ;
Build       = "build", { Common | Arg("--explain", TargetName) } ;
Run         = "run", { Common }, [ "--", { ShellArgument } ] ;
Test        = "test", { Common | "--list" | Arg("--filter", String) | Arg("--label", String) } ;
Install     = "install", { Common | Arg("--prefix", Path) | Arg("--components", Components) } ;
Components  = Component, { ",", Component } ;
Component   = "runtime" | "dev" | "symbols" ;
Tool        = "tool", NinjaTool, { ToolSelector }, [ "--", { ShellArgument } ] ;
ToolSelector = Arg("--root", Path) | Arg("--preset", Identifier)
             | Arg("--platform", Identifier) | Arg("--compiler", Driver)
             | Arg("--build-dir", Path) | "--help" ;
Doctor      = "doctor", { Common } ;
Graph       = "graph", { Common | Arg("--format", "dot" | "json") } ;

Common      = Arg("--root", Path)
            | Arg("--preset", Identifier)
            | Arg("--platform", Identifier)
            | Arg("--compiler", Driver)
            | Arg("--build-dir", Path)
            | Arg("--target", TargetName)
            | "--targets", TargetName, { TargetName }
            | "--targets=" TargetName
            | "--tests"
            | "--help" ;

Arg(flag, value) = flag, value | flag "=" value ;
Driver      = "msvc" | "llvm-mingw" | "clang" | "gcc" ;
```

Each space-separated item is a shell argument. Quote paths that contain spaces.
The separated form of `--targets` consumes every argument up to the next
token starting with `--`. Those arguments are interpreted as labels. The
`--targets=LABEL` form consumes one label, including any commas literally.
For scripts, repeated `--target NAME` gives each option a fixed arity.
Repeated target selections are deduplicated. Repeated scalar options use the
last value. `--explain` selects the explained target when the target list is
empty. `graph` emits DOT by default.

Without `--root`, the generator searches upward from the working directory
for `slate.json`. Relative `--root` paths resolve against that working
directory. Ninja's regeneration rules use internal commands and options.

`build` uses the same target selection as `gen` and runs the generated
default Ninja target. It returns Ninja's exit status.

`run` requires exactly one target name. It builds the host-runnable
executable and runs it with the build directory as its working directory.
Arguments after `--` are passed directly to the executable. `run` returns
the build's exit status on build failure and the executable's exit status
otherwise.

`graph --format json` writes the resolved `preset`, `platform`, `compiler`,
`buildDirectory`, graph `inputs`, and `targets`. Each target has `name`,
`folder`, `manifest`, `kind`, `runnable`, `test`, and lists `sources`, `headers`,
`taskInputs`, `taskOutputs`, `publicDeps`, `privateDeps`, `runtimeDeps`. `sources`
contains the resolved absolute source paths for the target. `headers` contains absolute paths from
`install.headers`, headers next to selected sources, headers in the target's
own module-local include directories, and header outputs declared by the
target's `preTasks`. `taskInputs` and `taskOutputs` list the resolved file paths
for tasks owned by the target. A target filter limits the result to the selected
graph.
Without a filter, the JSON includes every admitted target.

`test` enables test targets and applies the same project and target selectors
as `build`. `--filter` matches a case-sensitive substring of the test name.
`--label` matches an exact label. With `--list`, Slate prints the matching
names and skips generation and building. Otherwise, Ninja builds the
matching test targets and their prerequisites, then Slate runs each test.
The command fails if no tests match, if Ninja fails, or if any test fails.

`install` selects the same roots as `build`. Ninja builds the inputs for the
requested components before installation begins. The default prefix is
`<build-dir>/stage`. An explicit relative prefix resolves from the project
root. The default component list is `runtime,dev,symbols`.

`NinjaTool` is the installed Ninja's subtool name. Use
`slate tool --help` for wrapper usage and `slate tool list` for
available names. `ShellArgument` is a complete argument passed through
unchanged after `--`. The command invokes `ninja -t` in the existing build
directory and returns its exit status. An explicit `--build-dir` bypasses
toolchain resolution. Relative build directories use the project root
when available and otherwise the working directory. `list` can run without
generated files.

### Runner commands

These Ninja forms can be used after generating a build.

```ebnf
NinjaBuild  = "ninja", "-C", BuildPath, { NinjaTarget } ;
ListTargets = "ninja", "-C", BuildPath, "-t", "targets", "all" ;
NinjaTarget = "all" | "install" | "module/", TargetName ;
```

`BuildPath` is the directory reported by `gen` or `build`. The default
Ninja target is `all`, which builds the selected roots. The
`install` target uses the default prefix and all components.

## JSONC and names

Project manifests are UTF-8 JSON with optional BOM, `//` and `/* … */`
comments, and a trailing comma after the final member or element. String
literals use JSON escaping. Object keys must be unique and contain no NUL
characters. Declared string fields must also contain no NUL characters.

```ebnf
Identifier  = IdChar, { IdChar } ;
IdChar      = Letter | Digit | "_" | "-" | "." ;
Letter      = "A" … "Z" | "a" … "z" ;
Digit       = "0" … "9" ;
EnvKey      = (Letter | "_"), { Letter | Digit | "_" } ;
TargetName  = Identifier ;
Visibility  = TargetName | "*" ;
Path        = String ;
Number      = JSON number ;
```

Identifiers exclude `.` and `..`. Target names are unique across all
declared module manifests, including targets disabled for the current
configuration. For example, `--target fast` selects a named target `fast`
declared in `apps/check/module.json`. DOT groups it under `apps/check`.

## Preset fragments

```ebnf
PresetFragment = record {
  "$schema"?: String;
  "include"?: list(Path);
  "defaultPreset"?: Identifier;
  "modules"?: list(Path);
  "moduleFiles"?: list(Path);
  "platforms"?: map(Identifier, Platform);
  "toolchains"?: map(Identifier, Toolchain);
  "tools"?: map(Identifier, HostTool);
  "presets"?: map(Identifier, Preset);
  "styles"?: map(Identifier, Style);
  "env"?: list(EnvKey);
};

Platform = record {
  "os": String;
  "arch": String;
  "toolchains": list(Candidate);
};

Candidate = record {
  "id": Identifier;
  "vcpkgTriplet"?: String;
  "vcpkgRoot"?: Path;
};

HostTool = record { "program": Path; "args"?: list(String); };

Toolchain = record {
  "driver": Driver;
  "cc": Path; "cxx": Path; "ar": Path; "ld": Path;
  "launcher"?: Path;
  "hostOs"?: String; "hostArch"?: String;
  "triple"?: String; "sysroot"?: Path;
  "versionContains"?: String;
  "runtimeBins"?: list(Path);
};

Preset = record {
  "inherits"?: Identifier;
  "config"?: Identifier;
  "targetPlatform"?: Identifier;
  "toolchain"?: Identifier; "hostToolchain"?: Identifier;
  "vsDevCmd"?: Value;
  "buildDir"?: Path; "templateDir"?: Path;
  "cStd"?: String; "cxxStd"?: String;
  "linkPoolDepth"?: Number;
  "options"?: Options;
  "defines"?: Defines;
  "styles"?: list(Identifier);
  "flags"?: list(String);
  "packages"?: Packages;
  "tests"?: Boolean;
};

Style = record {
  "extends"?: list(Identifier);
  "options"?: Options;
  "defines"?: Defines;
  "flags"?: list(String);
};

Packages = record { "vcpkg"?: Vcpkg; };
Vcpkg = record { "triplet"?: String; "root"?: Value; "config"?: Value; };
Options = map(String, Value) ;
Defines = map(String, String | null) ;
```

- Preset includes resolve from the including document, stay within the
  repository, and are loaded once. Include cycles, duplicate named
  declarations, and duplicate `defaultPreset`, `modules`, or `moduleFiles`
  fail.
- `env` lists allowed variable names. Declarations from included fragments
  are combined and duplicate names fail.
- `modules` lists directories and loads `module.json` from each one. Use
  `"."` for a manifest at the project root. `moduleFiles` lists explicit
  manifest paths and may be used alongside `modules`. Both lists resolve
  paths from the project root and require existing files inside it. The same
  manifest path is loaded once, including when listed in both fields.
- The selected preset must resolve to a `config` after inheritance. Preset
  inheritance applies base settings first. Styles apply their base styles first.
  The module layers project options, preset styles, module styles, module
  settings, and matching variants in that order.
- A platform lists candidates in preference order. A pinned toolchain must
  occur in that list. Native auto-selection needs a unique OS and architecture
  match. A cross build also needs a native platform for host tools.
- LLVM-MinGW and Clang pass `triple` to the compiler and link driver.
  Executable paths are either qualified paths or program names resolved on
  `PATH`. Use a compiler driver for GNU-style linking and `link.exe` for MSVC.
- `linkPoolDepth` uses an integer from 1 to 1024, with a default of 4.
- The package configuration consumes `packages.vcpkg.triplet` as an ABI
  override. The selected candidate's `vcpkgRoot` supplies the SDK root and
  the effective `Preset.config` selects the configuration-specific tree.
  The parser accepts and ignores `packages.vcpkg.root` and
  `packages.vcpkg.config` for compatibility. Their type is `Value` because
  their values are unused and unchecked.

### Candidate selection

The explicit `--platform` takes precedence over `targetPlatform`. Otherwise
one platform must match the host OS and architecture. That platform also
supplies host tools in a cross build. An explicit native target platform can
serve as both host and target platform.

For the target platform, `toolchain` filters by candidate ID and
`--compiler` filters by driver. Both filters must match when supplied.
`hostToolchain` pins the host candidate when needed.
Candidates are tried in list order. The generator checks host restrictions,
resolves the four required executables and any launcher, and validates a
configured `sysroot` directory. The compiler probe checks the effective
target OS and architecture for GNU-style drivers and the reported target
architecture for MSVC. `versionContains` is a case-sensitive substring on
the first reported compiler version line. This probe runs for public
commands. Ninja's light regeneration check reuses the locked IDs and checks
tool identity.

On Windows, a preset may set `vsDevCmd` to a Visual Studio developer batch
file, for example `"${envPath:VSDEVCMD_BAT}"`. This selects an MSVC candidate
and initializes the environment for the selected target architecture before
tool discovery. An existing matching Visual Studio developer environment can
also supply the compiler and SDK settings.

Set `triple` for Clang or LLVM-MinGW when the selected compiler's default
target differs from the desired one. GCC needs an executable already
configured for the target. Set `sysroot` when the GNU-style compiler needs
target headers and libraries from a separate SDK tree. See the [cross-build
example](guide.md#add-a-cross-target).

## Module manifests

```ebnf
ModuleManifest = (ModuleRecord | record { "$schema"?: String; }) + ManifestFields ;
ManifestFields = Groups + record { "include"?: list(Path); } ;
Groups = record {
  "targets"?: map(Identifier, NamedTarget);
  "forEach"?: list(FileGroup);
};
NamedTarget = (ModuleRecord without the "name" field)
              + record { "targets"?: Value; "forEach"?: Value; } ;

ModuleRecord = record {
  "$schema"?: String;
  "kind": Kind;
  "name"?: Identifier;
  "visibility"?: list(Visibility);
  "toolchainRole"?: "target" | "host";
  "extends"?: list(Identifier);
  "sources"?: Sources;
  "public"?: Usage; "private"?: Usage;
  "options"?: Options;
  "ninjaRules"?: NinjaRules;
  "variants"?: list(Variant);
  "runtimeDeps"?: list(TargetName);
  "runtimeBins"?: list(Path);
  "install"?: InstallMappings;
  "package"?: Package;
  "preTasks"?: list(Task);
  "test"?: Test;
  "testOnly"?: Boolean;
  "when"?: When;
  "outputPrefix"?: String;
};

Kind = "executable" | "static" | "shared" | "cmeta_plugin"
     | "header_only" | "prebuilt" | "external"
     | "static_lib" | "shared_lib" | "prebuilt_lib" ;

Sources = record {
  "files"?: list(Path); "exclude"?: list(Path); "objc"?: list(Path);
};

Usage = record {
  "includeDirs"?: list(Path); "requiredIncludeDirs"?: list(Path);
  "libDirs"?: list(Path); "defines"?: Defines;
  "libs"?: list(String); "flags"?: list(String); "linkFlags"?: list(String);
  "deps"?: list(TargetName);
};

NinjaRules = record {
  "compile"?: Identifier; "archive"?: Identifier;
  "link"?: Identifier; "task"?: Identifier;
};

Variant = record {
  "when": When;
  "sources"?: Sources;
  "public"?: Usage; "private"?: Usage;
  "options"?: Options;
  "ninjaRules"?: NinjaRules;
  "runtimeDeps"?: list(TargetName); "runtimeBins"?: list(Path);
  "install"?: InstallMappings;
  "package"?: Package;
  "preTasks"?: list(Task);
};

When = record {
  "os"?: Choice; "arch"?: Choice; "driver"?: Choice; "config"?: Choice;
  "native"?: Boolean;
};
Choice = String | list(String) ;

FileGroup = record {
  "files": list(FilePattern);
  "exclude"?: list(FilePattern);
  "namePrefix"?: String;
  "target": FileTarget;
};
FileTarget = ModuleRecord without the "name" field ;
FilePattern = String ;
```

`include` is a top-level list of literal module manifest paths resolved
relative to the including manifest. A manifest can consist of `include`
alone. Every file must exist within the repository. Slate loads each
resolved file once and rejects include cycles. Included files keep their
own relative paths and folder in DOT. Ninja tracks every loaded manifest
for regeneration.

The top-level manifest can combine a direct module with `targets` and
`forEach`. The direct module uses its `name` or the manifest directory's
basename by default. Each `targets` entry uses its map key as its name.
The parser accepts and ignores nested `targets` and `forEach` in named
entries. A `forEach.target` declares one target with a derived name.
`files` requires at least one
literal filename or local case-sensitive `*` pattern. Each literal in
`files` must exist. Hidden files and nested paths are outside the match set.

`forEach` removes the filename's final extension and prepends the group's
`namePrefix` if present. For `foo.c` and `namePrefix: "t_"`, the target
name is `t_foo`. Without a prefix, its name is `foo`.

The canonical kinds are `executable`, `static`, `shared`, `cmeta_plugin`,
`header_only`, `prebuilt`, and `external`. The compatibility aliases `static_lib`,
`shared_lib`, and `prebuilt_lib` are equivalent to `static`, `shared`,
and `prebuilt` respectively.

Absent `visibility` or an entry of `*` grants access to all consumers.
An empty list grants access to no other targets. Use target names to grant
access to particular consumers. Direct use of a dependency requires
visibility.
Normal link and runtime dependencies stay within the same toolchain role.

`when` matches every listed field. An array is an OR within that field.
`native` matches any host-role module or a target-role module in a native
build. Variants apply in list order. A source-producing kind needs a nonempty
selected source list. `header_only` and `prebuilt` have no compiled sources.

### Installation mappings

```ebnf
InstallMappings = record {
  "headers"?: list(InstallMapping); "assets"?: list(InstallMapping);
  "exportTarget"?: Boolean; "version"?: Identifier;
};
InstallMapping = record { "from": Path; "to": String; };
```

Mappings from matching variants append to the module's mappings.
`headers` belong to `dev` and `assets` belong to `runtime`.
`from` uses the normal module path expansion. A literal file keeps its
basename. A directory contributes its contents with relative paths
preserved. A case-sensitive `*` in the final path segment selects regular
files in that directory. A missing source or empty pattern match fails
installation. `to` uses slash-separated directories relative to the prefix.
`.` denotes the prefix and `..` is invalid. Conflicting files mapped to
the same destination fail before copying begins.

`exportTarget: true` includes relocatable metadata with the `dev` component.
It installs `share/slate/<name>/pkg.json` and
`lib/pkgconfig/<name>.pc`. The version defaults to `0.0.0`.
Exported include directories and explicit library paths must resolve to
installed files. Static exports include their link dependencies. Install
`runtime,dev` together when consuming a shared library and its runtime files.
Matching variants override `exportTarget` and `version` independently.
`outputPrefix` applies to `shared` and `cmeta_plugin` and may be empty.
Source extensions are `.c`, `.cc`, `.cpp`, `.cxx`, and `.C`. GNU-style
drivers also accept Objective-C `.m` and selected `.c` files listed under
`sources.objc`.

### Compile and link propagation

- Both `public.deps` and `private.deps` contribute the dependencies' public
  include directories, definitions, and compile flags to the current module.
  Only `public.deps` re-export that compile usage transitively. A module's
  own public compile usage is exported with it.
- Link traversal follows both kinds of dependency edges through `static`,
  `header_only`, and `prebuilt`. Thus a static library's private dependencies
  can reach the final link while their headers, macros, and flags remain
  private to the static library's compile.
- At a `shared` dependency the traversal consumes that library's artifact
  or Windows import library and stops following its internal link edges.
  Each visited dependency contributes its own public library directories,
  libraries and link flags. Static and prebuilt dependencies also contribute
  their private library directories, libraries and link flags.
  An executable or `cmeta_plugin` is rejected as a link dependency. Use
  `runtimeDeps` to make a dynamically loaded plugin available.

### Runtime staging

`runtimeDeps` selects modules and feeds runtime staging. It contributes
no compile usage or linker inputs. Executables, shared libraries, and
`cmeta_plugin` aliases collect shared artifacts and `runtimeBins`
transitively through public, private, and runtime dependency edges.
Module `runtimeBins` resolve at the module directory. Toolchain
`runtimeBins` resolve at the project root and are added to executable and
shared targets, including plugins.

On Windows, staging uses the consuming artifact's directory. This places
the DLLs used by a plugin alongside it and makes a plugin staged for an
executable available beside that executable. On other targets,
build-produced artifacts stay in their own directories and external
runtime files are copied to `<buildDir>/lib`. Build `all` or a
`module/…` alias to include the staging edges.

### Prebuilt libraries

Expose headers through `public.includeDirs` and put library files directly
in `public.libs`. For a vcpkg package, use paths such as
`"$vcpkg/include"` and `"$vcpkgConfig/lib/fmt.lib"`. Use driver or
configuration variants for the actual filenames in each installed triplet.
The generator validates a library path and tracks it as a link input.
Bare library names use the toolchain's search paths and are spelled
`name.lib` for MSVC or `-lname` for GNU-style drivers.
GNU-style entries beginning with `-` are passed through.
`libDirs` supplies ordered search directories. The bundled linker templates
emit `-L` or `/LIBPATH:`, and existing matching files become Ninja inputs.
`flags` supplies compiler arguments. `linkFlags` supplies linker arguments.

### Package providers

```ebnf
Package = VcpkgPackage | SaltsPackage ;
VcpkgPackage = record {
  "provider": "vcpkg"; "name": Identifier;
  "pc"?: Identifier; "static"?: Boolean;
};
SaltsPackage = record { "provider": "salts"; "file": Path; };
```

`external` requires a package. `prebuilt` accepts a package or explicit
usage. A matching variant can replace the package record. Module definitions
override provider definitions with the same name, and usage lists combine.

The vcpkg provider uses the selected role's `vcpkgRoot` and `vcpkgTriplet`.
It queries `pkgconf` on the host with the active configuration's
`lib/pkgconfig` directory and the triplet's `share/pkgconfig`. The search
excludes the host pkg-config directories. `pc` overrides the .pc name
when it differs from the vcpkg port, and `static` includes private pkg-config
dependencies. The provider reads `vcpkg/info/*.list` for runtime files and
PDBs and follows same-triplet dependencies recorded in `vcpkg/status`.
Without a corresponding .pc, the default lookup falls back to the port's
listed headers and directly listed libraries. An explicit `pc` requires
that metadata. Changes to installed metadata trigger Ninja regeneration.

The Salts provider loads an installed `pkg.json` and resolves its paths
relative to that file. It checks the target OS, architecture, Windows
compiler ABI, and MSVC debug versus non-debug configuration.

### Merge precedence

| Field | Effective behavior |
| --- | --- |
| Preset scalar fields and `flags` | A derived preset's supplied field replaces its base field. Preset `styles` lists accumulate base first. |
| `options` | Key-wise last value wins across top-level preset language defaults, preset options, preset styles, module styles, module settings, and matching variants. |
| `defines` | Key-wise last value wins. Effective compile order is preset, styles, public usage of direct dependencies, then the module's public and private usage. Direct public dependency usage precedes direct private dependency usage. `null` emits a bare macro such as `-DFOO` or `/DFOO`. |
| `sources.files`, `sources.objc` | Append unique resolved paths from matching variants. The final accumulated `sources.exclude` filters the source list before the file selected by `forEach` is added. |
| Usage `includeDirs`, `libDirs`, `libs`, `flags`, `linkFlags`, `deps` | Variants append. Include and library paths are deduplicated while preserving first occurrence. Flags and dependency lists append in order. |
| Usage `requiredIncludeDirs` | Existing directories checked when the usage is resolved. They append to the include search path. |
| `ninjaRules` | Key-wise merge by action. A later matching variant replaces only the supplied rule names. |

The built-in option keys are listed here. Template overrides still receive
the resolved context for the selected driver.

| Key | Value |
| --- | --- |
| `optimization` | `none`, `size`, `speed` |
| `debugInfo` | `none`, `full`, `minimal` |
| `warnings` | `default`, `none`, `strict` |
| `lto` | `none`, `thin`, `full`. Thin LTO requires `clang` or `llvm-mingw`. |
| `werror`, `rtti`, `exceptions` | Booleans. RTTI and exceptions apply to C++ sources. |
| `runtimeLib` | `dynamic` or `static` for MSVC |
| `sanitizers` | Driver-specific sanitizer string |
| `cStd`, `cxxStd` | Compiler dialect, for example `c11`, `c17` or `c++20`. A style, module or variant may override the preset default. |
| `pic` | Boolean for GNU-style non-Windows compilation. Defaults to true for libraries and false for executables. |
| `symbolVisibility` | `default` or `hidden`. Hidden emits `-fvisibility=hidden` where supported outside Windows. |
| `visibilityInlinesHidden` | Boolean. Applies `-fvisibility-inlines-hidden` to non-Windows C++ sources. |
| `abiCritical` | Boolean. An opted-in link target or static dependency checks applicable ABI options in the same link unit. Shared library boundaries remain independent. |

The ABI check compares `cxxStd`, `rtti` and `exceptions` for C++ pairs,
`sanitizers` for linked targets and `runtimeLib` for MSVC.
The MSVC template uses `/Z7` for `debugInfo: "full"` and `"minimal"`.
The MSVC linker emits `/DEBUG` and an explicit PDB path for a linked target
with debug information enabled.

## Tasks and tests

```ebnf
Task = record {
  "name": Identifier;
  "builtin"?: "copy" | "mustache";
  "tool"?: TargetName;
  "program"?: Path;
  "template"?: Path; "data"?: Path;
  "inputs"?: list(Path); "outputs": list(Path);
  "args"?: list(String);
  "restat"?: Value;
};

Test = record {
  "name"?: String; "namePrefix"?: String;
  "program"?: Path; "args"?: list(String);
  "timeout"?: Number;
  "workingDirectory"?: Path;
  "environment"?: list(String);
  "labels"?: list(String);
};
```

- Each task requires exactly one of `builtin`, `tool`, or `program`. A
  `tool` can name a host-runnable executable target or a preset fragment's
  `tools` entry. A module name takes precedence and must identify a
  host-runnable executable. Slate prepends the
  declared tool's `args` and resolves its `program` relative to the project
  root or through `PATH`. A task's own `program` resolves relative to the
  module or through `PATH`.
- Outputs are nonempty, unique across producers, and strictly inside
  `buildDir`. A builtin declares one output. `copy` takes one input.
  `mustache` requires `template` and accepts optional JSON `data`. The
  default Ninja task rule uses `restat = 1`. The parser accepts and ignores
  per-task `restat`.
- Module task builtins are `copy` and `mustache`. The generator's Jinja
  templates render the Ninja graph, naming policy, and build arguments.
- A test belongs to an executable. Use at most one of `name` and
  `namePrefix`. The test name is the explicit `name`, or
  `test.namePrefix + module name` when the prefix is supplied, or the module
  name by default. Concatenation inserts no separator. For a `forEach`
  match named `t_foo`, a test prefix of `"unit."` produces `unit.t_foo`.
  Runnable names in the selected graph must be unique. Timeout is a positive
  integer number of seconds within the unsigned integer range. `environment`
  overrides inherited variables using `NAME=value` strings. The default
  working directory is the build directory and the default program is the
  module artifact.
- `testOnly` controls test admission. When omitted, an executable with a
  `test` declaration is treated as test-only. `--tests` enables test targets.
  `slate test` runs only host-runnable tests.

## Environment and paths

```ebnf
EnvFile    = { EnvLine, [ CR ], LF }, [ EnvLine ] ;
EnvLine    = HSpace, [ "#", { LineChar } | EnvKey, HSpace, "=", EnvValue ] ;
EnvValue   = HSpace, [ Bare | SQuote, { LineChar }, SQuote
                         | DQuote, { LineChar }, DQuote ], HSpace ;
Bare       = BareInitial, { LineChar } ;
HSpace     = { " " | TAB } ;
SQuote     = Unicode U+0027 ;
DQuote     = Unicode U+0022 ;
BareInitial = LineChar except space, TAB, SQuote, and DQuote ;
LineChar   = any Unicode character except CR, LF, and NUL ;
```

The runtime accepts UTF-8 with optional BOM and either LF or CRLF. Leading
and trailing spaces are trimmed. Wrapping quotes are removed and interior
bytes are literal. Empty values are syntactically valid and fail when used
as required environment values. Assignment lines use `KEY=value`. Only
declared keys contribute values. A duplicate declared key fails.

| Reference | Meaning |
| --- | --- |
| `${env:NAME}` | Explicit string value of a declared key. The process environment takes precedence over the project's `.env`. Missing or empty referenced values fail. |
| `${envPath:NAME}` | Resolve the declared variable as a path from the project root. The result is absolute and uses forward slashes on Windows. |
| `${host:rid}`, `${host:exe}` | Host OS and architecture, and the host executable suffix. Available in expanded manifest fields. |
| `$root`, `$moduledir`, `$builddir`, `$target` | Project root, owning module directory, build directory, and module artifact in expanded module fields. |
| `$vcpkg`, `$vcpkgConfig` | Selected vcpkg triplet directory and its configuration-specific tree. The latter appends `debug` for the debug configuration. |

Environment values are expanded as literal strings. Fields that consume
paths resolve and validate them according to their own rules. Relative
toolchain paths, `buildDir`, and `templateDir` use the project root. Relative
module paths use the module directory. Use absolute environment paths when
sharing a portable setting across modules or passing it to a child process.
The CMake bootstrap reads its SDK path keys separately.

## Template contracts

| Input | Rendered form or role |
| --- | --- |
| `ninja-rules.json` | Default names for `compileGnu`, `compileMsvc`, `archive`, `link`, `task`, `regenerate`, and Ninja's `phony` rule. |
| `artifact-names.json.j2` | JSON object of filename prefixes and suffixes. Context contains `os`, `driver`, `arch`, and `config`. |
| `compile_gnu.json.j2`, `compile_msvc.json.j2` | JSON arrays of compiler arguments. Context includes compile policy, source, output, dependency file, includes, defines, and flags. |
| `link_*.json.j2`, `archive_*.json.j2` | JSON arrays of linker or archiver options. The generator appends the ordered objects and link dependencies where applicable. |
| `build.ninja.j2` | Ninja rules and edges. Context contains `rules`, `edges`, and `link_pool_depth`. |
| `pkg.json.j2`, `pkg.pc.j2` | Relocatable SDK metadata installed by the development component. |

### Exact context keys

| Templates | Context keys and types |
| --- | --- |
| `artifact-names.json.j2` | Strings `os`, `driver`, `arch`, `config`. |
| `compile_gnu.json.j2`, `compile_msvc.json.j2` | Strings `standard`, `triple`, `darwin_arch`, `sysroot`, `source`, `output`, `depfile`. Booleans `cxx`, `objc`, `pic`. Lists of strings `includes`, `defines`, `flags`. Record `policy` below. |
| `link_*.json.j2`, `archive_*.json.j2` | Strings `os`, `machine`, `triple`, `darwin_arch`, `sysroot`, `output`, `output_name`, `import_lib`, `pdb`, `lto`, `sanitizers`. Boolean `shared`. List of strings `lib_dirs`. |
| `build.ninja.j2` | Integer `link_pool_depth`. List `rules` with string fields `name`, `kind`. List `edges` with string fields `rule`, `outputs`, `implicit_outputs`, `inputs`, `implicit_inputs`, `order_only`, `run`, `response`, `argv`, `dep`. |
| `pkg.json.j2`, `pkg.pc.j2` | Strings `name`, `version`, `os`, `arch`, `abi`, `config`. The `usage` and `runtime` strings contain JSON. The `cflags` and `libs` strings contain escaped pkg-config fragments. |

Compile `policy` has string fields `optimization`, `debug_info`, `warnings`,
`runtime_lib`, `rtti`, `exceptions`, `lto`, `sanitizers`, and
`symbol_visibility`, plus booleans `debug_crt`, `werror`, and
`visibility_inlines_hidden`. RTTI and exception toggles use `enabled` or
`disabled` when supplied for C++. A definition entry is `NAME` or
`NAME=value`, ready for the driver's macro flag prefix.

Use truthiness checks for optional string fields such as `triple`, `depfile`,
`pdb`, and `import_lib`. Link `machine` is the configured architecture,
and `output_name` is the output's basename. Rule `kind` uses a key from
`ninja-rules.json`, while `name` is the resolved Ninja rule name. Ninja edge
fields arrive escaped for Ninja. Use `tojson` on dynamic strings in argument-array templates.

The filenames and their bundled contents are in
[`share/slates/`](https://github.com/qigao/slates-build/tree/main/share/slates). Add a same-named project override
to change the applicable template. The artifact-name template returns these
string fields.

```ebnf
ArtifactNames = record {
  "executableSuffix": String;
  "staticPrefix": String; "staticSuffix": String;
  "sharedPrefix": String; "sharedSuffix": String;
  "importPrefix": String; "importSuffix": String;
  "objectSuffix": String; "responseSuffix": String;
  "depfileSuffix": String; "pdbSuffix": String;
  "symbolsSuffix"?: String;
};
```

The [end-to-end fixture](https://github.com/qigao/slates-build/blob/main/tests/native.cmake)
exercises manifest parsing, generation, Ninja execution, regeneration, and `slate test`.
