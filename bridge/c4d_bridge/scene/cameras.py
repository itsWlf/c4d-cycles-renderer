# SPDX-License-Identifier: GPL-3.0-or-later
"""Camera builder."""

import bpy
from mathutils import Matrix

from .common import matrix_from, place

# Blender's panorama settings a `camera` item may carry in `panorama` (properties of the camera
# since Blender 4.x; they used to live on camera.cycles).
_PANORAMA_PROPERTIES = (
    "panorama_type", "fisheye_fov", "fisheye_lens", "latitude_min", "latitude_max", "longitude_min", "longitude_max",
    "fisheye_polynomial_k0", "fisheye_polynomial_k1", "fisheye_polynomial_k2", "fisheye_polynomial_k3",
    "fisheye_polynomial_k4", "central_cylindrical_range_u_min", "central_cylindrical_range_u_max",
    "central_cylindrical_range_v_min", "central_cylindrical_range_v_max", "central_cylindrical_radius",
)

# The custom property that marks the empty a slanted camera hangs from.
_SHEAR_MARK = "c4d_camera_shear"


def _is_sheared(matrix):
    """True when the viewing axis is not perpendicular to the picture's axes: an oblique view."""
    x, y, z = (matrix.col[i].xyz for i in range(3))
    return abs(z.normalized().dot(x.normalized())) > 1e-6 or abs(z.normalized().dot(y.normalized())) > 1e-6


def _shear_parent(obj):
    """The empty an oblique camera hangs from. Blender stores an object's own matrix as
    location, rotation and scale, which cannot hold a slant; the parent inverse matrix can,
    and Cycles keeps the slant of the resulting world matrix (it only normalises its axes)."""
    parent = obj.parent
    if parent is None or not parent.get(_SHEAR_MARK):
        parent = bpy.data.objects.new(obj.name + ".shear", None)
        parent[_SHEAR_MARK] = True
        bpy.context.scene.collection.objects.link(parent)
        obj.parent = parent
    parent.matrix_world = Matrix.Identity(4)
    return parent


def remove_shear_parent(obj):
    """Detach and delete the empty of an oblique camera, if it has one."""
    parent = obj.parent
    if parent is not None and parent.get(_SHEAR_MARK):
        obj.parent = None
        obj.matrix_parent_inverse = Matrix.Identity(4)
        bpy.data.objects.remove(parent)


def build_camera(scene, item, blobs):
    # A camera that exists is changed in place: moving the view is the most frequent edit of all.
    existing = scene.objects.get(item["id"])
    camera = existing.data if existing is not None and existing.type == 'CAMERA' else bpy.data.cameras.new(item["id"])
    camera.type = item.get("type", "PERSP")
    camera.sensor_fit = 'HORIZONTAL'
    camera.sensor_width = item.get("sensor_width", 36.0)
    camera.lens = item.get("lens", 50.0)
    camera.ortho_scale = item.get("ortho_scale", 6.0)
    camera.shift_x = item.get("shift_x", 0.0)
    camera.shift_y = item.get("shift_y", 0.0)
    camera.clip_start = item.get("clip_start", 0.1)
    camera.clip_end = item.get("clip_end", 1000.0)

    panorama = item.get("panorama") or {}
    for name, value in panorama.items():
        if name not in _PANORAMA_PROPERTIES:
            scene.warn(f"camera {item['id']!r}: unknown panorama setting {name!r}")
            continue
        setattr(camera, name, value)

    dof = item.get("dof")
    camera.dof.use_dof = dof is not None
    if dof is not None:
        camera.dof.focus_distance = dof["focus_distance"]
        camera.dof.aperture_fstop = dof["fstop"]
        camera.dof.aperture_blades = dof.get("blades", 0)
        camera.dof.aperture_rotation = dof.get("rotation", 0.0)
        camera.dof.aperture_ratio = dof.get("ratio", 1.0)

    obj = scene.link(item["id"], camera)
    matrix = matrix_from(item)
    if _is_sheared(matrix):
        # An oblique view (protocol.md, `camera`): the world matrix is the parent inverse matrix.
        if obj.animation_data is not None:
            obj.animation_data_clear()
        parent = _shear_parent(obj)
        obj.matrix_parent_inverse = parent.matrix_world.inverted() @ matrix
        obj.matrix_basis = Matrix.Identity(4)
    else:
        remove_shear_parent(obj)
        place(obj, item)
    if item.get("active"):
        bpy.context.scene.camera = obj
