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
- The test baseline is 42 tests (8 for the bridge alone, 34 inside Cinema 4D), all passing.
- Version 0.1.0 is being prepared as the first public release. Nothing has been published yet.

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
- [ ] First commit, tag `v0.1.0`, upload the two packages to the release page.

## Open bugs

- [ ] **Cycles Native Material: what is edited in Blender's Shader Editor does not reliably arrive in Cinema 4D, so the material is switched off in 0.1.0.** Reported twice from use by hand: first that nodes of the Texture category "and some others" are not synced once they are connected, then, after the fixes below, that nodes and their values still go missing. The cause is not known.
  - **Switched off:** without `g_cyclesRenderNativeMaterial=true` there is no command, and the material type is registered but hidden, so a scene that holds such a material still loads and renders. The code is all there.
  - **Not reproduced by any test.** Every node of the table, wired in from Python, is saved; a Checker wired with simulated mouse events is too; textures arrive in the final render and in the IPR. Not covered: a real hand at the mouse and keyboard, adding nodes from Blender's Add menu, choosing an image in the file browser, changing values with sliders.
  - **Found and fixed on the way** (see "Fixed for 0.1.0"): Ctrl+Z in the editor window and any error in the editor's timer each ended all syncing for the rest of the session. They did not settle it.
  - **Where to go on:** reproduce it by hand with the console of the editor's Blender open (*Window > Toggle System Console*): a failure in the editor's timer is printed there (`_tick` in `bridge/c4d_bridge/scene/native.py`). Compare what the editor shows with the `.blend` and `.state` files in the edit folder, to tell a save that did not happen from one Cinema 4D did not take in. Places not yet ruled out, from reading the code: `_load` takes the first material of a file; in the IPR's Blender the old material is swapped for the new one while the viewport renders.
- [ ] **Colour management set to sRGB (legacy) gives wrong results.** Reported from use by hand: with the document's colour management switched from OCIO to the legacy sRGB mode, a render sent to the Picture Viewer comes out too dark. Not investigated; whether the IPR is affected too is not known. All tests run with the default (OCIO, ACEScg render space); the legacy mode is not covered by any of them. Start by rendering the colour test's chart (`tests/c4d/colour.py`) in a document set to the legacy mode, as a final frame and in the IPR, next to the Standard renderer.
- [ ] **Cycles nodes: the three curve nodes have no control for their curves.** RGB Curves, Vector Curves and Float Curve render with Blender's default curves (which change nothing); neither the Node Editor nor the protocol carries curves yet. Cinema 4D's node descriptions know a spline control (`net.maxon.ui.splinemapper`), which has not been tried. Until then a Cycles Native Material keeps curves.

### Fixed for 0.1.0

- [x] **Ctrl+Z in the Shader Editor of a native material ended its syncing.** Blender's undo reads the changed datablocks anew, so the bridge lost the material; the first undo even went back to before the material existed. The bridge now pushes an undo step when the editor opens, finds its datablocks again after every undo and redo, saves the undone state, and steps forward again when an undo goes back beyond the opening of the editor.
- [x] **A preview picture of a material that shows nothing** (a Transparent BSDF, a Holdout) was never taken, so Cinema 4D kept the old thumbnail.
- [x] **Cycles nodes without a control for their value:** Value, RGB and Normal have their value as a setting in the Attribute Manager; the Color Ramp has its interpolation and up to eight stops (position, colour, alpha) there.
- [x] **Editor windows of native materials stayed open** when their material was deleted or its document closed. The plugin now closes the window.

## Known faults in translation

Each was measured and set aside. The test of each reports the number without failing.

