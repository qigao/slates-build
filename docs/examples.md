# Slate examples

The `apps/` project builds a shared statistics library from two modules.
The independent `apps/consumer/` project runs a program against the installed
SDK. Each project has its own `slate.json` and build directory.

<!-- Preserve the example URL used by earlier editor plugin releases. -->
<a id="make-an-http-request"></a>

## Export and consume an SDK

### Build and run the example

Add the [Slate CLI built from this checkout](guide.md#bootstrap-the-generator)
to the front of `PATH`. You also need Ninja and a native C toolchain. Run both
commands from the repository root with the same toolchain preset.

On Windows with LLVM-MinGW, set `LLVM_MINGW_ROOT` and add
`--preset release-mingw` to both commands. For MSVC, set `VSDEVCMD_BAT` and
add `--preset release-msvc` to both commands. Set the variable in the process
environment or in both `apps/.env` and `apps/consumer/.env`.
[Sample environment file](https://github.com/qigao/slates-build/blob/main/apps/.env.example)

```sh
slate install --root apps --target stats --components runtime,dev --prefix dist/stats-sdk
slate run --root apps/consumer --target stats-demo
```

The program calls `stats_mean()` for the values 2, 4, and 6 and prints
`Mean = 4.0`.

### Follow the producer and consumer graphs

| Project | Targets and dependencies |
| --- | --- |
| [`apps/slate.json`](https://github.com/qigao/slates-build/blob/main/apps/slate.json) | `stats` builds a shared library and privately links the static `sum` module. |
| [`apps/consumer/slate.json`](https://github.com/qigao/slates-build/blob/main/apps/consumer/slate.json) | `stats-demo` depends on `stats`, which the project loads from the installed SDK. |

```sh
slate graph --root apps --target stats --format dot
slate graph --root apps/consumer --target stats-demo --format dot
```

### Define the SDK export

The [`stats` module](https://github.com/qigao/slates-build/blob/main/apps/stats/module.json)
exports version `1.0.0`. Its `install` block copies the public headers and the
companion manifest alongside the generated package metadata.

```json
"install": {
  "exportTarget": true,
  "version": "1.0.0",
  "headers": [{ "from": "include", "to": "include" }],
  "assets": [{ "from": "sdk/module.json", "to": "share/slate/stats" }]
}
```

The install command writes the headers, link library, runtime binary, and
metadata under `apps/dist/stats-sdk/`.

| File in `share/slate/stats/` | Role |
| --- | --- |
| `pkg.json` | Generated metadata with the version, platform, ABI, include directories, link inputs, and runtime files. |
| `module.json` | The producer's [companion manifest](https://github.com/qigao/slates-build/blob/main/apps/stats/sdk/module.json), installed beside `pkg.json`. It declares `stats` as an external target and points `package.file` at `pkg.json`. |

### Use the SDK from another project

The consumer lists the installed companion in `libraries`. This path is
relative to `apps/consumer/slate.json`.

```json
{
  "libraries": ["../dist/stats-sdk/share/slate/stats/module.json"]
}
```

The [`stats-demo` module](https://github.com/qigao/slates-build/blob/main/apps/consumer/stats-demo/module.json)
declares the dependency with `"private": { "deps": ["stats"] }`.
Slate obtains the include and link paths from `pkg.json` and stages the
shared library for `stats-demo`. Other projects can use the same companion
with the path to their own copy of the installed SDK.
[Library manifest rules](guide.md#share-a-library-manifest-across-projects)

### Run from an editor

In VS Code, open the repository root. The workspace points the Slate extension
to the CLI in `out/bootstrap`. On Windows, the **CMake: configure and build Slate**
task builds that CLI. On other platforms, use CMake with `-B out/bootstrap`.
In OpenIDEA, configure the plugin to use the Slate CLI built from this checkout.

After installing the SDK, choose the consumer project in the Slate sidebar.
Select the toolchain preset used to install the SDK, then click **Run** on
`stats-demo`.
