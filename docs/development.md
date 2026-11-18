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
| Cinema 4D | 2024, 2025, 2026 and 2027 (2024.5.1, 2025.3.3, 2026.3.4, 2027.0.0) | With `c4dpy.exe` and `Commandline.exe`, which the tests run in, and the Python that comes with each (`resource\modules\python\libs`). One is enough to build and run for it; the baseline runs in each. The tests use 2027 unless told otherwise |
| Cinema 4D C++ SDK | One per version: 2024.5, 2025.3.2, 2026.3, 2027.0 | The `sdk.zip` of each Cinema 4D installation folder, or from developers.maxon.net. Unpack them anywhere; they are never modified. Building for 2024 also needs the 2025 SDK (its CMake files, see [reference/c4d-sdk.md](reference/c4d-sdk.md), "Cinema 4D versions") |
| Blender | 5.0, 5.1, 5.2 and 5.3 (5.0.0, 5.1.2, 5.2.3 LTS, 5.3.0) | The official installer's default location (`C:\Program Files\Blender Foundation\Blender <major>.<minor>`) is found automatically. One is enough to build and run; `tools/make_nodes.py` needs all four, and the baseline runs on each. The tests use 5.2, the long-term support release, unless told otherwise. Without a Blender chosen in the Preferences the plugin takes the one holding the bridge, else 5.2, else the newest |
| Visual Studio | 2026 with MSVC toolset v143 and Windows SDK 10.0.26100.0 | Visual Studio 2022 works with other parameters, see below |
| CMake | 3.30 or newer | |
| Python | 3.10 or newer on `PATH` | For the generators in `tools/` and the bridge tests. Blender brings its own Python for the bridge |

The project keeps its lookup material in a folder `references/` next to the sources, which is not part of the repository: the unpacked C++ SDKs (`references/Cinema_4D_CPP_SDK_2027_0_0`, `references/Cinema 4D 2026_sdk`, `... 2025_sdk`, `... 2024_sdk`, where the build script looks by default), the Python SDK, the Blender source of the supported version and its manual. Having the Blender source at hand is worth it: several facts in [reference/blender.md](reference/blender.md) were read there.

## Build

From the repository root, in PowerShell:

```powershell
.\tools\build_plugin.ps1                     # Release for Cinema 4D 2027; the SDK is looked for in references\
.\tools\build_plugin.ps1 -Version 2025       # for Cinema 4D 2025 (2024, 2025, 2026, 2027)
.\tools\build_plugin.ps1 -Version all        # for all four, one after the other (about 3 minutes from scratch each)
.\tools\build_plugin.ps1 -Sdk D:\sdk\c4d2027 # or name the SDK (or set C4D_SDK for 2027, C4D_SDK_<year> for any)
.\tools\build_plugin.ps1 -Config Debug
.\tools\build_plugin.ps1 -Reconfigure        # after changing CMake settings, the SDK or the toolset
```

The result is `build\plugin\bin\Release\plugins\cyclesrender\cyclesrender.xdl64` for 2027 and `build\plugin-<year>\bin\Release\plugins\cyclesrender\cyclesrender.xdl64` for the others, each with the module's `res` folder next to it. The first build of a version also builds the SDK's frameworks and takes a few minutes; later builds take seconds. A build for one Cinema 4D version does not load in another; the release puts all four side by side in one folder, and each Cinema 4D picks its own ([reference/c4d-sdk.md](reference/c4d-sdk.md), "Cinema 4D versions", which also says how the build set-up differs between the SDKs).

- With Visual Studio 2022: `-Generator "Visual Studio 17 2022" -Architecture x64`.
- The build must finish **without warnings**, for every version: the module is compiled with the SDK's style check at level 3, which turns style faults into errors.
- A name or signature that differs between the SDKs is answered in `source/sdk_compat.h`, never with a version test in the code that uses it. The 2024 build is C++17 and has no `cinema` namespace (the macro `cinema` is empty there): no `!=` on a `Result`, and no `cinema::Bool cinema::F()` (write `auto cinema::F() -> cinema::Bool`).
- The build fails while a Cinema 4D that has loaded the plugin is open, because the binary is locked. Close it first.
- After adding or removing source files the script builds twice by itself for 2027: the first pass only regenerates the project. The SDKs of 2024 to 2026 list the source files only when the project is configured and a build does not notice a new one (it fails to link); the script keeps the list it configured with (`build\plugin-<year>\source_files.txt`) and configures again when the files differ (since 2026-11-16; before, `-Reconfigure` was needed).

