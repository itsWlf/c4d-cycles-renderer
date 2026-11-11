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


def array_from(blobs, index, dtype):
    """One blob of the request as a numpy array, without copying."""
    try:
        return np.frombuffer(blobs[index], dtype=dtype)
    except (IndexError, TypeError) as exc:
        raise RequestError("bad_request", f"blob {index!r} is missing") from exc
