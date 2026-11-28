# Slate guide

`slate` validates project configuration and generates a Ninja build.
Ninja executes compile, archive, link, and task rules. `slate test` builds
and runs tests declared in the project's module manifests.

The CUDA, external-runner, and CMake-export examples cover additions in the
current source tree. If an installed release does not recognize those
options, use a build from source.

## Bootstrap the generator

Install CMake 3.27 or later, Ninja, and the ABI-matched SDKs listed in the
[bootstrap README](https://github.com/qigao/slates-build#bootstrap). On Windows with LLVM-MinGW,
use the VS Code task **CMake: configure and build Slate** or follow the manual
PowerShell commands in the bootstrap README. With another toolchain, supply
the SDK roots and exact `SALTS_BASE_VERSION`, `SALTS_UTILS_VERSION`, and
`CMETA_VERSION` environment values to CMake, then configure with the matching
compiler. The Windows environment loader reads these values from `.env`.

```sh
cmake --build out/bootstrap --target slate
ctest --test-dir out/bootstrap --output-on-failure --no-tests=error
cmake --install out/bootstrap --prefix out/bootstrap-install
```

Put the installed `bin` directory on `PATH` or invoke the executable by full
path. For a portable distribution, keep `bin/` and `share/slates/` under the
same prefix. Slate finds its templates relative to the executable directory.
The VS Code task
**CMake: build Slate** also builds `out/bootstrap`.

The compiler used to bootstrap the generator must match the SDK ABI. The
compiler selected by `slate gen` controls the project being generated.

## Generate and run a build

From the generator repository root, build the `stats` library in `apps/`.
It uses a separate `sum` module internally. Use the toolchain settings for
your platform. [Export and consume the sample SDK](examples.md#export-and-consume-an-sdk)

```sh
slate doctor --root apps --target stats
slate graph --root apps --target stats --format dot
slate build --root apps --preset release --target stats --build-dir out/stats-release
```

`slate run --root apps/consumer --target stats-demo` builds and runs the
downstream executable after the SDK is installed.

`build` generates the selected graph, runs Ninja in the build directory,
and returns Ninja's exit status. `gen` writes the files for a separate
`ninja -C DIR` invocation.

- Use `--explain stats` with `gen` or `build` to see effective include paths,
  definitions, options, and link inputs. It selects that target when the command
  has no explicit target list.
- Select multiple roots with repeated `--target` or with
  `--targets LABEL LABEL`. Generation includes their dependencies.
- Use `ninja -C apps/out/stats-release -t targets all` to list generated targets.
  The alias for `stats` is `module/stats`.
- Omit `--build-dir` to use `out/<preset>/<platform>/<toolchain>`. Each
  invocation that changes the selected roots should use the intended build
  directory explicitly.

## Install build outputs

```sh
slate install --root apps --preset release --target stats --components runtime,dev --prefix dist/stats-sdk
slate gen --root apps --preset release --target stats --build-dir out/stats-release
ninja -C apps/out/stats-release install
```

`install` accepts the same preset, platform, compiler, build directory, and
target selection as `build`. It builds the requested component inputs before
copying files. A relative `--prefix` is resolved from the project root. The
default prefix is `<build-dir>/stage`. Omit `--components` to install all
available components, or give a comma-separated list such as `runtime,dev`.
The generated Ninja `install` target uses all components and the default
prefix. Installed files are kept on a Ninja clean.

| Component | Installed files |
| --- | --- |
| `runtime` | Selected executables, required shared libraries and plugins, declared `runtimeBins`, and assets. |
| `dev` | Static archives, Windows import libraries, explicitly declared headers, and exported SDK metadata. |
| `symbols` | MSVC PDBs, ELF `.debug` files, and Mach-O `.dSYM` bundles for targets with debug information enabled. |

Executables and Windows DLLs go under `bin`, Unix shared libraries and
archives go under `lib`, and plugins and PDBs go under `plugins` and
`symbols` respectively. Runtime installation follows public, private, and
runtime dependencies. Development installation follows public dependencies
and also follows private dependencies of static archives. Host tools and
test-only modules are excluded. ELF extraction uses `llvm-objcopy` or
`objcopy` and Mach-O extraction uses `dsymutil` from `PATH`. Extraction
leaves the runtime binary intact. On ELF and Mach-O targets, use the bundled
linker templates for relative library lookup in the installed layout.

Declare extra files in a module or a matching variant.

```json
"install": {
  "headers": [{"from": "public", "to": "include/mylib"}],
  "assets": [{"from": "shaders/*.spv", "to": "share/mylib/shaders"}]
}
```

`from` can name a file, a directory whose contents are copied recursively,
or a `*` pattern in the final path segment. Patterns must match at least one
file. Generated assets and headers can refer to declared task outputs through
`$builddir`. `to` is a directory relative to the install prefix. Use `.` for
the prefix itself. See the [installation grammar](grammar.md#installation-mappings).

### Export and consume an SDK

Set `"exportTarget": true` in a library's `install` record and give any
exported include directory a header mapping. Set `"version": "1.2.3"`
to publish the version in both metadata formats. Install the development
component and the runtime component if the SDK includes shared libraries.

```json
{
  "kind": "external",
  "package": {
    "provider": "salts",
    "file": "${env:SDK_ROOT}/share/slate/mylib/pkg.json"
  }
}
```

List `SDK_ROOT` in `env` in the downstream `slate.json` and set its absolute
path in `.env` or the process environment. A publisher can also ship a
companion module that references `pkg.json`. Consumers can load the companion
through `libraries` and use the exported target name in `deps`.
[See the sample SDK](examples.md#export-and-consume-an-sdk)

### Export a CMake target

To let CMake applications consume the same installed SDK, add a namespaced
`cmakeTarget` to an exported library. This `install` block belongs to a
target named `mylib`; its public include directory must also be declared in
`public.includeDirs`.

```json
"install": {
  "exportTarget": true,
  "version": "1.2.3",
  "cmakeTarget": "Mylib::Mylib",
  "headers": [{ "from": "include", "to": "include" }],
  "assets": [{ "from": "cmake/MylibConfig.cmake", "to": "lib/cmake/Mylib" }]
}
```

Create `cmake/MylibConfig.cmake` beside the module's sources with this entry
point. Slate generates and installs `mylib-target.cmake` in the same directory.

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/mylib-target.cmake")
```

Install the SDK with `slate install --target mylib --components runtime,dev
--prefix dist/mylib-sdk`. A CMake application can then use the imported target.

```cmake
find_package(Mylib CONFIG REQUIRED)
target_link_libraries(app PRIVATE Mylib::Mylib)
```

Pass the installed SDK prefix through `CMAKE_PREFIX_PATH` when configuring
the application. The generated target uses the same installed include paths,
link inputs, and compile usage as the Slate SDK export. The project supplies
the package entry point; Slate generates the individual target file.
An export's version defaults to the top-level project `version`, or `0.0.0`
when neither version is declared. See the
[installation reference](grammar.md#installation-mappings).

## Run Ninja subtools

Run `slate tool list` to see which subtools your Ninja provides.
Use a build directory already generated by `gen` or `build` for graph and
log operations. Arguments after `--` go directly to the subtool.

```sh
slate tool targets --root apps --build-dir out/stats-release -- all
slate tool query --root apps --build-dir out/stats-release -- module/stats
slate tool graph --root apps --build-dir out/stats-release -- all > graph.dot
dot -Tpng graph.dot -o graph.png
slate tool compdb --root apps --build-dir out/stats-release > ninja-compdb.json
```

The wrapper uses Ninja target names such as `module/stats`.
`slate graph` shows the module dependency graph.
`slate tool graph` shows the generated Ninja graph, including its
file-level edges.

| Subtool | Purpose |
| --- | --- |
| `browse` | Browse the dependency graph through Ninja's local Python helper. |
| `clean` | Remove generated outputs for the requested targets. |
| `commands` | List commands required to rebuild the requested targets. |
| `deps` | Read records from Ninja's dependency log. |
| `graph` | Write a Graphviz DOT graph to stdout. |
| `query` | Show the inputs and outputs of a Ninja path. |
| `targets` | List targets by rule or dependency depth. |
| `compdb` | Write Ninja's compilation database as JSON to stdout. |
| `recompact` | Compact Ninja's internal build and dependency logs. |

Without `--build-dir`, the tool resolves the build directory from the
project's preset and toolchain selection. An explicit build directory lets
you inspect an existing graph directly, including after a compiler has
moved. Relative paths use the explicit or discovered project root and
fall back to the working directory when a project root is absent.

## Configure a project and register a test

The project root has `slate.json`. A manifest at
`modules/arith/module.json` defines `arith`. Named entries under `targets`
use their keys as global names. `slate graph` groups targets into nested
folders based on their manifest directories.

### `slate.json`

One repository can contain several independent `slate.json` projects. Slate
uses the nearest `slate.json` at or above the working directory. From the
repository root, select a project with `--root apps` or `--root apps/consumer`.
Each project resolves its own presets, target names, and build directory. Use
`modules` to build targets together in one project graph.

The VS Code and OpenIDEA plugins arrange these projects by their workspace
directories. For example, `apps/fetch/slate.json` appears under `apps` then
`fetch`. A project directory can contain further projects; its own targets
appear under `Targets`, grouped by their module directories. Each project
keeps its own preset and command root, including when target names overlap.
This hierarchy uses the existing directory layout and needs no additional
manifest fields. CLI commands continue to select one project with `--root`.
[Editor installation, row actions, and run arguments](editors.md).

After renaming an existing project config to `slate.json`, run `slate gen`
with the same selectors and build directory to refresh Ninja's dependencies.

List module directories in `modules`. Slate loads `module.json` from each
directory. Use `moduleFiles` to list manifest paths with custom filenames.
A module manifest can load its own child modules with
`"modules": ["examples", "tests"]`. Use `include` for explicit paths such
as `"include": ["examples/extra-targets.json"]`. Both resolve from the
current manifest's directory. A project with third-party targets can list
`third_party` in `modules`.

Copy the sample [NativeToolchains.json](https://github.com/qigao/slates-build/blob/main/apps/presets/NativeToolchains.json)
into the project's `presets/` directory. These presets give a native debug
build by default.

```json
{
  "include": ["presets/NativeToolchains.json"],
  "modules": ["modules/arith", "apps/check"],
  "defaultPreset": "debug",
  "presets": {
    "debug": { "config": "debug", "cStd": "c17" },
    "release": {
      "inherits": "debug",
      "config": "release",
      "options": { "optimization": "speed" }
    }
  }
}
```

### `modules/arith/module.json`

```json
{
  "kind": "header_only",
  "public": { "includeDirs": ["include"] }
}
```

Put this line in `modules/arith/include/arith.h`.

```c
static inline int twice(int value) { return 2 * value; }
```

### `apps/check/module.json`

```json
{
  "kind": "executable",
  "sources": { "files": ["main.c"] },
  "private": { "deps": ["arith"] },
  "test": { "name": "arith.check", "timeout": 10, "labels": ["smoke"] }
}
```

Use this source in `apps/check/main.c`.

```c
#include "arith.h"

int main(void) { return twice(21) == 42 ? 0 : 1; }
```

### Build and run the test

```sh
slate doctor --target check
slate test --target check --list
slate test --target check --build-dir out/check-debug --label smoke
```

Use `--filter` to select names containing a substring. For a model qualification,
select one registered test and pass its fixture arguments directly, for example
`slate test --target model_qualification -- fixture/model.json report.json`.
Use `--accept-exit 2` only when the qualification contract also accepts exit 2.
External script suites belong to `header_only` targets with `test.tool` or
`test.program`; their native fixtures remain executable or library targets.
Slate streams process
output, prints a result for every matching test, and returns a failure status
if any test fails or the selection is empty. A declared `test.program` runs as
the test command. Add `"$target"` to `test.args` when that command needs the
built executable's path.

### Run an external test suite

Keep a script suite in a `header_only` target with an explicit test runner.
For example, declare Python as a host tool in `slate.json`.

```json
"tools": {
  "python": { "program": "python" }
}
```

In the module manifest, declare the native fixture with its actual kind and
refer to its artifact in the suite's arguments. `verify.py` in this example
receives the fixture executable's path and any forwarded arguments.

```json
{
  "targets": {
    "fixture": {
      "kind": "executable",
      "sources": { "files": ["fixture.c"] }
    },
    "suite": {
      "kind": "header_only",
      "test": {
        "tool": "python",
        "args": ["$moduledir/verify.py", "${artifact:fixture}"],
        "workingDirectory": "$root",
        "environment": ["SUITE_MODE=qualification"],
        "labels": ["qualification"]
      }
    }
  }
}
```

```sh
slate test --target suite --list
slate test --target suite -- fixture/model.json report.json
slate test --target suite --accept-exit 2 -- fixture/model.json report.json
```

`${artifact:fixture}` is resolved before the file exists and records the
dependency, so Ninja builds the fixture before the runner starts.
Here `workingDirectory` makes the forwarded model and report paths relative
to the project root. The default working directory is the build directory.
`test.tool` can also name a host-runnable executable target. Use `test.program`
for a direct executable path; `tool` and `program` are mutually exclusive.

Arguments after `--` append to the declared arguments. Forwarded arguments
and `--accept-exit` require exactly one matching test. Accepting exit 2 is
appropriate only when that suite defines it as a successful result; launch
errors, signals, and timeouts still fail. A test's `environment` overrides
preset `testEnvironment`, which overrides inherited process values.

## Select a platform and compiler

Use `doctor` with the same preset, platform, compiler, and target selection
as `gen`. It reports candidate rejections and where each used environment
value came from. The generator uses the first usable candidate listed in
the platform, subject to the preset's `toolchain` and CLI `--compiler`
filters.

- Use `--platform NAME` for a named platform in the project's configuration.
  Native auto-selection needs a unique OS and architecture match.
- On Windows, set `LLVM_MINGW_ROOT` for the LLVM-MinGW candidate. The
  `debug-msvc` preset uses `vsDevCmd` to read `VSDEVCMD_BAT` from `.env` or the
  process environment. Set it to the installation's `Common7/Tools/VsDevCmd.bat`
  and use `slate doctor --preset debug-msvc` to verify the setup.
  For direct `ninja` invocations, start from the matching Visual Studio
  developer environment.
- In cross builds, set `"toolchainRole": "host"` on executables needed during
  the build. Declare them as a `preTasks[].tool` so Ninja builds the program
  before running it. Use `hostToolchain` to pin a host candidate when needed.
- Native builds include test targets by default. For a cross build,
  `--tests` can include target test binaries. `slate test` runs host-role tests
  in cross builds and all selected tests in native builds.

List allowed variable names in `env`, for example `"env": ["LLVM_MINGW_ROOT"]`.
The process environment overrides the project's `.env`. Every referenced key
needs an explicit, nonempty value. Use `requiredIncludeDirs` for SDK headers
that must be present during configuration and `includeDirs` for paths such
as directories created by build tasks.
Use `${envPath:NAME}` when a field needs the variable as an absolute,
normalized path, such as a path embedded in a C string macro.
See the [grammar](grammar.md#environment-and-paths) for substitutions and path
bases.

### Add a cross target

Add a platform and candidate to your project configuration. This fragment
adds a Windows ARM64 target using an LLVM-MinGW installation that includes
the ARM64 tools. The existing native platform supplies host tools. The
bundled `NativeToolchains.json` already declares `LLVM_MINGW_ROOT`.

```json
{
  "platforms": {
    "windows-arm64": { "os": "windows", "arch": "arm64", "toolchains": [{ "id": "mingw-arm64" }] }
  },
  "toolchains": {
    "mingw-arm64": {
      "driver": "llvm-mingw", "hostOs": "windows", "triple": "aarch64-w64-mingw32",
      "cc": "${env:LLVM_MINGW_ROOT}/bin/aarch64-w64-mingw32-clang.exe",
      "cxx": "${env:LLVM_MINGW_ROOT}/bin/aarch64-w64-mingw32-clang++.exe",
      "ar": "${env:LLVM_MINGW_ROOT}/bin/llvm-ar.exe",
      "ld": "${env:LLVM_MINGW_ROOT}/bin/aarch64-w64-mingw32-clang++.exe"
    }
  }
}
```

Include the fragment from `apps/slate.json` alongside the native platforms.
From a Windows x64 host, generate and build the repository's `stats` target
for ARM64.

```sh
slate doctor --root apps --platform windows-arm64 --target stats
slate build --root apps --platform windows-arm64 --target stats --build-dir out/stats-arm64
```

The resolver validates candidate tools before choosing one. A
`versionContains` constraint filters the compiler banner by substring.
A configured `triple` sets the target for Clang-based drivers. Set
`sysroot` when the target needs a separate SDK tree for headers and
libraries. See [candidate selection](grammar.md#candidate-selection).

## Compile CUDA sources

Use a CLI build with CUDA support and install the CUDA toolkit separately.
Slate requires an explicit `options.cudaCompiler`; it does not choose NVCC
from an unrelated toolchain. On Windows, select the MSVC host toolchain.

This Windows project uses the sample native toolchain and preset fragments.
Declare `CUDA_ROOT` and `VSDEVCMD_BAT` in the project environment, then point
them at the CUDA toolkit and Visual Studio developer script in `.env`.

```json
{
  "include": ["presets/NativeToolchains.json", "presets/DefaultPreset.json"],
  "env": ["CUDA_ROOT", "VSDEVCMD_BAT"],
  "modules": ["math"],
  "defaultPreset": "cuda",
  "presets": {
    "cuda": {
      "inherits": "release",
      "targetPlatform": "windows-x64",
      "toolchain": "msvc-win64",
      "vsDevCmd": "${envPath:VSDEVCMD_BAT}",
      "compilePoolDepth": 2,
      "options": {
        "cudaCompiler": "${envPath:CUDA_ROOT}/bin/nvcc.exe",
        "cudaArchitecture": "75",
        "cudaStd": "c++17"
      }
    }
  }
}
```

Place CUDA sources in `math/` and declare them in `math/module.json`.
Use a compute capability supported by the toolkit and intended GPU.

```json
{
  "name": "cuda_math",
  "kind": "static",
  "sources": { "files": ["a.cu", "b.cu"] },
  "options": { "cudaRdc": true, "cudaDeviceLink": true },
  "private": { "libs": ["${envPath:CUDA_ROOT}/lib/x64/cudart_static.lib"] }
}
```

```sh
slate doctor --preset cuda --target cuda_math
slate build --preset cuda --target cuda_math
```

`cudaRdc` enables relocatable device code; `cudaDeviceLink` requires it and
adds a device-link step before the archive or final link. Omit both for
targets that do not need separate device linking. Use `cudaFlags` for NVCC
arguments and `cudaHostFlags` for its host compiler. On Linux, use the
toolkit's `bin/nvcc`, select a compatible native host compiler, and declare
the appropriate CUDA link libraries for that platform.

`compilePoolDepth` limits concurrent compilation to an integer from 1 to
1024. It is independent of `linkPoolDepth`. `options.cFlags` and
`options.cxxFlags` add flags for their respective host source languages.
The [CUDA integration fixture](https://github.com/qigao/slates-build/blob/main/tests/cuda.cmake)
contains a complete two-source device-link example.

## Share a library manifest across projects

Put reusable library targets in a module manifest. The sample consumer's
`slate.json` loads the installed SDK companion using this entry.

```json
{
  "libraries": ["../dist/stats-sdk/share/slate/stats/module.json"]
}
```

The companion defines `stats` as an external target backed by the installed
`pkg.json`. The application adds `"private": { "deps": ["stats"] }` to its
module manifest. Use `public.deps` if the library is part of the consumer's
public API. [Run the complete example](examples.md#export-and-consume-an-sdk)

`libraries` entries resolve from their declaring preset fragment. A library
manifest can also `include` other module manifests. Paths in each library
manifest resolve from that file's directory. Slate adds the library targets
to the current project and tracks the manifests for Ninja regeneration.

## Use an installed vcpkg package

```json
{
  "kind": "external",
  "package": { "provider": "vcpkg", "name": "fmt", "static": true }
}
```

Set `vcpkgRoot` and `vcpkgTriplet` on the matching platform candidate and
make `pkgconf` available on the build host for .pc imports. Depend on this
module through `public.deps` or `private.deps`. The provider selects the
headers, libraries and runtime files for the active triplet and configuration.
Use `public.libDirs` with `public.libs` when declaring search directories
without a provider. [Package provider grammar](grammar.md#package-providers).

## Declare tasks and customize rules

- Use `forEach` to turn adjacent files matched by literal names or `*`
  patterns into separate modules. For `foo.c`, a group prefix of `"t_"`
  produces `t_foo`. A test prefix of `"unit."` then produces
  test name `unit.t_foo`.
- Give each `preTasks` entry one `tool`, `program`, or `builtin`. Declare inputs
  and outputs so the generator can order producers and consumers. Keep
  outputs under the selected build directory. For generated sources, list
  the same output path in the consuming module's `sources.files`.
- Builtins support `copy` and `mustache`. The latter takes a template and
  optional JSON context. The generator adds both files as dependencies when
  present.
- Use `ninjaRules` for module-specific rule names. Use a matching `variants`
  entry to override a rule name or a compile/link setting by OS or driver.
- Place a same-named template under `build/templates/` in the consuming
  project, or set `templateDir` in its preset. Artifact names come from
  `artifact-names.json.j2`. Compile, link, and archive argument templates
  return JSON arrays. Apply `tojson` to dynamic values.

The generator uses the same resolved artifact paths throughout the graph,
including task arguments and test commands. The Ninja graph tracks active template
overrides and can regenerate when they are added, edited, or removed.

### Record the source revision

Use `${source:revision}` in expanded module fields to record the full Git
HEAD object ID, for example in a compile definition.

```json
"private": {
  "defines": { "BUILD_REVISION": "\"${source:revision}\"" }
}
```

Slate tracks the Git references used to resolve the revision. For a source
archive without Git metadata, set the preset's `sourceRevision` to the full
40- or 64-character hexadecimal object ID supplied by the producer.

### Stage a plugin's runtime DLLs

List `modules/runtime` and `plugins/echo` in `modules`.
Give `modules/runtime` a `shared` manifest, then declare it
through `runtimeDeps` in `plugins/echo/module.json`. On Windows, building
the plugin alias stages the runtime DLL next to the plugin. An executable
that loads the plugin can add `"runtimeDeps": ["echo"]` to its
own manifest.

```json
{
  "kind": "cmeta_plugin",
  "sources": { "files": ["echo.c"] },
  "runtimeDeps": ["runtime"]
}
```

Build the alias `module/echo` from the generated Ninja directory.
See [runtime staging](grammar.md#runtime-staging) for other target platforms.

### Compile a generated source

Put `int generated(void) { return {{value}}; }` in `number.c.mustache`
and `{"value": 42}` in `number.json`, beside `modules/generated/module.json`.
Add `modules/generated` to `modules`.

```json
{
  "kind": "static",
  "sources": { "files": ["$builddir/gen/number.c"] },
  "preTasks": [{
    "name": "render", "builtin": "mustache",
    "template": "number.c.mustache", "data": "number.json",
    "outputs": ["$builddir/gen/number.c"]
  }]
}
```

### Select platform sources and libraries

This `apps/socket/module.json` uses a shared entry point and adds one
platform-specific source and system library. `when` limits admission to
Windows and Linux.

```json
{
  "kind": "executable",
  "when": { "os": ["windows", "linux"] },
  "sources": { "files": ["main.c"] },
  "variants": [
    { "when": { "os": "windows" }, "sources": { "files": ["socket_win.c"] }, "private": { "libs": ["ws2_32"] } },
    { "when": { "os": "linux" }, "sources": { "files": ["socket_posix.c"] }, "private": { "libs": ["pthread"] } }
  ]
}
```

## Diagnose and verify

| Symptom | Check |
| --- | --- |
| No usable toolchain | Run `doctor` with the same filters and inspect the candidate rejections. Check compiler target architecture and tool paths. |
| Toolchain identity changed | Generate into a fresh `--build-dir` using the intended SDK and toolchain. |
| Target or test is missing | Check the target name, `modules`, `moduleFiles`, `libraries`, manifest `include`, `when`, test inclusion, and host/target role. List graph targets with `graph` and runnable tests with `slate test --list`. |
| Generated source is missing | Match the producer's output to the consumer's source or input path. Inspect the graph for visibility failures or cycles. |
| Unexpected compiler arguments | Read `compile_commands.json` for the structured compiler command. Use `ninja -C DIR -t commands TARGET` to inspect the invoked commands. |
| Ninja keeps regenerating | Check which manifests, directories, environment values, or templates are changing. Run Ninja again after a stable build and check for `no work to do`. |
| CMake bootstrap reports an ABI mismatch | Match the CMake compiler to the installed SDKs. On Windows with the LLVM-MinGW SDKs, use Ninja and `LLVM_MINGW_ROOT`. |

In addition to `build.ninja`, generation writes `compile_commands.json`,
`toolchains.json` and `config-env.stamp`. The last
two support locked regeneration. Project changes belong in manifests and
templates. See the [grammar](grammar.md) for accepted configuration and CLI
syntax.
