# Changelog

All notable changes to Cycles Render. Version numbers follow [Semantic Versioning](https://semver.org); a release is the Cinema 4D plugin and the Blender bridge with the same number.

## 0.3.0 (in preparation)

- The Cycles Light's sun shows its Angle in the viewport: a cone around its ray, as wide as the angle (nothing at 0). It has no handle.
- The IPR window's "Add" menu is two menus with icons: "Objects" (Cycles Camera, which adds a camera with a Cycles Camera tag, Cycles Light, Cycles Environment, Cycles Fog, Cycles Scatter, Cycles Proxy) and "Materials" (Cycles Material, Cycles World Material). The entries that put the plugin's tags on the selected objects are gone from it; the tags are in the Object Manager's Tags menu as before.
- The IPR window can be made narrower: its status line no longer sets the window's least width (it was the texts' fixed columns plus what renders, as Blender names it). In a narrow window the time, the memory and the device close up, a text that does not fit ends in "..." and shows whole in a bubble under the pointer. The least width is now the toolbar's (707 pixels here).
- Extensions menu: the plugin's entries are one submenu "Cycles Render" with the IPR first, then "Materials" and "Objects" in the order of the IPR window's menus (before, Cinema 4D listed them by itself under "Cyclesrender" in alphabetical order). A new command "Cycles Camera" adds a camera with a Cycles Camera tag from there and from the Commander.
- Cycles Object tag, Light Linking tab: shadow linking. "Override Shadow Linking" with its own mode and list of lights decides which lights' shadows the object casts, apart from which lights light it. While it is off the object's shadows follow its light linking as before, so a scene saved earlier renders as it did. Cycles and Eevee.
- Cycles Camera tag: a button "Take from Camera" beside the focus distance writes the focus distance of the camera the tag sits on into the tag (its focus object's distance, or the distance the camera keeps; also the Redshift camera's, with its automatic focus), in one undo step.
- Cycles Light and Cycles Light tag, Light tab: power, exposure and normalize come before colour and temperature, and colour before temperature.
- Cycles Fog object: a "Texture" field in the Fog tab takes any of Cinema 4D's shaders (a Gradient, a Noise, a Layer, a bitmap, a Fusion) that shapes the density: a Gradient "2D - V" with white at the left knot fades the fog out towards the top. "Projection" says which of the box's sides the shader's picture faces (front, right, top). With the Noise tab on as well, both multiply. "Voxel Resolution" (before "Resolution", the last line of the Noise tab) is now in the Fog tab under the texture, for both, with a line that counts the voxels; what it costs is in the documents (at 512 a grid takes about 5 s to make and is a file of 100 to 400 MB).
- Cycles Light and Cycles Light tag: the Type menu reads Point, Area, Spot, Sun (before Point, Sun, Spot, Area). Saved scenes keep their types.
- Cycles Light: handles in the viewport (the yellow dots): an area light's size (and size Y for a rectangle or an ellipse) on its edges, a point's or spot's radius, a spot's cone and, with "Show Cone", its blend. Dragging one changes the value in the Attribute Manager, and Undo takes it back.
- Cycles nodes: a material can have several Material Outputs, and several World Outputs. As in Blender, each output has "Active Output" and "Target" (All, Eevee, Cycles); for the engine that renders, an output of its own Target beats one for All, then the one marked active, then the first. A material saved before has none marked and renders as it did (the first output).
- Cycles nodes: a material with both a Material Output and a World Output works in both places: on an object the Material Output counts, on a Cycles Environment of the type Material the World Output, and the console no longer names the other one.
- Node Editor: the Cycles nodes are filed under "Nodes > Cycles Nodes" in the node popup and the Asset Browser, beside Cinema 4D's own node folders, instead of under "Uncategorized".
- Node Editor: the Material Output node's material settings (displacement, emission sampling, volume and Eevee settings) are a tab of their own, "Material Settings", beside the node's inputs.
- Numbers step in fractions as in Blender, and values with a natural range are sliders. Cycles nodes: every number of every node steps by Blender's own step (0.1, or 0.01 on a slider) instead of 1, and factors (Roughness, Metallic, Factor, Randomness and the other values Blender keeps between 0 and 1), a Color Ramp's stops and the few whole numbers with a short range (Magic Texture's Depth, Brick's frequencies) are sliders. Cycles Light and its tag, Cycles Object tag, Cycles Camera tag (also the Imager tab's effects), Cycles Fog, Environment and Scatter: finer steps where Blender's are finer (power 0.1 W, exposure 0.01, temperature 4 K, sizes 1 mm) and sliders with Blender's ranges (exposure, factors, angles, the sky's densities, turbidity and albedo, the camera's blades, aperture ratio and latitudes, the fog's octaves and voxel resolution, scales of instances and of scattered copies). No limit changed: a value typed beyond a slider's end is kept.
- Preferences > IPR, "Refresh Whole Scene Only on Reload": for big scenes. The IPR then sends only what can go alone (the camera, lights, objects moved or changed) and never exports the whole scene by itself; whatever else changed waits until Reload is pressed.
- Instances on particles and Matrix objects (Cycles Object tag) and of the Cycles Scatter are faster: an object made of several meshes is one set of instances in Blender, not one per mesh (20 meshes on 120,000 particles: 0.26 s per frame instead of 6.7 s), and what is instanced on a particle group is no longer rebuilt in Blender at every frame.
- Hair renders with the hair material's shape (bend, curl, frizz, kink, twist, clump, length, scale) and all colours of its gradient. A material on a texture tag of the Hair object gives the hairs their colour while the hair material keeps giving the shape. In the IPR a hair is generated again only when it changed, which removes the wait after every edit in scenes with hair.
- Cycles Fog object: a box that renders as a volume, with Cinema 4D's noises for its density.
- Cycles Environment object: a colour, Blender's Sky Texture, an HDRI or a world material of Cycles nodes; separate environments for what the camera sees and what lights the scene; a light as the sun; world fog. Among Cycles Environment, Sky and Redshift dome light, the highest in the Object Manager is used (before, a Sky always won over a dome light).
- Cycles Scatter object: scatters the objects below it over a surface, its vertices or the places of a CSV file or sequence, with effectors, fields, a spacing radius, and copies that keep their place when a vertex map or the surface is animated.
- Cycles Proxy object: shows a Blender file, linked, at the object's place.
- Cycles Scatter object, as the maintainer laid it out: Poisson can stop at a count ("Limit to Count"); a new distribution "Volume" places the copies inside a closed mesh; the position shader moves copies across the surface along its U and V as well as along their Y, the rotation shader turns them by heading, pitch and bank, the scale shader scales them uniformly or per axis ("Uniform"), each value from a Min to a Max; position, rotation and scale have sections of their own under "Vary by Shader", the Effectors tab has an "Effectors" and a "Fields" section, and "Where Copies Go" is called "Shader Distribution". A scene saved before keeps its copies.
- Fixed: Cinema 4D could crash or freeze when a Cycles Scatter's copies were read (by the IPR) while Cinema 4D computed them again on another thread, most likely with a Poisson distribution, whose computation takes long. A Scatter's copies now stay whole while they are read, a computation stops when Cinema 4D asks it to, and no setting makes more than 10,000,000 copies.
- Cycles Camera tag, Imager tab: effects of Blender's compositor on the picture (Vignette, Sepia, Tune Image, Chromatic Aberration and more, as Blender ships them), as a list of layers with a switch and an opacity each, in final frames and in the IPR.
- Data of the scene in materials: Redshift's Color, Scalar, Integer and Vector User Data nodes and its Point Attribute node are translated, and the Color Splitter's alpha behind them. What a particle has (colour, alpha, id, age, lifetime, size, velocity, distance), what a MoGraph clone has (index, weight, UVW, colour, time offset), an object's own user data by its name, its Object ID and display colour arrive in Cycles and Eevee, on particles rendered as spheres and on objects instanced on them. A Jitter node keyed by a particle's id varies per particle. The Attribute node of a Cycles graph reads the same data (`particle_age`, `mograph_index`, ...: the list is in `protocol/protocol.md`).
- Redshift's Color Correct follows a wired gamma, hue, saturation and level.
- Fixed: a texture of a Redshift material that names an image of the Asset Browser rendered magenta (the file was not found); the same for a Redshift dome light's map from the Asset Browser.
- Render Settings, Output tab, "Files Written by Blender": Blender writes a multilayer OpenEXR of each final frame itself, from the same render the Picture Viewer gets, with the passes chosen for the file and Cryptomatte for objects and materials (named as in Cinema 4D, with the manifest), half or full float, any of Blender's EXR codecs. The file's name takes Cinema 4D's tokens and, in an animation, the frame number as the Output page names Cinema 4D's own pictures.
- Cycles nodes: a wire through Cinema 4D's Wire Rerouter is followed, and groups work: the nodes inside a group, also a group in a group, render as if they stood in the graph (before, a rerouter and a group were left out with their wires).
- Cycles nodes: the Texture Coordinate node has Blender's "Object" setting, a link to any object of the scene (a null, a camera, a hidden object too); its Object output then gives the coordinates in that object's axes, and follows the object when it moves, in final frames and in the IPR. The link survives saving and loading, a copy of the document and merging the material with the object into another scene; when the object is gone the console says so and the node renders as without one.

