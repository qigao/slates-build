# Slate examples

The repository's `hello` program makes a GET request with Chttp. Its
`http.json` manifest also shows how multiple Slate projects can share a library.

## Make an HTTP request

### Set up the SDKs

From the repository checkout, [build the current Slate CLI](guide.md#bootstrap-the-generator)
and put it on `PATH`. The sample uses the `libraries` manifest field. Install
Python 3 to run the local server used below.

Set the paths in `apps/.env` or the process environment. Choose installed SDKs
that match the target operating system, architecture, and compiler ABI. Replace
these example paths with your SDK installation directories.

```dotenv
HTTP_SERVICES_ROOT=/path/to/chttp
SALTS_CNET_ROOT=/path/to/salts-cnet
SALTS_BASE_ROOT=/path/to/salts-base
SALTS_UTILS_ROOT=/path/to/salts-utils
```

On Windows, use `--preset debug-mingw` with `LLVM_MINGW_ROOT` set to your
LLVM-MinGW directory, or `--preset debug-msvc` with `VSDEVCMD_BAT` set to
your Visual Studio developer batch file. Linux and macOS can use the `debug`
preset. [Sample environment file](https://github.com/qigao/slates-build/blob/main/apps/.env.example) ·
[Toolchain setup](guide.md#select-a-platform-and-compiler)

### Start the server and run the client

From the repository root, serve `apps/hello` on loopback in one terminal.

```sh
python -m http.server 8000 --bind 127.0.0.1 --directory apps/hello
```

In another terminal at the repository root, build and run `hello`. Use your
chosen preset if you need to select a specific toolchain.

```sh
slate run --root apps --target hello -- tcp://127.0.0.1:8000 127.0.0.1:8000 /hello.c
```

`--root apps` selects `apps/slate.json`. The three values after `--` go to `hello`.

| Argument | Meaning |
| --- | --- |
| `tcp://127.0.0.1:8000` | Chttp transport URI for the local TCP connection. |
| `127.0.0.1:8000` | HTTP authority sent in the `Host` header. |
| `/hello.c` | Request path served from `apps/hello`. |

The client prints `HTTP 200` followed by the contents of `hello.c`. Request
`/missing` to see an `HTTP 404` response and a failing exit status.

### Run from an editor

Open the repository as a workspace and select the `apps` project in the Slate
sidebar. Keep the local HTTP server running and select the preset that matches
the installed SDKs.

In VS Code with Slate Build 0.1.18 or later, add this to the workspace settings.

```json
{
  "slate.runArguments": {
    "apps/hello": ["tcp://127.0.0.1:8000", "127.0.0.1:8000", "/hello.c"]
  }
}
```

In OpenIDEA with Slate Build 0.1.9 or later, put this in `apps/slate.run.json`.

```json
{
  "hello": ["tcp://127.0.0.1:8000", "127.0.0.1:8000", "/hello.c"]
}
```

Click **Run** on the `hello` target in either editor.

### Follow the dependency graph

| File | Role |
| --- | --- |
| [`apps/slate.json`](https://github.com/qigao/slates-build/blob/main/apps/slate.json) | Lists the `hello` module and loads `libraries/http.json` directly. |
| [`apps/hello/module.json`](https://github.com/qigao/slates-build/blob/main/apps/hello/module.json) | Builds `hello.c` and declares a private dependency on `chttp_client`. |
| [`apps/libraries/http.json`](https://github.com/qigao/slates-build/blob/main/apps/libraries/http.json) | Declares `chttp_client` and its `salts_cnet` dependency. Platform variants select the link libraries and runtime files. |
| [`apps/hello/hello.c`](https://github.com/qigao/slates-build/blob/main/apps/hello/hello.c) | Calls `chttp_get()` and prints the response status and body. |

```sh
slate graph --root apps --target hello --format dot
```

## Share a library between projects

For two independent HTTP clients in one repository, give each its own
`slate.json` and put a copy of the example `http.json` in a shared directory.

```text
apps/fetch/slate.json
apps/health-check/slate.json
libs/http.json
```

Add the same library path to each project manifest alongside its own presets
and modules. Each project also declares the SDK environment variables it uses.
The relative path is resolved from the manifest that declares it.

```json
{
  "libraries": ["../../libs/http.json"],
  "env": ["HTTP_SERVICES_ROOT", "SALTS_CNET_ROOT", "SALTS_BASE_ROOT", "SALTS_UTILS_ROOT"]
}
```

A module can then declare `"private": { "deps": ["chttp_client"] }`. Build
each project from the repository root with its own `--root` selection.

```sh
slate build --root apps/fetch
slate build --root apps/health-check
```

Each invocation resolves the selected project's preset, targets, and build
directory. From inside a project, Slate discovers the nearest `slate.json`.
[Library manifest rules](guide.md#share-a-library-manifest-across-projects)
