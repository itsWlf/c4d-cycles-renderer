# Changelog

All notable changes to Cycles Render. Version numbers follow [Semantic Versioning](https://semver.org); a release is the Cinema 4D plugin and the Blender bridge with the same number.

## 0.1.0 (in preparation)

The first public release. For Cinema 4D 2027 and Blender 5.2 on Windows.

- A renderer in the Render Settings that renders with Cycles or Eevee: stills and sequences, render passes as multi-pass layers, motion blur, the render region, denoising.
- A live render (IPR) window that follows every change of the scene.
- Scene translation: meshes with their attributes, instances and Cloners, deformers, cameras, standard and Redshift lights, Sky and dome light, volumes, hair, particles.
- Materials: classic materials and shaders, Redshift Standard Materials, and a node space of Blender's shader nodes ("Cycles") whose materials get their thumbnails from Blender.
- The Cycles Object tag: ray visibility, light linking, instances on particles and Matrix objects, volume shading.
- The bridge is installed in Blender as an extension; the plugin finds it there and finds Blender by itself on its first start.

Licensed under Apache-2.0, the bridge under GPL-3.0-or-later.

Known gaps are listed in [docs/roadmap.md](docs/roadmap.md).