## 0.2.2

For Cinema 4D 2024 to 2027 and Blender 5.0 to 5.3 on Windows, one package for all. Details, and what renders differently from 0.1.0, are in [docs/roadmap.md](docs/roadmap.md).

- Cycles Camera tag: depth of field, orthographic and panoramic lenses.
- Cycles Light object and Cycles Light tag; Cinema 4D's lights cast shadows by default and follow their green check.
- An Output tab in the Render Settings and more render passes.
- Node Editor: an image dropped becomes an Image Texture node; buttons make a path relative or absolute; the curve nodes have curve controls.
- Redshift: 112 of 159 nodes translated; lights as bright and as coloured as in Redshift.
- IPR: an Add menu; the picture follows drags, playback and live values while they change; only what moves is sent; camera navigation in the IPR's picture; Cinema 4D's interface stays free while the IPR follows.
- Preferences: Blender's render devices, a default engine, switches for the IPR's live behaviours; a clear message when no usable Blender is chosen.
- Fixed: colour with "Basic" colour management, baked textures removed and reused, the material preview's orientation, Eevee lighting through the editor camera, scenes of more than about 800 meshes.

## 0.1.0

The first public release. For Cinema 4D 2027 and Blender 5.2 on Windows.

- A renderer in the Render Settings that renders with Cycles or Eevee: stills and sequences, render passes as multi-pass layers, motion blur, the render region, denoising.
- A live render (IPR) window that follows every change of the scene.
- Scene translation: meshes with their attributes, instances and Cloners, deformers, cameras, standard and Redshift lights, Sky and dome light, volumes, hair, particles.
- Materials: classic materials and shaders, Redshift Standard Materials, and a node space of Blender's shader nodes ("Cycles") whose materials get their thumbnails from Blender.
- The Cycles Object tag: ray visibility, light linking, instances on particles and Matrix objects, volume shading.
- The bridge is installed in Blender as an extension; the plugin finds it there and finds Blender by itself on its first start.

Licensed under Apache-2.0, the bridge under GPL-3.0-or-later.

Known gaps are listed in [docs/roadmap.md](docs/roadmap.md).
