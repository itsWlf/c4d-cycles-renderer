# Changelog

All notable changes to Cycles Render. Version numbers follow [Semantic Versioning](https://semver.org); a release is the Cinema 4D plugin and the Blender bridge with the same number.

## 0.3.0 (in preparation)

- Preferences > IPR, "Refresh Whole Scene Only on Reload": for big scenes. The IPR then sends only what can go alone (the camera, lights, objects moved or changed) and never exports the whole scene by itself; whatever else changed waits until Reload is pressed.
- Instances on particles and Matrix objects (Cycles Object tag) and of the Cycles Scatter are faster: an object made of several meshes is one set of instances in Blender, not one per mesh (20 meshes on 120,000 particles: 0.26 s per frame instead of 6.7 s), and what is instanced on a particle group is no longer rebuilt in Blender at every frame.
- Hair renders with the hair material's shape (bend, curl, frizz, kink, twist, clump, length, scale) and all colours of its gradient. A material on a texture tag of the Hair object gives the hairs their colour while the hair material keeps giving the shape. In the IPR a hair is generated again only when it changed, which removes the wait after every edit in scenes with hair.
- Cycles Fog object: a box that renders as a volume, with Cinema 4D's noises for its density.
- Cycles Environment object: a colour, Blender's Sky Texture, an HDRI or a world material of Cycles nodes; separate environments for what the camera sees and what lights the scene; a light as the sun; world fog. Among Cycles Environment, Sky and Redshift dome light, the highest in the Object Manager is used (before, a Sky always won over a dome light).
- Cycles Scatter object: scatters the objects below it over a surface, its vertices or the places of a CSV file or sequence, with effectors, fields, a spacing radius, and copies that keep their place when a vertex map or the surface is animated.
- Cycles Proxy object: shows a Blender file, linked, at the object's place.

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
