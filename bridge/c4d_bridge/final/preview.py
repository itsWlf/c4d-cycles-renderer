# SPDX-License-Identifier: GPL-3.0-or-later
"""Material previews: the picture of a ball that wears one material (`material_preview`).

The ball has a scene of its own, apart from the scene the plugin sends: a smooth sphere with
UVs, a camera that frames it, and the light of one of the environment pictures Blender ships
for its own material preview. It is rendered with Cycles on the CPU (a picture this small is
done before a GPU has loaded its kernels) over a transparent background.
"""

import math
import os

import bmesh
import bpy
import numpy as np

from ..errors import RequestError
from . import _render_cycles

_LIGHT = "forest.exr"       # in <Blender>/datafiles/studiolights/world; the one Blender's own material preview starts with
_STRENGTH = 0.5             # of that light: a white ball stays below white
_stage = None


def _build_stage():
    scene = bpy.data.scenes.new("Material Preview")
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.use_denoising = True
    scene.render.film_transparent = True
    scene.render.resolution_percentage = 100

    mesh = bpy.data.meshes.new("Material Preview Ball")
    work = bmesh.new()
    bmesh.ops.create_uvsphere(work, u_segments=64, v_segments=32, radius=1.0, calc_uvs=True)
    work.to_mesh(mesh)
    work.free()
    mesh.polygons.foreach_set("use_smooth", [True] * len(mesh.polygons))
    ball = bpy.data.objects.new("Material Preview Ball", mesh)
    scene.collection.objects.link(ball)

    # A long lens from the front: the ball fills the picture with a small margin.
    camera_data = bpy.data.cameras.new("Material Preview Camera")
    camera_data.lens = 100.0
    camera_data.sensor_width = 36.0
    camera = bpy.data.objects.new("Material Preview Camera", camera_data)
    camera.location = (0.0, -5.9, 0.0)
    camera.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    scene.collection.objects.link(camera)
    scene.camera = camera

    world = bpy.data.worlds.new("Material Preview Light")
    if world.node_tree is None:
        world.use_nodes = True
    tree = world.node_tree
    tree.nodes.clear()
    background = tree.nodes.new("ShaderNodeBackground")
    output = tree.nodes.new("ShaderNodeOutputWorld")
    background.inputs["Strength"].default_value = _STRENGTH
    tree.links.new(background.outputs["Background"], output.inputs["Surface"])
    path = os.path.join(bpy.utils.system_resource("DATAFILES"), "studiolights", "world", _LIGHT)
    if os.path.isfile(path):
        picture = tree.nodes.new("ShaderNodeTexEnvironment")
        picture.image = bpy.data.images.load(path, check_existing=True)
        tree.links.new(picture.outputs["Color"], background.inputs["Color"])
    else:
        # No environment picture in this Blender: an even light and a sun.
        background.inputs["Color"].default_value = (0.5, 0.5, 0.5, 1.0)
        sun = bpy.data.objects.new("Material Preview Sun", bpy.data.lights.new("Material Preview Sun", "SUN"))
        sun.data.energy = 3.0
        sun.rotation_euler = (math.radians(50.0), 0.0, math.radians(35.0))
        scene.collection.objects.link(sun)
    scene.world = world
    return {"scene": scene, "ball": ball}


def _get_stage():
    """The preview scene; built again when clearing the plugin's scene took it apart."""
    global _stage
    try:
        if _stage is not None and _stage["scene"].camera is not None and _stage["ball"].data is not None and _stage["scene"].world is not None:
            return _stage
    except ReferenceError:
        pass
    _stage = _build_stage()
    return _stage


def render(scene_data, header):
    """Handle a `material_preview` request. Returns (reply fields, blobs)."""
    material = scene_data.materials.get(header.get("material"))
    if material is None:
        raise RequestError("bad_request", f"material_preview: no material {header.get('material')!r} was sent")
    size = max(16, min(1024, int(header.get("size", 128))))
    stage = _get_stage()
    scene, ball = stage["scene"], stage["ball"]
    ball.data.materials.clear()
    ball.data.materials.append(material)
    scene.render.resolution_x = scene.render.resolution_y = size
    scene.cycles.samples = max(1, int(header.get("samples", 32)))
    image = _render_cycles([], scene)["Combined"]
    # Blender's rows run bottom-up; the protocol sends the top row first.
    pixels = np.ascontiguousarray(image[::-1, :, :4], dtype=np.float32)
    return {"width": size, "height": size, "channels": 4, "pixels": 0}, [pixels]
