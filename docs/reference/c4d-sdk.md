# Cinema 4D SDK reference

Identifiers, build rules and example paths for the C++ plugin. Entries marked **[T]** were built or run in this project. Everything else was read in the 2027 SDK headers, manuals and examples and is untested. Correct this file when work proves an entry wrong. The tests named as evidence (`tests/...`) are kept locally and are not part of the repository; see `docs/development.md`.

Path aliases:

- `SDK` = the Cinema 4D 2027 C++ SDK. It is not part of the repository; this project keeps its copy in `references/Cinema_4D_CPP_SDK_2027_0_0` (`docs/development.md` says where to get it).
- `CF` = `SDK/frameworks/cinema.framework/source`
- `DESC` = `CF/description`
- `PY` = the Cinema 4D 2027 Python SDK, kept likewise in `references/Cinema_4D_Python_SDK_2027_0_0`

## Build

**Build this project with `tools\build_plugin.ps1`** (`-Config Debug`, `-Reconfigure`, `-Sdk <folder>`). [T] It runs:

```
$env:PYTHONDONTWRITEBYTECODE = "1"
cmake -S references\Cinema_4D_CPP_SDK_2027_0_0 -B build\plugin -G "Visual Studio 18 2026" -T v143 `
      -A "x64,version=10.0.26100.0" "-DMAXON_SDK_MODULES_DIR=<repo>\plugin"
cmake --build build\plugin --config Release --target cyclesrender -- /m /nr:false /v:minimal
```

- The project is developed with Visual Studio Community 2026, MSVC 14.44 (toolset v143) and Windows SDK 10.0.26100.0; these are the build script's defaults. The SDK's preset `windows_vs2022_v143_x64` asks for the Visual Studio 2022 generator and Windows SDK 10.0.20348.0, so with another Visual Studio the generator, toolset and architecture are passed explicitly (parameters of the build script). [T]
- The unmodified SDK example `example.hello_world` built this way in 152 s with no warnings and loaded in Cinema 4D 2027.0.0. [T]
- The SDK is the CMake source tree; `-B` puts every output, including the source processor's generated files, in the build directory. Paths containing spaces work. [T]
- Without `PYTHONDONTWRITEBYTECODE=1` the SDK's source processor leaves `__pycache__` folders inside `SDK/tools/sourceprocessor`. [T]
- CMake ≥ 3.30, C++20. Presets: `SDK/CMakePresets.json`.
- The top-level `SDK/CMakeLists.txt` discovers every module under `MAXON_SDK_MODULES_DIR` (a cache path, default `SDK/plugins`).
- `SDK/cmake`, `SDK/frameworks` and `SDK/tools` stay unmodified.

**Test inside Cinema 4D with `tests\c4d\run_c4d_test.ps1`** [T]; how to use it is in `docs/development.md`. With `-Headless` it runs a script in `c4dpy.exe`; otherwise it starts the regular Cinema 4D with the harness plugin `tests\c4d\harness\cyclesrender_test_harness.pyp`, which runs the script once the application has started and quits. Cinema 4D's console goes to a log file through the `g_logFile` setting.

- Cinema 4D reads settings such as `g_additionalModulePath` (several paths separated by `;`) and `g_logFile` from environment variables as well as the command line; the full list is in `<C4D install>\resource\config.txt`. [T]
- `c4dpy.exe` and `Commandline.exe` need a licence of their own kind; licensed through the Maxon App they run without questions. [T] Both load the plugin from `g_additionalModulePath` and render with it: `c4dpy <script.py>` runs a script (it has its own preferences folder, so settings stored by the regular application are not visible to it), `Commandline.exe -nogui -render <file.c4d>` renders a saved scene with its saved render settings, including frame sequences.
- Module layout: `<module>/project/CMakeLists.txt`, `<module>/source/`, `<module>/res/`. Template to copy: `SDK/plugins/example.image/project/CMakeLists.txt`.
  - `target_settings` JSON lists `apis` (the frameworks whose headers the module may include), `"c4d": true`, `moduleid`, `"type": "DLL"`.
  - Likely `apis` here: `cinema.framework`, `core.framework`, `image.framework`, `image_ocio.framework`, `network.framework`, `exchange.framework`, `nodes.framework`, `nodespace.framework`, `graph.framework`, `volume.framework`, `asset.framework`.
  - `enable_rtti OFF`, `enable_exception_handling OFF`.
- Code rules: use the maxon types and error handling (`maxon::Result`, `iferr_scope`, `iferr_return`); the standard library is unsupported in SDK modules; lambdas capture `this` explicitly.
- Idioms that compile cleanly here, with the module's style check at level 3 [T]:
  - `Type value = Function() iferr_return;` inside a function that starts with `iferr_scope;`.
  - `iferr (Type value = Function()) return fallback;` — `value` stays usable afterwards.
  - `maxon::Result<T> r = Function(); if (r == maxon::OK) use(r.GetValue());` when an error must not propagate.
  - `iferr (Function()) { ApplicationOutput("… @", err); }` to log an error.
  - `return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "message"_s);` to raise one.
  - `MAXON_DISALLOW_COPY_AND_ASSIGN(Class);` for classes that own a process or socket; `std::move` to return or append move-only values such as `BaseArray`.
  - Tabs for indentation, braces on their own lines, `override` on virtual overrides, `///` comments on declarations.
- Description headers (`res/description/*.h`) and `c4d_symbols.h` are on the include path by bare name.
- Where a description id lives [T]: the SDK's description headers (`olight.h`, `ocamera.h`, `mmaterial.h`, …) declare plain global enums, so write `LIGHT_TYPE`, not `cinema::LIGHT_TYPE`. The exception is headers that `c4d.h` already includes inside its namespace, such as `ddoc.h`: write `cinema::DOCUMENT_DOCUNIT`. Type ids (`Olight`, `Mmaterial`, `Xbitmap`, `Ttexture`) and `ConstDescIDLevel`, `CreateDescID` are macros and take no namespace.
- `FormatString("@@-@", a, b, c)` fails to compile ("Argument mismatch" in `string.h`): `@@` is not two placeholders. Concatenate instead. The style check also rejects an `else` with braces after an `if` without them. `HashMap::Insert` returns the entry, not the value (`entry.GetValue()`). [T]
- Names the SDK claims as macros: a method called `Describe` does not compile. Enums need `MAXON_ENUM_LIST(NAME)` after the closing brace (`MAXON_ENUM_LIST_CLASS` inside a class). `iferr_return` inside an `if` needs braces. `static const Vector NAME(0.05);` is flagged as a badly named function: write `= Vector(0.05)`. [T]
- `res/` layout (see `SDK/plugins/example.image/res/`): `c4d_symbols.h`, `description/<name>.res` + `.h`, `strings_en-US/description/<name>.str`, `strings_en-US/c4d_strings.str`.
- Entry points in `main.cpp`: `cinema::PluginStart()`, `PluginEnd()`, `PluginMessage()` (handle `C4DPL_INIT_SYS` with `g_resource.Init()`). Example: `SDK/plugins/example.image/source/main.cpp`.
- Output: `<build>/bin/<cfg>/plugins/<module>/<module>.xdl64`. Start C4D with `g_additionalModulePath=<build>/bin/<cfg>/plugins/` to load it.
- Classic API types live in namespace `cinema::`; headers keep their old names (`c4d.h`, `c4d_gui.h`) under `CF`.
- ABI: binaries built with the 2027.0 SDK also load in 2026.
- Manuals: `SDK/docs/manual_build_systems.html`, `manual_quickstart_win.html`, `page_requirements.html`, `manual_migrating_to_2027.html`.

## Renderer (VideoPostData)

Headers: `CF/c4d_videopostdata.h`, `CF/c4d_tools.h` (`VPBuffer`, `Render`, `VideoPostStruct`), `CF/ge_prepass.h` (flags).

Proven by `plugin/cyclesrender/source/videopost/cycles_videopost.cpp` [T]:

- `Execute` runs on the render thread and may block for the whole frame: the plugin starts Blender, sends the scene and waits for the pixels inside the call that closes the render stage (`vps->vp == VIDEOPOSTCALL::RENDER && !vps->open`).
- Cancel: `vps->thread->TestBreak()` becomes true when the render is stopped, including when the thread passed to `RenderDocument` is ended. Polled every 100 ms while waiting for Blender, a 1920×1080 render stopped 0.09 s after the signal.
- Once a stage has returned an error, `*vps->error` holds it and `Execute` is still called for the remaining closing stages. Return `*vps->error` from those calls: returning `FAILED` (as the SDK example does) turns a user cancel into a failed render.
- A Picture Viewer render of 640×360 through Blender took 6 s, about 3 s of it Blender starting.

- Registered with `PLUGINFLAG_VIDEOPOST_ISRENDERER`, the renderer appears in the Render Settings' Renderer dropdown and gets its own page in the list.
- Returning `VIDEOPOSTINFO::EVALUATES_SCENE` from `GetRenderInfo` is accepted. `vps->doc` then arrives evaluated: caches and deform caches are built when the render stage closes, for `RenderDocument`, the Picture Viewer and `Commandline.exe`, and for every frame of a sequence. The plugin calls no `ExecutePasses` itself. [T]
- One render job is one instance of the video post: `FRAMESEQUENCE` opens once, every frame runs `FRAME` to `INNER`, and `FRAMESEQUENCE` closes once, also after an error or a cancel. The plugin keeps its Blender session in the instance between those two calls (`OpenSession`, `CloseSession`). [T: four frames, one Blender]
- `RenderDocument` and view transforms [T, `tests\c4d\colour.py`]: by default Cinema 4D's own renderers bake the view transform (tone mapping) into the pixels they return. With `RENDERFLAGS::OCIO_RAW_RENDERING` they return render-space values. A video post renderer has to do the same itself: `vps->render->GetRenderData().GetBool(RDATA_BAKE_OCIO_VIEW_TRANSFORM_RENDER, true)` (absent means on) tells whether to apply `OcioConverter::TransformColors(…, OCIO_RENDERING_TO_VIEW)` before writing the picture. For a Picture Viewer or command-line render the switch is off, and Cinema 4D applies the view transform when showing or saving (8-bit PNG and 32-bit EXR both matched the Standard renderer without the plugin doing anything).
- "Basic" colour management (`DOCUMENT_COLOR_MANAGEMENT_BASIC`) [T, `tests\c4d\colour.py`]: with Linear Workflow on, the Standard renderer's picture holds its linear values with the sRGB curve applied, in memory and in the Picture Viewer, raw flag or not, **unless Multi-Pass is on**: then the picture and every layer hold linear values and the Picture Viewer applies the curve when showing them. With Linear Workflow off the picture holds the sRGB numbers it calculated with, Multi-Pass or not. The Picture Viewer shows a float picture without a colour profile as it is. The rules for everything that goes in are under "Colour management" below; the plugin keeps them in `scene/document_colors`. (A linear image colour profile in the Render Settings is taken to mean no curve: untested.)
- Motion blur [T]: inside `Execute` (render stage closing) the render document can be moved in time and evaluated: `doc->SetTime(BaseTime(seconds))`, `doc->ExecutePasses(vps->thread, true, true, true, BUILDFLAGS::EXTERNALRENDERER)`, read the caches, and set the frame's time back the same way. Worked for `RenderDocument` and gave the expected streak lengths.
- Render passes [T, `tests\c4d\passes.py`]: in `AllocateBuffers`, `render->AllocateBufferFX(VPBUFFER_POSTEFFECT, name, 32, true)` returns an id; in `Execute`, `render->GetBuffer(VPBUFFER_POSTEFFECT, id)` is the buffer, or `nullptr` unless Multi-Pass is enabled and holds a "Post Effects" entry (`MULTIPASSOBJECT_TYPE = VPBUFFER_ALLPOSTEFFECTS`). The buffers have 4 channels like RGBA. They become layers named `name` in the multi-pass bitmap and files named `<multi-pass path>_<name in lower case>_<n>.exr` with "save one file per layer". From Python: `rd[c4d.RDATA_MULTIPASS_ENABLE] = True`, a `c4d.BaseList2D(c4d.Zmultipass)` with that type, `rd.InsertMultipass(entry)`; read layers with `MultipassBitmap.GetLayers(MPB_GETLAYERS_IMAGE | MPB_GETLAYERS_ALPHA)` and `GetParameter(c4d.MPBTYPE_NAME)`.
- Render region [T]: `RDATA_RENDERREGION` with `_LEFT`, `_TOP`, `_RIGHT`, `_BOTTOM` are borders in pixels from each side. The buffer keeps the full size; the Standard renderer leaves the outside black.
- `RenderEngineCheck` returning false hides the Anti-Aliasing, Options, Stereoscopic, Team Render and Material Override entries.
- `Execute` is called with `vps->vp` = 1 (frame sequence), 2 (frame), 3 (subframe), 4 (render), 5 (inner), each opened in that order and closed in reverse. Start Cinema 4D with `g_cyclesRenderLogCalls=true` to log the calls.
- The RGBA buffer is 32-bit float with 4 channels, both for `RenderDocument` into a bitmap and for a Picture Viewer render. Lines written with `SetLine(…, 32, false)` when the render stage closes arrive unchanged: a float bitmap rendered through Python's `RenderDocument` holds exactly the values written. The Picture Viewer applies the view transform for display.
- **Two top-level groups in the description appear as two tabs** on the renderer's Render Settings page. `ID_VIDEOPOSTPROPERTIES` is titled "Basic" / "Basic Properties".
- From Python: `documents.BaseVideoPost(id)`, `rd.InsertVideoPost(vp)`, `rd[c4d.RDATA_RENDERENGINE] = id` selects the engine. Setting `BIT_ACTIVE` on the video post does not switch the Render Settings dialog to its page.
- Rebuilding prints a `Get-Item … positional parameter` error from the SDK's post-build step, because the repository path contains spaces. It is harmless: the `res` junction next to the binary already exists.

