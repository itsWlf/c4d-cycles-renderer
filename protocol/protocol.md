# Cycles Render bridge protocol

The contract between the Cinema 4D plugin (client) and the bridge inside Blender (server). This file is the single definition; `bridge/c4d_bridge/protocol.py` and the plugin's `session/protocol` implement it.

**Protocol version: 0.** Both sides send the version they speak; a mismatch ends the connection.

## Start-up

1. The plugin starts Blender with the bridge's `main.py` as start-up script (`--python`) and passes, after `--`:
   - `--handshake <file>`: where the bridge must write its handshake file.
   - `--token <text>`: a random secret for this launch.
2. The bridge listens on `127.0.0.1`, on a port chosen by the operating system, then writes the handshake file (written to a temporary name and renamed, so it is never seen half-written):

   ```json
   {"protocol": 0, "port": 50123, "pid": 4711, "blender": "5.2.3 LTS",
    "blender_version": [5, 2, 3], "mode": "final", "bridge": "0.1.0"}
   ```

   `mode` is `final` for a background Blender and `viewport` for a windowed one. `bridge` is the release version of the bridge, for information only: whether the two sides can talk is decided by `protocol`.
   If the bridge cannot run in this Blender it writes `{"error": "<reason>"}` instead and exits with code 4.
3. The plugin connects and sends `hello`. The bridge accepts exactly one connection per launch.
4. The bridge quits when it receives `shutdown`, when the connection closes, or when no client connects within 60 s.

Exit codes of Blender: 0 after `shutdown`; 2 when the connection was lost; 3 when no client connected; 4 when the bridge refused to start; 1 for an unhandled Python error.

## Frames

Every message, in both directions, is one frame. All integers are little-endian.

| Field | Type | Meaning |
|---|---|---|
| magic | 4 bytes | `C4DB` |
| version | u16 | protocol version |
| flags | u16 | 0 (reserved) |
| header_length | u32 | byte length of the JSON header |
| blob_count | u32 | number of binary blobs |
| blob_length × blob_count | u64 each | byte length of each blob |
| header | UTF-8 JSON object | see below |
| blobs | raw bytes | back to back, in order |

Limits: `header_length` ≤ 16 MiB, `blob_count` ≤ 4096. A frame that breaks a limit or has a wrong magic or version ends the connection.

Blobs carry bulk data (mesh arrays, pixels) as raw little-endian arrays. The header says what each blob is; a blob is referred to by its index.

## Headers

**Request** (plugin → bridge): `{"op": "<name>", "id": <integer>, ...fields}`. `id` is chosen by the plugin and echoed in the reply.

**Reply** (bridge → plugin), exactly one per request, in request order:

- success: `{"op": "reply", "id": <id>, "ok": true, ...fields}`
- failure: `{"op": "reply", "id": <id>, "ok": false, "error": {"code": "<code>", "message": "<text>"}}`

**Event** (bridge → plugin, unrequested): `{"op": "event", "event": "<name>", ...fields}`. None are defined in version 0. Live frames and render progress do not travel over the socket at all; see "Frame block".

Error codes: `not_authenticated`, `bad_token`, `unknown_op`, `bad_request`, `internal` (an exception in the bridge; `message` holds the traceback).

## Operations in version 0

