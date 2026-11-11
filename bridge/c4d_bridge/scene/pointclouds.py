# SPDX-License-Identifier: GPL-3.0-or-later
"""Point cloud builder: particles as spheres."""

import bpy
import numpy as np

from ..errors import RequestError
from .common import array_from, place
from .objects import assign_materials

# The name a material uses to read the colour of a particle (Attribute node, type GEOMETRY).
COLOR_ATTRIBUTE = "particle_color"


def build_pointcloud(scene, item, blobs):
    points = array_from(blobs, item["points"], np.float32)
    count = len(points) // 3
    old = scene.pointclouds.pop(item["id"], None)
    cloud = bpy.data.pointclouds.new(item["id"])
    cloud.resize(count)
    cloud.points.foreach_set("co", points)
    if "radii" in item:
        radii = array_from(blobs, item["radii"], np.float32)
        if len(radii) != count:
            raise RequestError("bad_request", f"pointcloud {item['id']!r}: one radius per point is needed")
    else:
        radii = np.full(count, float(item.get("radius", 0.01)), dtype=np.float32)
    cloud.points.foreach_set("radius", radii)
    if "colors" in item:
        colors = array_from(blobs, item["colors"], np.float32)
        if len(colors) != count * 4:
            raise RequestError("bad_request", f"pointcloud {item['id']!r}: one colour (r, g, b, a) per point is needed")
        cloud.attributes.new(COLOR_ATTRIBUTE, 'FLOAT_COLOR', 'POINT').data.foreach_set("color", colors)
    for _ in item.get("materials", ()):
        cloud.materials.append(None)
    obj = scene.link(item["id"], cloud)
    if old is not None:
        bpy.data.pointclouds.remove(old)
    scene.pointclouds[item["id"]] = cloud
    place(obj, item)
    assign_materials(scene, obj, item)
