# Cycles Render

A render engine plugin for Cinema 4D 2024 to 2027 that renders the Cinema 4D scene with the Cycles and Eevee of Blender 5.0 to 5.3. It is two programs: a **plugin** (C++, `plugin/cyclesrender`) loaded by Cinema 4D, which reads the scene and translates it into Blender's terms, and a **bridge** (Python, `bridge/c4d_bridge`) that runs inside a Blender the plugin starts, rebuilds the scene there and renders it. They talk over a local socket; live frames come back through shared memory. The plugin is built once per Cinema 4D version from one source tree; what differs between the SDKs is answered in `plugin/cyclesrender/source/sdk_compat.h`, never by version tests spread through the code.

Final frames and the live render (IPR) window work end to end on Windows. What is done, approximate and open is in `docs/roadmap.md`.

## Where to start

| If you are about to | Read first |
|---|---|
| Do anything | `docs/roadmap.md`: where the project stands and what is open |
| Change how the parts work together | `docs/architecture.md`: the design and the reason behind each decision |
| Build, run, test or release | `docs/development.md` |
| Touch `bridge/` | `docs/reference/blender.md`: the Blender internals this project depends on, and which of them are tested |
| Touch `plugin/` | `docs/reference/c4d-sdk.md`: SDK build rules, identifiers and pitfalls, and how scene data is converted, with the measurements behind each rule |
| Change a message between the two | `protocol/protocol.md`, and change it in the same commit as the code |

Each source file starts with a comment that says what it is for; headers document every declaration. `docs/architecture.md` has a map of all source files.

## Commands

Run from the repository root in PowerShell. Details and options are in `docs/development.md`.

```powershell
.\tools\build_plugin.ps1                  # build the plugin for Cinema 4D 2027 -> build\plugin\bin\Release\plugins\cyclesrender
.\tools\build_plugin.ps1 -Version all     # for 2024, 2025, 2026 and 2027 (the others go to build\plugin-<year>)
.\tools\run_tests.ps1 -SkipBuild -Only fog_object   # the tests of the thing you changed, after a build (needs the local tests folder)
.\tools\run_tests.ps1 -Changed -DryRun    # which tests the map connects to what differs from the last commit: a help for choosing, often more than is relevant
.\tools\run_tests.ps1                     # the whole test baseline (Cinema 4D 2027, Blender 5.2), before a release, about 45 minutes
.\tools\run_tests.ps1 -C4D 2026           # the same in another Cinema 4D version, before a major release (with Blender 5.2)
.\tools\run_tests.ps1 -SkipBuild -Blender 5.0   # the same on another Blender, before a major release (with Cinema 4D 2027)
.\tools\run_tests.ps1 -SkipBuild -Only lights,materials
.\tools\make_release.ps1 -Version 0.1.0   # both release packages -> release\0.1.0
```

To try a build by hand, start Cinema 4D with the plugin and the repository's bridge:

```powershell
& "C:\Program Files\Maxon Cinema 4D 2027\Cinema 4D.exe" "g_additionalModulePath=<repo>\build\plugin\bin\Release\plugins" "g_cyclesRenderBridge=<repo>\bridge\c4d_bridge\main.py"
```

The build fails while a Cinema 4D that has loaded the plugin is open (the binary is locked): ask for it to be closed. A Cinema 4D you start while one of the same version is open hands its command line to the open one and ends: add `g_allowParallelInstance=true` (the test runner does), and give every Cinema 4D you start a preferences folder of its own (`g_prefsPath`).

## What is in the repository, and what is not

Published: `plugin/`, `bridge/`, `protocol/`, `tools/`, `docs/`, `examples/`, and `release/README.md`.

Kept on the developer's machine only (git-ignored), so do not expect them in a fresh clone:

- `references/`: read-only lookup material: both Cinema 4D SDKs, the Blender source and manual, design mockups. Never modify it.
- `tests/`: the test suite with its scenes and stored renders. `tools/run_tests.ps1` runs it.
- `prototypes/`: throw-away scripts that proved the Blender mechanisms.
- `build/`: all build output.
- `release/<version>/`: built packages.
- `internal/`: working notes and archived planning documents. The archive is history, not instructions: the documents in `docs/` are what counts. The one working file is the checklist of the release in preparation (`internal/roadmap-<version>.md`): if it is there, read it after `docs/roadmap.md`; it opens with where the work stands and what comes next. `internal/README.md` says what else is in the folder.

## Rules

- **The licence boundary.** `bridge/` is GPL-3.0-or-later (`bridge/LICENSE`); everything else is Apache-2.0 (`LICENSE`), except the two scripts in `tools/` that import `bpy`. Every source file starts with its `SPDX-License-Identifier` line. `plugin/` talks to the bridge only through the socket protocol, shared memory, and files the bridge writes. Keep GPL code out of `plugin/`, and keep knowledge of Cinema 4D out of the bridge.
- **Plugin ids are issued, not invented.** They are in `plugin/cyclesrender/source/ids.h`, which also says which hooks may share one. For a new hook that needs its own, ask the maintainer for an id.
- **End only the Blender processes the plugin or a test started.** The user may have a Blender of their own open: never stop processes by name.
- **Tests in the regular Cinema 4D run with a preferences folder of their own** (`g_prefsPath`, as `tools/run_tests.ps1` does): the user's own preferences and layout are never touched.
- **Tell the user what is not translated.** Anything in the scene the exporter cannot translate is named in the console (`SceneWriter::Warn`), never dropped silently.
- **Never edit generated files by hand.** The Render Settings page comes from `tools/make_settings.py` (its defaults are Blender's own), the Cycles nodes from `tools/make_nodes.py`. The list is in `docs/development.md`, "Generated files".
- **Parameter ids are stored in scene files.** Never renumber one and never reuse a retired number.
- **The protocol document and both implementations change together.**
- **Licensing questions, account choices, pushing and publishing belong to the maintainer.** Commit only when asked.
- **New work goes on `main`.** A branch named after a version (`0.1.0`) is that release as it was published, and is locked: never commit to it.

## When is a change done

- The plugin builds without warnings for every supported Cinema 4D version (`.\tools\build_plugin.ps1 -Version all`).
- The tests relevant to the change pass, and only those are run (`.\tools\run_tests.ps1 -SkipBuild -Only <names>`: the test of the feature and of what it directly touches, not the wide selection `-Changed` makes and not tests that will pass anyway), with no Blender left running, in Cinema 4D 2027 with Blender 5.2 (the default) and also in any version the change concerns (`-C4D <year>`, `-Blender <version>`). A release is done when all tests pass: the whole baseline on Cinema 4D 2027 with Blender 5.2 runs before every version release, unless the maintainer says otherwise, and not per change or per commit; before a major release it also runs on each other Cinema 4D version (2024, 2025, 2026, with Blender 5.2) and on each other Blender version (5.0, 5.1, 5.3, with Cinema 4D 2027), the commands in `docs/development.md`, "Which versions, when"; when it finds a fault that a selected run missed, connect the file to the test in the map in the same piece of work. Without the local tests: say so, and say what you checked by hand.
- A feature that reads the Cinema 4D scene is shown next to Cinema 4D's Standard renderer by a test. Things the Standard renderer cannot do (bounced light, depth of field) are tested on their own.
- What you measured is written down where the rule lives (`docs/reference/`), and anything you did not run is labelled untested.
- `docs/roadmap.md` says what is now done and what you found on the way. When a document and reality disagree, the document is corrected in the same change.
