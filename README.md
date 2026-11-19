# Cycles Render for Cinema 4D

A render engine plugin for Cinema 4D 2024, 2025, 2026 and 2027 that renders the Cinema 4D scene with Blender's **Cycles** and **Eevee**. It drives an unmodified Blender (5.0 to 5.3) in the background: the plugin reads the scene, a small bridge inside Blender rebuilds it, and the picture comes back into the Picture Viewer or into a live render window.

![The example scene rendered with Cycles Render](examples/cycles_render_examples.png)

> **Status: pre-release (current: 0.1.0).** Final frames and the live render window work end to end on Windows. Some features are approximations and a few are missing; see [docs/roadmap.md](docs/roadmap.md).
>
> **Windows only.** The plugin has not been built or tested for macOS.

## What it does

- **A renderer in the Render Settings.** Choose "Cycles Render", pick Cycles or Eevee, and render stills and animations to the Picture Viewer, with render passes as multi-pass layers (also motion vectors, object and material index, shadow catcher), motion blur, the render region and denoising. An Output tab sets the resolution, its percentage and the pixel aspect as Blender's Output tab does.
- **A live render window (IPR).** It follows every change of the scene and sends only what changed; meshes that did not change are not even exported again. It follows the timeline while it plays, and sends an object or camera moved with a tool to Blender while the drag goes on (a switch in the Preferences turns this off). In its picture, Cinema 4D's navigation keys (Alt and the mouse, or 1, 2, 3 held) orbit, pan and dolly the camera it renders through, and the picture follows during the drag. Its "Add" menu adds a Cycles Material, a camera with a Cycles Camera tag, a Cycles Light, and the plugin's tags. On a GPU a camera move reaches the screen in about 20 to 25 ms and an object edit in 20 to 70 ms (640 × 360; the window shows at most about 30 pictures a second, which keeps Cinema 4D's interface free while you drag); in a scene of 900 meshes and 720,000 polygons the export after moving one object takes about 10 ms.
- **Your Cinema 4D scene as it is.** Meshes with normals, UV sets, vertex maps and vertex colours; materials per polygon selection; instances, Cloners and their clone colours; deformers and animation; cameras in every projection, also the editor's views and the spherical camera; standard and Redshift lights with their include and exclude lists; a Sky object or a Redshift dome light; volumes (Volume Builder, Volume Loader); hair; particles.
- **Materials three ways.**
  - Cinema 4D's classic materials and shaders are translated to Blender nodes, or baked to textures where Blender has no counterpart.
  - Redshift Standard Materials are read option by option, with the textures, math, colour mix and colour correction nodes wired into them.
  - **Cycles nodes**: a node space in Cinema 4D's Node Editor with one node per Blender shader node, carried to Blender one to one.
- **A Cycles Object tag** for what Cinema 4D has no place for: ray visibility with holdout and shadow catcher, light linking, instances on particles and Matrix objects, and volume shading.
- **Blender's lights, two ways** (two variants to compare): a **Cycles Light** object with Blender's point, sun, spot and area light and all their settings, and a **Cycles Light tag** that gives Cinema 4D's and Redshift's lights Blender's settings. Cinema 4D's lights cast shadows in Cycles and Eevee whatever their Shadow setting; the tag's Cast Shadow switches them off.
- **Blender's render devices in the Preferences:** OptiX, CUDA, HIP, oneAPI or Metal, and which devices the GPU setting uses, as in Blender's own Preferences; and the engine, Cycles or Eevee, that new render settings start with.
- **A Cycles Camera tag** with Blender's depth of field (focus object, f-stop, aperture blades, rotation and ratio) and lens: perspective, orthographic or panoramic (equirectangular, fisheye and the other panoramas of Cycles).

| Live render window | Render Settings | Cycles nodes |
|---|---|---|
| ![The IPR window](docs/images/ipr_window.png) | ![The Render Settings page](docs/images/render_settings.png) | ![Cycles nodes in the Node Editor](docs/images/cycles_nodes.png) |

## Requirements

- Windows 10 or 11, 64-bit. (macOS is not supported yet; the platform code is isolated for a port.)
- Cinema 4D 2024, 2025, 2026 or 2027, kept at the latest update of its year (the plugin is built with 2024.5, 2025.3, 2026.3 and 2027.0). One package serves all four.
- Blender 5.0, 5.1, 5.2 or 5.3, installed normally (5.2 is the long-term support release). It is free: <https://www.blender.org/download/>. A shader node that only a newer Blender has is marked in the Node Editor and left out of the render, with a line in the console.
- A graphics card that Cycles supports is recommended; rendering on the CPU works.

## Install

A release consists of two parts, one for each application. Take both from the same release.

1. **The Blender bridge**: `cycles_render_bridge-<version>.zip`. In Blender, open *Edit > Preferences > Get Extensions*, choose *Install from Disk…* from the menu at the top right, and pick the .zip. Close Blender again; it does not need to be running.
2. **The Cinema 4D plugin**: `CyclesRender-<version>-C4D2024-2027-win64.zip`. Unpack it into a plugins folder of Cinema 4D, for example the `plugins` folder inside the folder that *Edit > Preferences > Open Preferences Folder…* opens, so that it contains `plugins/cyclesrender/` with `res` and the four files `cyclesrender_2024.xdl64` to `cyclesrender_2027.xdl64`. Restart Cinema 4D. The folder holds one build per Cinema 4D version and each Cinema 4D loads its own: keep all four, and use the same folder for every Cinema 4D you have. (Released before 0.2.0: for Cinema 4D 2027 only.)

On its first start the plugin looks for a supported Blender in its usual place and remembers it: of several, the one the bridge is installed into, else Blender 5.2 (the long-term support release), else the newest. If yours is somewhere else, choose `blender.exe` in *Edit > Preferences > Renderer > Cycles Render*. Until a Blender is chosen or found, the console says so at every start, and the IPR window, a final render and the material previews say why nothing renders (none selected, the file is gone, or a version that is not supported) and where to choose one; a click on the IPR window's message opens that page.

**Which device renders.** The Render Settings' **Device** decides: *Automatic* (what new render settings start with since 0.2.0) renders on the GPU when Blender finds one and on the CPU otherwise, *GPU Compute* on the GPU, *CPU Compute* on the CPU. The page's **Cycles Render Devices** say which GPU type (OptiX, CUDA, …) and which devices *Automatic* and *GPU Compute* use; they do nothing while the Render Settings say *CPU Compute*, and the console says so then. Scenes keep the device they were saved with: a scene of 0.1.0 renders on the CPU unless its Render Settings say otherwise. The IPR window's status line names what really renders, as Blender reports it ("Cycles on OptiX: …", "Cycles on CPU, 30 threads (Render Settings)").

On the same page, **Default Engine** (Cycles or Eevee) is the engine Cycles Render starts with when you choose it as the renderer of render settings that did not have it, and the engine the IPR renders a scene with that has no Cycles Render settings yet. Scenes keep the engine they were saved with, and so does a new scene made from your own template scene.

The page's **IPR** group has four switches, all on by default, and one number; they take effect in a running IPR at once:

- **Live Updates During Drags and Playback**: the IPR follows a change while you make it: an object or camera dragged with a tool, a light's value dragged in the Attribute Manager, and each frame while the timeline plays. Off, Blender gets the change once it is finished (the mouse released, playback stopped), which keeps Cinema 4D's interface free while you drag in a heavy scene. Navigating in the IPR's own picture still follows the drag.
- **Optimized Live Updates** (greyed out while the switch above is off): while you drag, only what changes is sent to Blender, and the whole scene is exported once when you let go. A camera you move or navigate is sent alone; so are an object you move, turn or scale with a tool or in the Coordinates tab (with the objects below it), a light's values, and the values you drag in the Attribute Manager of a camera (field of view, focal length, sensor, film offset, focus, clipping, the Cycles Camera tag's), of a primitive or generator (a Cube's size and segments, an Extrude's or Sweep's values and those of the splines below them, a Subdivision Surface's level, a Cloner's count) and of a deformer (a Bend's strength and angle, Twist, Bulge, Taper, an FFD): the meshes of the objects concerned are sent alone, with what shows them (a Cloner, instances). A heavy mesh follows at the pace its export allows (steps in between are left out). A material, a tag's value, the sky, volumes, hair and particles arrive when you let go. Off, the whole scene is exported at every step of a tool's drag, as in the first 0.2.0 test release, and of a drag in the Attribute Manager only lights follow. Playback exports each frame whole either way.
- **CPU Threads Left Free** (by default a quarter of the computer's logical processors, at least 2: 8 of 32, 2 of 8): how many of the computer's logical processors a Cycles render on the CPU in the IPR leaves to Cinema 4D, so that its interface and viewport keep cores of their own. The IPR on the CPU is that much slower; 0 lets it use them all. Final renders are not affected (the Render Settings' threads decide there). Changing it restarts a running render once.
- **Skip Unchanged Meshes**: a mesh that did not change is not exported again after an edit. Off is the slower way of earlier versions, for hunting a fault.
- **Navigate in the IPR Window**: Cinema 4D's navigation keys in the IPR's picture move the camera (see above). Off, every drag pans the picture.

**Updating:** in Blender, unfold *Cycles Render Bridge* under *Edit > Preferences > Add-ons* and press *Uninstall*, then install the new .zip; in Cinema 4D, delete the `cyclesrender` folder and unpack the new one (do not unpack over the old one: a build left from 0.1.0 would compete with the new one). Take both parts from the same release.

## Quick start

1. Open a scene, or `examples/cycles_render_examples.c4d` from this repository.
2. In the Render Settings choose the renderer **Cycles Render**.
3. Render to the Picture Viewer, or open the live render window with *Extensions > Cycles Render IPR* and press Start.

New node materials are made in the "Cycles" node space while Cycles Render is the renderer. Their thumbnails are rendered by Blender. Anything in the scene that cannot be translated is named in Cinema 4D's console instead of being dropped silently.

## How it works

```
Cinema 4D process                                Blender process
┌──────────────────────────────────────────┐     ┌──────────────────────────────────────────┐
│ renderer (VideoPost) ────────────────┐   │     │ bridge (Python)                          │
│ IPR window ─ controller ─ worker ────┼───┼─TCP─┼─ protocol → scene builders (bpy)         │
│ scene exporter + delta sync              │     │ viewport driver | final driver           │
│ material translators → node graphs       │◄─SHM┼─ frame block (float pixels) + progress   │
└──────────────────────────────────────────┘     └──────────────────────────────────────────┘
```

The plugin (C++) knows Cinema 4D and translates everything into Blender's terms. The bridge (Python, inside Blender) applies what it is sent and runs the render; it knows nothing about Cinema 4D. They talk over a local socket; live frames come back through shared memory. The two are separate programs, which is also what keeps the GPL-licensed bridge apart from the plugin. [docs/architecture.md](docs/architecture.md) has the whole design and the reason for each decision.

## Repository layout

| Folder | What it holds |
|---|---|
| [`plugin/`](plugin/) | The Cinema 4D plugin (C++): `plugin/cyclesrender`, a module of the Cinema 4D SDK |
| [`bridge/`](bridge/) | The Blender bridge (Python): a Blender extension that contains the package `c4d_bridge` |
| [`protocol/`](protocol/protocol.md) | The wire protocol between the two |
| [`tools/`](tools/) | Build script, release packaging, and the generators for the settings page, the nodes and the icons |
| [`docs/`](docs/) | Architecture, development guide, roadmap, and reference notes on Blender and the Cinema 4D SDK |
| [`examples/`](examples/) | An example scene that shows the supported cases |
| [`release/`](release/) | Where release packages are built (only its README is in the repository) |

## Building from source

You need Visual Studio with the C++ workload, CMake 3.30 or newer, the Cinema 4D C++ SDK of each version you build for (2024 to 2027; building for 2024 also needs the 2025 SDK), and Blender 5.0 to 5.3 (all four to regenerate the Cycles nodes).

```powershell
.\tools\build_plugin.ps1 -Sdk <folder of the unpacked Cinema 4D 2027 SDK>   # -Version 2024|2025|2026|all for the others
& "C:\Program Files\Maxon Cinema 4D 2027\Cinema 4D.exe" `
    "g_additionalModulePath=<repo>\build\plugin\bin\Release\plugins" `
    "g_cyclesRenderBridge=<repo>\bridge\c4d_bridge\main.py"
```

The second command starts Cinema 4D with the freshly built plugin and the bridge straight from the repository, with nothing installed. [docs/development.md](docs/development.md) covers the set-up, the tests, the development settings and how to add a feature. Contributors, human or AI, should start with [AGENTS.md](AGENTS.md).

## Licence

- `bridge/` is licensed under the **GNU General Public License v3.0 or later** ([bridge/LICENSE](bridge/LICENSE)), because it runs inside Blender and uses its Python API.
- Everything else (`plugin/`, `protocol/`, `tools/`, `docs/`, `examples/`) is licensed under the **Apache License 2.0** ([LICENSE](LICENSE)). The exception is the two scripts in `tools/` that run inside Blender (`dump_blender_nodes.py`, `make_native_placeholder.py`), which are GPL-3.0-or-later like the bridge.

Every source file names its licence in its first lines (`SPDX-License-Identifier`). The plugin and the bridge are separate programs that only exchange data.

## Acknowledgements

Cycles and Eevee are render engines of [Blender](https://www.blender.org), developed by the Blender Foundation and its community. Cinema 4D and Redshift are products of Maxon. This project is independent of both and is not endorsed by either.

Support Blender: [fund.blender.org](https://fund.blender.org/)