- Register: `RegisterVideoPostPlugin(id, name, PLUGINFLAG_VIDEOPOST_ISRENDERER, Alloc, "VPcyclesrender"_s, disklevel, priority)`. Extra flags: `PLUGINFLAG_VIDEOPOST_MULTIPASS`, `…_ISRENDERER_NET` (Team Render), `…_RENDERER_SUPPORTS_DIRTY_REGIONS`.
- The plugin id becomes the value of `RDATA_RENDERENGINE`.
- `GetRenderInfo` → return `VIDEOPOSTINFO::EVALUATES_SCENE` ("the videopost evaluates the scene itself"; skips C4D's internal object preparation; `ge_prepass.h:5199`).
- `RenderEngineCheck(const BaseVideoPost*, Int32 id) const` → return false for `RENDERSETTING_STATICTAB_ANTIALIASING`, `_OPTIONS`, `_STEREO`, `_MULTIPASS`, `_OVERRIDEMAT`, `_NET` to hide C4D tabs that do not apply.
- `Execute(BaseVideoPost*, VideoPostStruct* vps)` is called in this order, each opened and closed: `FRAMESEQUENCE` → `FRAME` → `SUBFRAME` → `RENDER` → `INNER`. Test `vps->vp` and `vps->open`.
- `VideoPostStruct`: `doc` (the render document), `time`, `fps`, `thread`, `render`, `renderflags`, `net` (non-null under Team Render).
- Pixels: `vps->render->GetBuffer(VPBUFFER_RGBA, NOTOK)` → `VPBuffer*`; `SetLine(x, y, count, Float32* data, 32, false)`; `GetBw()`, `GetBh()`, `GetCpp()`.
- Passes: override `AllocateBuffers`; `render->AllocateBufferFX(VPBUFFER_POSTEFFECT, "Name", 32, true)` returns a sub-id for a custom named buffer.
- Cancel: `vps->thread->TestBreak()` → return `RENDERRESULT::USERBREAK`.
- Progress text: `StatusSetText`, `StatusSetBar` (`CF/c4d_general.h`).
- Region render: `RDATA_RENDERREGION`, `_LEFT`, `_TOP`, `_RIGHT`, `_BOTTOM` from `render->GetRenderData()`.
- Material preview renders arrive with `RENDERFLAGS::PREVIEWRENDER`; interactive render region with `RENDERFLAGS::IRR`.
- Colour: override `GetColorProfileInfo(node, vps, ColorProfileInfo&)` to declare the output's colour spaces.
- Settings UI: description `CONTAINER VPcyclesrender { NAME …; INCLUDE VPbase; GROUP … { … } }`. Dynamic UI via `GetDDescription`.
- **Complete worked example:** `SDK/plugins/example.image/source/examples_ocio.cpp` (`OcioAwareRenderer::Execute`, line 639), registered in `plugin_image_api_examples.cpp`, description `res/description/vpocioawarerenderer.res`.

Render Settings page [T: `tests/c4d/settings.py`]:

- The page is generated: `tools/make_settings.py` holds every setting once and writes `vpcyclesrender.h/.res/.str` and `render_settings_table.h`.
- Top-level groups are tabs. Give `DEFAULT 1` to the first only: with it on every tab all tabs show as selected. A named group inside a tab is a foldable group (`DEFAULT 1` open, `DEFAULT 0` folded); an unnamed `GROUP { COLUMNS 2; ... }` puts two settings side by side.
- With no setting in `ID_VIDEOPOSTPROPERTIES` its "Basic" tab does not show.
- Hiding by engine: override `GetDDescription`, `LoadDescription(node->GetType())`, set `flags |= DESCFLAGS_DESC::LOADED`, then `description->GetParameterI(CreateDescID(DescLevel(id)), nullptr)->SetBool(DESC_HIDE, ...)` for groups and settings alike (`CreateDescID` is a macro: no namespace in front). Greying out: `GetDEnabling`.
- `BaseList2D::SetName` in `Init` names the entry in the Render Settings' list ("Cycles"); the renderer dropdown keeps the registered name.
- **Pixel aspect** (`RDATA_PIXELASPECT`, the Output page) [T: `tests/c4d/output.py`, probes 2026-11-12]: the Standard renderer keeps the camera's horizontal view over the picture's width; a picture of W × H pixels of aspect A shows what W·A × H square pixels show, squeezed into the width (400 × 200 at aspect 2: the ball's box 94..154 × 40..159, the same as 800 × 200 at aspect 1 halved across: 188..310 × 40..159). The same for a parallel camera. A film offset Y of 0.2 moves the picture by 0.2 of its height at any aspect. Blender does the same with `render.pixel_aspect_x` / `_y` (both at least 1: the wider side gets the ratio) and horizontal sensor fit, in Cycles and Eevee; Blender's `shift_y` (in image widths) then needs dividing by the aspect. Before 0.2.0 the plugin did not read the pixel aspect at all.
- **Setting the Output values from code** [T, c4dpy]: `SetParameter` of `RDATA_XRES` with "Lock Ratio" on did not change `RDATA_YRES`; `RDATA_FILMASPECT` stayed 1.7778 after Width 1000 and Pixel Aspect 2 (and on the Output page after the Output tab wrote them, screenshot); setting Pixel Aspect 2 chose the preset "Anamorphic" (`RDATA_PIXELPRESET` 7). The editor view's frame follows the new aspect.
- **The Output tab** (`videopost/output_settings.cpp`) [T: `tests/c4d/output.py`, screenshots `shots_020_f4_f13`]: parameters of the video post that `GetDParameter` / `SetDParameter` answer themselves; the video post's render data is found by walking the document's render data for the one whose video posts include it. A STATICTEXT with an empty label whose text `GetDParameter` returns ("= 960 x 540 px") works in a row of the grid. Values set from Python (`videopost[id] = v`) go through `SetDParameter` and wrote the render data; saved and loaded, the tab and the page kept their values.
- **Render passes on the AOVs tab** are BOOLs outside any grid: some are hidden for one engine (`ENGINE_ONLY`), and a hidden cell of a two-column grid would shift every cell after it.

## IPR window (GUI)

Headers: `CF/c4d_gui.h`, `CF/gui.h`, `CF/c4d_commanddata.h`.

The code: `plugin/cyclesrender/source/ipr/` (`ipr_dialog`, `ipr_image_area`, `ipr_controller`, `ipr_worker`, `ipr_testhook`). Facts found while building it, all [T]:

- The names of `gui.h` (`BFH_LEFT`, `BFM_ACTION`, `BFM_INPUT_MOUSEWHEEL`, `BMP_NORMAL`, `BORDER_NONE`, …) live in namespace `cinema`; the `BITMAPBUTTON_…` settings and `EVMSG_…` ids are macros and take no namespace.
- A user area tells its dialog something with `SendParentMessage(BaseContainer(BFM_ACTION))` carrying `BFM_ACTION_ID = GetId()`; it arrives in the dialog's `Command(id, msg)`.
- A bitmap button gets its picture with `SetImage(bitmap, true)` followed by `Redraw()`; a toggle button (`BITMAPBUTTON_TOGGLE`) is switched with `SetToggleState`. Icons are PNG files loaded from `GeGetPluginResourcePath() + "icons"`.
- A percent field is `AddEditNumberArrows` plus `SetPercent(id, value, min, max, step)` with value 1.0 = 100 %; `GetFloat` returns it the same way. It shows the decimals of the value it is given (reported from use: the wheel's zoom steps of 1.15 showed many), so the IPR keeps its zoom in steps of 0.1 % and puts exactly that into the field (`RoundZoom` in `ipr/ipr_image_area.cpp`). [T: the zoom four wheel notches give, 174.900625 %, set through the test hook, showed "174.9 %" on a screenshot; `tests/c4d/ipr_window_checks.py`, check `zoom_rounded`, also for typed values. The wheel itself is untested.]
- `BaseArray::GetIndex(x)` wants a reference to an element of the array and computes its position; it does not search. Searching for a value needs a loop.
- **Showing render-space floats.** `OcioConverter::Init(doc, oldConverter)` then `TransformColors(Vector32*, count, COLORSPACETRANSFORMATION::OCIO_RENDERING_TO_VIEW)` gives display values (0–1, ready for an 8-bit bitmap). It is slow: 78 ms for 640×360 when called per row. The image area samples it once into a 48³ table over a logarithmic scale (rebuilt when the document's OCIO names change) and interpolates, 0.7 ms for the same picture, 5–10 ms at 1920×1080 (`maxon::ParallelFor::Dynamic` over rows). `GetActiveOcioColorSpacesNames` tells when to rebuild. The SDK notes that colour profiles set on a bitmap do not affect drawing, so `DrawBitmap` gets the converted 8-bit bitmap. The table and its lookup are `DisplayTransform` (`ipr/ipr_image_area.h`), which the test hook also uses without a window (`display <file>`); for "Basic" colour management it holds the sRGB curve or nothing ("Colour management").
- **Picture Viewer.** A float bitmap (`Init(w, h, 96)`, rows set with `SetPixelCnt(…, 16, COLORMODE::RGBf, …)` straight from RGBA floats) shows with the right view transform after `SetColorProfile` for the render space, view transform and display space taken from `doc->GetOcioProfiles` (wrap each `maxon::ColorProfile` with `ColorProfile::Alloc`). `ShowBitmap` copies; the caller frees its bitmap. `maxon::ColorProfile` needs `image.framework` in the module's `apis` and `maxon/gfx_image_colorprofile.h`. In a "Basic" document `GetOcioProfiles` gives empty profiles, and a float bitmap without a profile is shown as it is; with `ColorProfile::GetDefaultLinearRGB()` as its image profile (`COLORPROFILE_INDEX_IMAGE`) the Picture Viewer shows it through the sRGB curve, and `Save` as a 32-bit EXR keeps the values [T: screenshots, `colour.py`].
- **When the scene is ready.** `EVMSG_CHANGE` says something changed; `EVMSG_DOCUMENTRECALCULATED` is sent "after the animation, expressions and cache building have been done". The window marks the scene changed on both and exports on its next timer tick; exports made this way always saw current caches in the tests.
- **Thread to window.** `SpecialEventAdd(id)` from any thread arrives as `CoreMessage(id)` in the dialog; the frame thread uses it so a frame is shown without waiting for the 15 ms timer.
- `C4DThread::End(true)` in a destructor ends a worker; a worker waiting on a Windows event (`platform/shared_memory`) should wake at least every 20 ms to test `TestBreak()`.
- A command plugin's `RestoreLayout` is only called for windows contained in a saved layout. Quitting Cinema 4D did not store the open IPR window by itself in two tries (with either command id). Not tested further: saving a layout would change the user's own.

General API:

- Dialog: `GeDialog` with `CreateLayout`, `InitValues`, `Command`, `CoreMessage`, `Message`, `Timer`/`SetTimer(ms)`. Open with `Open(DLG_TYPE::ASYNC, pluginId, …)`; restore docking with `RestoreLayout`.
- Command plugin: `CommandData` (`Execute`, `RestoreLayout`), `RegisterCommandPlugin`.
- Menus: `MenuFlushAll`, `MenuSubBegin`, `MenuAddString`, `MenuSubEnd`, `MenuFinished`, `MenuInitString(id, enabled, checked)`.
- Gadgets: `GroupBegin`/`GroupEnd`, `AddComboBox` + `AddChild`/`FreeChildren`, `AddEditNumberArrows`, `AddUserArea` + `AttachUserArea`, `AddCustomGui`.
- Zoom field: `SetFloat(id, v, min, max, step, FORMAT_PERCENT)`; drag values arrive as `BFM_ACTION` with `BFM_ACTION_INDRAG`. The SDK's own user area (`asynctest.cpp`) ends a drag with one more message with `BFM_ACTION_INDRAG` false; the IPR's zoom field relies on Cinema 4D's number field doing the same (it writes the rounded zoom back only then) [untested: no test can drag].
- Text in a user area: `DrawSetTextCol(fg, bg)` with a colour (vector) for `bg` paints the text's whole box in that colour; over a background of two colours (the IPR's samples bar, where the bar ends inside the text) the text's rows then showed the bar ending elsewhere than the rows above them. `COLOR_TRANS` as `bg` draws the glyphs only [T: screenshots of the samples bar, before and after].
- Icon buttons: `CUSTOMGUI_BITMAPBUTTON` (`CF/c4d_customgui/customgui_bitmapbutton.h`): `BITMAPBUTTON_BUTTON`, `_TOGGLE`, `_ICONID1`, `_ICONID2`, `_TOOLTIP`. Register icons with `RegisterIcon`.
- Progress bar: `CUSTOMGUI_PROGRESSBAR`, driven by `SendMessage(id, BaseContainer(BFM_SETSTATUSBAR))` with `BFM_STATUSBAR_PROGRESSON`, `BFM_STATUSBAR_PROGRESS` (0..1), `BFM_STATUSBAR_TXT`. Pattern: `ProgressDialog` in `CF/c4d_gui.cpp`.
- Image area: `GeUserArea` with `DrawMsg`, `InputEvent`, `Sized`, `Redraw`, `OffScreenOn`, `DrawBitmap`, `DrawImageRef(maxon::ImageBaseRef…)`, `GetPixelRatio()` for high-DPI.
- Mouse: `MouseDragStart`/`MouseDrag`/`MouseDragEnd`; wheel is `BFM_INPUT_MOUSEWHEEL` with the amount in `BFM_INPUT_VSCROLL`.
- OCIO view transform for display: `BaseDocument::GetOcioConfigAndProfiles`, `OcioConfigInterface::CreateConversionFunctionWithViewTransform` (`SDK/frameworks/image_ocio.framework/source/maxon/gfx_image_ocio.h`).
- Examples: `SDK/plugins/example.image/source/example_image_layers_area.cpp` (user area with float images, async dialog); `SDK/plugins/example.main/source/gui/asynctest.cpp` (wheel, drag, menus); `…/gui/gedialog_gadgets.cpp` (all gadgets).

Found later, all [T] unless marked:

- `Orscamera` (the camera Cinema 4D 2027 creates by default) is no instance of `Ocamera`: test `IsInstanceOf(Ocamera) || GetType() == Orscamera` wherever cameras are collected.
- `AddComboBox(id, flags, w, h, specialalign, allowFiltering = true)`: with the list open, typing narrows it [S; a test cannot type into an open list].
- `MouseDrag()` hands out the distance moved with the sign opposite to the mouse's way on screen as this code first assumed: the picture follows the mouse with `centre += drag / zoom` [seen by hand; no test can drag].
- A toggled bitmap button shows no state by itself here: the pause button gets a second icon while paused.

## Picture Viewer

- `ShowBitmap(BaseBitmap*)` (`CF/c4d_general.h`).
- Float bitmaps: `BaseBitmap::Init(w, h, depth)`, `SetPixelCnt(…, COLORMODE::RGBf / ARGBf, …)`; layered results with `MultipassBitmap`.
- Colour profiles: `SetColorProfile(profile, BaseBitmap::COLORPROFILE_INDEX_RENDERSPACE / _DISPLAYSPACE / _VIEW_TRANSFORM)`.

## Threads, messages, change detection

- Worker threads: `C4DThread` (`Main`, `Start`, `End`, `TestBreak`). Main-thread calls: `maxon::ExecuteOnMainThread`.
- Worker → UI: `SpecialEventAdd(id, p1, p2)`; received in `GeDialog::CoreMessage` (parameters in `BFM_CORE_PAR1/2`).
- Scene changed: core message `EVMSG_CHANGE`. `MessageData` plugins (`RegisterMessagePlugin`) also receive core messages and timers.
- Threading rule: only the main thread may modify the active document, add events, or touch GUI. Manual: `SDK/docs/page_manual_cinemathreads.html`.
- Caches of the active document are valid only after C4D has evaluated the scene. A `SceneHookData` (`CF/c4d_scenehookdata.h`, `RegisterSceneHookPlugin`) can execute after generators and also receives `MSG_DOCUMENTINFO` (document loaded, activated, undo, redo).
- Dirty counters: `C4DAtom::GetDirty(DIRTYFLAGS::MATRIX | DATA | CACHE)`, `GetHDirty(HDIRTYFLAGS::OBJECT | OBJECT_MATRIX | MATERIAL | SHADER | TAG | RENDERSETTINGS | ANIMATION)`, usable on the document.
- Content hashes: `VariableTag::GetDataHash()` — compare the `Tpolygon` hash for topology change and the `Tpoint` hash for point-only change. Tag data is copy-on-write and shared between identical caches.
- Identity: `BaseObject::GetGUID()` (works for cache objects), `GetUniqueIP()`, `FindUniqueID(MAXON_CREATOR_ID, …)` (persists across save/load).
- **A document's identity through a render's copy** [T, probes in c4dpy; `tests/c4d/baked_files.py`, `baked_files_window.py`]: a document has a `MAXON_CREATOR_ID` (16 bytes) from the start, also one made by `BaseDocument()` that was never in the document list. `GetClone(COPYFLAGS::DOCUMENT)` and `GetClone(0)` give the copy, its materials and its objects new ids; with `COPYFLAGS::PRIVATE_IDENTMARKER` all keep theirs. The copy that `RenderDocument` renders (c4dpy) and the copy of a render to the Picture Viewer (the application) carry the document's id, keep its name, are not in the document list, and reach the video post on a thread that is not the main thread. `materials/baked_files.cpp` uses that id to give baked textures an owner.
- **Seeing a document closed** [T, `tests/c4d/baked_files_window.py`]: File > Close (command 12664) sends `EVMSG_CHANGE` to a `MessageData`'s `CoreMessage`, and by then the document is gone from `GetFirstDocument()`/`GetNext()`: comparing the list with the documents seen before finds it. The hook in `materials/baked_files.cpp` also checks on its timer (once a second); in the test the change message was the one that found it. `MSG_DOCUMENTINFO_TYPE_REMOVE` would need a `SceneHookData`, which was not tried. A `MessageData` registered under the renderer's id works next to the renderer and the IPR command with the same number.
- Views and cameras: `doc->GetRenderBaseDraw()` (the "use as render view" viewport), `GetActiveBaseDraw()`, `BaseDraw::GetSceneCamera(doc)`, `GetEditorCamera()`. Enumerate cameras by walking the hierarchy for `Ocamera` and `Orscamera`.

## Process, sockets, paths

- Launch a long-running process [T]: `maxon::SystemProcessInterface::CreateProcess(url, args, flags, nullptr)` → `StartProcess()`, then `IsRunning()`, `GetExitCode()`, `KillProcess()`, and `SystemProcessInterface::WaitForProcess(process, maxon::Seconds(n))`. Flags used: `HIDE_APPLICATION` (no console window), `KILL_IF_STARTING_PROCESS_ENDS` (Windows only), `CAPTURELOGFILE | DONT_DELETE_LOGFILE` (output to `process.GetLogFile()`). Header: `SDK/frameworks/core.framework/source/maxon/system_process.h`. Working code: `plugin/cyclesrender/source/session/blender_process.cpp`.
- With `KILL_IF_STARTING_PROCESS_ENDS`, a Blender whose Cinema 4D was killed by force was gone a few seconds later. [T]
- **Arguments are passed to the new process unquoted** [T]: an argument containing a space arrives split. Wrap such arguments in double quotes yourself (`Quoted` in `blender_process.cpp`). Paths with spaces are the rule on Windows ("Program Files", user names), so every path argument hits this.
- Sockets [T]: the project uses blocking WinSock in `plugin/cyclesrender/source/platform/socket_win.cpp` behind `platform/socket.h`. That file includes no Cinema 4D headers and its header uses only plain C++ types, because the Windows headers clash with the SDK's; it links `ws2_32.lib` with a `#pragma comment`. The SDK's own TCP API (`maxon::NetworkTcpInterface`, `SDK/frameworks/network.framework/source/maxon/network_ip_tcp.h`) is asynchronous with completion handlers and is not used.
- Writing request headers [T]: `session/json_writer` builds JSON text directly (nested lists, matrices, escaped strings, numbers printed with `snprintf("%.9g")`). Use it for anything beyond flat key/value headers; `BridgeClient::Request(JsonWriter&, blobs, cancelThread)` adds the request id and closes the object.
- JSON [T]: `maxon::ParserClasses::JsonParser().Create()`, then `ReadString(text, PARSERFLAGS::NONE, GetUtf8DefaultDecoder(), receiver)` into a `maxon::SingleValueReceiver<const maxon::DataDictionary&>` and `Write(dictionary, text, false)` (`maxon/parser.h`). Build headers with `dictionary.Set("key"_s, value)`; read with `dictionary.Get<T>("key"_s)`, which fails when the stored type differs, so numbers are read through `GetJsonInt` in `session/json.cpp`, which tries the integer and float types in turn.
- Strings and bytes [T]: `text.GetCString()` gives the UTF-8 bytes (array size excludes the terminator); `maxon::String(const Char*, count)` builds a string from bytes; search with `text.Find("x"_s, &position)`.
- A unique token [T]: `maxon::Uuid::Create()`, `CreateId()`, `ToString()` (`maxon/uuid.h`).
- Timing and waiting [T]: `maxon::TimeValue::GetTime()`, `maxon::Seconds(n)`, `cinema::GeSleep(ms)`, `cinema::GeGetTimer()`.
- Developer settings [T]: `MAXON_CONFIGURATION_BOOL/INT/STRING(g_name, default, …, maxon::CONFIGURATION_CATEGORY::DEVELOPMENT, "help")` (`maxon/configuration.h`) defines a global that Cinema 4D fills from its command line or from an environment variable of the same name. The project's settings are listed in `docs/development.md`.
- Triggering C++ from a test [T]: `c4d.GePluginMessage(id, None)` in Python reaches the module's `PluginMessage(id, data)`. Its Python return value is `False` even when the module returned true, so report results through the log. `main.cpp` uses the renderer's id as the message id for the session self-test.
- Shared memory [T]: the SDK has none across processes. The project uses Win32 `OpenFileMappingA` / `MapViewOfFile`, and a named event as the wake-up signal, in `platform/shared_memory_win.cpp`.
- Environment variables [T]: the SDK offers no call that was found for reading one; `platform/environment_win.cpp` reads them with `GetEnvironmentVariableW` and hands them out as UTF-8. Used to find Blender's folder for user extensions as Blender itself resolves it (`FindBridgeScript` in `session/blender_install.cpp`): `BLENDER_USER_EXTENSIONS`, then `BLENDER_USER_RESOURCES`, then `portable` next to the executable, then `%APPDATA%\Blender Foundation\Blender\<major>.<minor>`.
- Start-up messages [T]: `PluginMessage` receives `C4DPL_STARTACTIVITY` once all plugins have registered, also in `c4dpy`; the plugin uses it to look for Blender when none is stored. In `c4dpy`, what a module prints during `PluginStart` and at that message reaches neither the console output nor the `g_logFile` log, and world plugin data set during a run is not kept for the next one.
- Paths: `GeGetPluginPath`, `GeGetC4DPath(C4D_PATH_PREFS)`, `maxon::Application::GetUrl(APPLICATION_URLTYPE::TEMP_DIR)`.
- Preferences folders [T]: the regular application uses `%APPDATA%\Maxon\Maxon Cinema 4D 2027_<id>\prefs`, `c4dpy` `…2027_<id>_p\prefs`. With `g_prefsPath=<folder>` `C4D_PATH_PREFS` is `<folder>\prefs`. `c4dpy` keeps no world plugin data between runs, but files in its preferences folder stay: 32 baked textures of earlier test runs were found in `…_p\prefs\cyclesrenderaked`. (A folder `…2027_<id>_x` also exists, probably `Commandline.exe`'s; not checked.)
- Files [T]: `BrowseFiles` (`Init(folder, 0)`, `GetNext()`, `IsDir()`, `GetFilename()` gives the bare name) lists a folder; `GeFKill(file)` deletes, `GeFKill(folder, GE_FKILL_DIRECTORY)` removes an empty folder (the plugin checks that it is empty first). On Windows a file another process holds open without sharing deletion cannot be deleted; `platform/file_lock_win.cpp` uses that for a lock file that tells other processes a session is running (opened for writing, shared for reading only). Seen in the test with a file Python held open for writing: the plugin could neither open it for writing nor did it delete it; two Cinema 4D processes on one preferences folder were not tried.
- Run a program and capture its output [T]: `SystemProcessInterface::StartProcessAndWait(url, args, CAPTURELOGFILE | DONT_DELETE_LOGFILE | HIDE_APPLICATION, callback, userData)`, then read `process.GetLogFile()` with `maxon::FileUtilities::ReadFileToMemory` and delete it. The callback is called once per second while the program runs; returning `'stop'` kills it. Pattern: `ProbeBlender` in `plugin/cyclesrender/source/session/blender_install.cpp`.
- Plugin settings [T]: `GetWorldPluginData(id)` returns a pointer to the stored container (null before the first write); `SetWorldPluginData(id, container, false)` replaces it. Cinema 4D saves it with its preferences on a normal quit. Pattern: `plugin/cyclesrender/source/prefs/settings.cpp`.
- Preferences page [T]: derive from `PrefsDialogObject` (`CF/c4d_libs/lib_prefs.h`), override `GetDParameter`/`SetDParameter` to read and write the settings, handle buttons in `Message` (`MSG_DESCRIPTION_COMMAND`, `DescriptionCommand::_descId`), and register with `PrefsDialogObject::Register(id, Alloc, name, "Prefs…", PREFS_RENDERER, 0)`. The SDK ships no example; the working one is `plugin/cyclesrender/source/prefs/prefs_page.cpp`. From Python the page is `c4d.plugins.FindPlugin(id, c4d.PLUGINTYPE_PREFS)`; its parameters can be read and set by id, buttons pressed with `c4d.CallButton`, and the dialog opened with `c4d.PrefsLib_OpenDialog(id)`.
- Directory listing [T]: `for (const auto& it : url.GetBrowseIterator(GETBROWSEITERATORFLAGS::NONE)) { const IoBrowseRef& entry = (it) iferr_return; … entry.GetCurrentPath() … }`.

## Reading parameters

**Read parameters with `GetParameter`, not from the node's `BaseContainer`.** [T] Several are not stored in the container under their id, and the container then answers 0 without complaint: a light's `LIGHT_TYPE` and a material's `MATERIAL_USE_*` switches both did. The helpers in `plugin/cyclesrender/source/scene/parameters.h` (`GetFloatParameter(node, id, fallback)` and its siblings for int, bool, vector, string, file name and shader link) wrap `node->GetParameter(CreateDescID(DescLevel(id)), data, DESCFLAGS_GET::NONE)` and check the type of the answer.

## Scene traversal

Implemented in `plugin/cyclesrender/source/scene/scene_exporter.cpp` (`SceneExporter::Walk`) and checked by `tests/c4d/traversal.py` against what the Standard renderer shows. [T]

```
Walk(op, mg, inherited):
  apply op's render dot to inherited.visible        (MODE_ON / MODE_OFF; MODE_UNDEF keeps the parent's)
  take op's unrestricted texture tag as inherited.material, if it has one
  if op is a light / sky: record it
  else if op is a render instance: Walk(reference object, mg, …)
  else if (cache = op->GetDeformCache() or op->GetCache()): Walk(cache, …)
  else if op is a polygon object and not BIT_CONTROLOBJECT: renderable mesh
  for each child: Walk(child, mg * child->GetMl(), inherited)
```

- A deform cache replaces the cache. Objects consumed by a generator carry `BIT_CONTROLOBJECT`.
- Matrices: cache objects have no parent pointer, so `GetMg()` is wrong for them. Accumulate from `GetMl()`: a generator's cache root gets `mg * cache->GetMl()`; a deform cache sits exactly where its object is and gets `mg` unchanged. [T]
- Visibility: the render dot is inherited down the hierarchy and into caches, and a child can switch it on again under a hidden parent. [T]
- Layers: `op->GetLayerData(doc)->render == false` hides the object and everything in its cache, but not its children in the Object Manager. [T]
- Instances [T]: a plain instance has a cache holding a copy of the original, and the copy keeps the original's own render dot. So an instance of an object whose own dot is off renders nothing, while an instance of an object hidden by its parent renders. A render instance has no cache: walk `InstanceObject::GetReferenceObject(doc)` with the instance's matrix, apply the reference's own dot, ignore its `BIT_CONTROLOBJECT`. Multi-instances are not exported yet.
- Materials: the last texture tag without a selection restriction wins; children and caches inherit it when they have none. [T]
- A render document is already evaluated (see "Renderer"). For any other document: `doc->ExecutePasses(thread, true, true, true, BUILDFLAGS::EXTERNALRENDERER)`.
- The exporter's view of a document as text: `SceneExporter::GetListing()`, reachable from tests (see `g_cyclesRenderDumpFile` in `docs/development.md`).

## Coordinates

`plugin/cyclesrender/source/scene/coords.h`. [T: `tests/c4d/coords.py`, `cameras.py`]

- A position (x, y, z) in Cinema 4D becomes (x, z, y) × metres-per-unit in Blender. The unit comes from the document (`DOCUMENT_DOCUNIT`, a `UnitScaleData`; centimetres by default).
- The swap mirrors space, so every face is sent with its corners in reverse order, and per-corner data (normals, UVs) in the same order.
- Object matrix: Blender's local X, Y, Z axes are Cinema 4D's local X, Z, Y axes, each swapped (`Coords::ObjectMatrix`).
- Cameras and lights look along +Z in Cinema 4D and along −Z in Blender, both with +Y up (`Coords::AimedMatrix`: X and Y axes swapped as above, Z negated, all normalised).
- UV: Blender's v is `1 - v` of Cinema 4D's.
- Camera: `CAMERA_FOCUS` and `CAMERAOBJECT_APERTURE` map to `lens` and `sensor_width` with horizontal sensor fit. Parallel projection shows 1024 units across the image at `CAMERA_ZOOM` 1, whatever the resolution. Film offset X is Blender's `shift_x`; offset Y is `-offsetY / aspect`, because Blender measures both shifts in image widths. When the image is larger than the camera's picture (the IPR's "Fit Window", `CameraFraming`), sensor width and parallel width are multiplied by image width / picture width, `shift_x` is `offsetX · pictureWidth / imageWidth` and `shift_y` is `-offsetY · pictureHeight / imageWidth`; the first two are tested, the shifts are not. Without clipping set on the camera the exporter sends 0.01 m to 10 km. Projections other than perspective and parallel, clipping of parallel views and the spherical camera: "Cameras and views" below.

## Light conversion

`plugin/cyclesrender/source/scene/light_export.cpp`. Both halves are measured, not assumed. [T]

Cinema 4D, Standard renderer (`tests/c4d/measure_c4d_lights.py`): a white diffuse surface facing a light of brightness B renders as

- B, at any distance, for an omni, spot, infinite or small area light without falloff;
- B × (r / d)² with "Inverse Square" falloff, r being the light's Radius/Decay and d the distance (3.98 at half of r, 0.989 at r, 0.30 at 1.8 r).
- A 200 × 200 area light at 500 gives 0.974, and the same value on the side it faces away from: area lights shine both ways.
- `LIGHT_DETAILS_OUTERANGLE` is the full cone angle: 30° lit a patch of radius 134 at distance 500.

Blender 5.2.3, Cycles (`tests/bridge/measure_lights.py`): the same surface renders as

- S / π for a sun of strength S;
- P / (4 π² d²) for a point or spot light of power P at distance d in metres;
- P / (π² d²) on the axis of a small area light, four times a point light of the same power.

Hence, with r in metres:

| Light | Blender | Power |
|---|---|---|
| infinite | `SUN`, angle 0 | S = π B |
| omni | `POINT`, radius 0 | P = 4 π² r² B |
| spot | `SPOT` | P = 4 π² r² B; `spot_size` = outer angle; `spot_blend` = (cos(inner/2) − cos(outer/2)) / (1 − cos(outer/2)) |
| area | `AREA` rectangle or disk | P = π² r² B |
| area, sphere shape | `POINT` with radius | P = 4 π² r² B |

A light with inverse-square falloff then matches at every distance. Blender has no light without falloff, so such a light gets the same power and matches Cinema 4D exactly at the distance r, its Radius/Decay value (500 cm unless changed), brighter closer and darker beyond. The other falloff types are treated the same way. `LIGHT_SHADOWTYPE_NONE` (Cinema 4D's default) becomes `use_shadow = false`.

## Material conversion

`plugin/cyclesrender/source/materials/material_export.cpp` turns a classic material into a Principled BSDF graph; `tests/c4d/materials.py` shows every channel next to the Standard renderer. [T]

- Colour → Base Color; a bitmap is mixed in as the channel's mix mode and strength say. Luminance → Emission at strength 1. Without a colour channel the base colour is black.
- Transparency → Transmission Weight (the brightest component) and IOR; the base colour moves towards the transparency colour by that weight.
- Reflectance (an approximation for the usual one or two layers): diffuse layers are skipped; the first other layer decides the specular lobe by the rules that follow. With two layers, the one with a dielectric Fresnel becomes the clear coat (Coat Weight = reflection strength, with its roughness and IOR) and the other the lobe; two layers of the same kind, and any layer after the second, are left out with a warning. The lobe: Legacy specular (a new material's "Default Specular"): roughness = width, Specular IOR Level 0.5 at 20 % strength. No Fresnel: Metallic = reflection strength, and at full strength the layer colour becomes the base colour. Dielectric: Specular IOR Level = 0.5 × strength, IOR from the layer. Conductor: Metallic = strength, base colour = layer colour. Reflectance off: Specular IOR Level 0.
- Alpha: the image's alpha channel when "Image Alpha" is on and the file type can hold one, otherwise its brightness; optional invert.
- Normal: a tangent-space Normal Map node, with the texture as it is. No channel flip is needed although v is flipped: the test's ridges are lit from the same side (correlation 0.97).
- Bump [T: measured against the Standard renderer]: Cinema 4D tilts the normal by the height difference between neighbouring texels times the strength. The result does not depend on the object's size, and a finer texture of the same pattern gives a shallower relief. At 100 % strength a step of 1 (black to white) between two texels is a slope of 7.4. Blender's Bump node works in space instead, so the exporter builds the rule from nodes: three texture look-ups one texel apart, their differences as the slopes of a normal fed to a Normal Map node (`ClassicMaterialGraph::Bump`). Matches at 5–100 % strength, 128 and 512 pixel textures, and two object sizes.
- Bitmaps: `GenerateTexturePath(doc->GetDocumentPath(), name, Filename(), &resolved)` finds a file the way Cinema 4D does (next to the scene, in `tex`, in the search paths). Colour textures are sent as `sRGB` (`Linear Rec.709` for .exr and .hdr), data textures as `Non-Color`. With Blender's working space set to the document's render space, an sRGB texture renders with the same numbers as in the Standard renderer. With "Basic" colour management the colour space follows the document and the bitmap's "Color Profile" ("Colour management" below).
- An object without a material: 80 % grey, which is 0.604 in linear light.
- Sky [T: `tests/c4d/sky_orientation.py`]: a Sky object needs its texture tag in spherical projection (UVW shows one flat colour). Seen from inside, the panorama's left edge is towards +X and its quarter point towards +Z, running on towards −X: mirrored compared with Blender's equirectangular lookup. `environment_export.cpp` mirrors X in a Mapping node and turns it with the Sky object.

## Standard renderer facts for comparisons

- It computes direct light only, and its picture sits about 0.28 px to the lower right of the exact projection. [T]
- A Sphere primitive with "Render Perfect" (default on) is drawn as a perfect sphere regardless of its segments. [T]
- It reflects and refracts the scene but shows no highlight of a light in a layer whose specular strength is 0; Cycles always mirrors lights. [T]

## Mesh data

- `PolygonObject`: `GetPointR`, `GetPointCount`, `GetPolygonR`, `GetPolygonCount`; `CPolygon` a/b/c/d with `IsTriangle()` (c == d). N-gons: `GetNGonTranslationMap`.
- Normals: `NormalTag` (`Tnormal`) when present; otherwise `PolygonObject::CreatePhongNormals()` (needs a Phong tag; returns `Vector32[4 * polyCount]`, free with `DeleteMem`).
- UVs: `UVWTag` (`Tuvw`), `GetDataAddressR()` + `UVWTag::Get(handle, i, UVWStruct&)`; several tags per object.
- Vertex maps: `VertexMapTag` (`Tvertexmap`), `GetDataAddressR()` → one `Float32` per point.
- Vertex colours: `VertexColorTag` (`Tvertexcolor`), `IsPerPointColor()` decides per-point or per-polygon-vertex.
- Selections: `SelectionTag::GetBaseSelect()` for `Tpolygonselection`.
- Texture tag (`Ttexture`, `DESC/ttexture.h`): `GetMaterial()`, `TEXTURETAG_PROJECTION` (UVW = 6), `TEXTURETAG_RESTRICTION` (selection name), `TEXTURETAG_OFFSETX/Y`, `TEXTURETAG_TILESX/Y`, `TEXTURETAG_MIX`.

Measured (`tests\c4d\geometry.py`, against the Standard renderer) [T]:

- With two UVW tags on an object, a texture tag placed before both uses the first, and one placed between them uses the second: a texture tag takes the UVW tag that follows it.
- The Vertex Map shader (`Xvertexmap`, link in `SLA_DIRTY_VMAP_OBJECT`) shows a vertex map as grey and a vertex colour tag as its colours; both are exported unchanged and read by name. `VertexColorTag::Get(handle, nullptr, nullptr, pointIndex)` reads per-point colours as `ColorA32`; `Get(handle, polygonIndex, VertexColorStruct&)` the per-polygon-vertex kind.
- A texture tag restricted to a selection covers an earlier unrestricted tag on those polygons only.
- `HashMeshSource` (in `scene/mesh_export.cpp`) must cover everything `ExportMesh` reads; extend both together.
- The Sphere primitive's UVs (type Standard): U runs from +X towards +Z, V from the top pole (0) down to the bottom (1); the poles are triangles. `PRIM_SPHERE_PERFECT` is on by default and makes Cinema 4D's renderers ignore the polygons.

## Instances and MoGraph

- `InstanceObject` (`CF/c4d_libs/lib_instanceobject.h`): `IsMultiInstance()`, `GetReferenceObject(doc)`, `GetInstanceCount()`, `GetInstanceMatrix(i)`, `GetInstanceColor(i)`, `GetInstancePersistentHashes()`.
- MoGraph data: find the tag `ID_MOTAGDATA` on the generator and send `MSG_GET_MODATA` with a `GetMoDataMessage` (`CF/c4d_baseeffectordata.h`). Arrays: `MODATA_MATRIX`, `MODATA_COLOR`, `MODATA_WEIGHT`, `MODATA_UVW`, `MODATA_FLAGS`.
- Each clone in a cache stores its MoData index in `BC_ID_MODATAINDEX`; its colour is in `ID_BASEOBJECT_COLOR`. [S; the exporter does not use them]
- **What a Cloner's cache holds** [T, probe in c4dpy]. Mode "Instance" (0): a Null whose children are the clones, one per visible MoData entry, in MoData's order. "Render Instance" (1): the same, but from the second clone of each kind on the child is a render-instance `Oinstance` referring to the first. "Multi-Instance" (2): a Null holding (a) a Null with one real clone per kind and (b) one multi-instance `Oinstance` per kind for all the others; `GetInstanceMatrices()` are global and `GetInstanceColors()` carries their colours. The exporter matches real clones to MoData by index when the counts agree and by position otherwise (`SceneExporter::Walk`).
- **MoGraph colours are display colours** [T]. A Random effector gives values below 0 and above 3. The Color shader (`Xmgcolor`, mode Color) limits them to 0–1, reads them as sRGB and converts to the render space: raw (0.181, 0.345, −0.173) renders as (0.050, 0.091, 0.011) in ACEScg. The exporter does the same with `OcioConverter::TransformColors(…, OCIO_SRGB_TO_RENDERING)`. The Standard renderer itself skips this in multi-instance mode (it shows the raw values, negatives cut off); Cycles Render does not copy that.
- Reading MoData in C++: `tag = op->GetTag(ID_MOTAGDATA)`, `GetMoDataMessage message; tag->Message(MSG_GET_MODATA, &message)`, then `message.modata->GetVectorArray(cinema::MODATA_COLOR)` and so on (the ids need the `cinema::` namespace). Clones with `MOGENFLAG_CLONE_ON` unset or `MOGENFLAG_DISABLE` set are not in the cache.
- Cloner instance mode: `MGCLONER_VOLUMEINSTANCES_MODE` (none, render instance, multi-instance).
- Effectors and fields are fully baked into MoData and caches after `ExecutePasses`.
- Python example of reading clone colours: `PY/examples/animation/simulation/modata_read_color.py`.

## Materials and shaders

- Classic material (`Mmaterial`, `DESC/mmaterial.h`): channels switched by `MATERIAL_USE_*`. Colour `MATERIAL_COLOR_COLOR`, `_BRIGHTNESS`, `MATERIAL_COLOR_SHADER`; luminance `MATERIAL_LUMINANCE_*`; transparency `MATERIAL_TRANSPARENCY_*` (incl. refraction index); alpha `MATERIAL_ALPHA_SHADER`; bump `MATERIAL_BUMP_SHADER`, `_STRENGTH`; normal `MATERIAL_NORMAL_SHADER`; displacement `MATERIAL_DISPLACEMENT_*`.
- Reflectance layers (`CF/c4d_reflection.h`): `GetReflectionLayerCount`, `GetReflectionLayerIndex`; parameter id = `layer->GetDataID() + REFLECTION_LAYER_*` (`_MAIN_DISTRIBUTION`, `_MAIN_VALUE_ROUGHNESS`, `_COLOR_COLOR`, `_FRESNEL_MODE`, `_FRESNEL_VALUE_IOR`).
- Shaders (`BaseShader`, `CF/c4d_basechannel.h`): type ids `Xbitmap`, `Xnoise`, `Xgradient`, `Xfresnel`, `Xlayer`, `Xcolorizer`, `Xfilter`, `Xfusion`, `Xtiles`, `Xvertexmap`, `Xmgcolor`. Bitmap: `BITMAPSHADER_FILENAME`, `BITMAPSHADER_OCIO_INPUT_COLORSPACE_NAME`. Layer shader: `CF/c4d_libs/lib_layershader.h`.
- Bake any shader: `BaseShader::BakeShaderIntoBaseBitmap(bmp, doc, thread, alpha, colorSpace, lwf, hdr, xmin, xmax, ymin, ymax)`.
- Texture paths: `GenerateTexturePath`; `asset:///` URLs resolve to an in-memory URL, so copy the file out with `maxon::Url::IoCopyFile` before giving Blender a path.
- Node materials: `mat->IsNodeBased()`, cast to `NodeMaterial`, `GetGraph(spaceId)`. Space ids: `net.maxon.nodespace.standard`, `com.redshift3d.redshift4c4d.class.nodespace`. Graph reading: `SDK/frameworks/graph.framework/source/maxon/graph.h`.
- **Neutral PBR export for any material:** `maxon::material::MaterialExportInterface::Export(material, config)` requesting the Standard Surface bundle (`SDK/frameworks/exchange.framework/source/maxon/material/materialexport.h`). Worked example: `SDK/plugins/example.nodes/source/exchange/materialexport_command.cpp`.
- Change detection: `mat->GetDirty(DIRTYFLAGS::DATA)`; node graphs `graph.GetModificationStamp()`.

Measured (`tests\c4d\shaders.py`) [T]:

- The shaders inside a shader are reached with `GetFirstShader()` on that shader (what Python's `InsertShader` fills), not with `GetDown()`. Walk both.
- Dirty counters do not identify a shader's state for caching: two shaders of one kind in one material had the same count, and the render's copy of the document differs. Hashing the container as Cinema 4D would save it works: `HyperFile` opened on a `Filename` in memory write mode (`SetMemoryWriteMode(MemoryFileStruct)`), `WriteContainer(shader->GetDataInstanceRef())`, then `MemoryFileStruct::GetData`.
- `BakeShaderIntoBaseBitmap(bitmap, doc, nullptr, false, colorProfile, linearWorkflow, true, 0, w-1, 0, h-1)` into a bitmap made with `Init(w, h, 96)` gives render-space floats; saved with `FILTER_EXR` and `SAVEBIT::USE32BITCHANNELS` and loaded in Blender as `Non-Color` they render like the shader itself (0.02–0.07 off, except the Filter shader).
- `BaseShader::GetRenderInfo()` [T, probe from Python]: a Noise shader with animation speed 0 answers 16 (`SHADERINFO::BUMP_SUPPORT`), with 1.0 it answers 1040 (`TIMEDEPENDENT` added). The plugin bakes such a shader per frame and every other shader once, whatever the frame (`IsTimeDependent` in `materials/material_export.cpp`); a bitmap whose timing range (`BITMAPSHADER_TIMING_FROM`/`_TO`) is not empty counts as changing too (untested).
- Bake reuse [T, `tests/c4d/baked_files.py`]: a second final render of an unchanged scene bakes nothing; at another frame only the shader that changes by itself and one whose settings are animated with keys are baked (the render's copy holds the animated values, so the fingerprint differs); an edited shader is baked again; the IPR at the same frame bakes nothing (it gets the final render's files). Before 0.2.0 the frame was part of every key, so every frame of an animation baked every shader again, and nothing was reused after a restart.
- Gradient shader: along V the gradient starts at the **bottom** of the UV square (V = 1 in Cinema 4D's terms), along U at the left. "Cycle" changes nothing between 0 and 1. Knots: `Gradient::GetKnot(i)` gives `maxon::GradientKnot` with `col`, `brightness`, `pos`, `bias`, `interpolation`; smooth knots match a Color Ramp set to Ease, linear knots Linear.
- Fresnel shader: the gradient's left end is where the surface is seen edge-on, its right end where it faces the viewer: position = |normal · view direction|.
- Checkerboard: U and V frequency count pairs of fields; the field at the top left of the UV square has colour 1.
- The Colorizer's "luminance" and the Fusion mask weigh red, green and blue equally (the test's inputs are grey, so other weights are not ruled out).
- A gradient's interpolation can be set from Python only for all knots at once (`SetData(c4d.GRADIENT_INTERPOLATION, …)`), and that did not change the render.
- Layer shader: `LayerShader::GetFirstLayer()` is the top layer, `GetNext()` goes down; `LAYER_S_PARAM_SHADER_LINK` holds a `BaseLink*` (`GeData::GetVoid`). The blend mode names (`BlendMultiply`, …) are in namespace `cinema`.

Texture tags, stacking, displacement, node materials (`tests\c4d\mapping.py`, `node_materials.py`) [T]:

- Offset and length of a texture tag, with V running down: u' = (u − offsetX) / lengthX, v' = (v − offsetY) / lengthY; tiles = 1 / length. With "Tile" off the Standard renderer shows nothing outside the one copy.
- `TextureTag::GetMl()` gives the tag's texture axes in the object's space. Flat projection: the tag's XY plane, −1…1 becomes 0…1, U to the right and V up the tag's Y. Spherical and cylindrical: around the tag's Y axis; U is 0.5 towards +X and grows towards −Z; V runs from the +Y pole (spherical) or +1 (cylindrical) down.
- A node material has the type `Mmaterial`, like a classic one. `static_cast<NodeMaterial*>(material)->HasSpace(maxon::Id("net.maxon.nodespace.standard"))` (or `"com.redshift3d.redshift4c4d.class.nodespace"`) tells them apart.
- `MaterialExportInterface::Export(material, config)` worked on the render thread in every test run. A Standard node material's colours come back with the sRGB curve applied (0.9 → 0.955); a Redshift material's come back as set. The parameters are read with `ParameterStorageInterface::Extract<TYPE>(data._parameters, maxon::MATERIAL::PORTBUNDLE::STANDARDSURFACE::BASE_COLOR)`.
- In Python a default node graph is made with `mat.GetNodeMaterialReference().CreateDefaultGraph(maxon.Id(space))` after the material is in a document. Standard space: node `bsdf` (asset `net.maxon.render.node.bsdf`), colour port `color`. Redshift: asset `com.redshift3d.redshift4c4d.nodes.core.standardmaterial`, port `…standardmaterial.base_color`. Set with `port.SetPortValue(maxon.Color(…))` inside `graph.BeginTransaction()`.
- "Image Alpha" on a file without an alpha channel uses the picture's brightness. `BaseBitmap::Init(file)` then `GetInternalChannel() != nullptr` tells whether there is one.
- Displacement type "Intensity (Centered)" is the default and moves by (value − 0.5) × 2 × height × strength along the normal.

Volumes, hair, particles, tags [T unless marked]:

- **Volumes.** A Volume Builder's cache is a volume object (`Ovolume`, `VolumeObject`); a Volume Loader's cache is a Volume Set (`Ovolumeset`) named after the frame's file, whose cache holds a Null with one volume object per grid; a Pyro Output's cache is a Volume Set with `density`, `temperature` (in Kelvin: the Pyro tag's defaults are 4000 and 1000) and `velocity`. `VolumeObject::GetVolume()` gives the `maxon::VolumeInterface`; ask it for `GetGridClass()` (`maxon::GRIDCLASS::FOG`, `SDF`) and `GetGridName()`. `maxon::VolumeToolsInterface::SaveVDBFile(url, 1.0, volumes)` writes them; needs `volume.framework` in the module's `apis`.
- A volume object's matrix **is** its grid's transform: voxel size as scale, and for a grid built in the scene the global position (`GetMl()` and `GetMg()` of a builder's volume both carry the voxel size, and the grid transform equals `GetMg()`). So a file saved from memory is already placed in the world, and a file read by a loader is placed by the volume object's parent.
- Two saves of the same grids give files that differ in some bytes, so a file's bytes cannot tell whether a volume changed. The plugin compares the grid objects' addresses and the owner's dirty count.
- **Pyro from a script:** setting the time frame by frame and calling `ExecutePasses` produces the grids but leaves them empty ("Voxel Count: 0"), headless and in the regular Cinema 4D. A Basic Emitter (`Ofpbasicemitter`) run the same way does fill its particle group.
- **Hair.** `HairObject::Lock(doc, nullptr, false, 0)`, `GenerateHair(HAIR_GENERATE_FLAGS_NO_MATERIAL)`, `Unlock()`; the returned `HairGuides` (free with `HairGuides::Free`) give `GetCount()`, `GetGuidePointCount()` and `GetPoints()`, which are in world space already. A Hair object carries `BIT_CONTROLOBJECT` itself, as does a Redshift light: do not treat them as consumed by a generator. The hair material hangs on a `Thairmaterial` tag (`HAIRMATERIAL_TAG_LINK`), not on a texture tag.
- **Particles.** A particle group (`Ofpgroup`, `ParticleGroupObject`) is a point object: `GetParticlePositionsR()`, `GetParticleRadiiR()`, `GetParticleColorsR()` (each empty when the group has no such channel). Points set on a group from Python are gone after the next evaluation; particles must come from an emitter.
- **Tags.** `RegisterTagPlugin(id, name, TAG_VISIBLE, Alloc, "Tcyclesobject"_s, nullptr, 0)` with a description that includes `Tbase`. From Python: `obj.MakeTag(<plugin id>)`.
- **Compositing tag**: `COMPOSITINGTAG_SEENBYCAMERA`, `_CASTSHADOW`, `_SEENBYGI`, `_SEENBYRAYS`, `_SEENBYREFLECTION`, `_SEENBYREFRACTION`, `_SEENBYTRANSPARENCY`; a tag on a parent counts for the objects below (measured against the Standard renderer).
- **Light lists**: `LIGHT_EXCLUSION_LIST` is an `InExcludeData` (`GetObjectCount()`, `ObjectFromIndex(doc, i)`), `LIGHT_EXCLUSION_MODE` include or exclude. An excluded object is neither lit by the light nor throws a shadow from it.

### Redshift Standard Material

The code is `materials/redshift_material.cpp`; measured against Redshift with `tests/c4d/redshift_material.py` [T unless marked].

- Reading a node graph: `nodeMaterial->GetGraph(spaceId)` → `GetViewRoot()` → `GetInnerNodes(NODE_KIND::NODE, ...)`; a node's kind is its asset id, `node.GetValue<decltype(maxon::nodes::AssetId)>()` (`.Get<0>()` of the `IdAndVersion`); ports are `node.GetInputs().FindChild(maxon::Id("..."))`; a value is `port.GetEffectivePortValue<T>()` (an `Opt`; colours are `maxon::Color`, numbers `maxon::Float`, whole numbers `maxon::Int`, a file path `maxon::Url`); what is wired into a port: `port.GetConnections(PORT_DIR::INPUT, ...)`, then `GetAncestor(NODE_KIND::NODE)` of the other end. Frameworks: `graph`, `nodes`, `nodespace`.
- Ids: space `com.redshift3d.redshift4c4d.class.nodespace`; output node `com.redshift3d.redshift4c4d.node.output` with port `...output.surface`; nodes `com.redshift3d.redshift4c4d.nodes.core.<name>` with ports `<node id>.<port>`: `standardmaterial`, `texturesampler` (`tex0` is a bundle whose child `path` is the file; `scale`, `offset` are `maxon::Vector2d`), `bumpmap` (`input`, `scale`, `inputtype` 0 height, 1 tangent-space normal, 2 object-space normal). In Python: `maxon.GraphModelHelper.FindNodesByAssetId(graph, id, True, list)`, `port.SetPortValue(value)` inside `graph.BeginTransaction()`.
- A Redshift colour port holds the value as rendered (no curve to undo).
- **Nodes wired into the Standard Material's inputs** (`materials/redshift_nodes.cpp`) [T: `tests/c4d/redshift_nodes.py`, 20 chains into the Emission Color against Redshift, every channel within 0.0006, 2026-11-12]. The node ids are `com.redshift3d.redshift4c4d.nodes.core.<name>`, a port `<node id>.<port>`; the id of the output a wire comes from is `<node id>.<output>` (`connection.first.GetId()`). Listed from the asset repository (`AssetInterface.GetUserPrefsRepository().FindAssets(AssetTypes.NodeTemplate(), ...)`): 159 Redshift nodes. Translated:

  | Redshift node | Ports | Blender |
  |---|---|---|
  | `rsscalarconstant`, `rscolorconstant` | `val`; `color` | Value, RGB |
  | `rsmathadd`, `sub`, `mul`, `div`, `min`, `max` | `input1`, `input2` | Math ADD to MAXIMUM |
  | `rsmathpow`, `rsmathmod` | `base`, `exponent`; `input`, `divisor` | Math POWER, MODULO |
  | `rsmathabs`, `floor`, `frac`, `sqrt`, `exp`, `sign` | `input` | Math ABSOLUTE to SIGN |
  | `rsmathinv`, `rcp`, `neg`, `ln`, `saturate` | `input` | Math 1 - x, 1 / x, x * -1, LOGARITHM to base e, ADD 0 with clamp |
  | `rsmathsin`, `cos`, `tan` | `input`, `isradians` (off: degrees, a RADIANS node first) | Math SINE, COSINE, TANGENT |
  | `rsmathrange` | `input`, `old_min`, `old_max`, `new_min`, `new_max`, `clamp` | Map Range |
  | `rsmathmix`, `rsmathmixvector`, `rscolormix` | `input1`, `input2`, `mixamount` | Mix (FLOAT, VECTOR, RGBA) |
  | `rsmathaddvector` to `maxvector`, `crossvector`, `dotvector`, `absvector`, `lengthvector`, `normalizevector` | `input1`, `input2`; `input` | Vector Math |
  | `rsmathinvcolor`, `rsmathsubcolor`, `rsmathsaturatecolor` | `input`; `input1`, `input2` | Invert; Mix SUBTRACT; Mix with a clamped result |
  | `rscolormaker`, `rscolorsplitter` | `red`, `green`, `blue`; `outr`, `outg`, `outb` | Combine Color, Separate Color (`outa` is not: named) |
  | `rsvectormaker`, `rsvectortoscalars` | `x`, `y`, `z`; `outx`, `outy`, `outz` | Combine XYZ, Separate XYZ |
  | `rscolor2hsv`, `rshsv2color` | `input` | Separate and Combine Color in HSV (hue 0 to 1 in both, measured) |
  | `rscolorcorrection` | `input`, `gamma`, `contrast`, `hue`, `saturation`, `level` | Gamma, six Vector Math nodes, Hue/Saturation/Value |
  | `texturesampler` | `tex0` > `path`, `scale`, `offset` | Image Texture (as before) |

  Redshift's Color Correct, measured on constant colours rendered by Redshift: gamma g gives x^(1/g) (0.5 at g 2 gives 0.707); contrast c is Schlick's gain with a = 1 - c around 0.5: x / (k(1 - 2x) + 1) below 0.5 and 1 - (1 - x) / (k(2x - 1) + 1) above, k = 1/a - 2 (at c 0.75, 0.2 gives 0.0909 and 0.8 gives 0.9092; 0.5 changes nothing; written as (x + k max(2x - 1, 0)) / (1 + k |2x - 1|)); hue turns the HSV hue by degrees ((0.5, 0.3, 0.1) at 90 gives (0.1, 0.5, 0.1)); saturation multiplies the HSV saturation and is not clamped at 1 (Blender's Hue/Saturation node clamps it: named in the console); level multiplies; in the order gamma, contrast, hue and saturation, level (a case of all four agreed to 0.0003). A Color Correct setting that is wired is named and only its value counts.
- Not translated, each named in the console, the value on the input used: Color Layer, Ramp and Scalar Ramp, Maxon Noise and Noise, Fresnel, Triplanar, UV Projection, the old Normal Map node, bias and gain, user data, switches, AO, curvature and the other utility nodes. The item asked for the easy ones; these need structures of their own in Blender (a ramp's input source and spline, a stack of seven layers with blend modes, noise algorithms Blender does not have, box projection).

| Standard Material port | Principled BSDF | Note |
|---|---|---|
| `base_color` x `base_color_weight` | Base Color of the opaque layer | |
| `diffuse_roughness` | Diffuse Roughness | |
| `metalness` | Metallic | |
| `refl_weight` | Specular IOR Level = weight x 0.5 | |
| `refl_color` | Specular Tint | |
| `refl_roughness` | Roughness | 1 - value when `refl_isglossiness` |
| `refl_ior` | IOR | |
| `refl_aniso`, `refl_aniso_rotation` | Anisotropic (absolute value), Anisotropic Rotation (+0.25 when negative) | untested |
| `refr_weight` | the factor of a Mix Shader between the opaque layer and a second BSDF with Transmission Weight 1 | |
| `refr_color` | Base Color of that second BSDF | |
| `refr_roughness`, `ss_depth`, `refr_abbe`, `refr_thin_walled` | none | a warning each |
| `ms_amount`, `ms_color` | Subsurface Weight; the opaque layer's Base Color is mixed towards `ms_color` by the amount | |
| `ms_radius`, `ms_radius_scale`, `ms_phase` | Subsurface Radius, Scale (x 0.01: centimetres), Anisotropy | |
| `sheen_weight`, `sheen_color`, `sheen_roughness` | Sheen Weight, Tint, Roughness | fainter than Redshift's (a known difference, `docs/roadmap.md`) |
| `thinfilm_thickness`, `thinfilm_ior` | Thin Film Thickness (nm), IOR | untested |
| `coat_weight`, `coat_color`, `coat_roughness`, `coat_ior` | Coat Weight, Tint, Roughness, IOR | |
| `emission_weight`, `emission_color` | Emission Strength, Color | |
| `opacity_color` | Alpha (mean of the three) | |
| `overall_color` | multiplies both layers' colours, and the emission when `overallaffectsemission` | untested |
| `bump_input`, `coat_bump_input` | Normal, Coat Normal, through a Bump node (height; Distance = scale x 0.01) or a Normal Map node | untested |

- Glass casts a straight, tinted shadow in Redshift; in Cycles that is a caustic. The transmitting BSDF is therefore mixed with a Transparent BSDF of the transmission colour by `max(Is Shadow Ray, Is Diffuse Ray)` of a Light Path node.

### Redshift dome light

[T: `tests/c4d/redshift_dome.py`] The map is a parameter of Redshift's own file type: read its path with both levels of the id typed, `CreateDescID(DescLevel(REDSHIFT_LIGHT_DOME_TEX0, 1036765, 0), DescLevel(REDSHIFT_FILE_PATH, DTYPE_STRING, 0))`; untyped levels return nothing in C++ (Python's `light[TEX0, PATH]` works either way). The value is a `file:///` URL. The map runs the other way round than a Sky object's panorama (no mirror in the Mapping node, rotation R⁻¹); brightness is 2^`REDSHIFT_LIGHT_DOME_EXPOSURE0`; without a map the dome shows `REDSHIFT_LIGHT_DOME_COLOR`.

### Redshift lights

Measured by rendering a white wall with Redshift (`tests\c4d\redshift.py`, renderer id 1036219) [T]. With intensity I, exposure e, v = I × 2^e, unit "Image" (the default), distances in metres (document centimetres ÷ 100):

| Redshift light (`REDSHIFT_LIGHT_TYPE`) | The wall renders as | Blender |
|---|---|---|
| Infinite (0) | v / π | sun of strength v |
| Point (1), Spot (2) | v / (π d²) | power 4π v watts |
| Area (3), "Normalize" off | radiance v | power π v × area |
| Area, "Normalize" on | radiance v / area | power π v |

A spot's cone angle is the full angle and its falloff angle lies inside the cone (Blender: `spot_size` = cone, blend from cone − 2 × falloff). A disc's size is its diameter. A sphere-shaped area light is about a hundred times brighter than a rectangle of the same size and setting; not translated. A new light is a point light of intensity 1. The Redshift camera (`Orscamera`) is not an instance of `Ocamera` but answers to the same parameter ids (focal length 500, sensor 1006, film offset 1118 and 1119, clipping, projection).

### The Cycles Light object and the Cycles Light tag

[T: `tests/c4d/cycles_lights.py`, `tests/bridge/test_lights.py`, screenshots `show_cycles_light.py`, 2026-11-12] unless marked. Code: `lights/`, `scene/light_export.cpp`.

- **The object** is an `ObjectData` registered with `RegisterObjectPlugin(ID_LIGHT_OBJECT, ..., 0, ..., "Ocycleslight"_s, icon, 0, OBJECTCATEGORY::LIGHT)`: info 0 (no generator), so the Object Manager shows no green "enabled" check for it, as for a Null; the render dot and layers switch it off. It draws itself in `Draw` for `DRAWPASS::OBJECT` and `HIGHLIGHTS` (`GetHighlightPassColor`), after `SetMatrix_Matrix(op, bh->GetMg())`: `DrawLine` and `DrawCircle(m)` with a matrix in the object's space (the circle lies in `m.sqmat.v1`/`v2`, the radii their lengths), seen on a screenshot of all four types. It does not light Cinema 4D's viewport or its renderers. The exporter finds it in the walk next to `Olight` and `Orslight`.
- **The tag and the object share parameter ids** (`ocycleslight.h`, `tcycleslight.h`, a `static_assert` in `light_settings.cpp`), so one reader serves both. `GetDDescription` hides by `DESC_HIDE` and renames by `DESC_NAME` and `DESC_SHORT_NAME` (Power or "Strength (W/m2)", Color or "Tint"): both seen on screenshots. `GetDEnabling` greys a tag's tab while its switch is off (the switch is chosen by the hundreds of the id).
- **A button on a tag** (`BUTTON` in the description, `MSG_DESCRIPTION_COMMAND` in `Message`) can be pressed from Python with `c4d.CallButton(tag, id)`, also in c4dpy.
- **Measured against the Standard renderer**, a Cycles Light given the power the plugin gives Cinema 4D's light ("Light conversion": 4 pi^2 r^2 x brightness for a point or spot, pi^2 r^2 for an area, pi for a sun, r = 5 m), with its Cast Shadow off as Cinema 4D's lights have none by default: point block difference 0.021 (limit 0.06; Eevee 0.021), sun 0.006 (limit 0.04), spot centre 1.05 px apart and peak ratio 1.009, area (rectangle) 1.11 px and 1.005. Its exclude list keeps a ball dark (754 pixels from lit to black, the floor unchanged).
- **The tag**: with every tab off the light item is exactly the item without the tag. "Take Values from the Light" writes what `TranslateLight` gives (the colour as stored, no Light Multiplier); with the Light tab on, the picture is then the light's own (mean ratio within 0.02, also on a Redshift point light: 1.000). Twice the tag's power gives 2.0 times the light; the Settings tab's Cast Shadow gives a shadow the light's own "None" did not; the Eevee tab's diffuse factor 0.5 halves the light in Eevee (0.50).
- **IPR**: the object's power tripled gave 2.19 times the picture's mean (the scene's second light stays), the tag's Light tab switched on (Blender's 10 W in place of 987 W) 0.60 times; Cycles and Eevee alike.

## Cameras and views

Measured against the Standard renderer in c4dpy, 2026-11-12, on the arrows-and-ball scene of `tests/c4d/coords.py` and on glowing balls in known directions [T: `tests/c4d/cameras.py`, `tests/c4d/camera_tag.py`; the code is `scene/camera_export.cpp`]. Before 0.2.0 the plugin sent every projection but perspective as a parallel camera with the camera's own rotation; every fixed view, every editor view but Perspective and Parallel, the spherical camera and a parallel camera whose plane cuts the scene rendered wrong (`docs/roadmap.md`).

- **`CAMERA_PROJECTION`** (`ocamera.h`): Perspective 0, Parallel 1, and the fixed views Left 2, Right 3, Front 4, Back 5, Top 6, Bottom 7, Military 8, Frog 9, Bird 10, Gentleman 11, Isometric 12, Dimetric 13. Pspherical 14 cannot be set from Python ("parameter set failed", also with the spherical camera enabled): the spherical camera is the Spherical tab's `CAMERAOBJECT_SPC_ENABLE` on a perspective camera.
- **Perspective and Parallel use the camera's rotation; the fixed views do not.** A camera turned by (32°, -22°, 12°) renders exactly the picture of an unturned one. A fixed view looks along its own axes from the camera's position (the picture is centred on it), with the width of a parallel view (1024 units at `CAMERA_ZOOM` 1). The axes are what `BaseView::GetMg()` gives for a view of that projection (right, up, forward): Left (0,0,-1) (0,1,0) (1,0,0); Right (0,0,1) (0,1,0) (-1,0,0); Front the identity; Back (-1,0,0) (0,1,0) (0,0,-1); Top (1,0,0) (0,0,1) (0,-1,0); Bottom (1,0,0) (0,0,-1) (0,1,0); Military, Frog and Bird (-√3/2,0,1/2) (-1/2,0,-√3/2) (0,-1,0), a top view turned by 30°; Gentleman the identity; Isometric (-1/√2,0,1/√2) (-1/√6,2/√6,-1/√6) (-1/√3,-1/√3,-1/√3); Dimetric (-0.937086259,0,-0.349097899) (0.115059725,0.944123613,-0.308855731) (0.32959157,-0.32959157,-0.884725265).
- **Oblique views.** Military, Frog, Bird and Gentleman are oblique parallel projections: `BaseView::GetViewParameter()`'s `scale_z` moves a point at depth z by (scale_z.x · z, -scale_z.y · z) across the picture (the SDK's `CameraToScreen`): Military (0, 1), Frog (0, 2), Bird (0, 0.5), Gentleman (0.3536, -0.3536). The rays then travel along right · (-scale_z.x) + up · scale_z.y + forward, not perpendicular to the picture. Blender's orthographic camera reproduces that when its matrix carries the slant (`docs/reference/blender.md`, "Cameras"); all four match the Standard renderer.
- **Clipping.** A parallel camera clips nothing behind its plane: the part of the scene behind the camera renders (a camera at z = 0 inside the scene showed the ball at z = -80). Its near and far clipping count, measured from the camera (near 400 left only what lies beyond 400, far 380 only what lies before it). A negative near clipping reads back as 0. The fixed views clip nothing at all and ignore both settings. Perspective clips as set. The exporter moves Blender's orthographic camera 10 km back along its rays where nothing is clipped (`UNCLIPPED_DEPTH`) and lets it see as far again beyond the camera's plane (or to the far clipping).
- **The editor's views** (no camera object: `GetRenderBaseDraw()->GetEditorCamera()`, or the active view's for the IPR): the view's projection is `BASEDRAW_DATA_PROJECTION`, and `GetEditorCamera()` then returns a camera of that view whose `CAMERA_PROJECTION` is the view's. For the fixed views it is a camera of its own, at the origin and unturned in a new document; its position (the pan: moved to (100, 500, 30), the Top view's picture moved with it) and its `CAMERA_ZOOM` count, its rotation does not (a turned one rendered the same). So the editor's views are read exactly as camera objects. In c4dpy, `BaseView::GetMg()` gives the view's axes but a stale position ((600, 300, -600), the Perspective view's camera), so the plugin does not use it.
- **The spherical camera** (`CAMERAOBJECT_SPC_ENABLE` with the lat-long mapping, `SPC_MAPPING_LATLONG`): the Standard renderer draws an equirectangular picture. Longitude grows from the camera's forward axis towards its right, the whole turn across the width; latitude 90° at the top. The range `CAMERAOBJECT_SPC_LONG_MIN`/`_MAX`, `_LAT_MIN`/`_MAX` (radians) does three different things: without "Fit Frame" (`CAMERAOBJECT_SPC_FIT_FRAME`) the whole sphere is drawn and what lies outside the range is black; with it the range fills the frame, and then latitude is upside down: the picture's top is -(Lat Min) and its bottom -(Lat Max) (a range of -30° to 60° showed latitude 30° at the top and -60° at the bottom); "Full Range" (`CAMERAOBJECT_SPC_FULL_RANGE_ENABLE`) ignores the range. The exporter sends Blender's equirectangular panorama with longitude as it is and latitude turned over (matches within 0.6 pixels); the cropping without Fit Frame, the cubic mappings, the dome and a spherical camera with a parallel projection are named in the console and rendered as the whole lat-long sphere [the last three untested].
- **The camera's own focus object** (`CAMERAOBJECT_TARGETOBJECT`): once the document is evaluated, Cinema 4D writes its distance into `CAMERAOBJECT_TARGETDISTANCE`, measured along the camera's view axis (1400 for an object 1400 ahead and 300 aside; the straight distance is 1432). So the camera's own depth of field already follows it. Blender measures a focus object the same way (`docs/reference/blender.md`).

**The Cycles Camera tag** (`tags/camera_tag.cpp`, ids in `res/description/tcyclescamera.h`) [T: `tests/c4d/camera_tag.py`, `show_camera_tag.py`]:

- Two tabs, Depth of Field and Lens. With `DEFAULT 1` on both, the Attribute Manager shows both at once and the Lens tab's panorama settings fell below its lower edge (screenshot); with `DEFAULT 1` on the first only, opening the Lens page (`ActiveObjectManager_SetObject(..., page)`) shows it alone.
- `GetDDescription` hides what the chosen lens and panorama type do not use (as Blender's Lens panel, `properties_data_camera.py`), a whole group by its id; `GetDEnabling` greys out the depth of field settings while the checkbox "Depth of Field" is unchecked, and Focus Distance also while a focus object is set (it is read again when the link changes: screenshots unchecked, checked, checked with a focus object, 2026-11-12). Verified on screenshots of every tab.
- **Depth of field of a tagged camera is the tag's alone** (the maintainer, 2026-11-12, as on Blender's camera): the box checked gives depth of field with the tab's values whatever the render setting says, unchecked (the default) none whatever the camera's own values and the render setting say. Measured (`camera_tag.py`, rim pixels of a near and a far box, sharp 327/434): the camera's own focus with the render setting on and no tag 467/3601; the same camera with a tag just added, unchecked, 327/434 (sharp). Cameras without the tag keep Cinema 4D's own depth of field through the render setting. Until that decision id 1000 was a choice of three ("as the camera says", "on", "off"); the tag was never released, so the id became the BOOL.
- A REAL field shows few decimals: Blender's default polynomial coefficients (about 1e-5 and less) read 0 in the Attribute Manager although they are stored and sent exactly. Not changed.
- A LINK field (`LINK id { ACCEPT { Obase; } }`) is read with `BaseContainer::GetObjectLink(id, tag->GetDocument())`; in a render's copy of the document the link resolves to the copy's object.
- The icon is `res/icons/camera_tag.png`, drawn by `tools/make_icons.py` (`camera_tag`): the plugin's hexagon in dark grey with a lighter, shaded circle. In the Object Manager at tag size the circle reads clearly; the dark hexagon stands only a little apart from the manager's dark background (screenshot).

## Lights, cameras, render data

- Standard light (`Olight`, `DESC/olight.h`): `LIGHT_TYPE` (omni 0, spot 1, distant 3, parallel 4, area 8), `LIGHT_COLOR`, `LIGHT_BRIGHTNESS`, `LIGHT_DETAILS_FALLOFF`, `LIGHT_DETAILS_INNERANGLE`/`OUTERANGLE`, `LIGHT_AREADETAILS_SHAPE`, `_SIZEX/Y/Z`, `LIGHT_EXCLUSION_LIST` (`InExcludeData`).
- Redshift light (`Orslight`, `DESC/orslight.h`): `REDSHIFT_LIGHT_TYPE`, physical intensity, exposure and unit parameters.
- Sky: `Osky` with a texture tag (HDRI in the material's luminance or colour channel). `Ostage` switches the active camera and sky.
- Camera (`Ocamera`, `DESC/ocamera.h`): `CAMERA_FOCUS` (focal length, mm), `CAMERAOBJECT_APERTURE` (sensor width), `CAMERA_PROJECTION`, `CAMERA_ZOOM` (parallel), `CAMERAOBJECT_FILM_OFFSET_X/Y`, `CAMERAOBJECT_TARGETDISTANCE`, `CAMERAOBJECT_FNUMBER_VALUE`, near/far clipping. Redshift camera `Orscamera` keeps the same ids for these.
- Render data (`DESC/drendersettings.h`): `RDATA_XRES`, `RDATA_YRES`, `RDATA_PIXELASPECT`, `RDATA_FRAMEFROM`/`TO`/`STEP`, `RDATA_FRAMERATE`.

## Volumes, hair, particles

- `VolumeObject` (`Ovolume`, `CF/c4d_libs/lib_volumeobject.h`): `GetVolume()`. Found inside the Volume Builder's cache.
- Volume Loader (`Ovolumeloader`): file path in `ID_VOLUMESEQUENCE_PATH`.
- Write VDB: `maxon::VolumeToolsInterface::SaveVDBFile(url, scale, volumes, meta)` (`SDK/frameworks/volume.framework/source/maxon/volumetools.h`).
- Pyro output (`Opyro`, `DESC/opyro.h`): cache path `PYRO_OBJECT_CACHE_PATH`.
- Hair: `HairObject` (`CF/c4d_libs/lib_hair.h`), `GenerateHair` → `HairGuides`.
- Particles: `ParticleGroupObject` (`CF/c4d_libs/lib_particlegroupobject.h`): `GetParticlePositionsR`, `ColorsR`, `VelocitiesR`, `RadiiR`.
- Compositing tag (`Tcompositing`, `DESC/tcompositing.h`): `COMPOSITINGTAG_CASTSHADOW`, `_SEENBYCAMERA`, `_SEENBYRAYS`, `_MATTEOBJECT`.

- **A .vdb file is right-handed; Cinema 4D reads and writes it with Z negated** [T: `tests/c4d/volumes.py`, `depth_from_the_side`]. Blender reads the file as it is, so the volume item's matrix negates the file's Z besides swapping the axes (`Coords::RawMatrix`). A check from the front cannot see the difference; this one looks from the side at a ball off-centre along Z.
- Grids from other programs have the class "unknown" (EmberGen: `density`, `flames`); only distance fields are left out. A fire grid that is no temperature (`flames`, 0 to about 4) drives the emission's strength instead of a black body's temperature (the tag's "Fire Grid Holds"; `volume_export.cpp`).
- Particle rotation: `GetParticleAlignmentsR()` gives `maxon::Quaternion<Float32>` (`w`, `v`). MoGraph matrices of a Matrix object: the `MoData` of its `ID_MOTAGDATA` tag, as for a Cloner; the matrices are in the Matrix object's axes.
- A Matrix object (`Omgmatrix`) carries `BIT_CONTROLOBJECT` itself, as a Hair object and a Redshift light do.
- The Cycles Object tag (`tags/object_tag.cpp`, ids in `res/description/tcyclesobject.h`): four tabs, each with an enable checkbox; an enabled tab counts for the object and everything below it until another tag with that tab enabled. An in/exclude list in a description: `IN_EXCLUDE ID { SCALE_V; NUM_FLAGS 0; INIT_STATE 0; ACCEPT { Obase; }; }`; read with `GeData::GetCustomDataType<InExcludeData>()` and `ObjectFromIndex(doc, i)`. In Python `InExcludeData.InsertObject(obj, flags)` puts each object on top of the list.

## Node space (the Cycles nodes)

All [T: `tests/c4d/cycles_nodes.py`] unless marked. Model: `SDK/plugins/example.nodes`. Frameworks: `nodes`, `nodespace`, `graph`, `corenodes`.

- A node space needs no description database (.json) to work (but see "How a node looks" below). Nodes can be made in code: a `Component<..., maxon::nodes::NodeTemplateInterface>` (base `NodeTemplateBaseClass`) whose `InstantiateImpl` creates the ports: `parent.CreateNodeSystem()` → `root.GetInputs().AddPort(id)`, `port.SetType<T>()`, `port.SetValue(maxon::NODE::BASE::NAME, label)`, `port.SetPortValue(default)`; an output gets `SetValue(maxon::nodes::Dynamic, true)` and every input a `Connect(output, Wires(Wires::DEPENDENCY))`; a value-only port gets `SetValue(maxon::nodes::ConstantParameter, true)`; order by `ReqOrderIndex`; `root.SetValue(NODE::BASE::NAME / COLOR, ...)` gives the node its title and header colour; end with `root.SetTemplate(self, args)` and `root.EndModification()`.
- A dropdown is a port of type `maxon::Id` whose options are in the node's description (`data.base.enum`); its value is the id of the option. (An enum type of its own, `DataTypeLib::GetEnumType`, works too but shows the ids instead of the labels.)
- Many nodes from one class: `Class.Create()`, set the instance's data through `Component::Get(ref)` (a `mutable` member, the reference is const), `SetId({id, Id()}, 0, AssetInterface::GetBuiltinRepository())`, then `maxon::nodes::BuiltinNodes::Register(id, template)` and keep the returned `GenericData`.
- The space: a `Component<..., NodeSpaceInterface>` (base `NodeSpaceBaseClass`); its data dictionary can be filled in code: `NODESPACE::SPACEID`, `SPACENAME`, `RENDERERS` (plugin ids of the renderers it belongs to: choosing that renderer makes it the active space), `SPACEKIND`; in `Init`: `CREATEMATERIALGRAPHFUNC` (the default graph), `MATERIALENDNODEIDS`, `NODESYSTEMCLASS`, `DISABLE_PREVIEWS`. Register with `MaterialNodeSpaces::Register(id, space)` in a `MAXON_INITIALIZATION`.
- **Register the space's component under the space's id** (`MAXON_COMPONENT_OBJECT_REGISTER(Class, "<space id>")`): `GetActiveNodeSpaceId()` and the key of a material's graph are the component's id, whatever `SPACEID` says.
- A node system class of its own (`Component<..., NodeSystemClassInterface>`, base `BaseCoreNodesNodeSystemClass().GetClass()`) keeps Cinema 4D's own nodes out of the space: a node template answers `SupportsImpl(cls)` with `cls.GetClass() == OwnClass::GetClass()`.
- **Only `Mmaterial` carries a node graph.** For a material type of a plugin (`MaterialData`), `NodeMaterial::CreateDefaultGraph` fails with "baseMaterial.IsInstanceOf(5703) not fulfilled", whatever `c4d_basematerial.h` says about any BaseMaterial. Settings that belong to the material as a whole are therefore value-only ports of the end node.
- Commands: Node Editor 465002211, its material mode 465002360 (call it after the editor has opened).- **An image dropped on the Node Editor** [code read in the SDK; T as far as a script reaches, `tests/c4d/image_nodes.py`]: the space names the node to make (`NODESPACE::IMAGENODEASSETID`) and four of its ports (`IMAGENODEPORTS`, NodePaths): a `maxon::Color` output, the `maxon::Url` input, and two `maxon::Int` inputs for the first and last frame of a sequence; the SDK's handbook ("Implementing Texture Nodes") says a texture node "has to provide at least" those four. The Cycles Image Texture node got the two frame ports as settings that never show (`tools/make_nodes.py`). `NodeSpaceHelpersInterface::GetNodeSpaceData(spaceId)` gives the data back; the test hook makes a node from it as the drop does, and that node renders the image. A real drop (a mouse) cannot be made by a script: untested.
- **A button on a node** [T in part]: Maxon's own Image node has buttons ("Reload Image" and others): in the description's layout, a "group" entry with `ui.base.groupcolumns` and per button an entry with the command "command", the identifier of a command, and `ui.base.commandtype` "button"; the label is a "command" entry with `string.base.translatedstring` in the names; nothing in the data (read in the installation's `resource.zip`, `shadernodes.module/net.maxon.patterns.module.db*.json`). The identifier is a command of the maxon command system: a `Component<..., maxon::CommandClassInterface>` registered with `MAXON_COMPONENT_OBJECT_REGISTER(Class, maxon::CommandClasses, "<id>")` (`command.framework` in the module's apis; no plugin id). Its data should carry `ARGUMENTS::NODECORE::GRAPHMODEL` and `NODEPATH` (`datadescription_nodes.h`; `INNERPORTPATH` is called "mandatory for all button messages" there, so the plugin's commands take a node from `NODEPATH`, `NODEPATHLIST` or that port, whichever comes). The buttons show in the Node Editor's attributes (screenshot `image_node_buttons`); invoking the command through `CommandDataRef::Invoke` with those two keys works (`image_nodes.py`). A posted mouse click on the button only highlighted it (as it only highlights a dropdown there, which does not open): neither `GetState` nor `Execute` was called, so what a real press hands over is untested; `g_cyclesRenderLogPathCommands=true` writes it to the console.
- **A relative path in a Url port:** `maxon::Url("tex/card.png")` has `URLSCHEME_RELATIVE`; the port's field shows it as "tex/card.png" (screenshot `image_node_relative_path`). `GetComponents` gives the parts of a file Url ("C:", folders, name); `Normalize()` resolves ".." after joining a relative Url to a folder with `+`.
- **Thumbnails of a node space's materials** [T: seen in the application, `tests/c4d/show_cycles_node_ui.py`]: the space names a function (`NODESPACE::CONFIGUREPREVIEWIMAGEREQUESTFUNC`) that puts the class of a `maxon::nodes::PreviewImageProviderInterface` component into each request (`PREVIEWIMAGEREQUEST::PROVIDER`). `Initialize` gets the size (`TEXTURESIZE`) and the node (`SUBJECT`; `subject.GetGraph()` is the graph) and runs while the graph may be read; `ComputeIteration` runs on a job thread and must not touch the graph or a document; `TakeOutput` hands over RGBA float samples, top row first, except for a request with `PREVIEWIMAGEREQUEST::FLIPVERTICALLY` true, which wants the bottom row first: the SDK header says that flip "is performed automatically on the provided buffer", but in 2027.0 it was not, and the Attribute Manager's big preview showed the ball upside down until the provider flipped the rows itself [T: `tests/c4d/show_material_preview.py`, a material red at the top, screenshot]. The requests seen for a material in the Attribute Manager and the Material Manager, all with `INTENT_NODE2D`: the big preview in the Attribute Manager 120 × 120 with the flag and `PROGRESSIONTHRESHOLD` 16; the thumbnail (Material Manager, the Attribute Manager's header, `BaseMaterial.GetPreview`) 90 × 90 without the flag, threshold 0. The same provider serves the material's thumbnail and the preview of every node. `NODESPACE::DISABLE_PREVIEWS` switches all of it off. To keep the thumbnail but nothing of it in the Node Editor, each node's description gets an attribute `net.maxon.node.base.displaypreview` with the default false (no preview on the node; setting `NODE::BASE::DISPLAYPREVIEW` on the template's root did nothing), and layout entries with `ui.base.show` "false" for the group `net.maxon.node.base.preview` (the "Preview" tab of a selected node), the attribute `net.maxon.node.base.nodepreviewimage` and `net.maxon.node.base.displaypreview` (the preview button on the node). Where a sample's alpha is below 1, Cinema 4D draws a checkerboard in the thumbnail, so a background has to be part of the picture. In `c4dpy` no preview is ever asked for: `GetPreview()` returns the checkerboard placeholder.
- **The samples are shown through the plain sRGB curve, whatever the document's colour management** [T, 2026-11-12, application, clean preferences: the thumbnail of a Cycles material that is one Emission of 0.5 read back with `GetPreview(0)`]: 188 in an OCIO document (ACEScg, ACES 2.0 SDR-video view, also for thumbnails), 188 in a "Basic" document with Linear Workflow and 188 without; an Emission of (0.8, 0.3, 0.1) shows (231, 149, 89) in all three. The request carries no document and no colour setting. A final frame of the same value shows as 144 in OCIO (through the view transform), 188 in Basic with Linear Workflow and 128 without (the strips under "Colour management"), so the thumbnails agree with renders only in Basic mode with Linear Workflow.
- A regular Cinema 4D for a test without the user's preferences: `g_prefsPath=<empty folder>`. Needed when a copy of the plugin is installed: of two modules with one id Cinema 4D loads one and skips the other ("Skipping module ... for better matching version"), and `g_additionalModulePath` does not decide which. Such a start shows the "Cinema 4D Home" window, and its layout has no Material Manager: `c4d.CallCommand(12159)` docks one [T: `tests/c4d/show_material_preview.py`].

### How a node looks: the description database

All [T: `tests/c4d/show_cycles_node_ui.py`] unless marked.

- Cinema 4D looks up a description by the node's id. A node built in code needs no description processor: registering a database with a description under the same id is enough (`DataDescriptionDefinitionDatabaseInterface::RegisterDatabaseWithUrl(databaseId, folder)`; the files are `<databaseId>.db.json` for data and layout and `<databaseId>.db.<language>.json` for names; unregister on exit). The SDK's example node "invertnode" is made this way.
- The file is the serialised form the Resource Editor writes; `tools/make_nodes.py` writes it directly. A description is a list of entries (dictionaries) under the node's id, once per category: data (`_descriptions`), layout (`_guiDescriptions`), names (`_db` in the language file). Every list starts with an include of `net.maxon.node.base`. Model: `SDK/plugins/example.nodes/res/nodes/*.json`.
- An entry has `net.maxon.description.base.command` ("attribute" for a port, "group", "string" for the name of a dropdown option, "exportedstring" for the node's own name under the identifier `net.maxon.object.base.name`) and `...base.identifier` (the port's id).
- Data of a port: `data.base.classification` (input, output), `data.base.datatype`, `data.base.defaultvalue`, `data.base.enum` (the options of a dropdown), `net.maxon.node.attribute.constantparameter` (takes no wire), `net.maxon.node.attribute.hideportinnodegraph` (not on the node at first; the Attribute Manager shows it and its circle brings it onto the node).
- Layout of a port: `ui.base.groupid` (`net.maxon.node.base.group.inputs` or `.outputs`, or the id of a group of one's own), `ui.base.guitypeid` (`net.maxon.ui.number`, `.bool`, `.color`, `.vector`, `.string`, `.url`, `.enum`), `ui.base.show` (a condition; while false the Attribute Manager hides the port): `prop.data_type=="RGBA" || prop.data_type=="ROTATION"`, a switch by its id alone or with `!`, joined with `&&`; "false" hides for good.
- A foldable group on the node: an entry with the command "group" in data and in layout; in layout `ui.base.groupid` (its parent), `ui.base.groupdefaultopen`, `ui.base.showgroupinportlist` (true: the group folds on the node as well); its title is a "group" entry in the names.
- A dropdown option's label: a "string" entry named `<port id>#enum#<option id>`.
- `ui.base.show` works on settings (constant ports) as on sockets: the Color Ramp's stops beyond the chosen count are hidden with `(prop.ramp_stops=="s3" || prop.ramp_stops=="s4" ...)`. Colour and vector settings take `net.maxon.ui.color` and `.vector` like sockets do.
- There is no gradient control among the GUI types (`SDK/frameworks/cinema_hybrid.framework/source/maxon/uiconversions.h` lists them; the installed `resource/modules/misc.module` names the same set). There is a spline control; see the next section.

### The spline control (the curves of the curve nodes)

All [T: screenshots of the Node Editor in the application with clean preferences (`tests/c4d/show_cycles_node_ui.py` with `"select": "floatcurve"`, `"rgbcurve"`, `"vectorcurve"`), probes through the IPR test hook's `curve` command, `tests/c4d/cycles_nodes.py`, 2026-11-12] unless marked.

- **A port of type `maxon::SplineMapper`** (`SDK/frameworks/math.framework/source/maxon/splinemapper.h`): in the description the data type is `net.maxon.interface.splinemapper-C`, the GUI `net.maxon.ui.splinemapper`, as in Maxon's own descriptions in the installation's `resource.zip` (`neutron.module`'s Sweep command, `tool_system.module`'s cloth brush), which also show the default's form: `{"referenceDataType": "net.maxon.interface.splinemapper-C", "_knots": [{"position": {"x", "y"}, "interpolation", "tangentLeft": {"x", "y"}, "tangentRight": {"x", "y"}, "flags"}, …], "_min", "_max", "_tension", "_linkTangent", "_linkPosition", "_snap"}`. In code: `port.SetType<maxon::SplineMapper>()`.
- **A default for a new node** can be stored in the generated description: the description's default alone gives the port that value (`GetEffectivePortValue<maxon::SplineMapper>()` read back two knots, range and tangents as written). `SetPortValue` with a spline built in `InstantiateImpl` gives the same; the plugin does both, as for its other settings.
- **In the Node Editor's attribute area** the port shows as a graph with "Load Preset..." and "Save Preset..." below it, a straight line for the default, and nothing of it on the node (`constantparameter`, `hideportinnodegraph`). The arrow next to its label opens the fields of the selected knot: Point X and Y with Lock X and Y, Interpolation ("Spline" is `BEZIER`), the left and right tangent, Break Tangent, Keep Visual Angle, Lock Angle, Lock Length, and "Show in Separate Window". Posted mouse messages open the fields and select a knot; typed text moves it: Point Y of the upper knot set to 0.25 arrived in the port's value, with the tangents kept. A posted click on the curve adds no knot, and posted drags do not work (Cinema 4D's drags read the real mouse), so dragging was not tried.
- **Selecting a knot changes the port's value:** the selection is the knot's `SELECTED` flag (64) in the data (flags 131072 became 131136).
- **The control shows and keeps only the square 0 to 1.** A spline whose range is -1 to 1 is drawn into 0 to 1 with labels 0 to 1, and the first edit writes it back with the range 0 to 1 and its knots moved into it ((-1, -1) became (0, 0)). Neither `data.base.minvalue`/`maxvalue` (as a number or as a 2D vector, with `limitvalue` "both") nor `ui.base.addminmax.minvalue`/`maxvalue` changed that. So the plugin keeps every spline in that square and maps it onto Blender's area (-1 to 1 for Vector Curves; `GetCyclesCurveArea`).
- **The data** (`SplineMapperKnot`): `position`, `interpolation` (0 `BEZIER`, 1 `LINEAR`, 2 `CUBIC`), `tangentLeft` and `tangentRight` relative to the knot, `flags` (the knot's id in the upper 16 bits, `SELECTED` 64, `BREAK_TANGENTS` 1 and the locks); `GetRange`, `GetTension` (1 by default), `GetKnots`, `GetKnotCount`.
- **Evaluating it:** `GetPoint(x)` gives the point on the curve at x, as the control draws it; it wants the knots sorted (`SortKnots` on a copy: the reference is copy-on-write). Measured: within 0.000001 of the exact value on a straight line; left of the first and right of the last knot the knot's own y (flat); with one knot that y everywhere; **with no knots 0 everywhere**; at two knots of one x a jump. `InsertKnot` moves a knot's x into the range (-0.5 and 1.5 became 0 and 1; y is kept).
- **Pitfalls:** `spline == nullptr` on a copy-on-write reference compares contents (`IsEqual`) and crashed in `math.module` for a spline that has a value: test `spline.GetPointer() == nullptr`. `SPLINEMAPPER_KNOT_SET_ID` compiles only inside `namespace maxon` (it names `Int` unqualified): set `flags = maxon::SPLINEMAPPER_KNOT_FLAGS(id << 16)`. Unrelated but met here: in `FormatString` an `@` followed by `{` starts a format specifier, so `"@{\"p\": …}"` swallows the text up to the next `}`.
- **Python** has no `SplineMapper` class: a port's value comes back as a `maxon.Data` that cannot be built or changed, and `SetPortValue(c4d.SplineData())` is refused ("not convertible"). Tests set curves through the IPR test hook (`curve <node> <port> <knots>`, `ipr/ipr_testhook.h`).
- **How a spline reaches Blender** (`materials/cycles_curves.cpp`): Blender's points have no free tangents (`AUTO`, `AUTO_CLAMPED`, `VECTOR`), so the knots alone cannot carry a Bezier curve. Measured against `GetPoint` at 1001 positions, Blender's `mapping.evaluate` of what would be sent, largest difference (shapes: an S curve of four Bezier knots, Bezier knots with tangents set by hand that overshoot to 1.26, a Bezier corner, four cubic knots, three linear knots with a corner):

  | Sent | S curve | hand-set tangents | Bezier corner | cubic | linear corner |
  |---|---|---|---|---|---|
  | the knots (Bezier and cubic as `AUTO`, linear as `VECTOR`) | 0.015 | 0.76 | 0.51 | 0.40 | 0.0028 |
  | `GetPoint` in 50 even steps, `VECTOR` | 0.00089 | 0.0012 | 0.00082 | 0.0018 | 0.0028 |
  | `GetPoint` in 250 even steps | 0.000057 | 0.000073 | 0.000065 | 0.00014 | 0.0028 |
  | **what the plugin sends:** 256 even steps and every knot, points a straight line between their neighbours already gives within 0.0001 of the range's height left out (115, 85, 125, 182 and 3 points) | **0.00011** | **0.00010** | **0.00011** | **0.00011** | **0.0028** |

  The 0.0028 of the corner is Blender's own: its table of 256 steps rounds every corner. A straight line sends its two ends, and Blender then evaluates exactly what its own new node does (difference 0.0 at all 1001 positions, also for Vector Curves' -1 to 1). Two knots at one x give a rise over one 256th of the span. Renders (`cycles_nodes.py`): within 0.0001 of the value `GetPoint` gives, for each node.
- **The show-condition does not hide a port on the node, only in the Attribute Manager.** For the node, register a `maxon::nodes::PortDescriptionProvider` under the node's id (`PortDescriptionProviders::Register`): it is called with the port's description for a category and may change it from the graph's state; setting `HIDEPORTINNODEGRAPH` there for `DATADESCRIPTION_CATEGORY_DATA` hides and shows the port with the node's values. [Not tested: that the node redraws at once when the value is changed by hand.]
- The folder of a node in the Asset Browser: `ui.info.menucategory` in the info of the layout description, the id of a category asset. Ids of categories of one's own follow `<domain>.assetcategory.<name>`.
- **Category assets cannot be stored in the built-in repository** (`StoreAsset` returns "unsupported operation"). Register a `BuiltinAssetLoaderNew` (`MAXON_DECLARATION_REGISTER(maxon::BuiltinAssetLoadersNew, id)`): it is asked for assets by type and id and hands each to the receiver with a dictionary of meta data; `ASSETMETADATA::Category` there is the parent folder. The asset: `CategoryAssetInterface::Create()`, then `SetId({id, Id()}, 0, GetBuiltinRepository())`.
- A category's name in that meta dictionary is not used. It is found as the name string ("exportedstring" `net.maxon.object.base.name`) of a description under the category's id, as a node's name is.
- In/exclusion lists in a description (`IN_EXCLUDE`): `SCALE_V` lets the list grow only while the Attribute Manager has room, which it has not once several tabs are open, so the list is one line. `SMALL_MODE_SIZE` and `BIG_MODE_SIZE` (pixels, empty and filled) give it a height of its own; the bar below it resizes it unless `NO_RESIZABLE` is set [T: `tests/c4d/show_object_tag.py`].
- A grid in a description: a `GROUP { COLUMNS 2; ... }` lines up labels and fields of all its rows; `STATICTEXT { JOINENDSCALE; }` is an empty cell [T].
- Hiding one cell of such a grid (`DESC_HIDE` in `GetDDescription`) moves every cell after it one place up: put what can be hidden into a grid of its own, or last [T].
- Two or more buttons side by side that choose one of several (a tab-like switch): `LONG id { CUSTOMGUI QUICKTABRADIO; CYCLE { A; B; } }` [T: the Simple and Advanced layout of the Render Settings].

- **Undo of a view's camera** [T: c4dpy probe, 2026-11-12, found with F11; not fixed]: after "Camera with Cycles Camera Tag" (`AddUndo(UNDOTYPE::CHANGE_SMALL, view)`, `view->SetSceneCamera(camera)`) and one `DoUndo`, a fresh `doc->GetRenderBaseDraw()->GetSceneCamera(doc)` names an object "RS Camera" (type 1057516) that is not in the document, where the camera before was "Shot"; a Python `BaseDraw` fetched before the undo still names "Shot" (which is why `ipr_add.py`'s check passed). Not tried in the application.
- **Undo and selection** [T: `tests/c4d/ipr_add.py`, a probe in c4dpy]: `SetActiveMaterial` / `SetActiveObject` / `SetActiveTag` after `EndUndo` is an undo step of its own: `DoUndo` then took back only the selection and left the new material. Inside the `StartUndo`..`EndUndo` group one undo removes both. (`materials/cycles_material.cpp` and `native_material.cpp` now select inside the step: one `DoUndo` after `CallCommand` removes the new material, tested in `ipr_add.py` and `native_material.py`.)- `SceneWriter`: a blob belongs to the item that is ended next. Allocate an item's blobs after the previous item's `EndItem`, never before writing a helper item (its material) in between [T: found with the example scene].

- **The Preferences page's description** [T: screenshots `show_prefs_devices.py`, 2026-11-12]: a `PrefsDialogObject`'s `GetDDescription` must load the description by its name (`LoadDescription("Prefscyclesrender"_s)`). With `LoadDescription(node->GetType())` it failed, the page fell back to its static description, and parameters added with `SetParameter` did not show; by name they show (one checkbox per device). In c4dpy, the object `FindPlugin(id, PLUGINTYPE_PREFS)` gives answers `GetDescription` with the static description only; its `GetDParameter`, `SetDParameter` and `CallButton` reach the page's code.
- **A long STATICTEXT label in a page of one column** widens the label column so far that the fields of every row move out of sight (screenshot): keep labels short and put a sentence into the text's value.

## A material type of the plugin (the Cycles Native Material)

All [T: `tests/c4d/native_material.py`, `tests/c4d/show_native_material.py`] unless marked.

- `RegisterMaterialPlugin(id, name, 0, Alloc, "Mcyclesnative"_s, 0)`; the description includes `Mpreview` and `Mbase`. Python makes one with `c4d.BaseMaterial(id)`.
- Data that is no parameter (bytes) lives in members of the `MaterialData` and needs all three of `Write`, `Read` and `CopyTo` (a render works on a copy of the document; undo and the thumbnail use copies too). `HyperFile::WriteMemory` / `ReadMemory` (free what it returns with `DeleteMem`); write a count first and skip the call for nothing.
- A button: `BUTTON id { }` in the description, pressed arrives as `MSG_DESCRIPTION_COMMAND` with `DescriptionCommand::_descId[0].id`; from Python `c4d.CallButton(material, id)`.
- A line of text that changes: `STATICTEXT id { }` with an empty name, its text returned by `GetDParameter`.
- Parameters outside the description still reach `GetDParameter` / `SetDParameter` (`material[2001] = "..."` from Python): used for tests.
- **What the material looks like in Cinema 4D** (thumbnail, Standard renderer) is `CalcSurface`: set `vd->col`. `InitRender` is the place to copy what `CalcSurface` reads, since the main thread may change the original meanwhile.
- **`BaseMaterial::Update(true, true)` alone does not render the thumbnail anew**; `SetDirty(DIRTYFLAGS::DATA)` and `Message(MSG_UPDATE)` before it do.
- A timer on the main thread without a window: a `MessageData` (`RegisterMessagePlugin`, its own plugin id) whose `GetTimer()` returns milliseconds and whose `CoreMessage` gets `MSG_TIMER`. c4dpy has no message loop: tests call the tick themselves (the IPR test hook's "tick").
- `GeGetC4DPath(C4D_PATH_PREFS)` + a folder of the plugin's is where files shared with Blender go (`cyclesrender\edit`, next to `baked` and `volumes`). Baked textures go into `baked\session-<process id>-<time>`, one folder per running Cinema 4D (`materials/baked_files.h`).

## Colour management

- Document settings (`DESC/ddoc.h`): `DOCUMENT_COLOR_MANAGEMENT` (OCIO is the default), `DOCUMENT_OCIO_CONFIG`, `DOCUMENT_OCIO_RENDER_COLORSPACE`, `_DISPLAY_COLORSPACE`, `_VIEW_TRANSFORM`.
- In OCIO mode every colour parameter is already stored in render space (ACEScg by default).
- API: `BaseDocument::GetOcioConfigPath`, `GetActiveOcioColorSpacesNames`, `OcioConverter::Init(doc)` + `TransformColor`. Manual: `SDK/docs/article_ocio.html`.

**"sRGB (legacy)" is "Basic" colour management** [T, 2026-11-12]. In 2027 `DOCUMENT_COLOR_MANAGEMENT` is hidden in the Project Settings; the button "Change Render Space..." (command "Convert Render Space", 450000273) opens a dialog that offers, for a document in ACEScg: ACES2065-1, scene-linear Rec.709-sRGB, scene-linear DCI-P3 D65, scene-linear Rec.2020 (all OCIO) and, below a line, "Legacy (sRGB linear workflow)", which is `DOCUMENT_COLOR_MANAGEMENT_BASIC`, with "Convert Colors" ticked. What that entry writes into Linear Workflow and the colours was not found out: the dialog's popup menu ignored posted mouse and key messages. In Basic mode `GetActiveOcioColorSpacesNames` and `GetOcioProfiles` give empty names; `DOCUMENT_COLOR_MANAGEMENT`, `DOCUMENT_LINEARWORKFLOW` and `DOCUMENT_COLORPROFILE` ("Input Color Profile": sRGB 0, Linear 1, Off 2) were in the container of every document tried. The pre-R26 strings explain the settings (`IDS_LWF_1` to `_8` in `c4d_strings.str`).

**What the Standard renderer calculates with in Basic mode** [T: strips of colour sources rendered in every combination of Linear Workflow and input profile, `tests\c4d\colour.py`; values below are what lands in the buffer, linear values with the curve when Linear Workflow is on]:

- Colour parameters (a material channel's colour times its brightness, a light's colour, gradient knots, a Sky's material, a shader's colour) go through `InitRenderStruct::TransformColor`: with Linear Workflow and input profile sRGB the sRGB curve is undone (a colour of 0.5 renders as linear 0.214, shown as 0.5); with Linear Workflow off and profile Linear the curve is applied (0.5 is calculated as 0.735); otherwise the stored value is used. The brightness multiplies **before** the conversion: (0.8, 0.3, 0.1) at 50 % renders as the linear of (0.4, 0.15, 0.05).
- Images are brought from their own space into the space of the calculation, linear with Linear Workflow and the stored sRGB numbers without it. Their own space is the bitmap shader's "Color Profile" when that is sRGB or Linear (`BITMAPSHADER_COLORPROFILE`), and otherwise linear for an EXR and sRGB for an 8-bit PNG. An embedded ICC profile made no difference (a PNG with and one without an iCCP chunk rendered the same), and the document's input profile sRGB or Linear does not change images either. With the input profile Off nothing is converted, files or colours, with Linear Workflow on or off. So: a PNG of 128 renders as linear 0.216 with Linear Workflow (shown 0.502) and as 0.502 without; an EXR of 0.2 as 0.2 with Linear Workflow and as 0.485 (the curve applied) without; a PNG whose bitmap is set to Linear as 0.502 linear with Linear Workflow (shown 0.737) and 0.737 without.
- A shader baked with `BakeShaderIntoBaseBitmap(…, colorProfile, linearWorkflow, …)` holds exactly what is calculated with, in every combination (a Noise of one colour matched to 0.000).
- Cinema 4D's own 32-bit EXR of a final frame holds the buffer linearised: 0.5 in the buffer is 0.214 in the file, with Linear Workflow on and off (read back with `BaseBitmap.InitWith`).
- Blender reproduces all of it in its working space `Linear Rec.709` (`scene/document_colors.cpp`): colour parameters converted as above; images loaded as `sRGB` (an sRGB file into a linear calculation), `Linear Rec.709` or `Non-Color` (no conversion); and with Linear Workflow off the sRGB numbers given to Blender as if they were linear and written back as they are, which is the same arithmetic. One case has no colour space in Blender: a linear file into a calculation without Linear Workflow needs the curve applied on loading. A bitmap shader is then baked instead (exact); a file named by a node material or a Redshift dome is used as it is, with a warning.
- Measured with the fix [T]: every strip of every combination within 0.0004 of the Standard renderer, also with Multi-Pass on; before it, colours typed in rendered too bright with Linear Workflow on (0.735 for 0.5; means 1.67 to 1.84 times the Standard renderer's) and textures too dark with it off (0.216 for 0.502; 0.55 times). Picture Viewer, screen values of four strips (colour 0.5, PNG 128, diffuse 0.5 under a white light, colour (0.8, 0.3, 0.1)): Standard (128) (128) (128) (204, 76, 26); Cycles Render before (188) (128) (190) (232, 149, 89) with Linear Workflow and (128) (56) (132) (204, 76, 26) without, after (128) (128) (128) (204, 76, 26) and (128) (128) (132) (204, 76, 26). The diffuse strip's 132 is a difference of the scene that OCIO shows as well (146 against 144).
- **The IPR** [T: screenshots in the application, and the test hook's `display` and `picture` commands in `colour.py`]: the window shows a frame the way the Picture Viewer shows a final frame, through the sRGB curve with Linear Workflow on and as it is without; before the fix it always applied the curve (188 for 128 with Linear Workflow off). A snapshot sent to the Picture Viewer was shown without any curve, because the Picture Viewer shows a float bitmap without a profile as it is: too dark ((127) (55) (131) for the strips above with the texture at 55). The snapshot and the saved EXR now hold linear values and carry `ColorProfile::GetDefaultLinearRGB()` as the image profile; the Picture Viewer then applies the curve (strips (127) (128) (130) (204, 76, 25)) and the EXR holds what Cinema 4D's own EXR holds (0.214 for a buffer of 0.5).
- **Not measured:** colours of MoGraph clones, vertex colours, a light's brightness other than 100 %, the transparency colour, the hair material's gradient, the Cycles Object tag's volume colours and the renderer's world colour (all converted like colour parameters, by analogy), a bitmap with a "Custom" profile or an embedded profile other than sRGB, the image colour profile Linear in the Render Settings, Redshift lights and materials and other node materials in Basic mode (sent as before), and Cycles node materials (their colours are Blender's values and are sent as they are).
- **OCIO, found on the way, not changed:** an EXR written by `BaseBitmap.Save` without a colour profile renders in the Standard renderer as if it were sRGB (0.2 becomes 0.033 render-space), while Cycles Render reads it as linear (0.2). EXRs from other programs were not tried; see "A bitmap's OCIO input colour space is not read" in the roadmap.

## Python SDK

`c4d.plugins` offers command, message, object, tag, shader, tool, scene loader/saver, bitmap loader/saver and preference plugins, but no `VideoPostData`. Here it drives the tests: the harness plugin `tests/c4d/harness/cyclesrender_test_harness.pyp` runs a script when it receives `C4DPL_PROGRAM_STARTED` and quits with `c4d.CallCommand(12104)`. To stop a Picture Viewer render the call is `c4d.documents.StopExternalRenderer()`; it blocks, and deadlocks when called from the harness script while a render is running, because the script occupies the main thread. To render off the main thread in a test, subclass `c4d.threading.C4DThread`, call `documents.RenderDocument(clone, settings, bitmap, flags | c4d.RENDERFLAGS_NODOCUMENTCLONE, self.Get())` in `Main`, and cancel with `thread.End(False)` (`tests/c4d/render_cancel.py`). Useful calls proven in the test scripts [T]: `c4d.plugins.FindPlugin(id, type)`, `documents.BaseVideoPost(id)`, `documents.RenderDocument(doc, rd.GetDataInstance(), bitmap, flags)` into a `bitmaps.MultipassBitmap(w, h, c4d.COLORMODE_RGBf)`, `bitmap.GetPixelCnt(…)` into a `memoryview` to read float pixels, `c4d.CallCommand(12161)` (Render Settings) and `12099` (Render to Picture Viewer).

## Plugin IDs

Ids come from developers.maxon.net; the module id is a reverse-domain string the developer owns (`net.maxon.*` is reserved). This project's ids are in `plugin/cyclesrender/source/ids.h`: 1073816 (renderer, shared by the IPR command and by the message plugin that watches for closed documents), 1073899 (Cycles Object tag), 1073985 (Cycles Camera tag), 1073986 (Cycles Light tag), 1073987 (Cycles Light object), 1073900 (Preferences page), 1073908 (the command "Cycles Material"), 1073916 (the Cycles Native Material, shared by the command that makes one) and 1073918 (the message plugin that takes in its edits). The module id is `com.wolfnimations.c4dcyclesrender` (`plugin/cyclesrender/project/CMakeLists.txt`). A fork published under another name needs ids and a module id of its own.

- Commands of the maxon command system (`maxon::CommandClasses`, the buttons on the image nodes) and entries of a dialog's own menu (the IPR window's "Add" menu) are known by ids of their own (a string, a number local to the dialog) and need no plugin id. [T]
- A hook with a description registers that description under its plugin id (`RegisterDescription(id, name)` inside `RegisterVideoPostPlugin`, `PrefsDialogObject::Register`, `RegisterTagPlugin`, …). Two such hooks with one id both register, but share one description: with the Preferences page on 1073816 it showed the render settings. [T] They need separate ids.
- Hooks are otherwise stored per plugin type (`GeRegisterPlugin(type, id, …)`, found with `FindPlugin(id, type)`). The IPR command is registered with the renderer's id 1073816: `FindPlugin(1073816, PLUGINTYPE_VIDEOPOST)` and `FindPlugin(1073816, PLUGINTYPE_COMMAND)` each find their own, `CallCommand` and the dialog work, and all renderer tests pass. [T] The same number serves as core-message id. The same holds for a material type and a command: both are registered with 1073916 and each is found by its type. [T: `tests/c4d/native_material.py`, check `command`] The message plugin of the native material has an id of its own (1073918). A second message plugin, the watch over closed documents (`materials/baked_files.cpp`), is registered with the renderer's 1073816: registration succeeds, it receives `EVMSG_CHANGE` and its timer, and the renderer and the IPR command keep working (`tests/c4d/baked_files_window.py`, `ipr`). [T] Scene hooks were not needed and so not tried.

## Reading the scene for the IPR

- `BaseObject::GetGUID()` identifies an object of the document across changes; scene item ids are built from it. What a generator produces is identified by its generator's id and its position in the generated hierarchy (`MixKeys` in `scene/scene_exporter.cpp`), because cache objects are rebuilt on every change.
- A document built in a test and made active (`InsertBaseDocument`, `SetActiveDocument`) has no caches until `ExecutePasses`; in `c4dpy` nothing evaluates it for you.
- The render size is `RDATA_XRES`/`RDATA_YRES` of the active render data; the Cycles Render settings are the container of the video post with the renderer's id in that render data (`GetDocumentRenderSettings`), or the defaults when the document does not use Cycles Render.
- The view's camera: `BaseDraw::GetSceneCamera(doc)`, else `GetEditorCamera()`; polled on every timer tick, so following the camera does not depend on a change event (whether navigating the editor camera sends one was not checked).

## Unknown until tested

- Progressive output into C4D's own viewport or interactive render region. (Both render: "Render Viewport" 12163 and the Interactive Render Region 430000021. For the region Cinema 4D hands over a document whose generators are not built: call `ExecutePasses` when `RENDERFLAGS::IRR` is set. Progressive refinement is not built.)
- Thread-safety of `MaterialExportInterface::Export`, and whether Redshift materials have a direct Standard Surface exporter.
- The cache structure of Pyro output. (A render instance has no polygon cache; the exporter walks the object it refers to.)
- How stacked texture tags choose a UVW tag and inherit from parents.
