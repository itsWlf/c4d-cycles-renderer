# The Cinema 4D plugin

`cyclesrender/` is a module of the Cinema 4D C++ SDK: the render engine "Cycles Render" with its Render Settings page, the IPR window, the Cycles Object tag, the Cycles node space and the Preferences page. (The Cycles Native Material is in the code but switched off in this release.)

It contains everything that knows Cinema 4D. It reads the scene, translates it into Blender's terms and sends it to the bridge ([`../bridge`](../bridge)), which runs inside a Blender that this plugin starts. It never links against Blender or the bridge: the two exchange data over a local socket and shared memory ([`../protocol/protocol.md`](../protocol/protocol.md)).

```
cyclesrender/
  project/CMakeLists.txt   module settings for the SDK's build system
  source/                  the code; main.cpp registers every hook
  res/                     descriptions, strings, icons and the node database Cinema 4D loads at run time
```

- **Build:** `..\tools\build_plugin.ps1` from the repository root; see [`../docs/development.md`](../docs/development.md).
- **Find your way around:** every header starts with what its unit is for, and [`../docs/architecture.md`](../docs/architecture.md) has a map of all source files.
- **Facts about the SDK** that this code relies on, with how each was established: [`../docs/reference/c4d-sdk.md`](../docs/reference/c4d-sdk.md).
- **Generated files** (never edit by hand): `res/description/vpcyclesrender.*`, `res/strings_en-US/description/vpcyclesrender.str`, `source/videopost/render_settings_table.h`, `source/nodespace/cycles_nodes_table.h`, `res/nodes/*.json`, `res/icons/*.png`, `res/native_default.preview`.

The plugin ids in `source/ids.h` and the module id in `project/CMakeLists.txt` are issued for this project. A fork published under another name needs its own.
