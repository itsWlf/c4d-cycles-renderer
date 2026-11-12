# Blender reference

The Blender behaviour this project depends on. Evidence labels:

- **[T]** tested here on Blender 5.0.0 and 5.2.3 LTS (Windows 11, RTX 4090, OpenGL back end) with the project's prototype scripts and bridge tests (`prototypes/`, `tests/bridge/`; both are kept locally and are not part of the repository). Both versions behaved the same unless a line says otherwise.
- **[S]** read in the Blender 5.2.3 source, not run.
- **[I]** inferred from source; treat as a hypothesis.

Source paths below are relative to the root of the Blender 5.2.3 source tree (this project keeps a copy in `references/blender-v5.2.3_source/`, which is not part of the repository).

## Version coupling

The bridge reaches into Blender in places that are not public API, so it runs on listed Blender versions only (`SUPPORTED_BLENDER` in `bridge/c4d_bridge/__init__.py`). Before adding a version to that list, check each of these on it:

- **The offset of Cycles' status text** inside the engine struct ("Cycles status text" below). It is what the IPR's sample counter reads.
- **The wrapped methods of Cycles' Python class**: `view_draw` (viewport) and `render_frame_finish` (final frames), in `intern/cycles/blender/addon/__init__.py`.
- **Viewport readback**: the draw handlers and the framebuffer they read ("Reading viewport pixels").
- **The window set-up** the viewport driver performs ("Window and viewport setup"), including the operators it calls.
- **The shader nodes**: run `tools/make_nodes.py` and review the tables it holds by hand, as Blender does not tell Python which panel a socket is in.
- **Defaults of render properties**: compare `tools/make_settings.py` with a new scene.
- **The extensions folder** the plugin looks into for the bridge (`FindBridgeScript`): `<user folder>/<major>.<minor>/extensions/user_default/`.

## Launch

**IPR (windowed):**

```
blender --factory-startup --no-window-focus -p 0 0 W H --gpu-backend opengl --gpu-vsync off
        --python bridge/c4d_bridge/main.py -- <bridge args>
```

**Final frames (background):**

```
blender -b --factory-startup --offline-mode -Y --python-exit-code 1 --log-file <file>
        --python bridge/c4d_bridge/main.py -- <bridge args>
```

- `--factory-startup` skips user preferences, so the bridge sets everything it needs on each launch. [S]
- `-p sx sy w h`: lower-left origin, client size, minimum 640×480, clamped to the monitor work area. [S]
- `--no-window-focus` opens the window behind others without activating it. [S]
- `--gpu-vsync off` stops the window swap from throttling the loop. [I]
- The prototypes ran without `--gpu-backend`; the default back end on Windows was OpenGL. [T]
- Arguments after `--` belong to the script. [T]
- `--python` runs after the window exists but before the main loop starts. [S] Doing the real setup from `bpy.app.timers.register(fn, first_interval=0)` works. [T]
- Python is 3.13 with numpy 2.3 bundled; `socket`, `mmap`, `threading`, `ctypes` work. `PYTHONPATH` is ignored unless `--python-use-system-env` is passed. [S]

## The bridge

`bridge/c4d_bridge` implements protocol version 0 (`protocol/protocol.md`). Tested on 5.2.3 with `python tests\bridge\test_protocol.py "<blender.exe>"` and from the plugin's session self-test. [T]

- The bridge is packaged as a Blender extension (`bridge/blender_manifest.toml`, id `cycles_render_bridge`). `blender --command extension validate <zip>` accepts the package, and `blender --command extension install-file -r user_default -e <zip>` (what "Install from Disk" does) unpacks it into `<user folder>/extensions/user_default/cycles_render_bridge/`. The user folder is `%APPDATA%\Blender Foundation\Blender\5.2` (`bpy.utils.user_resource('EXTENSIONS', path='user_default')` gives the same place under `--factory-startup`), or the folder named by `BLENDER_USER_RESOURCES`. The plugin starts Blender with `c4d_bridge/main.py` from there as `--python` script; with `--factory-startup` the add-on itself is not loaded, and need not be. Its `__init__.py` registers only a line of explanation for Blender's Preferences. [T: installed into a throw-away profile; self-test and a render through it]
- `main.py` is the script Blender runs. Its arguments after `--`: `--handshake <file>`, `--token <text>`, and `--show-window` to keep the window visible for debugging. It refuses Blender versions outside `SUPPORTED_BLENDER` (`__init__.py`) by writing an error into the handshake file and exiting with code 4.
- `server.py` holds the `Bridge` class: a socket thread that only queues frames, and handlers that run on the main thread. A background Blender serves from a blocking loop (`run_blocking`); a windowed Blender serves from a `bpy.app.timers` callback plus the 2 ms window timer (`run_on_timer`). Add an operation by adding a method and an entry in `self.handlers`; a handler returns `(reply fields, reply blobs)` or raises `RequestError(code, message)`.
- `protocol.py` encodes and decodes frames and imports no `bpy`, so tests use it directly.
- From launch to an answered `hello`: 2.3 s in background mode, 2.9 s windowed. A `ping` carrying 1 MiB takes about 1 ms in background mode and 3 ms windowed.
- In background mode the script ends with `sys.exit(code)` and Blender exits with that code. In windowed mode it calls `bpy.ops.wm.quit_blender()` after `shutdown` (exit code 0) and `os._exit(code)` otherwise.
- To refuse a client and still deliver the error reply, close only the sending side (`sock.shutdown(SHUT_WR)`); closing the socket outright while the reader thread is blocked on it resets the connection and the client never sees the reply.
- `scene/` builds scene items; `scene/__init__.py` holds the `Scene` class and the `BUILDERS` table, one module per kind next to it (`meshes`, `objects`, `cameras`, `lights`, `materials`, `world`). Items are kept by the plugin's ids, so sending an id again replaces that item; `Scene.clear()` removes everything, including Blender's start-up cube. A new kind is one builder function plus an entry in `BUILDERS`. A builder reports problems that should not stop the render with `scene.warn(text)`. [T: `tests\bridge\test_scene.py`]
- `scene/materials.py` builds materials and the world from node graphs described by the plugin in Blender's own names (`build_nodes`); it knows nothing about Cinema 4D. Materials are assigned on the object (`slot.link = 'OBJECT'`), so objects sharing a mesh can differ. [T]
- `render/settings.py` applies `settings` requests: engine, device, working colour space, and `[path, value]` pairs set with `scene.path_resolve`. `render/devices.py` enables a GPU back end. [T]
- `final/__init__.py` is the final driver: it sets the resolution, runs `bpy.ops.render.render()` and returns the pixels, flipped so the top row comes first. Cycles: the `Combined` pass captured by the wrapped `CyclesRender.render_frame_finish`. Eevee: rendered to an uncompressed 32-bit EXR in a temporary folder and read back with the bundled `OpenImageIO`; the file's linear colour space is set to the working space so values come back unchanged (0.8, 0.2, 0.1 in ACEScg reads back as 0.7998, 0.2, 0.1). A 160×90, 32-sample frame takes well under a second. Passes and progress events are not wired up yet. [T]
- The window is hidden right at start (`main.py`). The live viewport is `viewport/`; see "The viewport driver" below.
- Scene items are updated in place where possible, because other things point at them: a mesh keeps its datablock and its material slots (clearing `mesh.materials` empties the slots of every object that shows the mesh [T]); a camera keeps its object and data; an object whose mesh is unchanged only gets its matrix and materials set. `remove` deletes objects before meshes: removing a mesh that an object still uses takes the object along and leaves a dead reference [T].

