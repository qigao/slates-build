# Slate examples

The `apps/` project builds a shared statistics library from two modules.
The independent `apps/consumer/` project runs a program against the installed
SDK. Each project has its own `slate.json` and build directory.

<!-- Preserve the example URL used by earlier editor plugin releases. -->
<a id="make-an-http-request"></a>

## Export and consume an SDK

Add the `bin/` directory of [Slate v0.1.15 or later](https://github.com/qigao/slates-build/releases)
to the front of `PATH`. You can also [build the CLI from source](guide.md#bootstrap-the-generator).
You need Ninja and a native C toolchain. Run the commands from the repository
root and use the same toolchain preset for the producer and consumer.

<a id="build-and-run-the-example"></a>

## Configure the producer

The producer's [`apps/slate.json`](https://github.com/qigao/slates-build/blob/main/apps/slate.json)
loads native toolchains and build presets, selects `release` by default, and
lists the `stats` and `sum` module directories.

```json
{
  "include": ["presets/NativeToolchains.json", "presets/DefaultPreset.json"],
  "defaultPreset": "release",
  "modules": ["stats", "sum"]
}
```

The [`stats` module](https://github.com/qigao/slates-build/blob/main/apps/stats/module.json)
declares a shared library and exposes its `include` directory. Its
`private.deps` declares the [`sum` static library](https://github.com/qigao/slates-build/blob/main/apps/sum/module.json)
as an implementation dependency.

On Windows with LLVM-MinGW, set `LLVM_MINGW_ROOT` and add
`--preset release-mingw` to the commands below. For MSVC, set `VSDEVCMD_BAT` and
use `--preset release-msvc`. Set the variable in the process
environment or in both `apps/.env` and `apps/consumer/.env`.
[Sample environment file](https://github.com/qigao/slates-build/blob/main/apps/.env.example)

On macOS, add the LLVM installation's `bin/` directory to `PATH` so Slate can
find `llvm-ar`.

<a id="follow-the-producer-and-consumer-graphs"></a>

## Build the library

Select the producer with `--root apps` and build the `stats` target.
Slate generates the Ninja build, compiles `sum`, and links `stats` against it.

```sh
slate build --root apps --target stats
```

To inspect the dependency graph, run the following command.

```sh
slate graph --root apps --target stats --format dot
```

<a id="define-the-sdk-export"></a>

## Install the SDK

The `stats` module exports version `1.0.0`. Its `install` block tells Slate to
generate package metadata and copy the public headers and companion manifest.

```json
"install": {
  "exportTarget": true,
  "version": "1.0.0",
  "headers": [{ "from": "include", "to": "include" }],
  "assets": [{ "from": "sdk/module.json", "to": "share/slate/stats" }]
}
```

Install both the runtime and development components under `apps/dist/stats-sdk/`.
Slate resolves the relative `--prefix` from the producer project root.

```sh
slate install --root apps --target stats --components runtime,dev --prefix dist/stats-sdk
```

The SDK now contains the headers, link library, runtime binary, and two
metadata files.

| File in `share/slate/stats/` | Role |
| --- | --- |
| `pkg.json` | Generated metadata with the version, platform, ABI, include directories, link inputs, and runtime files. |
| `module.json` | The producer's [companion manifest](https://github.com/qigao/slates-build/blob/main/apps/stats/sdk/module.json), installed beside `pkg.json`. It declares `stats` as an external target and points `package.file` at `pkg.json`. |

<a id="use-the-sdk-from-another-project"></a>

## Consume the SDK

The independent [`apps/consumer/slate.json`](https://github.com/qigao/slates-build/blob/main/apps/consumer/slate.json)
lists the installed companion in `libraries`. The path is relative to the
consumer's project directory.

```json
{
  "libraries": ["../dist/stats-sdk/share/slate/stats/module.json"]
}
```

The [`stats-demo` module](https://github.com/qigao/slates-build/blob/main/apps/consumer/stats-demo/module.json)
declares `"private": { "deps": ["stats"] }`. Slate reads `pkg.json` for the
include and link paths and stages the shared library for the executable.

```sh
slate run --root apps/consumer --target stats-demo
```

The program calls `stats_mean()` with the values 2, 4, and 6 and prints
`Mean = 4.0`. [Library manifest rules](guide.md#share-a-library-manifest-across-projects)

### Run from an editor

In VS Code, open the repository root. The workspace points the Slate extension
to the CLI in `out/bootstrap`. On Windows, the **CMake: configure and build Slate**
task builds that CLI. On other platforms, use CMake with `-B out/bootstrap`.
In OpenIDEA, configure the plugin to use Slate v0.1.15 or later.

After installing the SDK, expand **apps → consumer** in the Slate sidebar.
The producer's targets stay under **apps → Targets**; the consumer has its
own **Targets** branch and preset. Select the toolchain preset used to install
the SDK, then click **Run** on the `stats-demo` target. This program needs no
run arguments. [Workspace hierarchy and editor actions](editors.md).
