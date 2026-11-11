# The Blender bridge

The part of Cycles Render that runs inside Blender. The Cinema 4D plugin ([`../plugin`](../plugin)) starts a Blender of its own with this bridge; the bridge rebuilds the scene it is sent, renders it with Cycles or Eevee, and gives the picture back.

The bridge knows nothing about Cinema 4D. It receives scene items in Blender's own terms (coordinates, node type names, property paths) and copies them into Blender's data. What it understands is defined in [`../protocol/protocol.md`](../protocol/protocol.md).

## Installing it

This folder is a Blender extension. Users install the packaged `cycles_render_bridge-<version>.zip` in Blender 5.2 with *Edit > Preferences > Get Extensions > Install from Disk…*, and that is all: the Cinema 4D plugin finds the bridge in Blender's extensions folder and starts Blender with it when it renders. Blender runs with factory settings then, so the extension does not need to be enabled, and nothing of the user's own Blender set-up takes part.

For development nothing is installed: the plugin is pointed at this folder with the setting `g_cyclesRenderBridge=<repo>\bridge\c4d_bridge\main.py` (see [`../docs/development.md`](../docs/development.md)).

## What is in it

```
blender_manifest.toml   what Blender needs to install the extension
__init__.py             the add-on Blender sees: it explains itself in the Preferences and has an Uninstall button
LICENSE                 GNU General Public License, version 3
c4d_bridge/             the bridge
  main.py               the script Blender is started with; checks the Blender version
  server.py             socket thread, queue, request handlers, both run loops
  protocol.py           frame encoding and decoding (no bpy, so any Python can use it)
  errors.py             RequestError, raised by a handler to answer with an error reply
  scene/                builders for scene items, one module per kind
  render/               render settings and devices
  final/                the final driver: one blocking render per frame, in a background Blender
  viewport/             the viewport driver: the live render for the IPR, in a windowed Blender
```

Two modes, chosen by how Blender was started:

- **Background** (`blender -b`): the final driver. The script serves requests in a blocking loop and Blender exits with the bridge's exit code.
- **Windowed**: the viewport driver. The script installs itself on Blender's own loop, hides the window, and renders in a real Rendered viewport whose pixels it copies into shared memory after every redraw.

```
blender -b --factory-startup --python c4d_bridge/main.py -- --handshake <file> --token <text>
blender    --factory-startup --python c4d_bridge/main.py -- --handshake <file> --token <text> [--show-window]
```

## Working on it

- Read [`../docs/reference/blender.md`](../docs/reference/blender.md) first: the Blender behaviour the bridge depends on, with how each fact was established. Several mechanisms use internals that change between Blender versions, which is why the bridge runs on listed versions only (`SUPPORTED_BLENDER` in `c4d_bridge/__init__.py`).
- A new kind of scene item is one builder function in `c4d_bridge/scene/` and an entry in `BUILDERS`. A new operation is a handler in `server.py`. Describe both in the protocol document in the same change.
- All `bpy` work happens on Blender's main thread; the socket thread only moves bytes.
- Dependencies: `bpy`, `numpy` and the standard library, all of which Blender brings.

## Licence

GPL-3.0-or-later ([LICENSE](LICENSE)), as the bridge uses Blender's Python API. Every file carries the SPDX identifier.