## The viewport driver

`bridge/c4d_bridge/viewport/` is the live render of the IPR: `__init__.py` (the `Viewport` class: set-up, the `view_` operations, the draw callbacks), `frame_block.py` (shared-memory writer, no `bpy`), `window.py` (hide, resize, process priority through Win32). Tested by `tests\bridge\test_viewport.py`. What building it taught, all [T] on 5.2.3:

- **A property set from Python does not restart the viewport render.** After a `settings` request the bridge calls `scene.update_tag()`; without it a changed `cycles.preview_samples` is ignored until something else changes.
- **Cycles' "Rendering Done" is not trustworthy right after a change.** The text belongs to the render thread, which picks a change up a moment later (a resize is also held back 0.2 s), so for a while the viewport still says "done" about the previous render, with a black picture after a resize. The driver therefore reports "done" only when 0.6 s have passed since the last change (`_SETTLE_SECONDS`), and asks for one more redraw at that time in case the change restarted nothing.
- **Start in the right size.** `view_start` resizes the window and switches to Rendered only once the region has the new size. Switching at once renders the old size first, restarts, and for a session being created that restart took longer than the settle time.
- **Adaptive sampling ends a viewport render early.** With `cycles.use_preview_adaptive_sampling` on (Blender's default) a simple scene reports "Rendering Done" after a few samples whatever `preview_samples` says. The plugin sets it from the same Cinema 4D setting as final-frame adaptive sampling.
- **Cycles' viewport shows no negative values in data passes:** a face with normal −Y is black in the `NORMAL` pass; Eevee shows the negative value.
- **The viewport's own view can stand in for a camera** (`view_camera`). With `view_perspective = 'PERSP'` Blender treats the view as a 36 mm sensor fitted to the larger side of the region, at half the nominal focal length (`BKE_camera_params_from_view3d`, zoom 2). So a camera with lens L and sensor width S spanning the image width is matched by `space.lens = 2 · L · 36 / S`, times width/height for a tall image (`free_view_lens`). `space.lens` accepts 1–250 mm. `region_3d.view_matrix` is the inverse of the camera's world matrix. Pictures through the view and through the camera object differ by 0.0–0.6 %.
- **Process priority.** The viewport Blender sets itself to below-normal priority: a CPU render uses every core, and Cinema 4D's interface stuttered without this.
- **Draw callbacks may register timers** (`bpy.app.timers.register`), which is how the driver schedules its late redraw.
- **Timing, 480×270, CPU:** first finished 32-sample picture 0.7–0.8 s after `view_start`; an object edit reaches the frame block in 30–95 ms as seen by the pure-Python test reader, which is itself slow.

- **A view set during a resize can come out shifted.** Cycles takes a new region size a moment after the region has it. When `view_camera` arrives in between, the finished picture is sometimes (2 of 16 tries) the right picture drawn around the centre of the old size, off by half the size difference. The driver sets the view once more 0.25 s after the region has reached its new size (`_after_resize`: a change of 0.001 % in the lens, alternating in sign); 60 of 60 tries were right with it.

## Main loop, timers and threads

- In `-b` mode there is no event loop: `bpy.app.timers`, modal operators and render jobs never run, and `render.render()` blocks. [S]
- In windowed mode timers run once per loop iteration. The loop sleeps 5 ms when idle. [S]
- `context.window_manager.event_timer_add(0.002, window=win)` shortens that sleep to about 2 ms. Measured over a socket, it cut the median edit-to-frame round trip from 18.0 ms to 12.6 ms (Cycles on GPU). [T] It makes the loop spin continuously, so remove it (`event_timer_remove`) while the IPR is stopped or paused.
- A worker thread can also wake the loop with `PostMessageW(hwnd, WM_NULL, 0, 0)`. Without the timer this saved about 3 ms; with the timer running it saved under 1 ms, within noise. [T]
- A socket thread plus a `queue.Queue` drained by a `bpy.app.timers` callback works as the command path; commands waited about 1 ms in the queue. The bridge does this in `bridge/c4d_bridge/server.py`. [T]
- Inside a timer `area` and `region` are None. [S] Get the window from `bpy.context.window_manager.windows[0]` and run operators inside `with bpy.context.temp_override(window=win, area=area, region=region):`. [T]
- All `bpy` work happens on the main thread. A socket thread only moves bytes into a `queue.Queue`. [S]
- All edits made in one timer call produce one scene evaluation and one render restart, provided the code does not force evaluation (`evaluated_depsgraph_get()`, `view_layer.update()`). [S]
- `bpy.ops.wm.quit_blender()` from a timer exits cleanly; set `preferences.view.use_save_prompt = False` first. [T]

## Window and viewport setup

Sequence that works [T]:

1. `preferences.view.show_splash = False`; `preferences.view.render_display_type = 'NONE'`.
2. Find the `VIEW_3D` area and its `WINDOW` region; inside `temp_override` call `bpy.ops.screen.screen_full_area(use_hide_panels=True)`.
3. **In a later timer call**, find the area again (the screen was swapped). The region is now exactly the window's client size.
4. On the space: `overlay.show_overlays = False`, `show_gizmo = False`, `shading.use_scene_lights_render = True`, `shading.use_scene_world_render = True`, `shading.type = 'RENDERED'`.
5. On `space.region_3d`: `view_perspective = 'CAMERA'`, `view_camera_zoom = 50 * (2 - sqrt(2))` (≈ 29.2893), `view_camera_offset = (0, 0)`. With the render aspect equal to the region aspect, the camera frame then fills the region exactly. [T: viewport frame matched the final render pixel for pixel]
6. `scene.render.preview_pixel_size = '1'` forces one render pixel per region pixel regardless of display scaling.

Window control (Windows) [T]:

- Blender's window class is `GHOST_WindowClass`. Find the handle with `EnumWindows` filtered by the Blender process id.
- A client size as small as 320×180 works with `SetWindowPos`, below the 640×480 minimum of the `-p` argument. [T]
- `ShowWindow(hwnd, SW_HIDE)` hides it and rendering continues. Blender skips drawing only for *minimized* windows, so keep it un-minimized.
- `Window.x/y/width/height` are read-only in Python. [S] Resize with Win32 `SetWindowPos`; Blender follows the external resize and the region stays equal to the client area. [T] Cycles debounces a resize by 0.2 s. [S]
- Pass `SWP_NOSENDCHANGING` (0x0400). Without it Windows clamps the window to the desktop (a 1920×1080 request on a 1600×900 desktop gave 1604×881). With it the client size is exact and can exceed the desktop: 3840×2160 rendered and read back correctly on a 1600×900 desktop, for both engines. [T]
- Compute the outer window size from the difference between `GetWindowRect` and `GetClientRect` (`set_client_size` in `bridge/c4d_bridge/viewport/window.py`). [T]
- After a resize set the render resolution to the region size again, so the camera frame keeps filling the region. [T]
- Readback cost grows with pixel count: about 1.3 ms at 960×540, 3–6 ms at 1600×881, 20–23 ms at 3840×2160. [T]
- A DPI change makes Blender resize its own window; keep it on one monitor. [S] Display scaling other than 100 % is untested.

## Reading viewport pixels

- The viewport render texture is RGBA16F and holds scene-linear values. View transform, look, exposure, gamma and dither are applied later, when Blender composites to its window. (`source/blender/gpu/intern/gpu_viewport.cc`, `source/blender/draw/engines/external/external_engine.cc`) [S, confirmed T]
- Row order is the same as `RenderPass.rect` from a final render. [T] Both are bottom-up. [S]
- Values are in the file's working colour space. [T] Alpha is premultiplied [S]; untested, because every test scene had alpha 1.

**Route A — draw handlers, both engines** [T]:

```python
_fb = [None]
def pre_view():  _fb[0] = gpu.state.active_framebuffer_get()
def post_view():
    fb, region = _fb[0], bpy.context.region
    with fb.bind():
        fb.read_color(0, 0, region.width, region.height, 4, 0, 'FLOAT', data=buf)
bpy.types.SpaceView3D.draw_handler_add(pre_view, (), 'WINDOW', 'PRE_VIEW')
bpy.types.SpaceView3D.draw_handler_add(post_view, (), 'WINDOW', 'POST_VIEW')
```

**Route B — wrap Cycles' draw method** [T]: assign a wrapper to `cycles.CyclesRender.view_draw` that calls the original, then reads `gpu.state.active_framebuffer_get()` the same way. Blender looks the method up on every call, so the wrapper takes effect immediately. Route B is also where the engine object (`self`) is available for the status text.

Both routes returned identical pixels on every frame.

**Zero-copy into shared memory** [T]:

```python
mm  = mmap.mmap(-1, w * h * 16, tagname="<name>")
arr = numpy.frombuffer(mm, dtype=numpy.float32)
buf = gpu.types.Buffer('FLOAT', (h, w, 4), arr)     # wraps arr, no copy
```

- Read pixels from `arr`. Converting a multi-dimensional `gpu.types.Buffer` with `numpy.asarray` returns scrambled data. [T]
- Cost at 960×540: 1.2–1.7 ms median, occasional spikes to 10–35 ms. [T]
- `read_color(x, y, w, h, channels, slot, format, data=)`; formats `'FLOAT'`, `'UBYTE'`, `'INT'`, `'UINT'`. [S]

Rules inside draw callbacks:

- Leave scene data untouched there: Blender rejects writes to scene data during drawing. Make edits from timers. [S]
- Keep the engine object out of long-lived variables, or its session is never freed. [S]

What to expect in the frames:

- After a restart Cycles first draws low-resolution frames scaled up, then full resolution. [S]
- A redraw without a new sample redraws the last image, so frames can repeat. Tag frames with an id. [S]
- Eevee redraws continuously while shaders compile. The first non-black frame of the default scene arrived 8.9 s after start on the first 5.0 run and 0.33 s on 5.2.3, so first-frame time varies widely with shader compilation and caching. [T]

## Cycles status text

- Cycles writes `Sample N/M`, `Rendering Paused`, `Rendering Done`, `Updating Shaders` into `RenderEngine.text`. Blender exposes no Python property for it. [S]
- Read it with `ctypes.string_at(engine.as_pointer() + 64, 512).split(b"\0", 1)[0]` inside the `view_draw` wrapper. Offset 64 was found by scanning on both 5.0 and 5.2.3 [T] and matches the struct in `source/blender/render/RE_engine.h`.
- Validate at start-up by scanning offsets 0–512 for a printable string matching a known status; refuse to guess.
- The text updates on change or once per second, so sample numbers skip. [T]
- Fallback: `--log cycles --log-level info --log-file <f>` emits "Rendered N samples". [S]
- Eevee exposes no sample count. Count redraws since the last restart; it has converged when redraws stop. [S]

## Viewport controls

- Cycles viewport properties on `scene.cycles`: `preview_samples` (0 = unlimited), `use_preview_adaptive_sampling`, `preview_adaptive_threshold`, `use_preview_denoising`, `preview_denoiser`, `preview_denoising_start_sample`, `preview_denoising_input_passes`, `preview_pause`. [S; `preview_samples`, `preview_pause` T]
- `preview_pause` keeps buffers and continues accumulating on resume. Its update callback needs an area in context, so set it inside `temp_override`. [T]
- **Cycles viewport pass:** set `space.shading.cycles.render_pass`, then call `window.screen.update_tag()`. Setting the property from Python notifies nobody; `area.tag_redraw()` alone leaves the old pass on screen. [T] Ids that switch correctly [T]: `COMBINED`, `EMISSION`, `BACKGROUND`, `AO`, `SHADOW_CATCHER`, `DIFFUSE_DIRECT`, `DIFFUSE_INDIRECT`, `DIFFUSE_COLOR`, `GLOSSY_DIRECT`, `GLOSSY_INDIRECT`, `GLOSSY_COLOR`, `TRANSMISSION_DIRECT`, `TRANSMISSION_INDIRECT`, `TRANSMISSION_COLOR`, `VOLUME_DIRECT`, `VOLUME_INDIRECT`, `POSITION`, `NORMAL`, `UV`, `MIST`, `DENOISING_ALBEDO`, `DENOISING_NORMAL`, `SAMPLE_COUNT`. There are no AOV or light-group items. [S]
- **Eevee viewport pass:** set `space.shading.render_pass`; that alone restarts the render. Ids that switch correctly [T]: `COMBINED`, `EMISSION`, `ENVIRONMENT`, `AO`, `SHADOW`, `TRANSPARENT`, `DIFFUSE_LIGHT`, `DIFFUSE_COLOR`, `SPECULAR_LIGHT`, `SPECULAR_COLOR`, `VOLUME_LIGHT`, `POSITION`, `NORMAL`, `MIST`. AOV and crypto items also exist. [S]
- Data passes come back as raw values, e.g. `NORMAL` reads (1, 0, 0) on a face pointing along +X. [T]
- Set either property inside `temp_override(window, area, region)`. The bridge does so in `bridge/c4d_bridge/viewport/__init__.py`.
- Setting `shading.type` to anything but `'RENDERED'` frees the engine session and its GPU memory. Changing `scene.render.engine` restarts viewport renders. [S]
- Eevee engine id is `'BLENDER_EEVEE'` on 5.0 and 5.2 [T]; viewport sample count is `scene.eevee.taa_samples` (0 = unlimited).
- Cycles viewport renders no motion blur. Depth of field works in camera view, as does `render.use_border`. [S]
- Changing samples does not rebuild the session; changing device, pixel size or shading system does. [S]

## What a live scene edit costs

Measured with a prototype script on 5.2.3, hidden, 960×540, Cycles on OptiX. [T]

- Creating a 1,000,000-quad mesh with `foreach_set` and `mesh.update(calc_edges=True)` takes 0.41–0.46 s; the first frame follows about 0.1 s later.
- Replacing all positions of that mesh (`vertices.foreach_set("co", …)`, `mesh.update()`) takes about 0.1 s to apply and 0.3 s to the first frame.
- With only that mesh in the scene, a camera-object move reaches pixels in 14 ms: polygon count alone does not slow edits.
- With 100,000 Geometry Nodes instances added, **every** scene edit costs 0.16–0.21 s before the first frame, including a camera-object move, because Cycles walks all object instances on each scene update.
- Changing the viewport's own view (`region_3d.view_distance`, `view_location`, `view_rotation`, `view_matrix` with `view_perspective = 'PERSP'`) changes no scene datablock and reaches pixels in 18 ms in the same scene. Depth of field and camera-only features need camera view.
- Eevee redraws all instances for every sample: about 0.07–0.15 s per update and 2 s for 32 samples with 100,000 instances.

## Viewport versus final-render properties

The viewport uses viewport-facing settings. Set both sides from the same value. [S]

| Purpose | Viewport | Final |
|---|---|---|
| Object visibility | `hide_viewport`, collection hide | `hide_render` |
| Modifiers | `show_viewport` | `show_render` |
| Subdivision | `levels` | `render_levels` |
| Cycles samples | `cycles.preview_samples` | `cycles.samples` |
| Eevee samples | `eevee.taa_samples` | `eevee.taa_render_samples` |
| Denoising | `cycles.use_preview_denoising` | `cycles.use_denoising` |
| Lights and world | `shading.use_scene_lights_render`, `use_scene_world_render` | always scene |

## Final frames in background mode

- `bpy.ops.render.render()` blocks until the frame is done. [T]
- **Cycles pixels in memory** [T]: wrap `cycles.CyclesRender.render_frame_finish`; after the original call:
  ```python
  result = self.get_result()
  p = result.layers[0].passes.find_by_name("Combined", "")
  pixels = numpy.empty(result.resolution_x * result.resolution_y * p.channels, numpy.float32)
  p.rect.foreach_get(pixels)      # rows bottom-up
  ```
- `bpy.data.images['Render Result'].pixels` is always empty. [S]
- **Eevee pixels, combined only** [T]: `image_settings.file_format = 'OPEN_EXR'`, `color_depth = '32'`, `exr_codec = 'NONE'`, `render.render(write_still=True)`, then `bpy.data.images.load(path)` and `pixels.foreach_get`.
- OpenImageIO: `ImageInput.read_image(subimage, miplevel, chbegin, chend, format)`; the first argument chooses the part, so pass the part's index or every pass reads the first part's pixels. [T]
- **Eevee pixels, all passes** [T on 5.2.3]: set `image_settings.media_type = 'MULTI_LAYER_IMAGE'`, then `file_format = 'OPEN_EXR_MULTILAYER'`, and render with `write_still=True`. The file is a **multi-part** EXR with one part per pass; channels are named `ViewLayer.<Pass>.<C>`. Read it with the bundled `OpenImageIO` module, stepping through parts with `ImageInput.seek_subimage(i, 0)`; rows are top-down, so flip them. Reading 17 parts at 960×540 took 78 ms.
- Enable passes with `view_layer.use_pass_z`, `use_pass_mist`, `use_pass_position`, `use_pass_normal`, `use_pass_vector`, `use_pass_uv`, `use_pass_object_index`, `use_pass_material_index`, `use_pass_diffuse_direct`/`_indirect`/`_color` (same for `glossy`, `transmission`), `use_pass_emit`, `use_pass_environment`, `use_pass_ambient_occlusion`, `use_pass_cryptomatte_object`; Cycles extras on `view_layer.cycles`: `denoising_store_passes`, `pass_debug_sample_count`, `use_pass_shadow_catcher`, `use_pass_volume_direct`; custom AOVs with `view_layer.aovs.add()`. [T]
- **Cycles pass names and channel counts as read in memory** [T]: `Combined`[4], `Depth`[1], `Mist`[1], `Position`[3], `Normal`[3], `Vector`[4], `UV`[3], `Object Index`[1], `Material Index`[1], `Diffuse Direct`[3], `Diffuse Indirect`[3], `Diffuse Color`[3], `Glossy Direct`[3], `Glossy Color`[3], `Transmission Direct`[3], `Volume Direct`[3], `Emission`[3], `Environment`[3], `Ambient Occlusion`[3], `Shadow Catcher`[3], `Debug Sample Count`[1], `CryptoObject00`–`02`[4], `Denoising Albedo`[3], `Denoising Specular Albedo`[3], `Denoising Normal`[3], `Denoising Roughness`[1], `Denoising Depth`[1], and each AOV by its own name [4].
- **Eevee pass names in the EXR** [T]: `Combined`, `Depth`, `Mist`, `Normal`, `Position`, `Vector`, `Diffuse Direct`, `Diffuse Color`, `Glossy Direct`, `Glossy Color`, `Emission`, `Environment`, `Ambient Occlusion`, `CryptoObject00`–`02`, and each AOV by name.
- `render.use_persistent_data = True` keeps Cycles' session, BVH and loaded textures between renders. Switching engine or rebuilding the depsgraph frees it. [S]
- Progress: `bpy.app.handlers.render_stats` receives one status string per update. [S]
- Cancel: the only route is `signal.raise_signal(signal.SIGINT)`, which works once per process; a second one exits Blender. Terminate the process instead. [S]
- Optional progressive final render: `cycles.use_sample_subset`, `sample_offset`, `sample_subset_length` render one slice of the sample sequence per call; the sample-weighted average of the slices equals one full render when adaptive sampling is off. [S]

## Building scene data fast

- Mesh fill order: `vertices.add`, `loops.add`, `polygons.add`; `attributes["position"].data.foreach_set("vector", …)`; `polygons.foreach_set("loop_start", …)` then `foreach_set("vertices", …)`; `mesh.update(calc_edges=True)`. Pattern: `from_pydata` in `scripts/modules/_bpy_types.py`. [S]
- `foreach_set`/`foreach_get` take numpy arrays directly when the dtype matches (float32, int32, bool). [T for `foreach_get`]
- Normals: a `FLOAT_VECTOR` attribute named `custom_normal` on the `CORNER` domain sets shading normals directly, in the mesh's own axes: `mesh.attributes.new("custom_normal", 'FLOAT_VECTOR', 'CORNER').data.foreach_set("vector", values)` before `mesh.update()`. Tilting them by 60° halves a plane's diffuse brightness, as it must. `use_auto_smooth` no longer exists. [T]
- UVs: `uv_layers.new(name=…, do_init=False)`, then `layer.uv.foreach_set("vector", …)` [T]. Colours: `color_attributes.new(name, 'FLOAT_COLOR', 'POINT' | 'CORNER')`. Vertex maps: a float `POINT` attribute, read in shaders with the Attribute node. [S]
- Instancing at scale, as built in `scene/instancers.py` [T, `tests\bridge\test_geometry.py`]: a mesh of loose vertices with a `FLOAT4X4` point attribute (`c4d_transform`) and a `FLOAT_COLOR` point attribute (`mograph_color`); a Geometry Nodes modifier with Object Info (As Instance) → Instance on Points → Set Instance Transform fed by a Named Attribute node of type `FLOAT4X4`. Point attributes travel to the instances, so a shader reads the colour with an Attribute node of type `INSTANCER`, in Cycles and in Eevee.
  - Do not call the matrix attribute `instance_transform`: that is the instances' own built-in attribute, and a point attribute of that name is dropped (every instance then sits unrotated and unscaled at its point).
  - `foreach_set("value", …)` on a `FLOAT4X4` attribute takes each matrix column by column.
  - The prototype object need not be in any collection: Object Info still instances it, with the materials of its object-linked slots, and it does not render by itself.
  - 100,000 instances are built in 0.02–0.05 s.
- Mesh attributes [T]: `mesh.attributes.new(name, 'FLOAT', 'POINT')` with `foreach_set("value", …)`; `'FLOAT_COLOR'` on `'POINT'` or `'CORNER'` with `foreach_set("color", …)`. An Attribute node of type `GEOMETRY` reads either; its Color output gives a float attribute as grey. `uv_layers[i].active_render = True` chooses the set image textures use without a UV Map node. `polygons.foreach_set("material_index", …)` assigns slots.
- Changing only positions: `vertices.foreach_set("co", …)`, the `custom_normal` attribute's `foreach_set`, then `mesh.update()`; no need to rebuild the topology. `clear_geometry()` on a mesh keeps its material slots but not its attributes.
- Images from memory: `bpy.data.images.new(name, w, h, float_buffer=True)` and `pixels.foreach_set`. [S]

## Scene property notes

- Light brightness, measured on a white diffuse surface facing the light [T: `tests/bridge/measure_lights.py`]: sun of strength S gives S / π; point or spot of power P at d metres gives P / (4 π² d²); a small area light gives P / (π² d²) on its axis. `use_shadow = False` lets a light shine through objects.
- Lights: `type` `POINT`/`SUN`/`SPOT`/`AREA`; `energy` in watts (sun: W/m²); `spot_size`, `spot_blend`; area `shape`, `size`, `size_y`. Light linking: `object.light_linking.receiver_collection` / `blocker_collection`. [S]
- Ray visibility: `visible_camera`, `visible_diffuse`, `visible_glossy`, `visible_transmission`, `visible_volume_scatter`, `visible_shadow`; `is_holdout`, `is_shadow_catcher`. [S]
- "Indirect only" (`LayerCollection.indirect_only`) is nothing but "not seen by the camera" in Cycles: `intern/cycles/blender/object.cpp` clears `PATH_RAY_VISIBILITY_CAMERA` for it and does nothing else. So `visible_camera = False` on an object is the same thing, per object. Holdout and shadow catcher are object properties and work per object. [T: `tests/c4d/object_tag.py` for holdout and camera; S for the source]
- Principled Volume: the `Temperature` input (1000) times the temperature attribute is the black body's kelvin. A fire grid in the range 0 to 4 (EmberGen's `flames`) barely glows that way; for such a grid an Attribute node's `Fac` drives `Emission Strength`, and a Blackbody node at `Fac` times kelvin gives `Emission Color`. [T: `tests/c4d/fire_vdb.py`]
- A volume object shows the file's grids at the file's own coordinates (its transform included), in the object's axes: a .vdb written Y-up lies along Blender's Y until the object matrix swaps the axes. [T]
- `ShaderNodeMix` has several inputs and outputs of one name: address them by identifier (`Factor_Float`, `A_Color`, `B_Color`, `Result_Color`); a Math node's inputs are `Value`, `Value_001`, `Value_002`. A Light Path node's `Is Shadow Ray` and `Is Diffuse Ray` let a material be something else for shadow and gathering rays. [T: `tests/c4d/redshift_material.py`]
- Every scene property of the settings page was read from Blender 5.2.3 with its default and its options (`scene.path_resolve(owner).bl_rna.properties[name]` gives `default`, `hard_min`, `hard_max` and `enum_items`); `cycles.denoiser`, `preview_denoiser` and `sampling_pattern` list only what this machine offers. Eevee's `ray_tracing_options` has no `screen_trace_precision` in 5.2. [T: `tests/c4d/settings.py` sets all 101]
- Camera: `lens`, `sensor_width`, `sensor_fit`, `shift_x`, `shift_y`, `clip_start`, `clip_end`, `ortho_scale`, `type` `PERSP`/`ORTHO`/`PANO`; `dof.use_dof`, `dof.focus_distance`, `dof.aperture_fstop`. [S]
- Principled BSDF inputs: Base Color, Metallic, Roughness, IOR, Alpha, Normal, Subsurface Weight, Specular IOR Level, Specular Tint, Anisotropic, Transmission Weight, Coat Weight, Coat Roughness, Sheen Weight, Emission Color, Emission Strength, Thin Film Thickness. Use `ShaderNodeMix`, not the legacy MixRGB. [S]
- World panorama: `ShaderNodeTexEnvironment` puts the image centre towards +X and runs on towards −Y. A Mapping node (type `POINT`) between `ShaderNodeTexCoord`'s `Generated` output and the texture rotates or mirrors it. [T]
- Node sockets are best named by identifier: a Mix node in colour mode has `Factor_Float`, `A_Color`, `B_Color`, `Result_Color`; Math has `Value`, `Value_001`; Vector Math has `Vector`, `Vector_001`, `Vector_002`. A colour output linked to a float input is converted to its brightness. [T]
- `use_nodes` on materials, worlds and lights is deprecated and always on. `Material.displacement_method` lives on the material. [S]
- Volumes: `bpy.data.volumes.new()`, `filepath`, `is_sequence`, `frame_start`, `frame_duration`, `sequence_mode` [S]. Grids cannot be created in memory from Python; write `.vdb` files. As built in `scene/volumes.py` [T]: a volume with a file path, an object with a matrix (a mirroring matrix is fine), and a material slot holding a Principled Volume whose "Density Attribute" names the grid. `volume.render.space` is `'OBJECT'` by default, which scales density with the object: a file in centimetres shown at scale 0.01 was a hundred times too thick; `'WORLD'` takes density per metre. The Principled Volume's emission lights every active voxel of the grid, whatever the density. Eevee renders the same volume with a softer edge.
- Hair curves [T]: `bpy.data.hair_curves.new(name)`, `curves.add_curves([points per curve, …])`, `curves.points.foreach_set("position", …)`, a float `POINT` attribute `radius`. Point clouds [T]: `bpy.data.pointclouds.new(name)`, `cloud.resize(n)`, `cloud.points.foreach_set("co" | "radius", …)`, a `FLOAT_COLOR` point attribute read by an Attribute node of type `GEOMETRY`. Cycles renders the points as spheres.
- Light linking [T]: `light_object.light_linking.receiver_collection` and `.blocker_collection` take a collection; an entry's `collection.collection_objects[i].light_linking.link_state = 'EXCLUDE'` turns "only these" into "all but these". Ray visibility [T for camera and shadow]: `object.visible_camera`, `visible_shadow`, `visible_diffuse`, `visible_glossy`, `visible_transmission`.
- Motion blur [T, `tests\c4d\motion_blur.py`]: `render.use_motion_blur`, `motion_blur_shutter` (in frames; the interface offers up to 1, the property accepts more), `motion_blur_position`. Cycles asks the scene for the frame at shutter open, centre and close (`RE_engine_frame_set`) and reads every object's and the camera's matrix there, so an object must be animated in Blender to blur: three keys on location, quaternion rotation and scale. With a shutter of 2 and keys on the frames before and after, the samples fall exactly on the keys. A new object needs no animation check: Cycles samples all of them.
- Deformation blur in Cycles [T]: a `POINT` float-vector attribute named `velocity`, in units per second. Cycles places each point at position ± velocity × shutter / (2 × fps). Eevee does not read it [T]; it blurs moving objects only.
- `render.use_persistent_data = True` with one changed object per frame rendered 50 frames correctly [T, `sequence.py`].
- Region [T]: `render.use_border` with `border_min_x` … as fractions from the lower left corner; `use_crop_to_border = False`. For the viewport: `space.use_render_border` and `space.render_border_min_x` … for the viewport's own view, the scene's border for the camera view. The viewport then renders the region only and leaves the rest of its framebuffer as it was.
- Denoising [T]: `cycles.denoiser` / `preview_denoiser` (`OPENIMAGEDENOISE`, `OPTIX`; setting an option the machine lacks raises `TypeError`), `cycles.denoising_input_passes` / `preview_denoising_input_passes` (`RGB`, `RGB_ALBEDO`, `RGB_ALBEDO_NORMAL`).
- Working colour space is per file: read `bpy.data.colorspace.working_space` (default `Linear Rec.709`), change with `bpy.ops.wm.set_working_color_space(working_space='ACEScg', convert_colors=False)`. Call it before the viewport starts rendering: it stops all viewport renders and cancels while a job is running or an image is unsaved. [T]
- Colour values pass through numerically unchanged in the working space: an emission colour of (0.8, 0.2, 0.1) reads back as (0.8, 0.2, 0.1) from both the viewport and a final render, in `Linear Rec.709` and in `ACEScg`. The viewport value is rounded to half-float (0.7998). [T]
- `scene.view_settings.view_transform` (`Standard`, `AgX`, `Filmic`, `Raw`, …) does not affect the readback. A custom OCIO config is set with the `BLENDER_OCIO` environment variable at launch (untested). Texture colour space: `image.colorspace_settings.name`. [S]
- `bpy.data.images.load(path, check_existing=True)` hands back the image already loaded from that file, and a colour space set on it applies to everyone who uses it. Two materials reading one PNG as `sRGB` and as `Linear Rec.709` (a bitmap set to the Linear profile in a "Basic" document) then both rendered with the colour space set last [T: `tests/c4d/colour.py`, strips "PNG sRGB 128" and "PNG 128, bitmap profile Linear", 0.737 instead of 0.502 before the change]. The bridge loads one image per `image` item (`check_existing=False`, `scene/materials.py`); the plugin sends one item per file and colour space.
- Adaptive subdivision is on the Subdivision Surface modifier (`use_adaptive_subdivision`). [S]

