# Development guide

How to build Cycles Render, run it, test it, change it and release it. For how the parts fit together read [architecture.md](architecture.md) first.

## Contents

- [What you need](#what-you-need)
- [Build](#build)
- [Run the build in Cinema 4D](#run-the-build-in-cinema-4d)
- [Tests](#tests)
- [Development settings](#development-settings)
- [Adding a feature](#adding-a-feature)
- [Generated files](#generated-files)
- [Code conventions](#code-conventions)
- [Releasing](#releasing)
- [Porting to macOS](#porting-to-macos)
- [Pitfalls](#pitfalls)

## What you need

| | Version the project is developed with | Notes |
|---|---|---|
| Windows | 11 | The only platform so far |
| Cinema 4D | 2027.0 | With `c4dpy.exe` and `Commandline.exe`, which the tests run in |
| Cinema 4D C++ SDK | 2027.0 | `sdk.zip` in the Cinema 4D installation folder, or from developers.maxon.net. Unpack it anywhere; it is never modified |
| Blender | 5.2.3 LTS | The official installer's default location is found automatically |
| Visual Studio | 2026 with MSVC toolset v143 and Windows SDK 10.0.26100.0 | Visual Studio 2022 works with other parameters, see below |
| CMake | 3.30 or newer | |
| Python | 3.10 or newer on `PATH` | For the generators in `tools/` and the bridge tests. Blender brings its own Python for the bridge |

The project keeps its lookup material in a folder `references/` next to the sources, which is not part of the repository: the unpacked C++ SDK (`references/Cinema_4D_CPP_SDK_2027_0_0`, where the build script looks by default), the Python SDK, the Blender source of the supported version and its manual. Having the Blender source at hand is worth it: several facts in [reference/blender.md](reference/blender.md) were read there.

## Build

From the repository root, in PowerShell:

```powershell
.\tools\build_plugin.ps1                     # Release; the SDK is looked for in references\
.\tools\build_plugin.ps1 -Sdk D:\sdk\c4d2027 # or name it (or set the environment variable C4D_SDK)
.\tools\build_plugin.ps1 -Config Debug
.\tools\build_plugin.ps1 -Reconfigure        # after changing CMake settings, the SDK or the toolset
```

The result is `build\plugin\bin\Release\plugins\cyclesrender\cyclesrender.xdl64`, with the module's `res` folder next to it. The first build also builds the SDK's frameworks and takes a few minutes; later builds take seconds.

- With Visual Studio 2022: `-Generator "Visual Studio 17 2022" -Architecture x64`.
- The build must finish **without warnings**: the module is compiled with the SDK's style check at level 3, which turns style faults into errors.
- The build fails while a Cinema 4D that has loaded the plugin is open, because the binary is locked. Close it first.
- After adding or removing source files the script builds twice by itself: the first pass only regenerates the project.

The bridge is plain Python and needs no build.

## Run the build in Cinema 4D

Nothing has to be installed. Start Cinema 4D with the built plugin and with the bridge from the repository:

```powershell
& "C:\Program Files\Maxon Cinema 4D 2027\Cinema 4D.exe" `
    "g_additionalModulePath=<repo>\build\plugin\bin\Release\plugins" `
    "g_cyclesRenderBridge=<repo>\bridge\c4d_bridge\main.py"
```

Cinema 4D's console (*Extensions > Console*) shows `Cycles Render <version>: module loaded` and everything the exporter could not translate. Without `g_cyclesRenderBridge` the plugin uses the bridge installed in Blender, as it does for a user.

## Tests

The test suite lives in `tests/` and is **kept locally: it is not part of the repository** (nor are the stored renders and test scenes it uses). If you have it:

```powershell
.\tools\run_tests.ps1 -Changed             # the tests for what differs from the last commit
.\tools\run_tests.ps1 -Changed -DryRun     # only print what it would run, and why
.\tools\run_tests.ps1                      # the whole baseline: build, 12 bridge tests, 45 Cinema 4D tests; about 18 minutes
.\tools\run_tests.ps1 -SkipBuild -Only lights,materials
```

**When to run which.** For every piece of work: the tests for what changed (`-Changed`). The whole baseline: once per sprint (a group of items the maintainer calls done), before a commit the maintainer asks for, and before a release, on every supported Blender and Cinema 4D version once there is more than one. A typical selected run takes one and a half to four minutes (measured: a bridge file 97 s, an exporter file 230 s with the build, a file the map does not know, which runs the core set, 203 s); the baseline about 14 minutes (820 s; with one start of Cinema 4D per test it was 940 s), and about two minutes more since the camera tests of 0.2.0 (estimated from their own times: `cameras` 91 s instead of 30 s, `camera_tag` 40 s, `test_cameras` about 20 s; and the tests of the 0.2.0 features measured in a selected run: `output` 21 s, `image_nodes` 10 s, `ipr_add` under 1 s, `test_output` 3 s; of F10 to F14: `cycles_lights` 61 s, `redshift_nodes` 44 s, `devices` 23 s, `test_lights` 3 s, `test_devices` 4 s; of F15 and F16: `ipr_reuse` 35 s, `ipr_follow_window` 37 s with its start; a `-Changed` run of 50 tests, close to the baseline, took 1002 s; the whole baseline was not run).

**How `-Changed` chooses.** It takes the files that differ from the last commit (staged or not, and new files git does not ignore) and, because `tests/` is not in git, the files in `tests/` written since the last commit was made. A map in `tools/run_tests.ps1` (the field `Covers` of each test, next to the test list) says which tests check which paths. A few files are everybody's (the scene walk and writer, the session layer, the videopost, the bridge's server and protocol, the runner itself): a change in one of them runs a named core set of twelve tests, and the script says next to the list why that set is enough. Every run of `-Changed` also includes an always-run set (`test_protocol`, `session_selftest`, `coords`: about a minute). A file the map does not know runs the core set and is named as unmapped; a change of documents only runs nothing. The script prints which tests it chose for which file. A selected run can miss a fault in code the map does not connect to the change: that is what the sprint's full run is for, and when it finds such a fault, the connection goes into the map in the same piece of work.

**Several tests in one start of Cinema 4D.** Tests that need the same start settings (the `g_cyclesRender...` settings of [Development settings](#development-settings), which Cinema 4D reads once at start) share one start of `c4dpy` and run one after another in a fixed order that the script prints. Each starts from a known state (`tests/c4d/lib/crrun.py`: no IPR, one new empty document, the capture file emptied), has its own result and time, and must pass alone as well as in its group: a test reads counters as differences and the log through `crtest.log_lines()`. A test that needs the end of the session for its verdict comes last in its start. When `c4dpy` ends before every test of a start has reported, the test that was running fails and the ones not yet run are run again, each alone. `-Alone` runs every test in a start of its own, as before.

It prints one line per test and "N of N passed", and fails when a Blender started during the run is still running at the end. Two tests open a Blender window for a few seconds, and the last three open the regular Cinema 4D for half a minute to a minute each, each with a preferences folder of its own, made fresh in `%TEMP%\cyclesrender_c4d_tests`. `test_viewport` and `render_cancel` depend on timing and can fail under load; run a failed test alone (`-Only <name>`) before looking for a cause. If you do not have the tests, check a change by hand as described under [Adding a feature](#adding-a-feature) and say so in the pull request.

What the suite consists of, for whoever writes or restores tests:

- **Bridge tests** (`tests/bridge`, plain Python plus Blender, no Cinema 4D): a small client speaks the protocol to a Blender started with the bridge. They cover the protocol, every scene builder, both drivers and the native material.
  `python tests\bridge\test_scene.py "<blender.exe>"`
- **Cinema 4D tests** (`tests/c4d`): one Python script per feature, run inside Cinema 4D by `tests\c4d\run_c4d_test.ps1`, which takes one script or several (comma-separated) for one start. With `-Headless` the scripts run in `c4dpy` (no window); without it the regular Cinema 4D starts with a small harness plugin, runs the scripts and quits, which is needed for anything with a window.
  `.\tests\c4d\run_c4d_test.ps1 -Headless -Script tests\c4d\lights.py -ModulePath build\plugin\bin\Release\plugins`
- **Comparison tests** are most of the Cinema 4D tests: a script builds a small scene, renders it with Cinema 4D's Standard renderer and with Cycles Render, and compares the pictures. Things to know when writing one:
  - Render with `RENDERFLAGS_OCIO_RAW_RENDERING`. Without it Cinema 4D's own renderers bake the view transform into the pixels and no number is comparable.
  - The Standard renderer computes direct light only: set Cycles to no bounces when brightness is compared. Differences from bounced light, depth of field and other things the Standard renderer lacks need no test against it.
  - The Standard renderer's picture sits about 0.3 px to the lower right of the exact projection, and it smooths edges differently. Compare centres of gravity, sizes and block averages; pixel-exact overlap of thin shapes is not reachable.
  - A Sphere primitive has "Render Perfect" on by default, which only Cinema 4D's own renderers honour. Switch it off in test scenes.
  - A primitive created from Python has no Phong tag and is flat shaded in both renderers.
- **Rules for tests:** run Cinema 4D tests one after another, never in parallel. A test in the regular Cinema 4D runs with a preferences folder of its own (`-Settings @{ g_prefsPath = "<empty folder>" }`), so that the user's preferences and layout stay untouched. Never end a Blender process by name: a test ends only the Blender it started, and the runner, after a start that crashed or was stopped, only the Blenders that start had started (found by their parent process).

## Development settings

Cinema 4D reads these from its command line (`name=value`) or from environment variables of the same name.

| Setting | Effect |
|---|---|
| `g_cyclesRenderBridge=<path to main.py>` | Use this bridge instead of the one installed in Blender |
| `g_cyclesRenderBlender=<path to blender.exe>` | Use this Blender instead of the one stored in the Preferences. For `c4dpy` and render nodes, which have preferences of their own |
| `g_cyclesRenderLogCalls=true` | Log every call Cinema 4D makes to the renderer, and when a render job starts and stops Blender |
| `g_cyclesRenderCapture=<file>` | Append every request and reply header exchanged with Blender to this file, with the exporter's listing of each scene. After sending settings the plugin then also reads them back from Blender |
| `g_cyclesRenderDumpFile=<file>` | `c4d.GePluginMessage(1073816, None)` writes the exporter's listing of the active document to this file, without Blender |
| `g_cyclesRenderSelfTest=<n>` | Without the dump file set, the same plugin message runs the session self-test for n cycles |
| `g_cyclesRenderIprTest=true` | The same plugin message first looks for an IPR test command (`ipr/ipr_testhook.h`) |
| `g_cyclesRenderLogPathCommands=true` | Write to the console what the image nodes' buttons "Make Relative" and "Make Absolute" receive when pressed (`nodespace/path_commands.cpp`) |
| `g_cyclesRenderNativeMaterial=true` | Offer the Cycles Native Material (its command and its entry in the material menus). Switched off in this release, see the roadmap, "Open bugs" |

The bridge takes `--show-window` after `--` to keep Blender's window visible; the IPR window has the same as a menu entry (*Options > Show Blender Window*), which is the quickest way to see what Blender was sent.

## Adding a feature

Every new kind of scene data follows the same three moves, in this order:

1. **Protocol.** Describe the item or field in [`protocol/protocol.md`](../protocol/protocol.md). The document and both implementations change in the same commit.
2. **Bridge.** Add or extend a builder in `bridge/c4d_bridge/scene/` (one function per item kind, registered in `BUILDERS`), or a handler in `server.py`. The bridge only copies values: it must not learn anything about Cinema 4D. Check it without Cinema 4D by sending the item from a small Python client.
3. **Plugin.** Write the exporter in `plugin/cyclesrender/source/scene/` or `materials/`, and call it from `SceneExporter`. Convert coordinates through `Coords`, read parameters through `scene/parameters.h`, and report what cannot be translated with `SceneWriter::Warn`.

Then **compare with Cinema 4D's Standard renderer**: build a small scene, render it with both, and look at the two pictures side by side (or measure them, with a test). A feature that reads the Cinema 4D scene is done when that comparison holds. Write down what you measured where the rule lives (the reference documents), and label anything you did not run as untested.

Other common changes:

| To | Do |
|---|---|
| Add a render setting | One line in `tools/make_settings.py`, run it; see [Generated files](#generated-files) |
| Translate another shader into nodes | A case in `ShaderTranslator::Native()` (`materials/shader_translate.cpp`) |
| Add a request the IPR sends | A handler in the bridge's `server.py`, a `PostRequest` in `ipr/ipr_controller.cpp`, an entry in the protocol |
| Add a parameter to the Cycles Object tag | `res/description/tcyclesobject.*`, a default in `tags/object_tag.cpp`, read it where it counts |
| Add a setting to the Cycles Light object and tag | The same id in `res/description/ocycleslight.*` and `tcycleslight.*` (the tag's switches decide by the hundreds of the id, `lights/light_tag.cpp`), its default in `InitLightSettings`, read it in `lights/light_settings.cpp` into `BlenderLight` (`scene/light_export.h`), write it in `WriteLight`, a field in the protocol and in `bridge/c4d_bridge/scene/lights.py` |
| Translate another Redshift node | A case in `RedshiftNodes::Translate` (`materials/redshift_nodes.cpp`), and a chain for it in `tests/c4d/redshift_nodes.py` against Redshift |
| Add a parameter to the Cycles Camera tag | `res/description/tcyclescamera.*`, a default in `tags/camera_tag.h` and `.cpp`, read it in `CameraValues::Read` (`scene/camera_export.cpp`) |
| Support a new Blender version | See the next section, and "Version coupling" in [reference/blender.md](reference/blender.md) |
| Register a new hook | It needs a plugin id from developers.maxon.net (`source/ids.h` says which may be shared) |

## Generated files

Never edit these by hand: change the tool or its input and run it again.

| Tool | Writes | Needs |
|---|---|---|
| `tools/make_settings.py` | The Render Settings page: `res/description/vpcyclesrender.h` and `.res`, `res/strings_en-US/description/vpcyclesrender.str`, `source/videopost/render_settings_table.h` | Python only |
| `tools/make_nodes.py` (with `dump_blender_nodes.py`) | The Cycles nodes: `source/nodespace/cycles_nodes_table.h` and the description database `res/nodes/*.json` | Blender: it asks Blender for every shader node |
| `tools/make_icons.py` | `res/icons/*.png` | Python only |
| `tools/make_native_placeholder.py` | `res/native_default.preview`, the picture of a native material that was never edited | Blender (its window shows for a moment) |
| `tools/make_example_scene.py` | `examples/cycles_render_examples.c4d` and its picture | Cinema 4D with the plugin, and Redshift |

**Moving to a new Blender version** means: add the version to `SUPPORTED_BLENDER` (`bridge/c4d_bridge/__init__.py`) and `SUPPORTED_VERSIONS` (`session/blender_install.cpp`); check the mechanisms listed under "Version coupling" in the Blender reference; run `make_nodes.py` and review its hand-written tables (`PANELS`, `MATERIAL_SETTINGS`, `SKIP_NODES`); compare the defaults in `make_settings.py` with a new scene in that Blender; run `make_native_placeholder.py`.

## Code conventions

**Plugin (C++, the Cinema 4D SDK's rules):**

- maxon types and error handling: `maxon::Result<T>`, `iferr_scope`, `iferr_return`. No exceptions, no RTTI, no standard-library containers.
- Tabs for indentation, braces on their own lines, `override` on virtual overrides.
- Every file starts with `// SPDX-License-Identifier: Apache-2.0`.
- Every header starts with what the unit is for, and documents its declarations with `///`. Every `.cpp` starts with a pointer to its header and, where it helps, a reading order. Comments say *why*, and give the measurement behind a constant.
- Everything specific to the operating system goes under `platform/`, behind a header of plain C++ types; those `.cpp` files include no Cinema 4D headers.
- Idioms that compile cleanly under the style check are collected under "Build" in [reference/c4d-sdk.md](reference/c4d-sdk.md).

**Bridge (Python):**

- Every file starts with `# SPDX-License-Identifier: GPL-3.0-or-later` and a docstring.
- Only `bpy`, `numpy` and the standard library, all of which Blender brings.
- All `bpy` work happens on Blender's main thread; the socket thread only moves bytes.
- A handler returns `(reply fields, reply blobs)` or raises `RequestError(code, message)`.

**The licence boundary:** `bridge/` is GPL-3.0-or-later; everything else is Apache-2.0, except the scripts in `tools/` that run inside Blender and import `bpy` (GPL-3.0-or-later). `plugin/` talks to it only through the socket protocol, shared memory and files the bridge writes. Never copy code from Blender or from the bridge into `plugin/`.

## Releasing

A release is two packages with the same version number, built by one command:

```powershell
.\tools\make_release.ps1 -Version 0.1.0
```

It checks that the version stands in all three places (`plugin/cyclesrender/source/version.h`, `bridge/blender_manifest.toml`, `bridge/c4d_bridge/__init__.py`), builds the plugin, and writes into `release/0.1.0/`:

- `CyclesRender-0.1.0-C4D2027-win64.zip`: the folder `cyclesrender` for Cinema 4D's plugins folder;
- `cycles_render_bridge-0.1.0.zip`: the extension to install in Blender;
- both unpacked, for trying them without unzipping.

Upload the two .zip files to the release on GitHub. [release/README.md](../release/README.md) has the checklist for a release.

The packages can be checked without touching your own Blender or Cinema 4D set-up. Blender installs the bridge into a throw-away profile, and the plugin, which reads the same environment variable as Blender, finds it there:

```powershell
$env:BLENDER_USER_RESOURCES = "$env:TEMP\cyclesrender_release_profile"
& "<blender.exe>" --command extension install-file -r user_default -e release\0.1.0\cycles_render_bridge-0.1.0.zip
.\tests\c4d\run_c4d_test.ps1 -Headless -InstalledBridge -Script tests\c4d\coords.py -ModulePath release\0.1.0\cinema4d
```

`-InstalledBridge` keeps the test runner from naming the repository's bridge, so the test renders through the installed one, with the packaged plugin. (The runner ends the PowerShell it runs in; start it in a shell of its own.)

Whether a plugin and a bridge of different releases can talk is decided by the protocol version alone (`PROTOCOL_VERSION` in `session/protocol.h`, `VERSION` in `bridge/c4d_bridge/protocol.py`): raise both when a message changes in a way the other side would misread.

## Porting to macOS

Not started, and nothing here was run on a Mac. What a port needs:

- Implementations of the four `platform/` headers: `socket.h` (BSD sockets), `shared_memory.h` (POSIX shared memory and a named semaphore or similar for the frame signal), `process_stats.h`, `environment.h`.
- In the bridge, `viewport/window.py` (hiding, showing and resizing Blender's window from outside its API, done with Win32 calls today) and the shared-memory names in `viewport/frame_block.py`.
- `DetectBlender` and `FindBridgeScript` (`session/blender_install.cpp`) for the places Blender uses on macOS.
- The module's `target_platforms` in `project/CMakeLists.txt`, and a build script.
- A check that a hidden Blender window keeps rendering: macOS may throttle windows that are hidden or covered, which the IPR depends on.

## Pitfalls

All of these were hit once already.

- A script in the regular Cinema 4D runs on the main thread. Calling `c4d.documents.StopExternalRenderer()` there while a Picture Viewer render is running deadlocks. To test cancelling, render on a `c4d.threading.C4DThread` and end that thread.
- Read parameters with `GetParameter` (the helpers in `scene/parameters.h`), not from the object's container: several parameters are not stored there under their id, and the container then silently answers 0.
- A blob belongs to the scene item that is ended next (`SceneWriter`). Allocate an item's blobs after the previous item's `EndItem`, never before writing a helper item, such as its material, in between.
- Python keyword arguments must be strings: pass Cinema 4D parameter ids in a dict, not as `**{id: value}`.
- Saving a float picture with Cinema 4D and loading it again changes the numbers (colour conversion). Stored reference renders are therefore raw floats.
- The camera Cinema 4D 2027 creates by default is an `Orscamera`, which is no instance of `Ocamera`. Check for both.
- A Hair object, a Matrix object and a Redshift light carry the "used by a generator" mark themselves; that mark must not hide them.
- In `c4dpy` nothing calls timers: a test that needs the IPR to advance calls its `tick` itself.
