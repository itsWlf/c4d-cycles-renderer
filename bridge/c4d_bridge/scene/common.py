# SPDX-License-Identifier: GPL-3.0-or-later
"""Helpers shared by the scene builders."""

import bpy
import numpy as np
from mathutils import Matrix

from ..errors import RequestError


# Blender's shutter time, in frames, whenever motion blur is rendered. The plugin decides how
# long the shutter really is by choosing the moments it reads the scene at.
MOTION_SHUTTER_FRAMES = 2.0


def _matrix(values):
    """A 4x4 matrix from 16 numbers in row-major order."""
    return Matrix([values[0:4], values[4:8], values[8:12], values[12:16]])


def matrix_from(item):
    """The item's world matrix."""
    return _matrix(item["matrix"])


def place(obj, item):
    """Put an object where the item says. With `motion` (its matrix when the shutter opens and
    when it closes) the object is also keyed one frame before and one frame after the current
    one; the final driver renders with a shutter of two frames, so Blender asks for exactly
    those three moments and never interpolates between keys."""
    if obj.animation_data is not None:
        action = obj.animation_data.action
        obj.animation_data_clear()
        if action is not None and action.users == 0:
            bpy.data.actions.remove(action)
    matrix = matrix_from(item)
    motion = item.get("motion")
    if not motion:
        obj.matrix_world = matrix
        return
    frame = bpy.context.scene.frame_current
    obj.rotation_mode = 'QUATERNION'
    centre = matrix.decompose()
    for offset, moment in ((-1, _matrix(motion[0:16])), (1, _matrix(motion[16:32])), (0, matrix)):
        location, rotation, scale = moment.decompose()
        if rotation.dot(centre[1]) < 0.0:
            rotation.negate()       # the same rotation, on the side of the centre one
        obj.location, obj.rotation_quaternion, obj.scale = location, rotation, scale
        for path in ("location", "rotation_quaternion", "scale"):
            obj.keyframe_insert(path, frame=frame + offset)


def give_name(datablock, name):
    """Give a datablock the name an item asks for (an item's `name`), which a Cryptomatte manifest
    lists. Blender keeps names unique by adding ".001" and so on; a name it made that way is kept,
    so an item sent again is not renamed back and forth. The bridge itself finds its datablocks by
    item id, never by name."""
    if not name:
        return
    current = datablock.name
    if current == name or (current.startswith(name + ".") and current[len(name) + 1:].isdigit()):
        return
    datablock.name = name


def array_from(blobs, index, dtype):
    """One blob of the request as a numpy array, without copying."""
    try:
        return np.frombuffer(blobs[index], dtype=dtype)
    except (IndexError, TypeError) as exc:
        raise RequestError("bad_request", f"blob {index!r} is missing") from exc


# How the `attributes` of a `pointcloud` or an `instancer` are stored: their type on the wire, the
# numbers each element has, Blender's attribute type and the name of its values for foreach_set.
_ATTRIBUTE_TYPES = {"float": (1, 'FLOAT', "value"), "vector": (3, 'FLOAT_VECTOR', "vector"), "color": (4, 'FLOAT_COLOR', "color")}


def named_attributes(item, blobs, count, reserved=()):
    """The `attributes` of a `pointcloud` or `instancer` item, checked: [(name, Blender's attribute
    type, the name of its values, the values)], one element per point or copy. `reserved`: names the
    item's builder uses itself."""
    found = []
    seen = set(reserved)
    for entry in item.get("attributes", ()):
        name, kind = entry.get("name"), _ATTRIBUTE_TYPES.get(entry.get("type"))
        if not name or name in seen or kind is None:
            raise RequestError("bad_request", f"{item['kind']} {item['id']!r}: attribute {name!r} needs a name of its own and a type "
                                              "(float, vector or color)")
        values = array_from(blobs, entry.get("data"), np.float32)
        if len(values) != count * kind[0]:
            raise RequestError("bad_request", f"{item['kind']} {item['id']!r}: attribute {name!r} needs {kind[0]} numbers per element")
        seen.add(name)
        found.append((name, kind[1], kind[2], values))
    return found