## The curve nodes (`CurveMapping`)

RGB Curves (`ShaderNodeRGBCurve`), Vector Curves (`ShaderNodeVectorCurve`) and Float Curve (`ShaderNodeFloatCurve`) keep their curves in `node.mapping`, a `CurveMapping`. Built by `_set_mapping` in `bridge/c4d_bridge/scene/materials.py` [T: `tests/bridge/test_scene.py`, `check_curve_mapping`, in Cycles and Eevee; `tests/c4d/cycles_nodes.py`; and a measuring script, Blender 5.2.3, 2026-11-12].

- **A new node** [S: `node_shader_curves.cc`, `BKE_curvemapping_set_defaults` in `blenkernel/intern/colortools.cc`; T]: `mapping.curves` holds 4 curves for RGB Curves (R, G, B, then C, the combined one), 3 for Vector Curves (X, Y, Z), 1 for Float Curve; each two points with handle `AUTO` in the corners of the area, 0 to 1, or -1 to 1 for Vector Curves; the clipping rectangle is that area, `use_clip` is on and `extend` is `EXTRAPOLATED`.
- **Points:** `curve.points.new(x, y)` inserts a point where its x belongs, selected and with the curve's default handle; `curve.points.remove(point)` refuses to go below two points (`BKE_curvemap_remove_point`); `point.location`, `point.handle_type` (`AUTO`, `AUTO_CLAMPED`, `VECTOR`; a point has no tangents of its own), `point.select`. [S, T]
- **`mapping.update()`** (`BKE_curvemapping_changed_all`) sorts the points of each curve by x (stable) and builds its table; with `use_clip` on it first moves the **selected** points into the clipping rectangle, all by the same amount. So the bridge deselects every point before `update()`, and the plugin sends a rectangle around all its points. It removes no doubles (only Blender's editor asks for that). [S]
- **`clip_min_x`, `clip_min_y`, `clip_max_x`, `clip_max_y`** lie within -100 to 100, and each is bounded by its opposite (`clip_min_x` up to `clip_max_x`): set the outer limits first. [S: `rna_color.cc`]
- **How a curve is evaluated** [S]: the points become Bezier segments (handles from `AUTO`, `AUTO_CLAMPED` or `VECTOR`, `calchandle_curvemap`), drawn with 32 points each, then resampled into a table of 256 even steps over the clipping rectangle widened to the points (`curvemap_make_table`). `mapping.evaluate(curve, x)` (`BKE_curvemap_evaluateF`) reads that table, and outside it continues along the first or last handle (`EXTRAPOLATED`) or flat (`HORIZONTAL`); it does not clamp to the rectangle. Cycles samples `BKE_curvemap_evaluateF` again in 256 steps between the first and the last point of the curves (`curvemapping_float_to_array`, `curvemapping_color_to_array` in `intern/cycles/blender/util.h`) and continues the last step's slope when `extend` is `EXTRAPOLATED` (`float_ramp_lookup`, `kernel/svm/ramp.h`). RGB Curves apply C first, then R, G and B (`R(C(x))`).
- **Precision** [T]: `VECTOR` points give straight segments, so a curve sent as many points follows them exactly up to the table: a corner is rounded by up to a quarter step times the change of slope (0.0028 for slopes 2 and -1 over 0 to 1). Two points `(0, 0)` and `(1, 1)` with `VECTOR` handles evaluate exactly as a new node's two `AUTO` points (difference 0.0 at 1001 positions; the same for -1 to 1).
- **Constant inputs are folded with a clamp** [T: rendered, Cycles; S: `nodes/intern/shader_nodes_inline.cc`, `node_shader_curves.cc`]: Blender 5.2 evaluates a node whose inputs are all constants itself, with the node's multi-function, before Cycles sees the material. Float Curve's multi-function calls `BKE_curvemapping_evaluateF`, which clamps the result to the clipping rectangle while `use_clip` is on: a new Float Curve at a constant 1.5 renders 1.0, whatever `extend` says, although `evaluate()` gives 1.5 and Cycles' own table extends the line for an input that varies. RGB and Vector Curves' multi-functions do not clamp. The plugin sends Blender's rectangle for an untouched curve, so its renders keep this behaviour.
- **What shader nodes ignore** [S]: `tone`, `black_level` and `white_level` of RGB Curves (Cycles does not read them; Eevee passes black 0 and white 1, "Shader nodes don't do white balancing"). Vector Curves' `Vector` input is declared from -1 to 1.

## Devices

With `--factory-startup` no GPU device is enabled. On each launch [T: OptiX on an RTX 4090, viewport rendering, OpenGL and Vulkan back ends]:

```python
prefs = bpy.context.preferences.addons['cycles'].preferences
prefs.compute_device_type = 'OPTIX'      # or CUDA, HIP, ONEAPI, METAL, NONE
prefs.refresh_devices()
for d in prefs.devices: d.use = True
bpy.context.scene.cycles.device = 'GPU'
```

Setting `compute_device_type` to a back end the build lacks raises `TypeError`; try `OPTIX`, `CUDA`, `HIP`, `ONEAPI`, `METAL` in turn and keep the first that lists devices (`bridge/c4d_bridge/render/devices.py`).

Alternatively pass `-- --cycles-device OPTIX` on the command line. [S]

`--gpu-backend vulkan` also works for both engines with the window hidden. [T]

## Where things are in the source

- Cycles add-on (class to wrap, properties, pass registration): `intern/cycles/blender/addon/__init__.py`, `properties.py`, `engine.py`.
- Viewport session, status text writer: `intern/cycles/blender/session.cpp`; display: `display_driver.cpp`.
- Viewport draw path: `source/blender/draw/engines/external/external_engine.cc`, `source/blender/draw/intern/draw_context.cc`.
- Python GPU API: `source/blender/python/gpu/gpu_py_framebuffer.cc`, `gpu_py_buffer.cc`, `gpu_py_offscreen.cc`.
- Render pipeline and engine API: `source/blender/render/intern/pipeline.cc`, `engine.cc`; RNA in `source/blender/makesrna/intern/rna_render.cc`.
- Command-line arguments: `source/creator/creator_args.cc`.
- Windows window handling: `intern/ghost/intern/GHOST_SystemWin32.cc`, `GHOST_WindowWin32.cc`.

## A material edited in Blender's Shader Editor

Measured for the Cycles Native Material, all [T: a prototype script, Blender 5.2.3, windowed with the main window hidden]; what the bridge does with it (`bridge/c4d_bridge/scene/native.py`) follows below, [T: `tests/bridge/test_native.py`, `tests/c4d/native_material.py`].

- **One material as bytes:** `bpy.data.libraries.write(path, {material}, fake_user=True, compress=True)` writes a .blend with that material and what it uses; `with bpy.data.libraries.load(path, link=False) as (source, target): target.materials = list(source.materials)` brings it back (`target.materials` then holds the datablocks; assigning the same list object to it replaces the names in that list, so copy it). A material of four nodes with a colour ramp: 63,594 bytes, 11 to 20 ms to write and read back from disk, 14 ms to load, identical afterwards (node types, links, ramp stops, values, `surface_render_method`).
- **A window with only the Shader Editor:** `with bpy.context.temp_override(window=main): bpy.ops.wm.window_new()`; the new window is `wm.windows[-1]`, has one area; `area.ui_type = "ShaderNodeTree"`. The editor shows the active material of the active object of the window's scene: give the window a scene of its own (`window.scene = scene`) with one object carrying the material and make it active (`window.view_layer.objects.active`). `space.edit_tree` is the material's node tree one tick later. The new window is visible although the main window was hidden with `ShowWindow(SW_HIDE)`.
- **Changes:** a `bpy.app.handlers.depsgraph_update_post` handler sees `Material:<name>` and `ShaderNodeTree:...` among `depsgraph.updates` within 1 ms of a changed socket value, a new node with a link, and a changed setting of the material; a moved colour-ramp stop and a moved node report the node tree only. Changes were made from a script; that edits by mouse take the same path is inferred, not seen.
- **Closing:** a WM_CLOSE to the editor window (what its close button sends) removes it from `wm.windows` and leaves Blender running, the main window still hidden, the material as edited. Blender quits only when its last window closes, and the hidden main window counts.
- **`wm.window_new` and `wm.window_new_main` refuse in the IPR's Blender:** their poll wants the active window's screen not to be maximised (`screen->state` normal, or not full), and the IPR's window shows one maximised area. `bpy.ops.screen.area_dupli("INVOKE_DEFAULT")` with `temp_override(window=main, area=..., region=...)` has no such condition and gives a second window with one area. Set its `ui_type` in the same call, before Blender draws: the duplicate of a Rendered viewport would start a render of its own otherwise.
- The editor window: `space.show_region_header = True`, `space.show_region_ui = False`; size, title and focus through Win32 on the new `GHOST_WindowClass` window of the process (`SetWindowPos`, `SetWindowTextW`, `SetForegroundWindow`).
- **A material's preview picture without blocking:** `with bpy.context.temp_override(id=material, window=window): bpy.ops.ed.lib_id_generate_preview()` starts Blender's preview job; some ticks later `material.preview.image_pixels_float` holds 128 x 128 RGBA, the ball over transparency (rows and columns 11 to 115). It renders with the engine of the window's scene: 0.3 s in Cycles, 7.7 s the first time in Eevee (shaders are compiled) [T: a prototype script]. Compare the pixels with the ones before asking to know when the new picture has come.
- **Working space:** a .blend carries its working colour space; `libraries.load` converts a material's colours into the working space of the file it is loaded into, so it looks the same. Setting the working space of a running Blender (`wm.set_working_color_space(convert_colors=False)`, as the bridge does) leaves existing values as they are: set it before a material is made or loaded [T: `native_material.py`, IPR and final render of one material agree to 0.001 only then].
- `bpy.data.libraries.write(..., path_remap="ABSOLUTE")` writes the paths of images as full paths, which another Blender reading the bytes from another folder needs [T: `tests/bridge/test_native.py`, a texture chosen in the editor renders elsewhere].
- A change notice is not the only way to know a material changed: a hash over its nodes (settings, socket values, links, what images, colour ramps and curves hold, node groups) compared twice a second catches whatever gives no notice (`_fingerprint` in `native.py`).
- `bpy.data.libraries.write` of the same material twice does not always give the same bytes; compare a hash only to skip what the bridge itself wrote.
- Datablocks an open editor shows must survive `Scene.clear()`: `bpy.data.batch_remove` gets the list without them (the material, the object and mesh that carry it, images and node groups its tree uses).
- **Undo (Ctrl+Z) in the editor window is Blender's global undo** [T: `tests/bridge/test_native.py`, and by simulated key presses]. It reads every datablock that changed anew: Python references to the material, its scene and its object go stale (`ReferenceError`), while `ID.session_uid` stays the same across undo and redo and finds them again; `bpy.app.handlers.undo_post` and `redo_post` are the moment to do it. Nothing made from Python is an undo step: without `bpy.ops.ed.undo_push()` after the editor is set up, the first Ctrl+Z returns to the state of the start-up file, where the material does not exist. Blender offers no way to cut the history off, so an undo beyond that step is answered with `bpy.ops.ed.redo()`.
- Simulated input for a test: start Blender with `--enable-event-simulate` and call `window.event_simulate(type, value, x=, y=, ctrl=)` from a timer, one event per tick; real input is ignored meanwhile. A node's sockets are found from `node.location` and `region.view2d.view_to_region` (first output 34 units below the node's top, each further row 22).
- A preview picture whose alpha is zero everywhere is what the buffer holds while the preview job runs, and also the finished picture of a material that shows nothing; only time tells them apart.
- `bpy.ops.screen.screenshot(filepath=...)` with `temp_override(window=window)` photographs one window of Blender, also when Blender did not start it from a key press.