The bridge is plain Python and needs no build.

## Run the build in Cinema 4D

Nothing has to be installed. Start Cinema 4D with the built plugin and with the bridge from the repository:

```powershell
& "C:\Program Files\Maxon Cinema 4D 2027\Cinema 4D.exe" `
    "g_additionalModulePath=<repo>\build\plugin\bin\Release\plugins" `
    "g_cyclesRenderBridge=<repo>\bridge\c4d_bridge\main.py"
```

Cinema 4D's console (*Extensions > Console*) shows `Cycles Render <version> for Cinema 4D <year>: module loaded` and everything the exporter could not translate. Without `g_cyclesRenderBridge` the plugin uses the bridge installed in Blender, as it does for a user. For another Cinema 4D version take that version's executable and build folder (`build\plugin-<year>\bin\Release\plugins`). If a Cinema 4D of the same version is already open, add `g_allowParallelInstance=true`: otherwise the new start hands its command line to the open one and ends.

## Tests

The test suite lives in `tests/` and is **kept locally: it is not part of the repository** (nor are the stored renders and test scenes it uses). If you have it:

```powershell
.\tools\run_tests.ps1 -Changed             # the tests for what differs from the last commit
.\tools\run_tests.ps1 -Changed -DryRun     # only print what it would run, and why
.\tools\run_tests.ps1                      # the whole baseline: build, 13 bridge tests, 57 Cinema 4D tests; about 20 minutes, with Blender 5.2 and Cinema 4D 2027
.\tools\run_tests.ps1 -SkipBuild -Only lights,materials
.\tools\run_tests.ps1 -SkipBuild -Blender 5.3   # the baseline with another supported Blender (a version, or the path of a blender.exe)
.\tools\run_tests.ps1 -C4D 2026                  # the baseline in another supported Cinema 4D (a year, or the folder of an installation)
```

