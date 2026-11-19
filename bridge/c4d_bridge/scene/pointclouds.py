# SPDX-License-Identifier: GPL-3.0-or-later
"""Point cloud builder: particles as spheres.

Blender 5.1 and later make the points directly (`PointCloud.resize`). Blender 5.0 has no way to
add points to a point cloud from Python, so there the points are the loose vertices of a mesh,
and a Geometry Nodes modifier turns them into a point cloud (Mesh to Points, radius from a point
attribute); attributes of the vertices go with the points, so a material reads the colour alike.
"""

import bpy
import numpy as np

from ..errors import RequestError
from .common import array_from, place
from .objects import assign_materials

# The name a material uses to read the colour of a particle (Attribute node, type GEOMETRY).
COLOR_ATTRIBUTE = "particle_color"
# Blender 5.0: the node group that makes points of a mesh's vertices, shared by every point cloud.
_POINTS_GROUP = "c4d_points_of_mesh"
_RADIUS_ATTRIBUTE = "radius"


def build_pointcloud(scene, item, blobs):
    """A `pointcloud` item: points with a radius and a colour each, made anew every time."""
    points = array_from(blobs, item["points"], np.float32)
    count = len(points) // 3
    if "radii" in item:
        radii = array_from(blobs, item["radii"], np.float32)
        if len(radii) != count:
            raise RequestError("bad_request", f"pointcloud {item['id']!r}: one radius per point is needed")
    else:
        radii = np.full(count, float(item.get("radius", 0.01)), dtype=np.float32)
    colors = None
    if "colors" in item:
        colors = array_from(blobs, item["colors"], np.float32)
        if len(colors) != count * 4:
            raise RequestError("bad_request", f"pointcloud {item['id']!r}: one colour (r, g, b, a) per point is needed")

    old = scene.pointclouds.pop(item["id"], None)
    # (hasattr() on the class does not see RNA functions; the RNA description lists them.)
    if "resize" in bpy.types.PointCloud.bl_rna.functions:
        data = bpy.data.pointclouds.new(item["id"])
        data.resize(count)
        data.points.foreach_set("co", points)
        data.points.foreach_set("radius", radii)
    else:
        data = bpy.data.meshes.new(item["id"])
        data.vertices.add(count)
        data.vertices.foreach_set("co", points)
        data.attributes.new(_RADIUS_ATTRIBUTE, 'FLOAT', 'POINT').data.foreach_set("value", radii)
    if colors is not None:
        data.attributes.new(COLOR_ATTRIBUTE, 'FLOAT_COLOR', 'POINT').data.foreach_set("color", colors)
    for _ in item.get("materials", ()):
        data.materials.append(None)
    if isinstance(data, bpy.types.Mesh):
        data.update()
    obj = scene.link(item["id"], data)
    if isinstance(data, bpy.types.Mesh) and not obj.modifiers:
        obj.modifiers.new("points", 'NODES').node_group = _points_group()
    if old is not None:
        remove_data(old)
    scene.pointclouds[item["id"]] = data
    place(obj, item)
    assign_materials(scene, obj, item)


def remove_data(data):
    """Remove what build_pointcloud made: a point cloud, or in Blender 5.0 its mesh."""
    if isinstance(data, bpy.types.Mesh):
        bpy.data.meshes.remove(data)
    else:
        bpy.data.pointclouds.remove(data)


def _points_group():
    """Blender 5.0: Mesh to Points over the vertices, each as big as its "radius" attribute."""
    group = bpy.data.node_groups.get(_POINTS_GROUP)
    if group is not None:
        return group
    group = bpy.data.node_groups.new(_POINTS_GROUP, 'GeometryNodeTree')
    group.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    group.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    group.is_modifier = True
    nodes, links = group.nodes, group.links
    source = nodes.new('NodeGroupInput')
    target = nodes.new('NodeGroupOutput')
    to_points = nodes.new('GeometryNodeMeshToPoints')
    to_points.mode = 'VERTICES'
    radius = nodes.new('GeometryNodeInputNamedAttribute')
    radius.data_type = 'FLOAT'
    radius.inputs['Name'].default_value = _RADIUS_ATTRIBUTE
    links.new(source.outputs[0], to_points.inputs['Mesh'])
    links.new(radius.outputs['Attribute'], to_points.inputs['Radius'])
    links.new(to_points.outputs['Points'], target.inputs[0])
    return group
