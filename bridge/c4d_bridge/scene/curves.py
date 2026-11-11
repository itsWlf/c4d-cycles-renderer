# SPDX-License-Identifier: GPL-3.0-or-later
"""Curves builder: hair as Blender's hair curves."""

import bpy
import numpy as np

from ..errors import RequestError
from .common import array_from, place
from .objects import assign_materials


def build_curves(scene, item, blobs):
    points = array_from(blobs, item["points"], np.float32)
    per_curve = int(item["points_per_curve"])
    if per_curve < 2 or len(points) % (per_curve * 3) != 0:
        raise RequestError("bad_request", f"curves {item['id']!r}: the points do not divide into curves of {per_curve} points")
    count = len(points) // (per_curve * 3)

    old = scene.curves.pop(item["id"], None)
    curves = bpy.data.hair_curves.new(item["id"])
    curves.add_curves([per_curve] * count)
    curves.points.foreach_set("position", points)
    if "radii" in item:
        radii = array_from(blobs, item["radii"], np.float32)
        if len(radii) != count * per_curve:
            raise RequestError("bad_request", f"curves {item['id']!r}: one radius per point is needed")
        if curves.attributes.get("radius") is None:
            curves.attributes.new("radius", 'FLOAT', 'POINT')
        curves.attributes["radius"].data.foreach_set("value", radii)
    for _ in item.get("materials", ()):
        curves.materials.append(None)
    obj = scene.link(item["id"], curves)
    if old is not None:
        bpy.data.hair_curves.remove(old)
    scene.curves[item["id"]] = curves
    place(obj, item)
    assign_materials(scene, obj, item)
