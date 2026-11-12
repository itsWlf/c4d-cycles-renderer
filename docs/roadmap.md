# Status and roadmap

What works, what is approximate, what is missing, and what is planned. This is the project's checklist: tick a box when the work is done and verified, and move anything newly found into the right list. How things work is in [architecture.md](architecture.md); how to work on them is in [development.md](development.md).

Words used here: **verified** means a test compares the feature with Cinema 4D's Standard renderer (or with Redshift, or measures it on its own where neither can serve) and passes. **Untested** means the code exists and has not been run for that case.

## Contents

- [Where the project stands](#where-the-project-stands)
- [Release 0.1.0](#release-010)
- [Open bugs](#open-bugs)
- [Known faults in translation](#known-faults-in-translation)
- [Planned features](#planned-features)
- [What is supported](#what-is-supported)
- [Known limits](#known-limits)
- [Untested](#untested)

## Where the project stands

- Final frames and the IPR work end to end on Windows with Cinema 4D 2027 and Blender 5.2.3, for Cycles and Eevee.
- The test baseline is 46 tests (9 for the bridge alone, 37 inside Cinema 4D). The 44 of then all passed for 0.1.0, and again on 2026-11-12 after the 0.2.0 bug fixes; the two camera tests added since (`test_cameras`, `camera_tag`) and the extended `cameras` passed in the selected run of their work, the whole baseline has not run with them yet; both with one start of Cinema 4D per test and with the tests sharing starts (`docs/development.md`, "Tests"). For each piece of work only the tests for what changed are run (`tools/run_tests.ps1 -Changed`); the whole baseline runs once per sprint, before a commit the maintainer asks for, and before a release.
- Version 0.1.0 is published as the first public release (branch `0.1.0`, tag `v0.1.0`). Work for 0.2.0 is on `main`.

## Release 0.1.0

- [x] Review of all code that is published (`plugin/`, `bridge/`, `tools/`, `protocol/`).
- [x] Repository structure: what is published and what stays local (`.gitignore`).
- [x] Documents for people and for agents: `README.md`, `AGENTS.md`, `docs/`.
- [x] Packaging: `tools/make_release.ps1` builds both packages into `release/<version>/`.
- [x] The bridge as a Blender extension; the plugin finds it in Blender and finds Blender by itself on first start.
- [x] Licences: Apache-2.0 for everything but `bridge/` and the two scripts in `tools/` that run inside Blender, which are GPL-3.0-or-later.
- [x] The tests stay local for this release. Publishing them later (without scenes or pictures that should stay private) would let a contributor check a change the way the project does.
- [x] `examples/cycles_render_examples.c4d` built anew with `tools/make_example_scene.py`: it matches `examples/README.md` and names no file outside the repository.
- [ ] Try the two packages by hand, best on a machine that never had the project: install both, render a frame, open the IPR. (Checked by script so far, on the development machine: the bridge package installed by Blender into a throw-away profile and the packaged plugin folder pass the session self-test in both modes and render a frame that matches the Standard renderer; without the bridge the plugin names what to install. Not yet tried: the installation through Blender's Preferences window and into Cinema 4D's own plugins folder.)
- [x] First commit, tag `v0.1.0`, the two packages on the release page.

## Open bugs

- [ ] **Cycles Native Material: what is edited in Blender's Shader Editor does not reliably arrive in Cinema 4D, so the material is switched off in 0.1.0.** Reported twice from use by hand: first that nodes of the Texture category "and some others" are not synced once they are connected, then, after the fixes below, that nodes and their values still go missing. The cause is not known.
  - **Switched off:** without `g_cyclesRenderNativeMaterial=true` there is no command, and the material type is registered but hidden, so a scene that holds such a material still loads and renders. The code is all there.
  - **Not reproduced by any test.** Every node of the table, wired in from Python, is saved; a Checker wired with simulated mouse events is too; textures arrive in the final render and in the IPR. Not covered: a real hand at the mouse and keyboard, adding nodes from Blender's Add menu, choosing an image in the file browser, changing values with sliders.
  - **Found and fixed on the way** (see "Fixed for 0.1.0"): Ctrl+Z in the editor window and any error in the editor's timer each ended all syncing for the rest of the session. They did not settle it.
  - **Where to go on:** reproduce it by hand with the console of the editor's Blender open (*Window > Toggle System Console*): a failure in the editor's timer is printed there (`_tick` in `bridge/c4d_bridge/scene/native.py`). Compare what the editor shows with the `.blend` and `.state` files in the edit folder, to tell a save that did not happen from one Cinema 4D did not take in. Places not yet ruled out, from reading the code: `_load` takes the first material of a file; in the IPR's Blender the old material is swapped for the new one while the viewport renders.
- [ ] **The IPR can miss that the active document changed, when the document it showed was the only one and is closed.** Found while making the tests run several to one start of Cinema 4D (2026-11-12): `InsertBaseDocument` replaces an untouched "Untitled" document, so closing the inserted one leaves none and Cinema 4D makes a new empty one. In `c4dpy` after `tests/c4d/redshift_material.py` the IPR then never sent the new document (nothing sent in 8 s), kept the closed one's scene in Blender, and so kept its baked files; in a fresh `c4dpy` the same steps worked. With another document in the list the IPR switches every time. Likely cause, not verified: `IprController::Tick` tells documents apart by their address (`doc != _document`), and the new document can be made at the address of the one just freed. Not seen by hand yet. `tests/c4d/baked_files.py` now keeps a second document in the list, so it tests the switch it means to; a test of this case itself is open.
- [x] **Colour management set to sRGB (legacy) gave wrong results** (fixed after 0.1.0, not released). The render space "Legacy (sRGB linear workflow)" is Cinema 4D's "Basic" colour management. There, colours typed into Cinema 4D (materials, shaders, lights, the sky) rendered too bright with Linear Workflow on, because they were not linearised as Cinema 4D does; with Linear Workflow off every texture rendered too dark; with Multi-Pass on the picture got the sRGB curve twice; the IPR window applied the curve also without Linear Workflow; and the IPR's snapshot showed linear values without any curve in the Picture Viewer, too dark. The likely cause of the report: a texture without Linear Workflow, or a snapshot. Now every colour source, the buffer, the Picture Viewer, the IPR window, its snapshot and its saved EXR match the Standard renderer in every combination of Linear Workflow and input colour profile (`tests/c4d/colour.py`; numbers in the SDK reference, "Colour management"). What the dialog's legacy entry does to Linear Workflow and to the colours was not found out (its menu ignores posted input), so all combinations are covered. Not measured in that mode, and converted by analogy: clone colours, the hair gradient, the Cycles Object tag's volume colours, the world colour; see "Untested".
- [x] **Cinema 4D's fixed camera projections and the editor's parallel views rendered wrong** (fixed after 0.1.0, not released). Every projection but Perspective and Parallel (Left, Right, Front, Back, Top, Bottom, Military, Frog, Bird, Gentleman, Isometric, Dimetric) was sent as a parallel camera turned like the camera object, while Cinema 4D ignores the camera's rotation for these and looks along the projection's own axes; the editor's views of those projections (no camera object) therefore showed a front view; the four oblique ones had no slant; a parallel camera clipped what lay behind its plane, which Cinema 4D shows; Cinema 4D's spherical camera rendered as a perspective one. All of it, final frames and the IPR, now matches the Standard renderer (`tests/c4d/cameras.py`, `camera_tag.py`; measurements in the SDK reference, "Cameras and views").
- [x] **Cycles nodes: the three curve nodes had no control for their curves** (fixed after 0.1.0, not released). RGB Curves (C, R, G, B), Vector Curves (X, Y, Z) and Float Curve now have one Cinema 4D spline control per curve in the Attribute Manager, and the setting "Extend" (Blender's Extrapolated or Horizontal). A new node starts with a straight line and renders exactly as Blender's own new node. The plugin evaluates each spline as Cinema 4D draws it and sends it as points joined by straight lines; Blender's curve follows it within 0.0001 of its height (measured on S curves, hand-set tangents, cubic knots and overshooting Bezier segments), corners land exactly. Limits: see "Known limits", "Cycles nodes, curves". Tested by `tests/c4d/cycles_nodes.py` (a curve set as a spline renders as Cinema 4D evaluates it, for each node) and `tests/bridge/test_scene.py` (Cycles and Eevee).

### Fixed for 0.1.0

- [x] **Ctrl+Z in the Shader Editor of a native material ended its syncing.** Blender's undo reads the changed datablocks anew, so the bridge lost the material; the first undo even went back to before the material existed. The bridge now pushes an undo step when the editor opens, finds its datablocks again after every undo and redo, saves the undone state, and steps forward again when an undo goes back beyond the opening of the editor.
- [x] **A preview picture of a material that shows nothing** (a Transparent BSDF, a Holdout) was never taken, so Cinema 4D kept the old thumbnail.
- [x] **Cycles nodes without a control for their value:** Value, RGB and Normal have their value as a setting in the Attribute Manager; the Color Ramp has its interpolation and up to eight stops (position, colour, alpha) there.
- [x] **Editor windows of native materials stayed open** when their material was deleted or its document closed. The plugin now closes the window.

## Known faults in translation

Each was measured and set aside. The test of each reports the number without failing.

- [ ] **Filter shader bakes too dark.** Hue is right, brightness is not (difference 0.31). Try Hue/Saturation, Brightness/Contrast and Gamma nodes instead of baking, or find the colour space the bake works in.
- [ ] **Shaders that work in 3D space are baked over the UV square.** The default Noise ("Texture" space), 3D gradients and the surface shaders show the right kind of pattern in the wrong place. Needs node versions of Cinema 4D's noises or a bake per object in its own space.
- [ ] **A bitmap's OCIO input colour space is not read.** Files are sent as sRGB, or linear for .exr and .hdr, whatever the shader says. Seen while fixing the legacy mode: an EXR written by Cinema 4D's `BaseBitmap.Save` without a colour profile renders in the Standard renderer as if it were sRGB (0.2 becomes 0.033), in Cycles Render as linear (0.2). EXRs of other programs were not tried. (With "Basic" colour management the bitmap's "Color Profile" is read.)
- [ ] **Cubic projection is mirrored or turned on some sides** (94.7 % agreement). It uses Blender's box projection of images; the fix is to build the six sides from nodes, and to apply the tag's offset there too.
- [ ] **Clone colours for MoGraph generators other than the Cloner.** Fracture, MoText, Matrix and the rest render without them. The Color shader's other modes and the Multi shader are not translated.
- [x] **Baked textures are removed, and reused** (fixed after 0.1.0, not released). Each Cinema 4D keeps them in a folder of its own, `<preferences>/cyclesrender/baked/session-<process id>-<time>`; final renders and the IPR reuse a file while nothing it depends on changed (an unchanged shader is not baked again at another frame unless it changes over time by itself); a file is deleted once every document it was made for has been closed and no Blender reads it, and everything goes when Cinema 4D quits. What a crashed session left, and what 0.1.0 left directly in `baked`, is removed at the next start. Left as it is: a document that was never in the document list (made by a script, rendered by the Render Queue or `Commandline.exe`) keeps its files until Cinema 4D quits. Tested by `baked_files` and `baked_files_window`.
- [ ] **Eevee does not blur a deforming mesh.** It ignores the `velocity` attribute; it needs the mesh itself to change over the shutter.
- [ ] **Instancers and lights have no motion blur.** The copies of a Cloner or multi-instance stand still during the shutter.
- [ ] **Pyro has never rendered real smoke in a test.** The code path is the one a Volume Builder uses, but a scripted simulation stays empty. Try a Pyro scene by hand, then check the density scale and the fire's temperature against Redshift.
- [ ] **Hair material:** only the colour gradient and the thickness are read.
- [ ] **Redshift sheen is fainter** than in Redshift (difference 0.18).
- [ ] **Redshift transmission depth, dispersion and thin-walled** are not translated; depth could become a Volume Absorption node.

## Planned features

- [ ] **macOS.** What a port needs is listed in the development guide. Nothing has been run on a Mac.
- [ ] **Team Render.** Nothing is built or tested. Each client would render whole frames with its own plugin, bridge and Blender; open questions are which files travel with the scene (textures of node materials, .vdb files, a dome light's map) and how to test with a second machine.
- [ ] **Skip the export of unchanged objects in the IPR.** Today the whole scene is exported after every change to find what changed, which is untested with large scenes.
- [ ] **A region drawn in the IPR window itself**, and the plugin's picture drawn live into Cinema 4D's own viewport.
- [ ] **Keep one Blender for Cinema 4D's Interactive Render Region.** Each refresh is a render job of its own today and starts its own Blender (about 3 s).
- [ ] **More render passes:** Cryptomatte, motion vectors, object and material index, shadow catcher, light groups, custom AOVs.
- [ ] **More shaders as nodes instead of baked:** Tiles and Brick, circular and diagonal gradients, further blend modes of Fusion.
- [ ] **More of Redshift:** node structures beyond a Texture plugged into an input, IES and sun lights, the remaining area shapes, the Redshift Object tag, the camera's depth of field and its own projections (the Redshift camera's panoramas are not read; it is read like Cinema 4D's camera).
- [ ] **The preview and the viewport look of a Cycles Native Material** before its first edit and in Cinema 4D's viewport (grey today).
- [ ] **The window's texts in the string resources** (English strings in the code today).
- [ ] **Pause the IPR while a Picture Viewer render runs.**
- [ ] **Redshift as the baseline.** Compare with Redshift instead of Cinema 4D's Standard renderer, in the tests and as the model for what is translated: Redshift is closer to Cycles in what it can do (bounced light, depth of field, physically based materials and lights).
- [ ] **An image dropped on the Node Editor makes an Image Texture node** that names the file.
- [ ] **An object dropped on the Node Editor makes the node that refers to it** (for example Object Info or a texture coordinate from the object).
- [ ] **A more responsive IPR:** the picture follows the camera while it is being moved, and objects while they are being moved, instead of updating when the movement ends.
- [ ] **More Blender versions:** 5.0 to 5.3 at least (5.2 only today). What has to be checked per version is listed in the development guide, "Moving to a new Blender version".
- [ ] **More Cinema 4D versions:** 2024 to 2027 at least (2027 only today). Each version needs a build against its own SDK and its own plugin package.
- [ ] **Blender's device settings in the Cycles Render preferences:** the Cycles render device (CUDA, OptiX and the others) and which devices are used, as in Blender's own Preferences.

## What is supported

| Area | Supported | Verified by |
|---|---|---|
| Final frames | Stills and sequences to the Picture Viewer and through `Commandline.exe`; one Blender per render job; only changed items sent per frame; cancel within 0.1 s | A 50-frame sequence starts Blender once and sends meshes and materials once |
| IPR | Start, stop, pause, reload; delta sync; camera list, lock, free view; passes; both engines; Fit Window and fixed resolutions; snapshot and save; restart after a lost Blender | Tests through the controller and through the real window |
| Engines | Cycles on CPU and GPU, Eevee | Every render setting is read back from Blender |
| Geometry | Meshes, n-gons, Phong and Normal tags, all UVW tags, vertex maps, vertex colours, polygon selections | Against the Standard renderer |
| Instances | Render instances, multi-instances, Cloners in all three modes with clone colours, automatic batching of equal clones | 100,000 clones arrive as one instancer |
| Animation | Deformers (points-only updates), animated transforms, motion blur for objects, camera and deforming meshes | Streak lengths measured for both engines |
| Cameras | Perspective, parallel and every fixed projection (Left to Dimetric, the oblique ones with their slant), the editor's views of each when no camera is active, sensor, film offset, clipping as Cinema 4D clips (a parallel view not behind its plane, the fixed views not at all), Cinema 4D's spherical camera (lat-long), depth of field; Redshift camera; in final frames and the IPR | 42 variants against the Standard renderer, eight of them in the IPR |
| Cycles Camera tag | Depth of Field tab, as on Blender's camera: a checkbox (unchecked by default, as Blender's) that alone decides for a tagged camera, checked on with the tab's values and unchecked off, whatever the camera's own values and the render setting say; focus object (its distance along the view axis, followed in the IPR; Focus Distance greyed out while one is set), f-stop, Blender's aperture (blades, rotation, ratio). Adding the tag to a camera that used its own depth of field switches it off until the box is ticked. Lens tab: the camera's projection, perspective, orthographic (Blender's Orthographic Scale) or panoramic (Cycles only) with all seven panorama types and their settings | Depth of field measured by itself (soft and sharp edges) in final frames and the IPR; equirectangular against Cinema 4D's spherical camera; a fisheye's known directions at known pixels; every field read back in Blender |
| Lights | Omni, spot, infinite, area; Redshift point, spot, infinite, area; include and exclude lists | Against the Standard renderer, and against Redshift |
| Environment | Sky object with a panorama, Redshift dome light, default world | 22 view directions; six against Redshift |
| Classic materials | Colour, luminance, transparency, reflectance (one lobe and a clear coat), alpha, bump, normal, displacement; texture tag offset, tiles, flat, spherical and cylindrical projection; stacked tags with alpha | Per channel against the Standard renderer |
| Shaders | Bitmap, colour, checkerboard, gradient, colorizer, fresnel, fusion, layer, vertex map, MoGraph colour as nodes; everything else that depends on UVs baked | 17 cases against the Standard renderer |
| Redshift materials | The Standard Material node, option by option | 13 materials against Redshift |
| Other node materials | Reduced to a Standard Surface | Base colour only |
| Cycles nodes | 89 of Blender's 98 shader nodes, with Blender's panels, labels and menu folders; the material settings Blender keeps on a material; the curves of the curve nodes through Cinema 4D's spline control | Blender builds every node with every socket; curves within 0.0001 of Cinema 4D's spline |
| Cycles Native Material (switched off in 0.1.0) | A Blender material stored in the scene, edited in Blender's Shader Editor, preview from Blender | Round trip, save, load, copy, render, IPR |
| Volumes | Volume Loader (also sequences, also files of other programs), Volume Builder; density, colour, emission, fire through the Cycles Object tag | Against the mesh the fog was built from |
| Hair, particles | Hair as curves with thickness and a colour gradient; particle groups as spheres with a colour each | Position and extent measured |
| Object controls | Compositing tag (camera, shadow, GI, reflection, refraction); Cycles Object tag: visibility, holdout, shadow catcher, light linking, instances on particles and Matrix objects, volume shading | Against the Standard renderer |
| Output | Render passes as multi-pass layers (17), render region, denoising, the colour pipeline for OCIO and Basic ("Legacy (sRGB linear workflow)") colour management, in final frames and the IPR | A colour chart matches to 0.0006; eight colour sources in six combinations of the Basic settings to 0.0004; the IPR's display and saved picture to 8-bit precision |
| Cinema 4D's own commands | "Render Viewport" and the Interactive Render Region | By eye |

## Known limits

- **The IPR needs a desktop session** and renders into a hidden Blender window. Its resolution can exceed the desktop's (3840 × 2160 tested), but every frame is read back on Blender's main thread: about 1 ms at 960 × 540 and 20 ms at 3840 × 2160.
- **The IPR shows no motion blur** (Cycles' viewport has none), and it uses Blender's viewport settings, so small differences from the final render are possible.
- **On the CPU a 1920 × 1080 IPR is slow to react** (0.8 s for an edit). Use the GPU or a smaller IPR resolution.
- **Lights without falloff** cannot be reproduced by a physically based renderer; they match Cinema 4D at one distance. Cinema 4D's area lights shine to both sides, Blender's to one.
- **Reflectance** is reduced to one specular lobe and one clear coat; the Principled BSDF has no layer stack.
- **Displacement** moves only the points a mesh has (no sub-polygon displacement).
- **A Sphere primitive with "Render Perfect"** shows its polygons; Cinema 4D's own renderers draw it perfectly round.
- **Clones that all differ** become one instancer each, which is slow from a few thousand on.
- **Two texture tags on one object that use different UVW tags:** the mesh has one render UV set, that of the tag covering the whole object.
- **Render passes** are written as rendered; the view transform is never baked into them. With "Basic" colour management that is what Cinema 4D's own layers hold too. Eevee has no UV pass and no indirect or transmission passes.
- **Render settings without effect:** the four Subdivisions values act on Blender's adaptive subdivision, which the plugin does not use (it sends the mesh Cinema 4D subdivided).
- **Cycles Native Material:** Cinema 4D's undo does not record an edit made in Blender; images are named by their full path, which works on one machine. Undo in Blender's editor is one history for all editor windows, and does not go back beyond the opening of the editor that was opened last.
- **Cycles nodes, Color Ramp:** at most eight stops, as plain settings (Cinema 4D's node descriptions have no gradient control); colour mode RGB only.
- **Cycles nodes, curves:** Cinema 4D's spline control shows and keeps only the square 0 to 1 (a spline of another range is rewritten into it on the first edit). For Vector Curves that square stands for Blender's -1 to 1 on both axes: the control's 0.5 is Blender's 0, and its labels read 0 to 1. Blender's points have no free tangents, so a curve is sent as up to a few hundred points (Blender's own table has 256 steps, which also rounds a corner by up to a quarter step times the change of slope, 0.003 for slopes of 2 and -1). Two knots at one position (a jump) become a rise over one 256th of the curve. A curve without knots gives 0, as Cinema 4D evaluates it, with a warning in the console. Selecting a knot in the control changes the setting's value (the selection is stored in it).
- **Cycles nodes:** what is wired into the output's Volume socket is not rendered.
- **Saving the IPR picture** writes RGB; alpha is not saved.
- **Panoramic cameras are Cycles only.** Eevee renders a panoramic camera (the tag's, or Cinema 4D's spherical camera) as a perspective camera with the camera's focal length; the console says so. The IPR's free view (the fast path for a moving camera) is used for perspective cameras only; any other lens, depth of field or a film offset goes through a camera object.
- **Cinema 4D's spherical camera:** only the lat-long mapping is translated. A range without "Fit Frame" (which crops in Cinema 4D) is rendered as the whole sphere, the cubic mappings and the dome as lat-long; each is named in the console. Stereo is not read.
- **In the IPR's "Fit Window"** an equirectangular or central cylindrical panorama shows more of its range around the camera's picture; the other panorama types are stretched to the window instead [untested].
- **The fixed views and a parallel view without near clipping** see 10 km to either side of the camera's plane (Blender's camera stands 10 km back); a scene deeper than that is cut. An oblique view's camera has no motion blur of its own.
- **Cycles Camera tag:** the Attribute Manager shows its numbers with few decimals, so Blender's default polynomial coefficients (1e-5 and smaller) read 0; they are stored and sent exactly.
- **Thumbnails of Cycles node materials** are shown by Cinema 4D through the plain sRGB curve whatever the document's colour management: they agree with renders in a "Basic" document with Linear Workflow, and are brighter than renders in an OCIO document (no view transform: an Emission of 0.5 shows 188, a render 144) and in a Basic document without Linear Workflow (render 128). The preview Blender also loads textures in its own default way. A fix would have to find the material's document from the preview request, which carries none.
- **Colour:** render spaces other than ACEScg, linear Rec.709 and linear Rec.2020 are not supported; Cinema 4D's OCIO configuration is not handed to Blender (both use their own ACES definitions, which agree). With "Basic" colour management and Linear Workflow off, Cinema 4D puts the sRGB curve on linear images (EXR, HDR, a bitmap set to the Linear profile), which Blender cannot do when loading a file: a bitmap shader is then baked, and a file named by a node material or a Redshift dome light is used as it is, with a warning. Redshift lights and materials, other node materials and Cycles nodes are sent as in OCIO mode in a Basic document.

## Untested

Built, and never run for these cases:

- Display scaling other than 100 %; several GPUs; OptiX denoising; custom OCIO configurations.
- The spline control of the curve nodes: dragging knots and tangents with the mouse, its presets, its separate window, and a curve edited while the IPR runs (selecting a knot and typing its position were tried, by posted clicks and keys).
- In the IPR window: mouse-wheel zoom and dragging the zoom field, including that both keep the zoom in steps of 0.1 % (no test can perform them; dragging the picture and docking the window were tried by hand).
- Motion Blur Position and Rolling Shutter in a render; simulations without a cache under motion blur.
- Textures in Redshift and other node materials, their tiling and offset, bump and normal maps, thin film, anisotropy.
- More than two stacked material layers; the Layer shader as nodes; the Physical mode of the Fresnel shader.
- A Volume Loader's scale, grid selection, offset, loop and speed settings.
- Particle rotation on instances; particle colours other than the emitter's default.
- Scenes saved with earlier builds (parameter ids were kept stable for this, but no old scene was opened).
- Cycles Camera tag: Mirror Ball, Equiangular Cubemap Face, Fisheye Equisolid, Fisheye Lens Polynomial and Central Cylindrical rendered (their fields are read back in Blender, their pictures were not checked); depth of field with an orthographic or panoramic lens; the aperture's blades, rotation and ratio in a picture (they reach Blender, nobody looked at the bokeh); the tag on a Redshift camera; a focus object inside a generator's cache; the tag's values animated; motion blur of a camera with a fixed or oblique projection; Eevee with depth of field from the tag. Cinema 4D's spherical camera with a cubic mapping, the dome, stereo, or a parallel projection. The editor's views in the regular application with a real hand panning and zooming (the tests move the editor camera from Python).
- Creating and wiring Cycles nodes by hand, and saving and loading a scene with such a material (the tests build graphs from scripts).
- Packed or generated images in a Cycles Native Material; several materials open in the editor at once.
- "Basic" colour management ("Legacy (sRGB linear workflow)"): clone colours, vertex colours, the hair material's gradient, the Cycles Object tag's volume colours and the world colour of the Render Settings (converted like other colour parameters, by analogy); a light's brightness other than 100 %; the transparency colour (sent as stored); a bitmap with the "Custom" profile or an embedded profile other than sRGB; the image colour profile Linear in the Render Settings; Redshift and other node materials and Cycles nodes in that mode; the IPR's snapshot of a "Fit Window" frame.
- Baked textures: a shader baked again after its texture file changed on disk, after the document's colour settings changed, or per frame for a bitmap playing a movie (the key covers all three); two Cinema 4D processes sharing one preferences folder (the lock file was tested against a file held by Python); a file that could not be deleted because a program had it open (it is tried again once a second and at the end); the Render Queue and `Commandline.exe` (their documents keep their files until the end of the session).