**Which versions, when** (the maintainer's rule, 2026-11-16). Every run is Cinema 4D 2027 with Blender 5.2 unless told otherwise: the baseline, `-Changed` and `-Only` alike, and the build script builds 2027 only unless told otherwise. The sprint's baseline is that pair. **Before a major release** the whole baseline also runs on each other Cinema 4D version with Blender 5.2, and on each other Blender version with Cinema 4D 2027, one run after the other (never two at once: they share the output folder in `%TEMP%`, the GPU and the plugin binary):

```powershell
foreach ($y in "2024","2025","2026") { .\tools\run_tests.ps1 -C4D $y > baseline_c4d$y.txt }            # each builds its version first
foreach ($v in "5.0","5.1","5.3") { .\tools\run_tests.ps1 -SkipBuild -Blender $v > baseline_blender$v.txt }
```

A piece of work that concerns one version (a difference found in it, a change in `sdk_compat.h`, a feature of one Blender) also runs its selected tests in that version.

**With which Cinema 4D.** `-C4D` decides the Cinema 4D of every Cinema 4D test and the plugin build they load (`build\plugin` for 2027, `build\plugin-<year>` for the others; without `-SkipBuild` the script builds that version first); the default is 2027. Every start of `c4dpy` gets a preferences folder of its own per version (`%TEMP%\cyclesrender_c4d_tests\prefs_c4dpy_<year>`), the window tests a fresh one each; the runner writes the licence method "Maxon App" into a new one (else c4dpy asks for it), leaves out plugins installed into the Cinema 4D folder itself except Redshift (`g_moduleNegatives`; OctaneRender in 2025's `plugins` folder ended c4dpy at start), and starts the regular Cinema 4D with `g_allowParallelInstance=true`, so that a Cinema 4D of the same version that is open stays untouched. A Cinema 4D without its Python runtime cannot run any test; the runner says so and counts them failed. What differs between the Cinema 4D versions and what the tests do about it: [reference/c4d-sdk.md](reference/c4d-sdk.md), "Cinema 4D versions". The tests make their documents with OCIO colour management in every version (2024 makes Basic ones) and ask for render-space pictures through `crtest.render_flags` (the flag of 2026 on, the render setting before). Tests that know a Cinema 4D version (each a measured difference of Cinema 4D's own, listed in the SDK reference): `golden` sets the render space of its scenes (saved with 2027) to ACEScg by name, since older versions read the stored index as another space, records what was read, and compares with a picture of the version where one is stored (`tests/golden/<name>.<year>.crf`: `materials.2024`, for 2024's default grey); `redshift` renders a black Redshift reference again and, under 2024 and 2026, reports a light that stays black as not compared (Redshift's behaviour there; `light_enable` and `light_shadows` do the same for their Redshift comparisons); in 2024 `shaders` allows the Layer shader's one rebake per render, `volumes` expects the plugin's own files for the nameless grids, and `ipr_add` and `ipr_window_checks` accept that undo and redo drop the render view's camera link. `crtest` answers the Python names that differ (`FP_BASIC_EMITTER`, `graph_root`). Every other test expects the same in every Cinema 4D version.

**With which Blender.** `-Blender` decides the Blender of every test: the bridge tests start it, the Cinema 4D tests get it as `g_cyclesRenderBlender`, the window tests too; the default is 5.2, and the other versions get the baseline before a major release (above). Every line of the summary names the Blender and the Cinema 4D it ran with. For each piece of work the tests of what changed run on the default version, and also on the version a change is about.

**Tests that expect something different per version** (each asks the Blender it runs with, `crtest.blender_version()` in a Cinema 4D test, the `hello` reply in a bridge test, and says in its result which case applied): `test_lights` (Eevee of 5.0 and 5.1 shows no light to the camera), `test_cameras` and `camera_tag` (Eevee renders panoramas from 5.3 on, and the plugin's warning is gone then), `test_versions` (the lighting nodes of 5.3, the shadow pool options of 5.2, every settings path by the version it exists from), `settings` (a setting newer than the Blender in use is not sent and is named), `volumes` (the Cycles of 5.0 and 5.1 renders Cinema 4D's VDB files empty: the warning is expected there), `redshift_material` (subsurface tolerance 0.2 before 5.2, where Random Walk changed), `cycles_nodes` (nodes newer than the Blender in use are left out and named; its section 8 exports one material as for 5.0, 5.2 and 5.3 whatever Blender is installed, through the test hook's `versioned`). Every other test expects the same in every version; the stored pictures of `golden` are one set (rendered with 5.2.3) and are compared within the test's 2 % on every version (measured 2026-11-16: at most 0.0001 on 5.0, 5.1 and 5.2, 0.0019 on 5.3 for `materials`). A test that fails on one version only is a difference between the versions until shown otherwise: find it, then either handle it (in the bridge for an API difference, in the plugin for what the user should be told) or make the test expect it, and write it into "Version coupling" in [reference/blender.md](reference/blender.md).

**When to run which.** For every piece of work: the tests for what changed (`-Changed`). The whole baseline on Cinema 4D 2027 with Blender 5.2: once per sprint (a group of items the maintainer calls done), before a commit the maintainer asks for, and before a release; on the other Cinema 4D and Blender versions before a major release ("Which versions, when"). A typical selected run takes one and a half to four minutes (measured: a bridge file 97 s, an exporter file 230 s with the build, a file the map does not know, which runs the core set, 203 s); the baseline about 14 minutes per Blender version (820 s; with one start of Cinema 4D per test it was 940 s), and about two minutes more since the camera tests of 0.2.0 (estimated from their own times: `cameras` 91 s instead of 30 s, `camera_tag` 40 s, `test_cameras` about 20 s; and the tests of the 0.2.0 features measured in a selected run: `output` 21 s, `image_nodes` 10 s, `ipr_add` under 1 s, `test_output` 3 s; of F10 to F14: `cycles_lights` 61 s, `redshift_nodes` 44 s, `devices` 23 s, `test_lights` 3 s, `test_devices` 4 s; of F15 and F16: `ipr_reuse` 35 s, `ipr_follow_window` 37 s with its start; of F18: `ipr_lights` 18 s, `ipr_lights_window` 39 s with its start; of F17: `ipr_navigate` 10 s, `ipr_navigate_window` 46 s with its start; of B8, B9 and F20: `light_enable` 54 s, `light_shadows` 78 s (173 s with the penumbra cases of F20's follow-up), `editor_camera` 44 s (B10), `new_light_window` 16 s with its start (B12), `ipr_lights_window` 52 s with its start; of F22: `ipr_live_window` 53 s with its start; of B11: `test_frame_rate` 9 s, `ipr_device` 25 s, `ipr_frames_window` 47 s with its start; a `-Changed` run of 50 tests, close to the baseline, took 1002 s). The whole baseline of 56 tests with Blender 5.2 took 1143 s on Cinema 4D 2027, 1075 s on 2026 and 1078 s on 2024 (2026-11-16, F9, without the build).

**How `-Changed` chooses.** It takes the files that differ from the last commit (staged or not, and new files git does not ignore) and, because `tests/` is not in git, the files in `tests/` written since the last commit was made. A map in `tools/run_tests.ps1` (the field `Covers` of each test, next to the test list) says which tests check which paths. A few files are everybody's (the scene walk and writer, the session layer, the videopost, the bridge's server and protocol, the runner itself): a change in one of them runs a named core set of twelve tests, and the script says next to the list why that set is enough. Every run of `-Changed` also includes an always-run set (`test_protocol`, `session_selftest`, `coords`: about a minute). A file the map does not know runs the core set and is named as unmapped; a change of documents only runs nothing. The script prints which tests it chose for which file. A selected run can miss a fault in code the map does not connect to the change: that is what the sprint's full run is for, and when it finds such a fault, the connection goes into the map in the same piece of work.

**Several tests in one start of Cinema 4D.** Tests that need the same start settings (the `g_cyclesRender...` settings of [Development settings](#development-settings), which Cinema 4D reads once at start) share one start of `c4dpy` and run one after another in a fixed order that the script prints. Each starts from a known state (`tests/c4d/lib/crrun.py`: no IPR, one new empty document, the capture file emptied), has its own result and time, and must pass alone as well as in its group: a test reads counters as differences and the log through `crtest.log_lines()`. A test that needs the end of the session for its verdict comes last in its start. When `c4dpy` ends before every test of a start has reported, the test that was running fails and the ones not yet run are run again, each alone. `-Alone` runs every test in a start of its own, as before.

It prints one line per test and "N of N passed", and fails when a Blender started during the run is still running at the end. Two tests open a Blender window for a few seconds, and the last nine open the regular Cinema 4D for half a minute to a minute each, each with a preferences folder of its own, made fresh in `%TEMP%\cyclesrender_c4d_tests` (the field `PrefsFiles` of the map puts files into it before the start: `default_engine_window` gets the user's template scene `new.c4d` there, which Cinema 4D reads only at start). `blender_missing_window` runs without `g_cyclesRenderBlender` and with `g_cyclesRenderBlenderSearch` pointing at an empty folder, so that the plugin finds no Blender; the supported Blender it renders with at one point reaches the script as `CYCLESRENDER_TEST_BLENDER`, which Cinema 4D does not read. One of them, `ipr_navigate_window`, posts mouse and key messages to that Cinema 4D's IPR window from a process of its own (`tests\c4d\lib\post_input.ps1`): the real mouse pointer is not moved and no other window is touched, but a window placed over the IPR window does not matter either, since the messages go to the window, not the screen. `test_viewport` and `render_cancel` depend on timing and can fail under load; run a failed test alone (`-Only <name>`) before looking for a cause. If you do not have the tests, check a change by hand as described under [Adding a feature](#adding-a-feature) and say so in the pull request.

What the suite consists of, for whoever writes or restores tests:

- **Bridge tests** (`tests/bridge`, plain Python plus Blender, no Cinema 4D): a small client speaks the protocol to a Blender started with the bridge. They cover the protocol, every scene builder, both drivers and the native material. The two tests of the Cycles Native Material (`test_native`, `native_material`) are on hold with the feature and are in no run: the field `OnHold` in the map of `tools/run_tests.ps1` takes a test out, and removing it takes the test back in.
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
| `g_cyclesRenderBlenderSearch=<folder>` | Detection looks for Blender in this folder instead of `C:\Program Files\Blender Foundation` (at the first start, the page's Detect button, and while none is chosen). An empty or missing folder makes a machine with Blender installed look like one without, without touching the installation: `blender_missing_window` (0.2.0, B7) |
| `g_cyclesRenderLogCalls=true` | Log every call Cinema 4D makes to the renderer, and when a render job starts and stops Blender |
| `g_cyclesRenderCapture=<file>` | Append every request and reply header exchanged with Blender to this file, with the exporter's listing of each scene. After sending settings the plugin then also reads them back from Blender |
| `g_cyclesRenderDumpFile=<file>` | `c4d.GePluginMessage(1073816, None)` writes the exporter's listing of the active document to this file, without Blender |
| `g_cyclesRenderSelfTest=<n>` | Without the dump file set, the same plugin message runs the session self-test for n cycles |
| `g_cyclesRenderIprTest=true` | The same plugin message first looks for an IPR test command (`ipr/ipr_testhook.h`) |
| `g_cyclesRenderIprUnoptimized=true` | The IPR exports as before B11 of 0.2.0, for comparing on a given machine and scene with one build: every redraw of a view exports the scene, a generated mesh is hashed byte by byte, and "Optimized Live Updates" counts as off. `internal/b11/run_probe.ps1 -Unoptimized` measures with it |
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
| Support a new Blender version | "Adding a Blender version" in the next section, and "Version coupling" in [reference/blender.md](reference/blender.md) |
| Support a new Cinema 4D version | "Adding a Cinema 4D version" below |
| Register a new hook | It needs a plugin id from developers.maxon.net (`source/ids.h` says which may be shared) |

## Generated files

Never edit these by hand: change the tool or its input and run it again.

| Tool | Writes | Needs |
|---|---|---|
| `tools/make_settings.py` | The Render Settings page: `res/description/vpcyclesrender.h` and `.res`, `res/strings_en-US/description/vpcyclesrender.str`, `source/videopost/render_settings_table.h` | Python only |
| `tools/make_nodes.py` (with `dump_blender_nodes.py`) | The Cycles nodes: `source/nodespace/cycles_nodes_table.h`, the description database `res/nodes/*.json`, and the list of nodes by version `docs/reference/cycles-nodes.md` | Every supported Blender (5.0 to 5.3): it asks each for every shader node and takes the union; it prints what each version adds and fails on a node no hand-written table names |
| `tools/make_icons.py` | `res/icons/*.png` | Python only |
| `tools/make_native_placeholder.py` | `res/native_default.preview`, the picture of a native material that was never edited | Blender (its window shows for a moment) |
| `tools/make_example_scene.py` | `examples/cycles_render_examples.c4d` and its picture | Cinema 4D with the plugin, and Redshift |

**Adding a Blender version** (since 0.2.0 the plugin supports several side by side; the versions stand in four places that change together):

1. Install it in the installer's usual place. Add it to `SUPPORTED_BLENDER` (`bridge/c4d_bridge/__init__.py`), `SUPPORTED_VERSIONS` (`session/blender_install.cpp`), `VERSIONS` (`tools/make_nodes.py`) and raise `blender_version_max` in `bridge/blender_manifest.toml` (exclusive: the first version that is not supported). The messages that name the versions follow `SUPPORTED_VERSIONS`. Decide whether detection's preferred version (`PREFERRED_MAJOR`/`PREFERRED_MINOR`, 5.2 today, with its reason next to it) should change.
2. Run the bridge tests on it (`.\tools\run_tests.ps1 -Blender <version> -Only test_protocol,test_render,...`, or the whole baseline) and go through the mechanisms under "Version coupling" in the Blender reference. Handle what differs in the bridge (feature tests such as `"resize" in bpy.types.PointCloud.bl_rna.functions`, not version numbers, where Blender's API changed) and write each difference into that table with the version it applies to.
3. Run `python tools\make_nodes.py`. It prints what the version adds and stops when the version brings a node that `NODES_BY_VERSION` does not name: read in the version's manual what the node does, then add it to `NODES_BY_VERSION` under the version, and to `SKIP_NODES` with the reason if it cannot work in a material the plugin sends (needs data the plugin does not send, belongs to another kind of tree, or is inserted by Blender itself). Look at new sockets of nodes that have panels (`PANELS`), new sockets Blender fills in itself (`SKIP_SOCKETS`), and the defaults it prints as changed. It also stops when something a version had is gone from a later one: the table has no "until" yet.
4. Compare the Render Settings with a new scene in that Blender (every path of `tools/make_settings.py` with its default and options, `tests/bridge/test_versions.py` checks the paths): mark a property an older supported version lacks with `since=`, and write differing defaults into the docstring of `make_settings.py`.
5. Run the whole baseline on every supported version and look at what fails on one version only (see "Tests"): here, when a version is added, all of them; afterwards the other versions only before a major release ("Which versions, when"). `make_native_placeholder.py` stays with one Blender (5.2).

**Adding a Cinema 4D version** (since 0.2.0 one source tree builds for 2024 to 2027; [reference/c4d-sdk.md](reference/c4d-sdk.md), "Cinema 4D versions", is the list of what differs):

1. Install it, unpack the `sdk.zip` of its installation folder into `references\` (or name it with `C4D_SDK_<year>`). Compare its `core.framework/source/maxon/apibase_version.h` with the newest supported SDK: a new ABI number means its build loads only in it; the same ABI and a lower core framework version means the older build would load there too.
2. In `tools/build_plugin.ps1` add the year to `-Version` and to the SDK folders; a new SDK whose module definition is neither `project/CMakeLists.txt` (2027) nor `projectdefinition.txt` (2024 to 2026) needs its own branch there. In `tools/run_tests.ps1` and `tools/make_release.ps1` add the year to their lists of versions.
3. Build it (`-Version <year>`). Every compiler error that comes from a name or signature of the SDK goes into `source/sdk_compat.h` as a helper or constant with the versions that differ, and into the table of the SDK reference; the build must end without warnings, also in the older versions (`-Version all`).
4. Run the baseline in it (`run_tests.ps1 -C4D <year>`), and once in each other supported version (afterwards they get it only before a major release, "Which versions, when"), and look at what fails in that version only: a difference of Cinema 4D's own (a default, a renderer's result, a Python name) or a fault of the plugin. Handle it in the plugin where the user would see it (and write it under "Differences in behaviour"), or make the test expect it with the reason.
5. Check by hand what no test reaches (screenshots with `tests/c4d/show_*.py` and `run_c4d_test.ps1 -C4D <exe> -ScreenshotDir`): the Render Settings page, the Preferences page, the Node Editor with the Cycles nodes, the tags, the Cycles Light, the IPR window. Then make a release folder and load it in every supported version: each must say "for Cinema 4D <its year>: module loaded".

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

It checks that the version stands in all three places (`plugin/cyclesrender/source/version.h`, `bridge/blender_manifest.toml`, `bridge/c4d_bridge/__init__.py`), builds the plugin for every supported Cinema 4D version, and writes into `release/0.1.0/`:

- `CyclesRender-0.1.0-C4D2024-2027-win64.zip`: the folder `cyclesrender` for Cinema 4D's plugins folder, with one build per Cinema 4D version (`cyclesrender_2024.xdl64` to `cyclesrender_2027.xdl64`) and one `res`. The same folder goes into every Cinema 4D; each loads the build of its own version and passes over the others without a word (measured in all four, [reference/c4d-sdk.md](reference/c4d-sdk.md), "Cinema 4D versions"). There is nothing to delete for the user, and no loader;
- `cycles_render_bridge-0.1.0.zip`: the extension to install in Blender;
- both unpacked, for trying them without unzipping.

Upload the two .zip files to the release on GitHub. [release/README.md](../release/README.md) has the checklist for a release. `-OutDir <folder>` writes the packages elsewhere, to try the packaging without replacing those of a release already made with the same number.

The packages can be checked without touching your own Blender or Cinema 4D set-up. Blender installs the bridge into a throw-away profile, and the plugin, which reads the same environment variable as Blender, finds it there:

```powershell
$env:BLENDER_USER_RESOURCES = "$env:TEMP\cyclesrender_release_profile"
& "<blender.exe>" --command extension install-file -r user_default -e release\0.1.0\cycles_render_bridge-0.1.0.zip
.\tests\c4d\run_c4d_test.ps1 -Headless -InstalledBridge -Script tests\c4d\coords.py -ModulePath release\0.1.0\cinema4d
```

`-InstalledBridge` keeps the test runner from naming the repository's bridge, so the test renders through the installed one, with the packaged plugin. (The runner ends the PowerShell it runs in; start it in a shell of its own.) For the other Cinema 4D versions add `-C4DPy "C:\Program Files\Maxon Cinema 4D <year>\c4dpy.exe"` and a preferences folder of their own (`-Settings @{ g_prefsPath = "<folder>" }`, with the `licensetype.prf` that `tools/run_tests.ps1` writes); Cinema 4D's console names the build that loaded ("for Cinema 4D <year>: module loaded").

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