- [ ] **Filter shader bakes too dark.** Hue is right, brightness is not (difference 0.31). Try Hue/Saturation, Brightness/Contrast and Gamma nodes instead of baking, or find the colour space the bake works in.
- [ ] **Shaders that work in 3D space are baked over the UV square.** The default Noise ("Texture" space), 3D gradients and the surface shaders show the right kind of pattern in the wrong place. Needs node versions of Cinema 4D's noises or a bake per object in its own space.
- [ ] **A bitmap's OCIO input colour space is not read.** Files are sent as sRGB, or linear for .exr and .hdr, whatever the shader says.
- [ ] **Cubic projection is mirrored or turned on some sides** (94.7 % agreement). It uses Blender's box projection of images; the fix is to build the six sides from nodes, and to apply the tag's offset there too.
- [ ] **Clone colours for MoGraph generators other than the Cloner.** Fracture, MoText, Matrix and the rest render without them. The Color shader's other modes and the Multi shader are not translated.
- [ ] **Baked textures are never removed** from `<preferences>/cyclesrender/baked`.
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
- [ ] **More of Redshift:** node structures beyond a Texture plugged into an input, IES and sun lights, the remaining area shapes, the Redshift Object tag, the camera's depth of field.
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
| Cameras | Perspective and parallel, sensor, film offset, clipping, depth of field; Redshift camera | Ten variants against the Standard renderer |
| Lights | Omni, spot, infinite, area; Redshift point, spot, infinite, area; include and exclude lists | Against the Standard renderer, and against Redshift |
| Environment | Sky object with a panorama, Redshift dome light, default world | 22 view directions; six against Redshift |
| Classic materials | Colour, luminance, transparency, reflectance (one lobe and a clear coat), alpha, bump, normal, displacement; texture tag offset, tiles, flat, spherical and cylindrical projection; stacked tags with alpha | Per channel against the Standard renderer |
| Shaders | Bitmap, colour, checkerboard, gradient, colorizer, fresnel, fusion, layer, vertex map, MoGraph colour as nodes; everything else that depends on UVs baked | 17 cases against the Standard renderer |
| Redshift materials | The Standard Material node, option by option | 13 materials against Redshift |
| Other node materials | Reduced to a Standard Surface | Base colour only |
| Cycles nodes | 89 of Blender's 98 shader nodes, with Blender's panels, labels and menu folders; the material settings Blender keeps on a material | Blender builds every node with every socket |
| Cycles Native Material (switched off in 0.1.0) | A Blender material stored in the scene, edited in Blender's Shader Editor, preview from Blender | Round trip, save, load, copy, render, IPR |
| Volumes | Volume Loader (also sequences, also files of other programs), Volume Builder; density, colour, emission, fire through the Cycles Object tag | Against the mesh the fog was built from |
| Hair, particles | Hair as curves with thickness and a colour gradient; particle groups as spheres with a colour each | Position and extent measured |
| Object controls | Compositing tag (camera, shadow, GI, reflection, refraction); Cycles Object tag: visibility, holdout, shadow catcher, light linking, instances on particles and Matrix objects, volume shading | Against the Standard renderer |
| Output | Render passes as multi-pass layers (17), render region, denoising, the colour pipeline for OCIO and Basic colour management | A colour chart matches to 0.0006 |
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
- **Render passes** are written as rendered; the view transform is never baked into them. Eevee has no UV pass and no indirect or transmission passes.
- **Render settings without effect:** the four Subdivisions values act on Blender's adaptive subdivision, which the plugin does not use (it sends the mesh Cinema 4D subdivided).
- **Cycles Native Material:** Cinema 4D's undo does not record an edit made in Blender; images are named by their full path, which works on one machine. Undo in Blender's editor is one history for all editor windows, and does not go back beyond the opening of the editor that was opened last.
- **Cycles nodes, Color Ramp:** at most eight stops, as plain settings (Cinema 4D's node descriptions have no gradient control); colour mode RGB only.
- **Cycles nodes:** what is wired into the output's Volume socket is not rendered.
- **Saving the IPR picture** writes RGB; alpha is not saved.
- **Colour:** render spaces other than ACEScg, linear Rec.709 and linear Rec.2020 are not supported; Cinema 4D's OCIO configuration is not handed to Blender (both use their own ACES definitions, which agree).

## Untested

Built, and never run for these cases:

- Display scaling other than 100 %; several GPUs; OptiX denoising; custom OCIO configurations.
- In the IPR window: mouse-wheel zoom and dragging the zoom field (no test can perform them; dragging the picture and docking the window were tried by hand).
- Motion Blur Position and Rolling Shutter in a render; simulations without a cache under motion blur.
- Textures in Redshift and other node materials, their tiling and offset, bump and normal maps, thin film, anisotropy.
- More than two stacked material layers; the Layer shader as nodes; the Physical mode of the Fresnel shader.
- A Volume Loader's scale, grid selection, offset, loop and speed settings.
- Particle rotation on instances; particle colours other than the emitter's default.
- Scenes saved with earlier builds (parameter ids were kept stable for this, but no old scene was opened).
- Creating and wiring Cycles nodes by hand, and saving and loading a scene with such a material (the tests build graphs from scripts).
- Packed or generated images in a Cycles Native Material; several materials open in the editor at once.