| op | Request fields | Reply fields | Notes |
|---|---|---|---|
| `hello` | `token`, `protocol`, `client` | `protocol`, `blender`, `blender_version`, `mode`, `bridge`, `cycles_devices` | Must be the first request. A wrong token is answered with `bad_token` and the connection is closed. `cycles_devices` is a list of `{"name", "type"}`. `bridge` is the bridge's release version; the plugin notes in the console when it differs from its own. |
| `ping` | any fields, any blobs | the same fields and the same blobs | Round-trip check. |
| `shutdown` | — | — | The bridge replies, then quits Blender. |
| `scene` | `clear`, `items`, optional `remove` | `count`, `warnings` | Creates or replaces scene items; see "Scene items". With `clear: true` everything sent earlier (and Blender's start-up scene) is removed first. `remove` is a list of item ids to delete (unknown ids are ignored); removals happen before `items` are built. `warnings` is a list of sentences for the user, for example a texture file that was not found. |
| `settings` | `engine`, `device`, `working_space`, `values`, `view_layer` (all optional) | `warnings`, `engine`, `device` | Sets render settings; see "Settings". |
| `get` | `paths` | `values`, `working_space` | Reads scene properties back, by the same paths `settings` uses. For tests and diagnostics. |
| `render` | `width`, `height`, optional `passes`, `region` | `width`, `height`, `channels`, `pixels`, `passes`, `warnings`, `seconds`, `engine` | Renders one final frame through the active camera with the current settings and returns it. Blocks until the frame is done. See "Render result". |
| `material_preview` | `material`, optional `size` (128), `samples` (32) | `width`, `height`, `channels`, `pixels` | The picture of a ball that wears the material with this id (sent before in a `scene` request), for a thumbnail: square, `size` pixels, RGBA float as from `render`, the ball over nothing (alpha 0 around it). The ball has a scene and a light of its own; the scene the plugin sent and its render settings are not touched. Always Cycles on the CPU. |

| `view_start` | `width`, `height` | viewport status | Viewport mode only, like every `view_` operation. Resizes the hidden window and puts the viewport into Rendered mode. Rendering begins once the window has the new size, so the first render already has it. |
| `view_stop` | — | viewport status | Leaves Rendered mode, which frees the render session and its memory. Blender keeps running. |
| `view_pause` | `paused` | viewport status | Holds or resumes the render. A resumed Cycles render continues where it was. |
| `view_size` | `width`, `height` | viewport status | Changes the image size while rendering. |
| `view_pass` | `pass` | viewport status | Shows one render pass. `pass` is Blender's identifier for the active engine (`COMBINED`, `NORMAL`, …; lists in `docs/reference/blender.md`). An unknown pass is a `bad_request`. |
| `view_camera` | `camera: true`, or `matrix`, `lens`, `sensor_width`, `width`, `height`, `clip_start`, `clip_end` | viewport status | What the viewport looks through; see "Viewport". |
| `view_region` | optional `region` | viewport status | Renders only a part of the image: `region` is [min x, min y, max x, max y] as fractions of the image, measured from its lower left corner. Without `region` the whole image is rendered again. Outside the region the frames hold whatever was there before; the plugin blacks it out. |
| `view_window` | `visible` | viewport status | Shows or hides Blender's own window (for debugging). |
| `edit_material` | `material`, `folder`, optional `name`, `test_change` | `opened`, `windows` | Viewport mode only. Opens a window that is only a Shader Editor on one native material, or keeps the one that is open (`opened` false). See "Native materials". `test_change` (for tests): after its next save Blender changes the material as an edit by hand would: a colour `[r, g, b]` becomes the base colour, `{"image": file}` an Image Texture wired into it, `{"image_path": file}` another file for the image that is there, `{"node": type}` a node of that type wired into the base colour (a shader: into the output), `{"undo": true}` is Ctrl+Z. |
| `close_material_editor` | optional `material` | `closed` | Closes the editor window of one native material, or of all. What follows is the same as when the user closes it. |
| `view_test_pattern` | `width`, `height` | viewport status | Publishes one frame holding a known pattern, for testing the frame block: value number i, in the block's storage order, is ((i × 2654435761) mod 2³²) >> 8. |

Any other request before a successful `hello` is answered with `not_authenticated`.

A blob is referred to from a header by its index in the frame (a plain integer). Inside a scene item the index counts from that item's first blob; see "Scene items".

**Viewport status**, the reply of every `view_` operation: `state_block` (name of the frame block's state block), `rendering`, `paused`, `width`, `height` (current size of the viewport), and `draw_error` (a traceback) when reading a frame failed since the last reply.

## Settings

- `engine`: `CYCLES` or `EEVEE`.
- `device`: `CPU` or `GPU`, for Cycles. With `GPU` the bridge enables the first GPU back end that lists a device (OptiX, CUDA, HIP, oneAPI, Metal, in this order) and falls back to the CPU with a warning.
- `working_space`: Blender's name of the colour space that colours and pixels are in (`ACEScg`, `Linear Rec.709`, `Linear Rec.2020`). The plugin sends the document's render space.
- `view_layer`: a list of `[path, value]` pairs like `values`, relative to Blender's view layer. It switches render passes on and off (`["use_pass_z", true]`).
- A value that is a string chooses one of a property's named options (`["cycles.denoiser", "OPTIX"]`). If Blender does not offer that option on this machine, the property keeps its value and the reply's `warnings` say so.
- `values`: a list of `[path, value]` pairs. A path names a property relative to Blender's scene, for example `["cycles.samples", 128]` or `["render.film_transparent", true]`. An unknown path is a `bad_request`. The table of settings and paths is in the plugin, `source/videopost/render_settings_table.h` (generated by `tools/make_settings.py`).

## Scene items

`items` is a list of objects, built in the order given: an item must come after the items it refers to. Each has a `kind` and an `id` (a string chosen by the plugin; sending an existing id replaces that item, in place where Blender allows it, so things that refer to it keep working). An item with blobs carries `blobs`: the frame index of its first blob. Blob indices in the item's other fields count from there. (A request that leaves unchanged items out still sends the same number of blobs; the ones of the items left out are empty.) Everything is already in Blender's terms: right-handed, Z up, metres, and a `matrix` is the 4×4 world matrix as 16 numbers in row-major order.

| kind | Fields | Notes |
|---|---|---|
| `mesh` | `corner_verts`, `face_sizes` (blob indices); `point_count`, or `points` (blob index); optional `normals` (blob index), `uvs`, `uv_render`, `floats`, `colors`, `material_indices`, `smooth`, `slots` | The topology of a mesh and everything bound to it. `corner_verts`: int32 point index per face corner, faces counter-clockwise seen from the front. `face_sizes`: int32 corner count per face. Positions come either with the mesh (`points`, and `normals`) or in a `points` item written after it; then the mesh gives `point_count`. `uvs`: list of `{"name", "data"}`, `data` a blob of float32 u,v per face corner with v = 0 at the bottom of the image. `uv_render`: index of the UV set that image textures use when a material names none (default: the first). `floats`: list of `{"name", "domain", "data"}`, one float32 per point (`domain` is `POINT`); a material reads it with an Attribute node. `colors`: the same with float32 r,g,b,a per point (`POINT`) or per face corner (`CORNER`). UV sets, floats and colours share one set of names. `slots`: number of material slots (default 1). `material_indices`: blob of one int32 slot index per face (default: all 0). Sending a mesh again rebuilds it and loses its positions, so the plugin always sends its `points` item with it. |
| `points` | `mesh` (id of a mesh), `points`, optional `normals`, `motion` (blob indices) | Positions and shading normals of a mesh that exists; all that changes while a mesh deforms. `motion`: float32 x,y,z per point, the way the point goes from shutter open to shutter close in the mesh's own axes (see "Motion blur"). `points`: float32 x,y,z per point. `normals`: float32 x,y,z per face corner, in the mesh's own axes; without it the mesh is flat shaded, or smooth when the mesh said `smooth: true`. The plugin uses the id `<mesh id>.points`. |
| `object` | `mesh` (id of a mesh), `matrix`, optional `materials`, `motion`, `visibility` | An instance of a mesh in the scene. `motion`: 32 numbers, the world matrix at shutter open and at shutter close (see "Motion blur"). `visibility`: `{"camera", "shadow", "diffuse", "glossy", "transmission", "volume_scatter"}`, booleans saying which rays see the object (default: all), plus `"holdout"` and `"shadow_catcher"` (default: false) for Blender's object properties of those names; an `instancer` takes it too. Blender's "indirect only" is `camera: false`: Cycles does nothing else for it. `materials`: list of material ids, one per slot of the mesh; they are set on the object, so two objects can show one mesh with different materials. |
| `instancer` | `mesh` (id of a mesh), `matrices` (blob index), optional `materials`, `colors` (blob index) | Many copies of one mesh. `matrices`: 16 float32 per copy, a world matrix in row-major order like `matrix`. `materials` as for `object`. `colors`: float32 r,g,b,a per copy in the working colour space; a material reads it with an Attribute node of type `INSTANCER` named `mograph_color`. Sending the item again with another number of copies replaces them. |
| `volume` | `path`, `matrix`, optional `materials` | A .vdb file shown as a volume. The file is in the plugin's own axes and units; `matrix` converts them. Density is taken per metre of the scene. A material reads a grid by its name (Principled Volume, "Density Attribute"). A missing file is a warning. |
| `curves` | `matrix`, `points_per_curve`, `points` (blob index), optional `radii` (blob index), `materials` | Hair. `points`: float32 x,y,z per point, every curve with `points_per_curve` points, root first. `radii`: float32 per point, in metres. |
| `pointcloud` | `matrix`, `points` (blob index), optional `radii` (blob index) or `radius`, `colors` (blob index), `materials` | Particles, rendered as spheres. `colors`: float32 r,g,b,a per point; a material reads it with an Attribute node named `particle_color`. |
| `camera` | `matrix`, optional `motion` (as for `object`), `type`, `lens`, `sensor_width`, `ortho_scale`, `shift_x`, `shift_y`, `clip_start`, `clip_end`, `dof`, `active` | The camera looks along its local −Z with +Y up. `type`: `PERSP` or `ORTHO`. `lens` and `sensor_width` in millimetres; the sensor width always spans the image width. `ortho_scale`: width of the view in metres. `shift_x`, `shift_y`: Blender's lens shift, in image widths. `dof`: `{"focus_distance", "fstop"}`, absent for no depth of field. `active: true` makes it the render camera. |
| `light` | `matrix`, `type`, `energy`, `color`; optional `use_shadow`, `shadow_soft_size`, `spot_size`, `spot_blend`, `shape`, `size`, `size_y`, `angle`, `spread`, `linking` | A light shines along its local −Z. `type`: `POINT`, `SUN`, `SPOT` or `AREA`. `energy` in watts (W/m² for `SUN`). The optional fields are the Blender light properties of the same name; angles in radians, sizes in metres. `linking`: `{"mode": "INCLUDE" | "EXCLUDE", "objects": [ids of object and instancer items]}`: the light lights only these, or all but these, and the same goes for the shadows it throws. The ids may name items that come later in the request; the bridge resolves them after every request. |
| `image` | `path`, optional `colorspace`, `alpha_mode` | An image file. `colorspace` is Blender's name (`sRGB`, `Non-Color`, `Linear Rec.709`, …). A missing file is a warning, not an error. |
| `material` | `nodes`, `links`, optional `props`; or `native`, `name`, optional `blend` | A material as a node graph; see "Node graphs". `props`: properties of the Blender material itself (`{"displacement_method": "BOTH", "surface_render_method": "BLENDED"}`); a name with a dot sets a property of a part of the material (`"cycles.emission_sampling"`). |
| `world` | `nodes`, `links` | The environment as a node graph ending in a `ShaderNodeOutputWorld`. There is one world; the id is ignored. |

### Motion blur

Motion blur is switched on with the setting `render.use_motion_blur` and applies to final frames. The plugin decides how long the shutter is open by reading its scene at the moment the shutter opens and at the moment it closes, and sends where things are then: `motion` on `object` and `camera` items (two world matrices) and on `points` items (one vector per point). The bridge keeps Blender's own shutter fixed at two frames, centred on the current frame, and keys an object's three positions on the frame before, the current frame and the frame after; Blender then samples exactly these keys. A point's `motion` becomes the mesh attribute `velocity`. An item without `motion` stands still.

### Node graphs

Materials and the world are described in Blender's own names, so the bridge needs no knowledge of Cinema 4D.

- `nodes`: list of `{"name", "type", "props", "inputs", "outputs"}`; `inputs` and `outputs` are optional. `type` is a Blender node type (`ShaderNodeBsdfPrincipled`). `props` sets properties of the node (`{"interpolation": "Closest"}`; numbers and booleans too); the property `image` takes the id of an `image` item. `inputs` sets the value an input socket has while nothing is linked to it. `outputs` sets the value an output socket holds: the Value, RGB and Normal nodes keep their value there (`{"Value": 0.8}`, `{"Color": [0.2, 0.5, 1.0]}`).
- `links`: list of `[from node, output socket, to node, input socket]`, nodes by `name`.
- A socket is named by its identifier (`Factor_Float`, `Vector_001`) or its visible name (`Base Color`); an integer is its index. Colours may be given as three numbers.
- Inside an input value, `{"image": "<image id>", "is": "texel_width"}` (or `texel_height`) stands for the size of one texel of that image in UV units, 1 / width or 1 / height. Blender knows the image size, the plugin does not.

A node property named `color_ramp` sets the stops of a Color Ramp node (`ShaderNodeValToRGB`): `{"interpolation": "LINEAR" | "EASE" | "CONSTANT" | …, "elements": [[position, r, g, b, a], …]}`, at least one stop, in rising order of position.

### Native materials

A `material` item with `"native": true` is a Blender material that is stored, not described: `blend` is the index of a blob holding a .blend file that contains that one material (what `bpy.data.libraries.write(path, {material})` writes), `name` its name for the user. Without `blend` it is Blender's default material. The item's id is the same from request to request for one material (`native_<key>`), unlike the ids of translated materials.

The bridge loads the bytes when they differ from the ones it has for that id. It ignores them while the material is open in an editor window: the window is ahead of the plugin then. Clearing the scene leaves such a material, and what it uses, alone.

`edit_material` opens the editor. The plugin sends it to a Blender it keeps for editing, apart from the one the IPR renders in; the bridge is the same in both. From then on the bridge writes into `folder`, replacing each file whole:

| File | Content |
|---|---|
| `<id>.blend` | the material, as the same kind of bytes, with the paths of the files it names (images) made absolute; written when the editor opens and a quarter of a second after each change |
| `<id>.preview` | a picture of a ball wearing the material: two little-endian int32 (width, height), then width x height x 3 bytes, RGB as displayed, top row first; only the ball's bounding box, black outside the ball |
| `<id>.state` | one line of text, `blend=<n> preview=<n> closed=<0 or 1>`: how often each of the two files was written, and whether the window has been closed |

If the bridge does not hold a material of that id (the IPR is not rendering), it starts from `<folder>/<id>.blend` if the plugin put one there, else from a default material. There is no message from the bridge to the plugin: the plugin reads the state file on a timer.

## Viewport

In viewport mode Blender's window is one hidden 3D viewport in Rendered mode. Scene and settings requests work as for final frames, and the live render follows every one of them. Two things differ:

- **Viewport properties.** The viewport uses its own settings: `cycles.preview_samples`, `cycles.use_preview_denoising`, `cycles.use_preview_adaptive_sampling`, `eevee.taa_samples`. The plugin's settings table sends them next to the final-render properties.
- **What the viewport looks through** (`view_camera`). `camera: true`: the scene's active camera object, as in a final render. Otherwise the viewport's own view, placed like a camera: `matrix` is a camera's world matrix, `lens` and `sensor_width` in millimetres with the sensor width spanning the image width, `width` and `height` the image size in pixels. Moving this view changes nothing in Blender's scene, which makes it the fast way to follow a moving camera. It cannot do depth of field, lens shift or parallel projection, and Blender accepts only an equivalent focal length of 0.5 to 125 mm at a 36 mm sensor; the plugin uses a camera object whenever one of those is needed. Both ways frame the scene identically (tested).

## Frame block

Live frames and render progress reach the plugin through shared memory, not the socket. The bridge creates the blocks; the plugin opens them for reading. All names start with `cyclesrender_<token>`, the token of the launch.

**State block**, `…_state`, 256 bytes, little-endian:

| Offset | Type | Meaning |
|---|---|---|
| 0 | u32 | magic `0x54535243` ("CRST") |
| 4 | u32 | layout version, 1 |
| 8 | u64 | header sequence: odd while the four fields at 16–28 are being changed |
| 16 | u32 | generation: number of the pixel block in use; 0 while there is none |
| 20, 24 | u32, u32 | width, height of the frames |
| 28 | u32 | index (0 or 1) of the buffer holding the newest complete frame |
| 32, 40 | u64, u64 | sequence of buffer 0 and buffer 1: odd while that buffer is being written |
| 48 | u64 | frame id: counts published frames; 0 while there is none |
| 56 | u32 | state: 0 stopped, 1 rendering, 2 paused, 3 done |
| 60, 64 | u32, u32 | samples rendered, samples wanted (0 = unlimited) |
| 72 | f64 | when the current render began (Unix seconds); time spent paused is left out |
| 80 | f64 | when it finished or was paused; 0 while it runs |
| 128 | 128 bytes | the engine's progress text, UTF-8, NUL-terminated |

**Pixel block**, `…_px<generation>`: two buffers back to back, each `width × height × 4` float32 values, RGBA, scene-linear in the working colour space, rows **bottom-up**. A new pixel block with the next generation number replaces it whenever the image size changes; the previous one stays valid until the one after that.

**Writing a frame:** the bridge picks the buffer that does not hold the newest frame, makes its sequence odd, writes the pixels (the GPU reads straight into the shared memory), makes the sequence even, then stores the buffer index and the new frame id. **Reading:** read the header sequence, geometry, newest-buffer index and its sequence; if a sequence is odd, try again later; copy the buffer; if a sequence changed meanwhile, discard the copy. The writer alternates buffers, so a reader has a whole frame interval for its copy.

**Signal:** a named auto-reset event `…_frame` (Windows) is raised after every published frame, so the reader can wait instead of poll.

"Done" is reported at the earliest 0.6 s after the last change that restarts the render, because Cycles needs a moment before its own progress text belongs to the new render.

## Render result

`pixels` is the index of a blob holding `width × height × channels` float32 values: scene-linear in the working colour space, premultiplied alpha, row by row with the **top row first**, `channels` = 4 (RGBA).

**Passes.** The request's `passes` lists the render passes wanted, by Blender's names (`Depth`, `Normal`, `Diffuse Direct`, …); they must have been switched on with `settings`. The reply's `passes` lists the ones delivered, and their pixels follow the picture as one blob each, in that order, in the same layout (a pass of one value is repeated in R, G and B; alpha is 1 where the pass has none). A pass the engine does not have is left out and named in `warnings`.

**Region.** The request's `region` is [min x, min y, max x, max y] as fractions of the image, measured from its lower left corner. Only that part is rendered; the reply still has the full size, black outside the region. `engine` is Blender's identifier of the engine that rendered (`CYCLES`, `BLENDER_EEVEE`). `seconds` is the time Blender spent rendering.
