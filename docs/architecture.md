# Architecture

How Cycles Render is built, and the reason behind each decision. Read this before changing how the parts work together. Exact identifiers and measured facts are in [reference/blender.md](reference/blender.md) and [reference/c4d-sdk.md](reference/c4d-sdk.md); the message format is in [../protocol/protocol.md](../protocol/protocol.md); what works and what is open is in [roadmap.md](roadmap.md).

## Contents

- [The product](#the-product)
- [Vocabulary](#vocabulary)
- [The two programs](#the-two-programs)
- [Source map](#source-map)
- [Design decisions](#design-decisions)
- [Final frames](#final-frames)
- [The IPR](#the-ipr)
- [The IPR window](#the-ipr-window)
- [Reading the scene](#reading-the-scene)
- [What becomes what](#what-becomes-what)
- [Materials](#materials)
- [The Render Settings page](#the-render-settings-page)
- [Risks](#risks)

## The product

A render engine plugin for Cinema 4D 2024, 2025, 2026 and 2027 that renders the Cinema 4D scene with Blender's Cycles and Eevee engines by driving a Blender process (5.0 to 5.3). It provides:

- a renderer that can be chosen in the Render Settings, with its own settings page;
- rendering to the Picture Viewer, including animations and render passes;
- a live render (IPR) window;
- support for the scene as Cinema 4D users build it: meshes and their attributes, materials and shaders, cameras, lights, volumes, MoGraph, deformers and animation.

The first priority of the design is that **the IPR reacts as fast as possible**. Where that conflicted with keeping Blender invisible and headless, responsiveness won.

## Vocabulary

| Term | Meaning |
|---|---|
| Plugin | The C++ module loaded by Cinema 4D (`plugin/cyclesrender`) |
| Bridge | The Python package that runs inside Blender (`bridge/c4d_bridge`) |
| IPR | The interactive render window inside Cinema 4D |
| Viewport driver | The bridge's mode for the IPR: a windowed Blender whose 3D viewport renders live |
| Final driver | The bridge's mode for Picture Viewer renders: a background Blender that does one blocking render per frame |
| Scene item | One thing the plugin sends: a mesh, an object, a light, a material, … Each has a kind and an id |
| Delta | The items that changed since the last request, which is all that is sent |
| SyncState | The plugin's record of what Blender currently holds, used to compute deltas |
| Frame block | The named shared-memory block that carries float pixels from Blender to Cinema 4D |

## The two programs

```
Cinema 4D process                                Blender process
┌──────────────────────────────────────────┐     ┌──────────────────────────────────────────┐
│ VideoPost renderer ──────────────────┐   │     │ bridge (Python)                          │
│ IPR window ─ IprController ─ worker ─┼───┼─TCP─┼─ protocol → scene builders (bpy)         │
│ SceneExporter + SyncState (deltas)       │     │ viewport driver | final driver           │
│ Material translators → node graphs       │◄─SHM┼─ frame block (float pixels) + progress   │
└──────────────────────────────────────────┘     └──────────────────────────────────────────┘
```

- The **plugin** reads the scene and translates it into Blender's terms: coordinates, units, shader node graphs and Blender property paths. All knowledge about Cinema 4D lives here.
- The **bridge** applies what it is sent and runs the render. It has two drivers that share the same scene builders, and it knows nothing about Cinema 4D.
- **Control channel:** length-prefixed frames over loopback TCP. Each frame is a small JSON header plus raw little-endian blobs for bulk arrays. The bridge binds a port chosen by the operating system and writes it into a handshake file; the plugin connects as the client and authenticates with a secret made for that launch. Requests are strictly request and reply.
- **Pixel channel:** the frame block. Blender's GPU reads the viewport into shared memory; a small state block next to the pixels carries frame id, size and render progress, and a named event wakes the plugin's frame thread after every frame. Nothing about a live frame travels over the socket.
- **One exporter, two consumers:** the IPR and the Picture Viewer render both use the same `SceneExporter` and `SceneWriter`.

How Blender is started and how the plugin finds the bridge: the bridge is a Blender extension the user installs in Blender. The plugin starts Blender with the bridge's `main.py` as start-up script (`session/blender_process.cpp`) and finds that script in Blender's folder for user extensions (`FindBridgeScript` in `session/blender_install.cpp`). Blender runs with `--factory-startup`, so nothing of the user's Blender set-up takes part, and the extension does not even need to be enabled.

## Source map

Files marked *generated* are written by a tool in `tools/` and never edited by hand.

```
plugin/cyclesrender/            the Cinema 4D module
  project/CMakeLists.txt        module settings: the SDK frameworks used, the module id (the 2024 to 2026
                                SDKs get a projectdefinition.txt written from it by tools/build_plugin.ps1)
  source/
    main.cpp                    module entry points; registers every hook
    ids.h                       plugin ids
    version.h                   the plugin's version number
    sdk_compat.h                what differs between the SDKs of Cinema 4D 2024 to 2027, in one place
    videopost/
      cycles_videopost          the renderer: one Blender per render job; per frame, exports the scene and
                                writes the pixels and passes
      render_settings_map       sends the render settings; hides those of the other engine and of the
                                Advanced layout
      render_settings_table.h   generated: every setting with the Blender property it sets
      output_settings           the Output tab (resolution, %, pixel aspect in Blender's terms), which
                                writes Cinema 4D's own Output values; the pixel aspect every render uses
    scene/
      scene_exporter            walks the document, decides what renders, writes all scene items
      scene_writer              one `scene` request under construction: items, blobs, warnings; reduces it
                                to what changed (SyncState)
      coords                    Cinema 4D's axes and units to Blender's
      document_colors           the document's colour management: how colours are read, which colour space
                                an image is loaded in, whether the picture gets the sRGB curve
      parameters.h              reading parameters with GetParameter (always use these helpers)
      mesh_export, light_export, environment_export
                                one kind of item each
      camera_export             the camera: Cinema 4D's projections (the fixed views with their own
                                axes, oblique ones slanted), its spherical camera, the Cycles Camera tag,
                                clipping; or the free view of the IPR; and the way back, the ray through
                                a point of the picture
      volume_export             Volume Loader, Volume Builder and Pyro grids as `volume` items
      curves_export             a Hair object as `curves`, a particle group as `pointcloud`
      scene_dump                development aid: a listing of what would be exported
    materials/
      material_export           which translator a material goes to; the classic material as a Principled
                                BSDF graph; image items; what a baked shader's id depends on
      baked_files               the files of baked shaders: reused within a session, owned by documents,
                                deleted when the documents are closed and no Blender reads them; its
                                message plugin also calls ipr_add's FollowAddedCameras
      shader_translate          a Cinema 4D shader as Blender nodes, or baked to a texture
      node_graph                a Blender node graph under construction
      redshift_material         the Redshift Standard Material, option by option
      redshift_nodes            the Redshift nodes wired into its inputs: the Texture node, math, vector
                                math, colour mix, invert, colour correction, makers and splitters
      redshift_lookalikes       look-alikes of Jitter, Random Color Switch, Brick and Flakes: the same kind
                                of picture as Redshift's without its own random numbers and noises
      exchange_material         other node materials through Cinema 4D's material exchange (approximation)
      cycles_node_material      a material of Cycles nodes, written node for node
      cycles_curves             the curves of RGB Curves, Vector Curves and Float Curve: a Cinema 4D spline
                                sampled into the points of Blender's curve
      cycles_material           the command "Cycles Material"
      material_preview          the thumbnail of a material of Cycles nodes: a ball rendered by a Blender of its own
      native_material           the Cycles Native Material: a Blender material kept as bytes, its command,
                                the Blender it is edited in, the timer that takes in the edits
    nodespace/
      cycles_nodespace          the "Cycles" node space: registers the nodes, their description database
                                and their folders in the Asset Browser; the previews, among them the
                                version card of a node the chosen Blender does not have
      cycles_nodes              Blender's shader nodes as a table, with the Blender version each node,
                                socket, setting and option exists from; which sockets show for which settings
      cycles_nodes_table.h      generated: that table
      path_commands             the image nodes' buttons "Make Relative" and "Make Absolute": commands of
                                the maxon command system
    tags/object_tag             the Cycles Object tag
    tags/camera_tag             the Cycles Camera tag: depth of field and lens, Blender's settings
    lights/
      light_object              the Cycles Light object: Blender's light types and settings, drawn in the
                                viewport
      light_tag                 the Cycles Light tag on Cinema 4D's and Redshift's lights: Blender's light
                                settings in place of the light's own, and Blender's settings they lack
      light_settings            what the two share: defaults, which settings show, reading them
    prefs/                      the Preferences page, and the settings stored in Cinema 4D's preferences:
                                which Blender and its version, and why none can be started (settings),
                                the engine new render settings start with (settings), Blender's Cycles
                                render devices (devices)
    session/
      blender_install           finds Blender and the bridge installed in it; reads Blender's version;
                                the supported versions
      blender_process           starts Blender with the bridge, waits for the handshake, ends it
      bridge_client             connect, hello, synchronous request and reply with cancel, shutdown
      protocol                  frame encoding and decoding
      json, json_writer         parsing replies; writing request headers
      frame_reader              reads frames and render progress from the frame block
      frame_watcher             a thread woken by Blender per frame; hands the frame to the window (a core
                                message, and a callback for modal loops)
      capture, selftest         development aids: a record of every message; a self-test of this layer
    ipr/
      ipr_controller            the IPR: state machine, what to send after a change, camera, lights alone,
                                passes, engine, latency measurement
      ipr_worker                a thread that owns one windowed Blender and sends every request
      ipr_dialog                the IPR window and the command that opens it
      ipr_scenehook             the IPR in Cinema 4D's modal loops: a scene hook called in every evaluation,
                                and the frame thread's way to the window, both through the main thread's queue
      ipr_add                   what the window's "Add" menu adds to the document; the render view's
                                camera through undo and redo of its camera entry
      ipr_navigation            navigating the scene from the picture: the key scheme, Cinema 4D's navigation
                                preferences, one drag's orbit, pan or dolly of the IPR's camera with its undo
      ipr_image_area            the picture: view transform (DisplayTransform), zoom, pan, the drag loop that
                                navigates; the float bitmap for snapshots and saving
      ipr_testhook              lets test scripts drive the IPR (and set curves of Cycles nodes, which
                                Python cannot make)
      ipr_trace                 a timeline of what the IPR does, from every thread, for measurements
                                and tests (off unless the test hook switches it on)
    platform/                   everything specific to the operating system (Windows only so far):
                                sockets, shared memory and events, process statistics, environment,
                                a lock file
  res/
    description/                vpcyclesrender (the Render Settings page, generated), prefscyclesrender,
                                tcyclesobject, tcyclescamera, tcycleslight, ocycleslight, mcyclesnative
    nodes/                      generated: the description database of the Cycles nodes
    icons/                      drawn by tools/make_icons.py
    native_default.preview      generated: the picture of a native material that was never edited

bridge/                         the Blender extension
  blender_manifest.toml         what Blender needs to install it
  __init__.py                   the add-on Blender sees; it only explains itself
  c4d_bridge/                   the bridge itself
    main.py                     the script Blender is started with; checks the Blender version
    server.py                   socket thread, queue, request handlers, both run loops
    protocol.py                 frame encoding and decoding (no bpy, so tests can use it)
    errors.py                   RequestError, raised by handlers to answer with an error reply
    scene/                      builders for scene items, one module per kind; native.py holds native
                                materials and their editor window; cameras.py hangs an oblique camera
                                from an empty to keep its slant
    render/                     settings (engine, device, colour space, properties) and devices
    final/                      the final driver; preview.py renders material thumbnails
    viewport/                   the viewport driver: the Viewport class, the frame block, window control

protocol/protocol.md            the wire protocol
docs/reference/cycles-nodes.md  generated: Blender's shader nodes with the version each exists from
tools/                          build_plugin.ps1, make_release.ps1, run_tests.ps1 and the generators
```

## Design decisions

Each decision is stated with its reason, so that it can be revisited when the reason no longer holds.

1. **The plugin is C++.** Cinema 4D's Python API has no `VideoPostData`, so a renderer cannot be registered from Python.

2. **Blender runs as a separate, stock process.** This gives both Cycles and Eevee with Blender's own scene handling, needs no custom Blender build, and keeps the GPL code (the bridge) in a different program from the plugin.

3. **The IPR uses a windowed Blender and its real viewport renderer.** In background mode Blender has no event loop: timers, modal operators and viewport rendering do not run, and a render is a blocking call that yields pixels only when a frame is finished. A windowed Blender in Rendered viewport shading restarts at once on edits, refines continuously, and has pause, viewport denoising and adaptive sampling built in. The window is hidden, never minimised (Blender does not draw minimised windows). *Rejected:* approximating progressive rendering with repeated short final renders in a headless Blender. It updates in visible steps.

4. **Pixels are read from the viewport's framebuffer inside draw handlers.** The viewport's render texture is half-float, scene-linear, and untouched by view transform or dither. A `PRE_VIEW` handler keeps the framebuffer and a `POST_VIEW` handler reads it into the frame block. This works for both engines. Cycles' `view_draw` method is wrapped in addition, because that is the only place where the engine object, and with it the status text, can be reached.

5. **Final frames run in a separate background Blender.** They need no viewport, must work on machines without a desktop session, and must not compete with a running IPR.

6. **Translation happens in the plugin; the bridge stays generic.** Materials arrive as node graphs in Blender's own node names, settings as pairs of property path and value. The bridge therefore needs no knowledge of Cinema 4D, which keeps the GPL side small and lets the protocol be tested without Cinema 4D.

7. **Scene updates are deltas keyed by stable identity, found by fingerprint.** Sending a whole scene on each edit would ruin the IPR's latency. Identity comes from Cinema 4D's object GUIDs. After a change the whole scene is exported into memory, every item is hashed (its text and its blobs), and only items whose hash differs from what Blender holds are sent. This is right by construction, with no bookkeeping of what Cinema 4D may have touched, and costs a few milliseconds for small scenes. Its price was exporting unchanged meshes (74 ms after moving one object in a scene of 900 meshes); since 0.2.0 the IPR skips that one part, and only that: the walk, the objects, materials, lights and world are still exported and compared every time, so everything that decides *where* and *how* a mesh is shown stays right by construction. A mesh is left as Blender holds it when a signature of what it is made from equals the one recorded when Blender got it (`SyncState` keeps both; `SceneWriter::KeepMesh` puts the kept items into the request by their fingerprints, without content). The signature is the full data of the mesh, hashed, except for a polygon object of the document without animation and with only plain data tags, where its identity is enough (address, GUID, the dirty counts of object and tags), because every edit measured changed it. *Rejected:* trusting dirty counters for generated meshes (an instance's copy changed its content at the same address with the same counts after a redo), and skipping whole objects (material ids follow the order of the export, which would shift). A switch in the Preferences turns it off (SDK reference, "Skipping unchanged meshes in the IPR").

8. **Blender calculates with the numbers Cinema 4D calculates with.** With OCIO the plugin sends the document's render space (ACEScg by default) and Blender switches to it, so colour values pass through unchanged and sRGB textures are converted exactly as Cinema 4D converts them. Pixels come back scene-linear and Cinema 4D applies its own view transform, for the IPR and final frames alike. This keeps the IPR, the Picture Viewer and saved files consistent. With "Basic" colour management (the render space "Legacy (sRGB linear workflow)") Blender works in linear Rec.709 and the plugin converts what Cinema 4D would convert, in one place (`scene/document_colors`): colours typed into Cinema 4D are linearised with Linear Workflow on, images get the colour space that gives Cinema 4D's values, and the picture gets the sRGB curve where Cinema 4D's own picture has it. With Linear Workflow off Cinema 4D calculates on the stored sRGB numbers themselves; Blender is given those numbers as if they were linear and they go back unchanged, which is exactly Cinema 4D's arithmetic. *Rejected:* letting Blender convert at the edges in that mode (linear inside, curve at the end): light added on sRGB numbers is not the same as light added linearly.

9. **One coordinate module.** Cinema 4D is left-handed, Y up, in centimetres; Blender is right-handed, Z up, in metres; cameras and lights also look along different local axes. All conversion lives in `scene/coords`.

10. **Materials in tiers.** Classic materials and shaders are translated into nodes, and what has no counterpart is baked. Redshift Standard Materials are read from their own graph; other node materials go through Cinema 4D's neutral material exchange. The Cycles node space and the Cycles Native Material give direct access to Blender's shading.

11. **Windows first.** Everything specific to the operating system is isolated under `platform/`, behind headers that use plain C++ types only, so that macOS can follow.

12. **The bridge supports specific Blender versions only.** It wraps Cycles' Python class and reads an internal struct, so it checks the Blender version at start and refuses unknown ones. The list is `SUPPORTED_BLENDER` in `bridge/c4d_bridge/__init__.py`; the plugin applies the same rule (`SUPPORTED_VERSIONS` in `session/blender_install.cpp`) before it ever starts Blender: every start asks `GetBlenderProblem()` (`prefs/settings.h`) first, which names the case (none selected or found, the file gone, a version not supported) and where to choose another, and the IPR, final frames and material previews show that text (0.2.0, B7). Since 0.2.0 that is 5.0 to 5.3, side by side: one bridge and one plugin for all of them. Differences of Blender's Python API are handled in the bridge by testing for the feature; what the user should know (a node, setting or light behaviour an older Blender lacks) is decided in the plugin, which knows the version from the bridge's `hello` and, before Blender runs, from the executable chosen in the Preferences. *Rejected:* one package per Blender version (the user would have to match them), and a node space per version (a scene would change its nodes with the Blender chosen). While the user has chosen none in the Preferences, detection picks of several installed versions the one the bridge is installed into, else the preferred version (5.2, the long-term support release, while 5.3 has open bugs: `PREFERRED_MAJOR`/`PREFERRED_MINOR` in `session/blender_install.cpp`), else the newest.

13. **Plain blocking sockets, not the SDK's network API.** The SDK's TCP interface is asynchronous and callback-driven; a request and reply connection on a thread of our own needs none of that. The few socket calls live in `platform/socket_win.cpp`, which includes no Cinema 4D headers because the Windows headers clash with them.

14. **The bridge is installed in Blender, the plugin in Cinema 4D.** A release is two packages, one per application, and the plugin finds the bridge in Blender's extensions folder. During development the bridge runs straight from the repository, named by the setting `g_cyclesRenderBridge`.

15. **Scene features are verified against Cinema 4D's Standard renderer.** "What Cinema 4D renders" is the specification. A test builds a small scene, renders it with both renderers and compares the pictures; this has corrected several assumptions (which instances render, how bump is scaled, which way a sky panorama faces). Conversions with a free constant (lights, bump) are calibrated by measurement, and the measurement is recorded in the SDK reference. Where the Standard renderer has no counterpart (bounced light, depth of field), the feature is tested on its own.

16. **One render job, one Blender.** The renderer starts Blender for a job's first frame and shuts it down when the frame sequence closes, so a sequence pays the start once and sends unchanged data once.

17. **The user is told what was not translated.** Anything in the scene the exporter cannot translate is named in Cinema 4D's console, once per render job, instead of being dropped silently (`SceneWriter::Warn`).

18. **Frames come through shared memory with a wake-up signal, not through messages.** A state block in shared memory carries what a "frame ready" message would, the plugin needs no second reader on the socket, and requests stay strictly request and reply. A frame thread in the plugin waits for the signal, copies the frame and notifies the window, so showing a frame waits neither for a timer nor for Blender's replies.

19. **All talking to the IPR's Blender happens on one worker thread.** The main thread only exports (it must: it reads the document) and posts tasks. While tasks are waiting, further changes of the document are not exported; the next export then covers everything, which keeps a slow Blender from building up a queue.

20. **By default the IPR image has the size of the window's picture area ("Fit Window").** Resizing the window resizes the render, and the part of the image outside the camera's picture is darkened like the border of Cinema 4D's views. The camera's picture has the aspect of the Render Settings resolution and is the largest centred rectangle of that aspect in the image. Blender renders the larger image through a wider sensor: sensor width, parallel width and film offsets are scaled by image width over picture width while the focal length stays, so depth of field is unchanged. A window being resized is followed once its size has stood still for 0.12 s, because every new size costs Blender a new frame buffer and a restarted render.

21. **The plugin ends only the Blender processes it started.** It holds their handles and never searches for processes by name, so a Blender the user runs for other work is left alone. Blender is also tied to Cinema 4D by the operating system, so it ends even when Cinema 4D is killed (from Cinema 4D 2025 on; the 2024 SDK lacks the flag, and there Blender ends when its connection to the plugin closes).

22. **The Cycles nodes are built in code and described by a generated database.** One table, asked from Blender itself by `tools/make_nodes.py`, feeds both: the plugin builds each node's ports from it and writes a graph back to Blender with it, and a description database generated from it says how a node looks. A new Blender version is one run of the tool, plus a look at the tables the tool holds by hand because Blender does not give them to Python: the panels of a node (`PANELS`), the material's own settings shown on the Material Output (`MATERIAL_SETTINGS`), the nodes each version brought (`NODES_BY_VERSION`, which the tool insists on) and the nodes left out (`SKIP_NODES`). The tool asks every supported Blender and takes the union; every node, socket, setting and option carries the first version that has it, so one node space serves all versions: the exporter leaves out what the Blender in use lacks and says so, and a node a version lacks shows a card on itself (`docs/reference/c4d-sdk.md`, "A node the chosen Blender does not have").

23. **A Cycles Native Material is a Blender material kept as bytes, edited in a Blender of its own.** The material holds a .blend file with that one material. Nothing is translated, so everything Blender's Shader Editor can do is kept (colour ramps, curves, node groups). The editor runs in a second windowed Blender that the plugin keeps for editing, apart from the IPR's, because with both in one Blender the IPR's picture went dark when the editor opened. Edits come back through files in the preferences folder that a timer reads four times a second, because the protocol has no messages that Blender starts. The IPR and final renders receive the material as bytes like any changed item.

24. **One source tree, one build per Cinema 4D version, all builds in one folder.** Since 0.2.0 the plugin supports Cinema 4D 2024 to 2027. A module carries the ABI version of the SDK it was built with, and 2024, 2025 and 2026/2027 have three different ones, so no one binary loads in all of them; the code is built once per version against that version's SDK (`tools/build_plugin.ps1 -Version`), and every difference between the SDKs is answered in `source/sdk_compat.h` rather than by version tests spread through the code. The release puts the four builds side by side in one folder (`cyclesrender_2024.xdl64` to `cyclesrender_2027.xdl64`, one `res`): Cinema 4D's own module loader refuses a build of another ABI and of several builds with the same module id loads the one that matches it best, without a dialog or a console line (measured in all four versions). *Rejected:* a small loader module that picks the right binary (it would itself need the ABI of every version, and Cinema 4D offers no way for a module to hand it another binary); one package per version (the user would have to match them, and delete files when changing versions). Differences of Cinema 4D's behaviour that the user would see are followed in the plugin where it can (`docs/reference/c4d-sdk.md`, "Cinema 4D versions").

25. **In a modal loop the IPR follows the scene's evaluation, and sends the lights alone.** A drag in the Attribute Manager gives a window neither its timer nor a message until the mouse is released, but the document is evaluated at every step and jobs handed to the main thread's queue run (measured, SDK reference, "Modal loops and the IPR window"). A scene hook is the one hook of the plugin that every evaluation calls, whatever kind of object changed, so it notices the step; it hands the window one call through the main thread's queue, and the window, only while its timer starves, writes every light of the last export again and sends the ones that changed. Only the lights, because writing them needs no walk and no cache (their own values and matrices) and costs hundredths of a millisecond, and because a full export called from inside an evaluation might read caches not built yet (as with F15's rule that only a tool's message says the document is consistent; not tried); their items are written by the same code as in the full export, so the export after the drag finds them unchanged. *Rejected:* the plugin's own light object and tag noticing their changes in `SetDParameter` (it would leave Cinema 4D's and Redshift's lights out), polling the lights on the window's timer (it does not run in the loop), and a full export per step from the hook (caches, and the cost of a large scene at every step). The hook's id is temporary (`ids.h`): a scene hook is stored in every scene saved with it.

26. **Navigating from the IPR's picture moves the camera the IPR renders through, with Cinema 4D's keys, inside the picture's own drag loop.** The picture already had its own pan (drag) and zoom (wheel), like the Picture Viewer; navigating takes what navigates in Cinema 4D's views (Alt with the mouse buttons, the navigation keys held), so nothing that existed is taken away, and the scheme sits in one function. The camera moved is the IPR's: the camera object it renders through (one undo step per drag, as a drag through a camera in Cinema 4D's view; refused for a Protection tag or a locked layer), or the editor camera of the view it renders through (that view's own undo). Cinema 4D's navigation preferences are read at the start of each drag (point of interest, cursor placement, dolly to cursor, reverse orbit, trackball and its Ctrl modifier, the three speeds). The point under the mouse is found by the plugin itself, a ray against the meshes the exporter's walk finds (`SceneExporter::FindNearestHit`): the same meshes Blender renders, no round trip to Blender, under a millisecond for 115,200 polygons. A drag is a modal loop of the picture (`MouseDrag`) in which the window's timer does not run, so each step there does what the timer would: the camera is sent while Blender is idle (`IprController::FollowCamera`, the free view of perspective cameras), the newest frame shown, and Cinema 4D's view that looks through the camera redrawn, for at most half of the time so that a slow view does not hold the IPR back. *Rejected:* asking Blender for the depth under the mouse (a synchronous round trip at every drag start, and Blender's `ray_cast` builds trees of the evaluated meshes), Cinema 4D's viewport picking (it needs a view that shows the same camera at the same framing), taking the wheel for dollying (it zooms the picture), and moving a locked camera's kept values instead of the camera (the lock means the picture stands still).

## Final frames

`CyclesVideoPost` (`videopost/cycles_videopost.cpp`) is what Cinema 4D calls.

- For the first frame of a render job it starts a background Blender and sends the render settings once. For every frame it exports the scene, sends the items that changed since the frame before (everything for the first frame), asks for a render, waits for the pixels while polling for the user's cancel, and writes them into Cinema 4D's buffers. When the frame sequence closes it shuts Blender down.
- **Motion blur:** the renderer evaluates the render document also at the moments the shutter opens and closes. Objects, the camera and deforming meshes carry where they are at those moments. Blender always renders with a shutter of two frames around keys one frame apart; the real shutter time is decided by the moments the plugin reads the scene at.
- **Render passes** travel back with the picture and are written into named post-effect buffers, which Cinema 4D shows as layers and saves with the multi-pass file. Switching a pass on enables Multi-Pass with a "Post Effects" entry, without which Cinema 4D creates no such buffers.
- **The render region** of the Render Settings becomes Blender's render border.
- **Colours:** for an OCIO document the picture is written in the render space (Cinema 4D converts for display and saving), with the view transform applied when the render settings ask for that. For "Basic" colour management it is written with the sRGB curve when Linear Workflow is on and Multi-Pass is off, and as rendered otherwise (with Multi-Pass on Cinema 4D's own picture is linear and the Picture Viewer applies the curve). Passes are always written as rendered.
- Cycles' pixels are read in memory; Eevee renders into a temporary EXR that the bridge reads back.
- Cancelling, or any error, ends that Blender at once; the next render starts a new one.

## The IPR

`IprController` (`ipr/ipr_controller.cpp`) is a state machine driven by the window's buttons and its timer. What each control does in Blender:

| Control | Mechanism |
|---|---|
| Start | Send the scene, switch the viewport's shading to Rendered |
| Stop | Switch shading to Solid (frees the Cycles session and its GPU memory), clear the scene, keep the process for a quick next start |
| Pause, resume | `scene.cycles.preview_pause`: buffers are kept and sampling goes on after resume. For both engines the plugin holds changes of the document while paused and sends them on resume |
| Reload | Forget the SyncState, clear and send the scene again |
| Camera list | `<Render View>`: the camera of the view marked as render view. `<Auto>`: the camera of the active view, following it. Then every camera of the scene |
| Camera lock | The render keeps the view it has. The plugin keeps the camera's values from the moment of locking and goes on sending those, so the framing still follows the window's size |
| Snapshot | Copies the current frame into a float bitmap and shows it in the Picture Viewer |
| Zoom field, frame button | Zoom and pan of the display only; 100 % is one render pixel per screen pixel |
| Pass list | The viewport's `render_pass` property of the active engine. Switching restarts the sampling |
| Engine list | `scene.render.engine`, an override of the render setting for the IPR only |
| Settings button | Opens Cinema 4D's Render Settings |
| Time | Elapsed since the last restart, from the frame block. Stops when the render is done; leaves out time spent paused |
| RAM, VRAM | Measured by the plugin on the Blender process |
| Samples | Cycles: parsed from the engine's status text. Eevee: redraws counted since the last restart |

Rules that keep it responsive:

- Edits are applied on Blender's main loop in one batch, giving one scene evaluation and one restart of the render.
- A 2 ms window timer keeps Blender's loop from idling while the IPR runs (measured: 12 ms median round trip instead of 18 ms). It is removed while the IPR is stopped or paused.
- The frame thread always hands over the newest frame; frames the window did not get to are skipped.
- Camera moves take a fast path that skips the scene walk in Cinema 4D: the camera is compared on every timer tick (15 ms). In Blender they move the viewport's own view where possible instead of a camera object: with 100,000 instances a move of the camera object costs 0.16 s, a move of the view 0.018 s. A camera object is used when the camera needs depth of field, a film offset or a parallel projection.
- **During a drag the window's timer does not run** (measured: not once in an emulated tool drag of 1.2 s), so before 0.2.0 the IPR followed a drag only when it ended. Cinema 4D's tools tell the managers about every step synchronously (`EVMSG_ASYNCEDITORMOVE`, sent with `GeSyncMessage`, as the SDK asks tools to); when such a message arrives while the timer has not run for three of its periods, the window does what the timer would: the change is exported and sent while Blender is idle, and the newest frame is taken. Any other message in such a loop sends the camera and the lights only. Measured in the application with an emulated drag: 39 of 40 steps sent, answered in 1.6 ms, the new frame back 3 ms after the step. Whether the window draws it before the drag ends depends on Cinema 4D's drag loop (in the emulated one nothing is drawn until it ends; with events processed between steps the picture is on screen 8 ms after each step; since F18 a frame also reaches the window through the main thread's queue, which the Attribute Manager's loop runs, a tool's loop untested). Navigating a view sends no known message: it is followed when it ends.
- **A value dragged in the Attribute Manager reaches no window either** (measured 2026-11-16 with mouse messages posted to the Attribute Manager's sliders: a light's intensity, its colour's sliders, a Cycles Light's colour): the drag is a modal loop of 1.3 s in which the window's timer did not run and no core message arrived, while Cinema 4D set the value and evaluated and drew the document at every step. So before 0.2.0 a light followed only when the mouse was released. What runs in that loop is the scene's evaluation and the main thread's job queue: a scene hook (`ipr/ipr_scenehook.cpp`, called at the end of each evaluation, on the main thread or a drawing thread) hands the window one call through that queue, and the window, if its timer starves, sends **the lights alone** (`IprController::FollowLights`: every light of the last export that is an object of the document is written again by `SceneExporter::WriteLightAlone`, byte for byte as the full export writes it, and only those that differ are sent; no walk of the document, no other item). The frame thread reaches the window the same way, so the new picture is drawn during the drag. Lights made by generators, and a light that is gone or new, are left to the full export after the drag. Outside a modal loop the hook does nothing: the change event exports as before (measured: with events processed between the steps, nothing was sent twice).
- **Navigating from the picture** (design decision 26) runs in the picture's own drag loop, a modal loop like the others above (measured: the window's timer not called once in a drag of 1.2 s). Each step moves the camera, sends it while Blender is idle, shows the newest frame and redraws Cinema 4D's view; while the mouse stands still with the button held the loop goes on taking frames. Measured with mouse messages posted to the picture (which, unlike a view, takes them; table below).
- **Playback sends no change event** (measured: none in 3 s of playing; one when it stopped), so before 0.2.0 the IPR stood still while the timeline played. The document's time is compared on every tick (the timer runs during playback): measured, 91 of 91 frames sent in 3 s, a picture on screen 4 ms after each was sent.
- Blender runs at below-normal priority, so that a render on the CPU does not starve Cinema 4D's interface.
- Expect the latency of an edit to grow with the number of instances, not of polygons (measurements in the Blender reference).

Measured end to end, from Cinema 4D's change event to the changed picture in the window (three spheres, 64 samples, one RTX 4090):

| Render size, device | Object moved | Camera moved |
|---|---|---|
| 640 × 360, GPU (OptiX) | 18–73 ms | 12–15 ms |
| 640 × 360, CPU | 80–213 ms | 49–97 ms |
| 1920 × 1080, GPU | 138 ms | 38 ms |
| 1920 × 1080, CPU | 777 ms | not measured |

Where the time goes (640 × 360, GPU, typical): export and compare 3–6 ms; Blender applies the change 1–3 ms; Cycles restarts and renders its first sample 11–60 ms; picture on screen 1–3 ms.

A light's value dragged in the Attribute Manager (2026-11-16; Cinema 4D 2027, Blender 5.2.3, 320 × 180, GPU, a cube on a floor; the drag made of 40 mouse messages posted 25 ms apart to the slider; scratch probes, numbers in `internal/roadmap-0.2.0.md`, F18):

| | Before 0.2.0 (F18) | Now |
|---|---|---|
| Sent during a drag of 1.3 s | nothing; one export when the mouse was released | 39 to 40 of about 40 steps, each the changed light alone |
| Writing and comparing the lights | (a full export: 6–11 ms with 400 generated meshes, `tests/c4d/ipr_lights.py`) | 0.02–0.09 ms for one to five lights |
| Light sent to a new frame from Blender | — | 3 ms (median) |
| Light sent to the picture on screen | after the release only (one picture, about 15 ms after it) | 12–21 ms (median) during the drag; 43–60 pictures drawn in it |

A drag in the IPR's picture with a navigation key held (2026-11-16; Cinema 4D 2027, Blender 5.2.3, 320 × 180 at 100 %, GPU, a cube on a floor; 40 mouse messages posted 25 ms apart with the key 3, 1 or 2 held; `tests/c4d/ipr_navigate_window.py`):

| | Orbit | Pan | Dolly |
|---|---|---|---|
| Steps of the drag that moved and sent the camera | 41 of 41 | 40 of 40 | 40 of 40 |
| The window's timer during the drag | 0 calls | 0 | 0 |
| Camera sent to Blender's answer (median) | 1.5 ms | 1.0 ms | 1.0 ms |
| Step to a new frame from Blender (median) | 2.6 ms | 2.2 ms | 3.0 ms |
| Step to the picture on screen (median) | 6.0 ms | 3.6 ms | 3.9 ms |
| Frames arrived / pictures drawn during the drag (1.2 s) | 137 / 71 | 209 / 72 | 215 / 71 |
| Redraws of Cinema 4D's view during the drag (median time) | 40 (3.9 ms) | 39 (3.1 ms) | 39 (3.0 ms) |
| Finding the point under the mouse | 0.02 ms | 0.02 ms | 0.04 ms |

The point under the mouse in a scene of 400 spheres (115,200 polygons): 0.3–1.0 ms (`tests/c4d/ipr_navigate.py`).

A large scene (900 meshes of 800 polygons, 720,000 polygons; 200 × 112, GPU, `tests/c4d/ipr_reuse.py`, 2026-11-13), one object moved: the export and comparison took 74–87 ms with every mesh exported (55–66 ms of it the meshes) and 8–15 ms with unchanged meshes kept (3–4.5 ms the meshes: 600 judged by identity, 300 generated ones hashed); the change reached the picture in 120–131 ms and in 50–63 ms (two runs). What is left is Blender's: of the 50–63 ms, Blender applied the change in 1–2 ms and the first frame came back 39–46 ms later (Cycles restarts with 900 objects), on screen 1–8 ms after that.

## The IPR window

A dockable dialog opened by the command "Cycles Render IPR" (`ipr/ipr_dialog.cpp`).

![The IPR window](images/ipr_window.png)

- **Menu:** File (send a snapshot to the Picture Viewer, save the image, close); Add (a Cycles Material; a camera with a Cycles Camera tag where the render view looks from, which becomes the render view's camera; a Cycles Camera tag on the selected cameras; a Cycles Object tag on the selected objects; a Cycles Light where the render view looks from; a Cycles Light tag on the selected lights; each one undo step; Cinema 4D's undo cannot hold a view, so the camera entry keeps the view out of its step and the renderer's message plugin puts the view back on its earlier camera when the step is undone, and on the new one when it is redone); Options (IPR resolution: Fit Window, or 25, 50, 100 or 200 % of the Render Settings; show Blender's window; restart Blender).
- **Toolbar, left to right:** start or stop, pause, reload; the camera list with the lock; snapshot; the zoom percentage (it can be dragged and typed into) with the frame button; then, at the right, the pass list, the engine list and the settings button.
- **Picture:** the frame with Cinema 4D's view transform (with "Basic" colour management: as the Picture Viewer shows a final frame, through the sRGB curve with Linear Workflow on and as it is with it off), zoom around the cursor with the mouse wheel, drag to pan. With "Fit Window" the area outside the camera's picture is shown at 45 % brightness. Snapshots and saved images carry the render-space values with the document's profiles (OCIO), or linear values with the linear profile ("Basic"), as Cinema 4D's own renders do.
- **Navigating from the picture** (0.2.0): a drag with Cinema 4D's navigation keys moves the camera the IPR renders through, as in Cinema 4D's views: Alt with the left button orbits, with the middle button pans, with the right button dollies (a parallel view zooms); the keys of Cinema 4D's key map for Rotate, Move and Scale Camera (by default 3, 1, 2) held while dragging do the same. Plain drags and the wheel keep their meaning (the picture's own pan and zoom). The scheme is one function, `NavigationForDrag` in `ipr/ipr_navigation.cpp`, and a switch in the Preferences ("Navigate in the IPR Window") turns it off. A scene camera moves in one undo step per drag, an editor camera in its view's own undo ("Undo View"); Escape puts the camera back. The status bar says what is moved, or why nothing is (the IPR not running, its camera locked, a Protection tag, a locked layer, orbiting a fixed projection). Design decision 26.
- **Status line:** time, RAM and VRAM of the Blender process, and a progress bar labelled with the samples.
- The window's texts are English strings in the code, not yet in the string resources.

## Reading the scene

`SceneExporter` (`scene/scene_exporter.cpp`) works in two passes: `Collect()` walks the document and records what renders, `Write()` turns that into scene items.

- **The walk** follows the SDK's recipe for caches and carries inherited state down the tree: render visibility, layer flags, the global matrix, the stack of texture tags, the Compositing tag, the Cycles Object tag. Deformers, generators, effectors and fields are already part of the caches once the document has been evaluated, so they need no handling of their own.
- **What renders**, as measured against the Standard renderer: a generator's input objects do not, its cache does; render visibility is inherited from the parents until a child switches it on again; a layer with rendering off hides the objects assigned to it and what they generate, but not their children; an instance of an object whose own render dot is off renders nothing, while an instance of an object hidden by its parent renders.
- **A mesh is two items:** its topology (`mesh`) and its positions and normals (`points`), so a deforming mesh sends the second alone. Equal meshes are sent once and shared.
- **Materials per polygon.** The texture tags that apply to an object form a stack: those of its ancestors first, its own last. A later tag covers an earlier one, everywhere or on the polygons of the selection it is restricted to. A tag with an alpha channel leaves what was below it. The mesh gets one material slot per combination in use and a slot index per polygon (`SceneExporter::Resolve`).
- **Which UV set.** Every UVW tag becomes a UV set. When an object has several, its texture tag uses the UVW tag that follows it, else the first; that set is marked as the mesh's render set.
- **Instancers.** A multi-instance becomes one `instancer` per mesh of the object it refers to. What a generator produces is compared by content: copies of one mesh with the same materials inside one generator are sent as one instancer when there are at least 16, and always when they are clones of a Cloner, because the instancer is what carries MoGraph's colour per clone. Objects of the document itself are never batched.
- **Who reads the document.** The IPR exports on Cinema 4D's main thread after the scene has finished evaluating and hands the result to the worker thread. A final render reads the render document Cinema 4D passes to the renderer, on the render thread.

## What becomes what

| Cinema 4D | Blender | Notes |
|---|---|---|
| Polygon meshes, n-gons | Mesh | Points-only update for deforming meshes |
| Phong tag, Normal tag | Corner normals | Computed from the Phong tag when there is no Normal tag |
| UVW tags, vertex colours, vertex maps | UV layers, colour attributes, float attributes | Shaders read them by the tag's name |
| Polygon selections with texture tags | Material slots and a slot index per face | |
| Multi-instances, Cloners | A mesh of loose points with a matrix and a colour each, shown through Geometry Nodes ("Instance on Points") | Equal clones of a generator are batched automatically |
| MoGraph Color shader | Attribute node of type `INSTANCER`, `mograph_color` | Colour mode only |
| Vertex Map shader | Attribute node naming the vertex map or vertex colour tag | |
| Classic material channels | Principled BSDF with emission, alpha, bump, normal and displacement | Reflectance: the first reflecting layer is the specular lobe, a second dielectric one the clear coat |
| Texture tag: offset, tiles, projection | Nodes that compute the UV pair from the mesh's UVs or from the point in the tag's axes | |
| Stacked texture tags | One Blender material per pile, the layers joined by Mix Shader nodes with the upper layer's alpha | |
| Shaders | Nodes where Blender has the same thing; otherwise baked to a float texture and cached | See [Materials](#materials) |
| Redshift Standard Material | Principled BSDFs, option by option | Table in the SDK reference |
| Redshift nodes wired into it | Blender's Math, Vector Math, Mix, Invert, Gamma, Hue/Saturation, Combine and Separate nodes, Image Texture | Colour Correct's contrast as six Vector Math nodes (measured formula); untranslated nodes named |
| Other node materials | Principled BSDF through Cinema 4D's material exchange | An approximation |
| Cycles nodes | Blender's shader nodes, one to one | |
| Standard lights | Point, Spot, Sun, Area | Cinema 4D's lights have no physical unit; the conversion is calibrated by measurement |
| Redshift lights | Sun, Point, Spot, Area with exact powers | Measured against Redshift |
| Cycles Light object | Point, Sun, Spot, Area with Blender's own values | Nothing to convert; lengths from document units to metres |
| Cycles Light tag on a light | The tag's Blender values in place of the light's own (Light tab), Blender's light settings added (Settings, Visibility, Eevee tabs) | Each tab only while its switch is on |
| Sky object with a panorama, Redshift dome light | World environment | Without either: the default world of the render settings |
| Camera, Redshift camera | Lens, sensor, shift, clipping, parallel projection, depth of field | |
| Pixel Aspect of the Output page | Blender's pixel aspect X and Y of the final render | The IPR renders square pixels and shows the picture as wide as the pixel aspect makes it |
| Compositing tag's first enabled Object Buffer | The object's pass index (Object Index pass) | |
| Volume Loader | Volume read from the file's path | No copy: the item names the frame's file |
| Volume Builder, Pyro | `.vdb` files saved to the preferences folder | |
| Volume shading | Principled Volume reading the grids by name; the Cycles Object tag sets density, colour, emission, fire | |
| Hair | Hair curves with a radius per point, Principled Hair BSDF | Shape, thickness and colour gradient |
| Particle groups | Point cloud with radius and colour per point | |
| Animation | Deltas per frame | |
| Motion blur | Matrices of objects and camera at shutter open and close as keys; the `velocity` attribute for deformation | Final frames only |
| Compositing tag, Cycles Object tag | Ray visibility per object, holdout, shadow catcher | |
| Include and exclude lists of lights | Light linking | Also from the object's side, in the Cycles Object tag |

## Materials

`MaterialExporter::WriteLayers` (`materials/material_export.cpp`) decides which translator a material goes to:

1. **A classic material** becomes a Principled BSDF graph, channel by channel (`ClassicMaterialGraph`). Its textures go through `ShaderTranslator`, which tries three ways in order:
   - *Nodes*, for shaders with an exact counterpart: bitmap, colour, checkerboard, gradient along U or V, colorizer, fresnel, fusion and layer in the blend modes Blender computes the same way, vertex map, MoGraph colour.
   - *Baked*, for any other shader that depends only on the UV coordinates: Cinema 4D renders it over the UV square into a float EXR, once per session, and again only when its settings, a texture file inside it or the document's colour settings change, or, for a shader that changes over time by itself, at another frame. Final renders and the IPR share the files. They live in a folder of the Cinema 4D session and are deleted when every document they were made for is closed and no Blender reads them any more, and at the end of the session (`materials/baked_files.h`).
   - *Neither*: the channel falls back to its plain value and the user is told.
2. **A material of Cycles nodes** is written node for node, value for value and wire for wire. Nothing in Cinema 4D evaluates these nodes.
3. **A Redshift material** whose output is a Standard Material node is read port by port.
   The nodes wired into its inputs are translated node by node where Blender has the same thing (`materials/redshift_nodes`); four whose random numbers and noises are Redshift's own become look-alikes that say so (`materials/redshift_lookalikes`): their per-object numbers vary by keys the exporter sends with each object that wears such a material (an object's `properties`, an instancer's per-copy `floats`), so that Blender varies them with the same objects as Redshift; any other node is named in the console and the input's own value is used.
4. **Any other node material** is asked to describe itself as a Standard Surface, the route Cinema 4D's own FBX and USD exporters take.
5. **A Cycles Native Material** is sent as the bytes it holds.

A material none of them can read is replaced by its average colour, with a warning.

## The Render Settings page

The page mimics Blender's Render properties: Blender's panels as foldable groups with Blender's names, a Simple and an Advanced layout, the groups of the engine that is not chosen hidden, and settings in two columns that line up within a group. **Defaults are Blender's own.** Five tabs: Render Settings, General Settings, Object Settings (the plugin's own settings for reading the Cinema 4D scene), AOVs (the render passes; a pass only one engine has is hidden for the other) and Output, right of AOVs (the maintainer's choice). The Output tab is Blender's Format panel (Resolution X and Y, %, Aspect X and Y) over Cinema 4D's own Output values: Cinema 4D allocates the picture from its Width, Height and Pixel Aspect and the IPR reads them, so they stay what renders; the tab writes them and shows them back when they were changed on Cinema 4D's page (`videopost/output_settings`).

![The Render Settings page](images/render_settings.png)

The whole page is generated from one definition, `tools/make_settings.py`: it writes the parameter ids, the layout, the texts, and the table the plugin sends the settings from. To add a setting, add one line there and run the tool. Parameter ids are stored in scene files: never renumber one and never reuse a retired number.

## Risks

- **Coupling to Blender's version.** The offset of Cycles' status text and the wrapped Cycles methods must be checked again for every Blender release.
- **Proven on one kind of machine.** Every mechanism is measured on Windows 11 with one RTX 4090 at 100 % display scaling. Untested: other display scaling, several GPUs, custom OCIO configurations, large production scenes.
- **macOS.** Hidden or covered windows may be throttled there, which would hit the IPR's design. The port is open.
- **Stalls on the main thread.** Large scene updates briefly block Blender's viewport, and the export may briefly block Cinema 4D's interface.
- **Matching the look.** Cinema 4D's lights and procedural noises differ from Blender's; expect calibration and baking.
- **Gaps in the Cinema 4D SDK.** It ships one trivial example renderer and no IPR example; several behaviours of a renderer are known only from header comments and from trying. They are collected in the SDK reference.
