# SPDX-License-Identifier: GPL-3.0-or-later
"""Mesh builders: `mesh` (topology and everything bound to it) and `points` (positions and
shading normals, which is all that changes while a mesh deforms)."""

import bpy
import numpy as np

from ..errors import RequestError
from .common import MOTION_SHUTTER_FRAMES, array_from

_NORMALS = "custom_normal"
_VELOCITY = "velocity"


def build_mesh(scene, item, blobs):
    corner_verts = array_from(blobs, item["corner_verts"], np.int32)
    face_sizes = array_from(blobs, item["face_sizes"], np.int32)
    corner_count = len(corner_verts)
    if int(face_sizes.sum()) != corner_count:
        raise RequestError("bad_request", f"mesh {item['id']!r}: face sizes do not add up to the corner count")
    points = array_from(blobs, item["points"], np.float32) if "points" in item else None
    point_count = len(points) // 3 if points is not None else int(item["point_count"])

    mesh = scene.meshes.get(item["id"])
    if mesh is None:
        mesh = bpy.data.meshes.new(item["id"])
        scene.meshes[item["id"]] = mesh
    else:
        # Everything is built again; the datablock stays, because objects point at it.
        mesh.clear_geometry()
        for attribute in list(mesh.attributes):
            if not attribute.is_required and not attribute.is_internal:
                mesh.attributes.remove(attribute)

    loop_starts = np.zeros(len(face_sizes), dtype=np.int32)
    np.cumsum(face_sizes[:-1], out=loop_starts[1:])
    mesh.vertices.add(point_count)
    mesh.loops.add(corner_count)
    mesh.polygons.add(len(face_sizes))
    mesh.polygons.foreach_set("loop_start", loop_starts)
    mesh.loops.foreach_set("vertex_index", corner_verts)
    mesh.polygons.foreach_set("use_smooth", np.full(len(face_sizes), bool(item.get("smooth")), dtype=bool))

    for uv in item.get("uvs", ()):
        values = array_from(blobs, uv["data"], np.float32)
        if len(values) != corner_count * 2:
            raise RequestError("bad_request", f"mesh {item['id']!r}: UV set {uv['name']!r} has the wrong length")
        layer = mesh.uv_layers.new(name=uv["name"], do_init=False)
        layer.uv.foreach_set("vector", values)
    # The set that image textures use when a material names none.
    render_uv = item.get("uv_render")
    if render_uv is not None and 0 <= render_uv < len(mesh.uv_layers):
        mesh.uv_layers[render_uv].active_render = True
        mesh.uv_layers.active_index = render_uv

    lengths = {"POINT": point_count, "CORNER": corner_count}
    for spec in item.get("colors", ()):
        _attribute(mesh, item, spec, blobs, 'FLOAT_COLOR', "color", 4, lengths)
    for spec in item.get("floats", ()):
        _attribute(mesh, item, spec, blobs, 'FLOAT', "value", 1, lengths)

    # One slot per material the plugin may assign; the objects fill the slots.
    # An existing mesh keeps its slots: clearing them would also empty the slots of the objects
    # that show it.
    slots = int(item.get("slots", 1))
    while len(mesh.materials) < slots:
        mesh.materials.append(None)
    while len(mesh.materials) > slots:
        mesh.materials.pop(index=len(mesh.materials) - 1)
    if "material_indices" in item:
        indices = array_from(blobs, item["material_indices"], np.int32)
        if len(indices) != len(face_sizes):
            raise RequestError("bad_request", f"mesh {item['id']!r}: one material index per face is needed")
        mesh.polygons.foreach_set("material_index", indices)

    if points is not None:
        _set_points(mesh, item, points, array_from(blobs, item["normals"], np.float32) if "normals" in item else None)
    mesh.update(calc_edges=True)


def build_points(scene, item, blobs):
    """Positions and shading normals of a mesh that exists."""
    mesh = scene.meshes.get(item["mesh"])
    if mesh is None:
        raise RequestError("bad_request", f"points {item['id']!r} refer to unknown mesh {item['mesh']!r}")
    normals = array_from(blobs, item["normals"], np.float32) if "normals" in item else None
    _set_points(mesh, item, array_from(blobs, item["points"], np.float32), normals)
    _set_motion(mesh, item, array_from(blobs, item["motion"], np.float32) if "motion" in item else None)
    mesh.update()


def _set_motion(mesh, item, motion):
    """Deformation blur. Cycles reads a point attribute named `velocity`, in units per second,
    and moves each point half the shutter time back and forth along it. The final driver
    renders with a shutter of MOTION_SHUTTER_FRAMES, so the way a point goes while the shutter
    is open becomes this velocity."""
    attribute = mesh.attributes.get(_VELOCITY)
    if motion is None:
        if attribute is not None:
            mesh.attributes.remove(attribute)
        return
    if len(motion) != len(mesh.vertices) * 3:
        raise RequestError("bad_request", f"points {item['id']!r}: motion has the wrong length")
    render = bpy.context.scene.render
    velocity = motion * np.float32(render.fps / render.fps_base / MOTION_SHUTTER_FRAMES)
    if attribute is None:
        attribute = mesh.attributes.new(_VELOCITY, 'FLOAT_VECTOR', 'POINT')
    attribute.data.foreach_set("vector", velocity)


def _set_points(mesh, item, points, normals):
    if len(points) != len(mesh.vertices) * 3:
        raise RequestError("bad_request", f"{item['kind']} {item['id']!r}: {len(points) // 3} points for a mesh of {len(mesh.vertices)}")
    mesh.vertices.foreach_set("co", points)
    attribute = mesh.attributes.get(_NORMALS)
    if normals is None:
        if attribute is not None:
            mesh.attributes.remove(attribute)
        return
    if len(normals) != len(mesh.loops) * 3:
        raise RequestError("bad_request", f"{item['kind']} {item['id']!r}: normals have the wrong length")
    if attribute is None:
        # A float vector attribute with this name on the corner domain is used as the shading
        # normal directly, in object space.
        attribute = mesh.attributes.new(_NORMALS, 'FLOAT_VECTOR', 'CORNER')
    attribute.data.foreach_set("vector", normals)


def _attribute(mesh, item, spec, blobs, data_type, field, width, lengths):
    domain = spec.get("domain", "POINT")
    if domain not in lengths:
        raise RequestError("bad_request", f"mesh {item['id']!r}: attribute {spec['name']!r} has the unknown domain {domain!r}")
    values = array_from(blobs, spec["data"], np.float32)
    if len(values) != lengths[domain] * width:
        raise RequestError("bad_request", f"mesh {item['id']!r}: attribute {spec['name']!r} has the wrong length")
    attribute = mesh.attributes.new(spec["name"], data_type, domain)
    attribute.data.foreach_set(field, values)
