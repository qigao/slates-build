# Slate

`slate` resolves shared platform and compiler templates into a Ninja build. The repository includes a multi-module library, an independent SDK consumer, and a CTest fixture covering a mixed C/C++ build.

Start with the [Slate documentation site](https://qigao.github.io/slates-build/).
See the [guide](docs/guide.md) for generation, installation, and test workflows and the
[grammar](docs/grammar.md) for the CLI, presets, and module manifest syntax.

The GitHub Pages workflow builds the site from [`site/`](site/) and renders
the guide, examples, reference, and [editor guide](docs/editors.md) from `docs/`.

Slate is licensed under [Apache-2.0](LICENSE). The CLI archives also include
[third-party notices](THIRD_PARTY_NOTICES.md).

## Bootstrap

Download a CLI archive for your host from [GitHub Releases](https://github.com/qigao/slates-build/releases).
Extract it and put its `bin/` directory on `PATH`. Slate uses Ninja 1.10 or later
and the compiler selected by your project's toolchain.

Use CMake 3.27 or later, Ninja, a C11 compiler, and ABI-matched SDKs. Bootstrap links **SaltsBase 0.1.8**, **SaltsUtils 0.1.6**, and **CMeta 0.1.8**. [Native CI](.github/workflows/ci.yml) downloads and checks the [pinned SDK archives](.github/ci-inputs.json) before building and running CTest on Windows x64, Linux x64 and ARM64, and macOS ARM64.

On Windows with LLVM-MinGW SDKs, set `SALTS_BASE_ROOT`, `SALTS_UTILS_ROOT`,
`CMETA_ROOT`, and `LLVM_MINGW_ROOT` in the repository's `.env`. In VS Code,
run **CMake: configure and build Slate**. Both steps load the bootstrap
environment. Also set `SALTS_BASE_VERSION`, `SALTS_UTILS_VERSION`, and
`CMETA_VERSION` to the exact SDK release numbers, without `v`; see
[`.env.example`](.env.example). Configure runs a fresh Ninja configure in `out/bootstrap`, and
build honors `CMAKE_BUILD_PARALLEL_LEVEL` when supplied. Relative paths resolve
from the repository root, and parent environment values take precedence over
`.env`.

For a manual Windows bootstrap, run these commands in one PowerShell session
from the repository root.

```powershell
$ErrorActionPreference = 'Stop'
$compiler = & ./.vscode/load-env.ps1 -ForConfigure
cmake --fresh -S . -B out/bootstrap -G Ninja "-DCMAKE_C_COMPILER=$compiler" -DCMAKE_BUILD_TYPE=Release
cmake --build out/bootstrap --target slate
ctest --test-dir out/bootstrap --output-on-failure
```

For direct CMake configuration, supply the SDK roots through the parent
environment or set `SaltsBase_ROOT`, `SaltsUtils_ROOT`, and `CMeta_ROOT`
with `-D`. Select a compiler that matches the SDK ABI.
The three SDK version variables must also be present in the environment.

Use a fresh build directory when switching the compiler or installed SDKs.
The Windows bootstrap places the SDK runtime DLLs next to the executable and
includes them in the CMake installation.

## Generate a build

Put Slate v0.1.15 or newer on `PATH`. The project at `apps/` exports the
`stats` library. The independent project at `apps/consumer/`
reads the installed SDK through `libraries`. Set any toolchain variables for
both projects using the process environment or each project's `.env`.
[Sample environment file](apps/.env.example)

The macOS preset uses Apple Clang and `llvm-ar`. Add the LLVM installation's
`bin` directory to `PATH` so Slate can find the response-file-capable archiver.

The `env` list declares which variable names the project reads. Supply a nonempty value for every referenced variable through `.env` or the process environment.

The [Slate Build extension](https://marketplace.visualstudio.com/items?itemName=slates-build.slates-extension)
adds a Slate icon to VS Code's Activity Bar and opens a target tree with Build,
Test, and Run actions. A [VSIX download](https://github.com/qigao/slates-extension/releases/latest)
is also available. Open the repository root in VS Code to browse both sample
projects. The workspace uses the CLI built in `out/bootstrap`.
The [OpenIDEA plugin](https://github.com/qigao/slates-openidea) uses the same
workspace hierarchy and tree icons. Each project keeps its own **Targets**
branch, preset, and command root. [Editor setup and run arguments](docs/editors.md).

```sh
slate install --root apps --target stats --components runtime,dev --prefix dist/stats-sdk
slate run --root apps/consumer --target stats-demo
```

The consumer prints `Mean = 4.0`. [SDK export and consumption walkthrough](docs/examples.md#export-and-consume-an-sdk)

Both projects default to `release`. On Windows, use `--preset release-mingw`
or `--preset release-msvc` on both commands to select the SDK toolchain.
Use `--build-dir <dir>` for a separate output directory. `build` generates
the selected graph and runs Ninja. Use `gen` for a separate Ninja invocation.
`slate gen --root apps --explain stats` prints the producer's effective
settings. `slate graph --root apps --target stats --format dot` shows its
private dependency on `sum`.

Use `slate tool list` to discover Ninja subtools. For an existing
build, `slate tool query --build-dir DIR -- module/stats`
inspects the generated target. See the [subtool guide](docs/guide.md#run-ninja-subtools).

`install` builds and copies the selected components. Generated builds expose
`ninja -C DIR install`, which installs all components to `DIR/stage`.
See the [installation guide](docs/guide.md#install-build-outputs) for headers,
assets, and the installed layout.

After switching to Slate, run `slate gen` with the original preset and build
directory to refresh Ninja's recorded generator path. New SDK exports place
`pkg.json` under `share/slate/<name>/`.

Use `--platform <name>` to select a configured target platform and architecture. The generator defaults to the host OS and architecture. It keeps the target and host toolchains separate for cross builds and preserves the selection when Ninja regenerates.

Toolchain selection verifies the compiler target against the selected OS and
architecture. Clang and LLVM-MinGW use the configured `triple` for both
compilation and linking. Ninja checks the locked tool paths and timestamps
before reusing a build directory.

On Windows, run `slate gen --compiler msvc` and Ninja from a Visual Studio developer shell for the target architecture. `slate` uses the compiler, linker and SDK available in that shell.

The generator renders `build.ninja` from [the bundled Jinja template](share/slates/build.ninja.j2).
Place a same-named file under `build/templates/` to override any bundled
template. Set `templateDir` in a preset to use a different override directory.
Relative paths resolve from the project root. Ninja detects additions, edits,
and removals of active overrides.

A standard `cmake --install` puts the executable in `bin/` and its templates
in `share/slates/`. Slate locates that directory relative to its executable,
so the installation can be copied or archived as one bundle. On Windows,
keep the DLLs installed in `bin/`. On Linux and macOS, keep the bundled `lib/`
directory. The installed templates remain external
files and can be updated in the bundle.

Each compilation uses `compile_msvc.json.j2` or `compile_gnu.json.j2`. The
GNU-style template serves GCC, Clang, and LLVM-MinGW, using the resolved
target triple, architecture, and sysroot as needed. The template returns the
same argument array used for the Ninja response file and
`compile_commands.json`. Its context supplies `policy` (optimization,
diagnostics, runtime, RTTI, exceptions, LTO and sanitizers), `includes`,
`defines`, `flags`, `standard`, `source`, `output`, and `depfile`.

Shared libraries and executables use one of four Jinja linker option
templates in [`share/slates/`](share/slates). The generator selects
`link_msvc.json.j2`, `link_mingw.json.j2`, `link_elf.json.j2`, or
`link_macho.json.j2` by toolchain and target OS. An override with the same
filename can render a JSON array of linker arguments using `output`,
`output_name`, `shared`, `import_lib`, `pdb`, `os`, `machine`,
`triple`, `darwin_arch`, `sysroot`, `lto`, `sanitizers`, and `lib_dirs`.
Use `tojson` for dynamic arguments. The generator appends objects and
libraries in dependency order and quotes the final response-file entries.
Static libraries use `archive_msvc.json.j2` or `archive_gnu.json.j2` for
the archiver options. Those templates receive `output` and `machine`.

A module can name its Ninja rules by action in `module.json`.

```json
{
  "kind": "static",
  "sources": { "files": ["core.c"] },
  "ninjaRules": {
    "compile": "compile_core",
    "archive": "archive_core"
  }
}
```

`ninjaRules` accepts `compile`, `archive`, `link`, and `task` (for
declared `preTasks`). Matching `variants` can override individual names.
A name can be shared when the action and dependency scanning mode match.
The main template emits the rule bindings for each action. Defaults come
from [`share/slates/ninja-rules.json`](share/slates/ninja-rules.json), so modules
can omit the field.

The `artifact-names.json.j2` template defines prefixes and suffixes for
executables, libraries, objects, response files, dependency files, and PDBs.
It receives `os`, `driver`, `arch`, and `config` for each toolchain role.
Project overrides use the same template lookup and regeneration mechanism.

List module directories in `slate.json` under `modules`. Slate loads `module.json` from each directory. Use `moduleFiles` for custom manifest filenames. Each module can list its nested manifests in `include`, with paths relative to that manifest. For example, `"include": ["examples/module.json", "test/module.json"]` loads a module's examples and tests. An executable declares `test` to appear in `slate test`. A shared library can set `outputPrefix` to match its established filename.

Use `--root` to select a `slate.json` project and `libraries` to share library manifests across projects. [Share a library manifest](docs/guide.md#share-a-library-manifest-across-projects)

Use `"modules": ["third_party"]` in `slate.json` to load `third_party/module.json`. Target names are global within that project's graph. Reference a target named `fmt` as `"deps": ["fmt"]`. `slate graph` groups it under the `third_party` folder.

Library modules use `kind` values `static`, `shared`, and `prebuilt`.
The original names ending in `_lib` are accepted as aliases.

A platform's toolchain entry can set `vcpkgRoot` to an installed tree, using a literal path or `${env:NAME}`, alongside its `vcpkgTriplet`. Module paths can use `$vcpkg/include` for that triplet's headers and `$vcpkgConfig/lib` or `$vcpkgConfig/bin` for its libraries and DLLs. For `config: "debug"`, `$vcpkgConfig` resolves to the triplet's `debug` subdirectory.

An optional `when` admits a target if its `os`, `arch`, `driver`, and `config` conditions match. Each field accepts one string or a list of alternatives. The boolean `native` condition checks whether the owning executable can run on the build host. Consumers can declare conditional dependencies in matching `variants`.

Clang and GCC toolchains use the C compiler for Objective-C `.m` sources. `sources.objc` selects individual `.c` files from `sources.files` that need Objective-C compilation. A toolchain can declare `runtimeBins` to stage required DLLs or shared libraries alongside its linked outputs.

Use a top-level `forEach` array to declare a group of targets with the same settings. Each group matches regular files beside the manifest using literal filenames or case-sensitive `*` patterns. `exclude` uses the same syntax. Each matched file becomes a source in its own target, whose name is its filename stem with the optional `namePrefix` prepended. A file matched by more than one pattern in the group creates a single target. Ninja regenerates when files are added or removed.

```json
{
  "forEach": [
    {
      "files": ["regression_*.c", "*_test.c"],
      "exclude": ["math_test.c"],
      "namePrefix": "tinytest_",
      "target": {
        "kind": "executable",
        "private": { "deps": ["saltscore_tinytest"] },
        "test": { "namePrefix": "saltscore.native.", "timeout": 300 }
      }
    }
  ]
}
```

Declare generated sources and headers in `preTasks`. Each task names one `program` (an installed host executable), one `tool` (an executable target name), or one `builtin` (`copy` or `mustache`). Use `args` for the executable's arguments and list every file dependency in `inputs`. A producer's `outputs` must be inside the build directory. Ninja orders tasks through matching input and output paths and tracks the executable as an input. Task paths and arguments support `$root`, `$moduledir`, `$builddir`, `$target`, and `${env:NAME}`. The `copy` builtin takes one input and produces one output.

Add `"test": { "name": "suite.case", "timeout": 30 }` to an executable manifest. Use `name` to set the full test name, or `namePrefix` to prepend a prefix to the target name. With both fields omitted, the test uses the target name. Optional fields include `timeout` in seconds, `workingDirectory` relative to the module, `environment` as an array of `NAME=value` strings, and `labels` as an array of strings.

```sh
slate test --list
slate test --preset debug --filter suite.
```

`slate test` enables test targets, builds the matching targets with Ninja, and runs them directly. Use `--label` to select an exact label. Slate runs the module executable by default. Set `test.program` and `test.args` to use a wrapper. Arguments support task path substitutions, including `$target` for the module artifact.

Native builds include test targets by default. Cross builds can include them with `--tests` or `"tests": true` in a preset. `slate test` runs host-role tests when cross-compiling. Use `testOnly: true` for a test helper target, or `testOnly: false` for a regular tool that also declares a test. Host tool paths can use `${host:rid}` (such as `windows-x64`) and `${host:exe}` (the host executable suffix) in the manifest or an environment default.

## Maintain the documentation website

The [GitHub Pages workflow](.github/workflows/pages.yml) stages `site/`, copies
the contents of `docs/` into that staging directory, and builds the site with
Jekyll. Edit the Markdown source for a documentation page; there is no separate
HTML copy to maintain.

| Website page | Source |
| --- | --- |
| Homepage | [`site/index.html`](site/index.html) |
| Guide | [`docs/guide.md`](docs/guide.md) |
| Examples | [`docs/examples.md`](docs/examples.md) |
| Manifest reference | [`docs/grammar.md`](docs/grammar.md) |
| Editor guide | [`docs/editors.md`](docs/editors.md) |

Shared layouts live in `site/_layouts/`; navigation and page defaults live in
those layouts and `site/_config.yml`. Plugin READMEs are maintained in their own
repositories and are not imported automatically. When adding a documentation
page, set its layout and title in the site configuration and link it from the
relevant guide or navigation.

Pull requests build and verify the generated pages. Updates to `main` deploy
through GitHub Pages after the site build succeeds.
