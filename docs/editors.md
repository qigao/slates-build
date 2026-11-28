# Slate editor guide

The VS Code and OpenIDEA plugins discover `slate.json` projects in a workspace,
show their resolved targets, and invoke the Slate CLI for generation, building,
testing, and running. Install Slate, Ninja, and the compiler used by your
project before opening the Slate sidebar.

## Install and select the CLI

In **VS Code**, install [Slate Build from the Marketplace](https://marketplace.visualstudio.com/items?itemName=slates-build.slates-extension),
or install a [VSIX](https://github.com/qigao/slates-extension/releases/latest)
from disk. Open a trusted workspace and choose **Slate Targets** in the
Activity Bar. Set `slate.executable` in workspace settings when `slate` is
not on the workspace host's `PATH`.

In **OpenIDEA**, install **Slate Build** under **Settings → Plugins → Available**,
or install an [`.oiplugin` package](https://github.com/qigao/slates-openidea/releases/latest)
from disk. Open **Slate** in the left sidebar. Choose the executable in the
plugin's permission settings when OpenIDEA asks to approve `process:slate`.

Both plugins read the graph from `slate graph --format json`. Check
`slate --version` in the same environment as the editor if a graph cannot
load. Packages published before workspace nesting was added can show a flat
project list. To use the newest tree, follow the source-build instructions for
[VS Code](https://github.com/qigao/slates-extension#build-a-vsix-from-this-repository)
or [OpenIDEA](https://github.com/qigao/slates-openidea#develop).

## Browse the workspace hierarchy

Projects follow their directories. For example, a workspace with its own
`slate.json`, `apps/fetch/slate.json`, and `apps/health-check/slate.json`
has this tree. The `hello` target is declared in
`apps/fetch/src/module.json`.

```text
workspace
├── Targets
└── apps
    ├── fetch
    │   └── Targets
    │       └── src
    │           └── hello
    │               └── main.c
    └── health-check
```

A directory with `slate.json` is a project node and can contain further
projects. Its own targets stay under **Targets**. A directory without
`slate.json` groups projects and has no build action. The workspace root
does not need a manifest. In VS Code, multiple workspace folders keep their
own project branches.

Within **Targets**, module folders come from the CLI graph's `folder` field.
Targets loaded through `libraries` appear in each project that uses them;
manifests outside the project root are grouped under `external`.
This is a directory hierarchy. Dependencies remain build prerequisites and
can be inspected with `slate graph`.

Each project keeps its own preset, command root, target names, and build
directory. Two projects can both have a target named `hello`. Building one
project acts on that project's graph. Use `modules` and `libraries` when
targets must participate in the same graph. No additional workspace manifest
or grouping fields are required.

## Choose a preset and use row actions

Project rows show the resolved preset, platform, and compiler. The two
plugins use the same Slate sidebar mark and Codicons for tree actions.
OpenIDEA's tree follows the host's colors and fonts, and reveals row actions
on hover, selection, or keyboard focus.

| Row | Action | Icon |
| --- | --- | --- |
| Project | Generate `build.ninja` and `compile_commands.json` without compiling. | Gear |
| Project | Select the preset used for subsequent actions. | List selection |
| Target | Build the target and its prerequisites. | Tools |
| Runnable executable | Build and run the executable. | Play |
| Declared test, when available | Build and run the test. | Beaker |
| Source, header, or manifest | Open the file. | File |

Libraries use a package icon, executables use a file-code icon, and tests
use a beaker. **Run** is offered for executables runnable on the host.
The selected preset and platform determine which targets and tests are
admitted.

In VS Code, **Select Preset** opens a picker containing presets from
`slate.json` and its includes. **Use Configured Preset** restores the
`slate.preset` setting or project default. In OpenIDEA, the preset button
opens the project's **Presets** branch; choose **Project default** to clear
the override. Both plugins retain selections per project.

The CLI reads the selected project's `.env`. Use an MSVC preset with
`vsDevCmd`, or start the editor in a matching developer environment.
[Select a platform and compiler](guide.md#select-a-platform-and-compiler).

## Pass arguments to an executable

In VS Code, `slate.runArguments` maps target names to argument arrays.
A workspace-relative project prefix overrides the unscoped target entry.
For `apps/fetch/slate.json`, this selects the second argument array when
running `my_tool` from that project.

```json
{
  "slate.runArguments": {
    "my_tool": ["--input", "default.txt"],
    "apps/fetch/my_tool": ["--input", "file with spaces.txt"]
  }
}
```

In OpenIDEA, place `slate.run.json` beside the selected project's
`slate.json`. Its keys are target names within that project.

```json
{
  "my_tool": ["--input", "file with spaces.txt"]
}
```

Each array entry is one process argument. The plugins append these arguments
after `slate run --target NAME --`. To try the repository's shipped example,
[install the sample SDK](examples.md#install-the-sdk), expand **apps → consumer**,
and run `stats-demo` with the matching toolchain preset. That program needs
no run arguments.

## Read output and refresh the tree

VS Code streams build and test output to **Output → Slate Build**, executable
output to **Output → Slate Run**, and compiler locations to **Problems**.
Use **Slate: Show Output** for graph-loading diagnostics.

OpenIDEA uses the **Run** panel when a launch configuration is available and
a named terminal otherwise. Its toolbar can also act on the current
project or target selection.

Use **Refresh** after external manifest changes or project additions.
VS Code also refreshes projects when a tracked graph input is saved.
OpenIDEA loads a project's graph on expansion and refreshes expanded
projects. If a target is missing, compare the editor's root and preset
with `slate graph --root PATH --preset NAME --format json --tests`.
