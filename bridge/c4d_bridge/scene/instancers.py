# SPDX-License-Identifier: GPL-3.0-or-later
"""Instancer builder: many copies of one mesh, each with its own matrix, colour and numbers.

One instancer is three datablocks: the prototype object (the mesh with its materials, in no
collection, so it does not render by itself), a mesh of loose points carrying one matrix, one
colour and the named numbers (`floats`) per instance, and the object that shows them through a
Geometry Nodes modifier. The points' attributes become the instances' own.
"""

import bpy
import numpy as np

from ..errors import RequestError
from .common import array_from
from .objects import assign_materials, set_visibility

# Not "instance_transform": that is the name of the instances' own built-in attribute, and a point
# attribute of that name is dropped when the points become instances.
TRANSFORM = "c4d_transform"
COLOR = "mograph_color"


def build_instancer(scene, item, blobs):
    """An `instancer` item: its three datablocks made once, then the copies' matrices, colours and
    numbers written into the points."""
    mesh = scene.meshes.get(item["mesh"])
    if mesh is None:
        raise RequestError("bad_request", f"instancer {item['id']!r} refers to unknown mesh {item['mesh']!r}")
    matrices = array_from(blobs, item["matrices"], np.float32)
    if len(matrices) % 16:
        raise RequestError("bad_request", f"instancer {item['id']!r}: matrices must be 16 numbers each")
    count = len(matrices) // 16
    colors = array_from(blobs, item["colors"], np.float32) if "colors" in item else None
    if colors is not None and len(colors) != count * 4:
        raise RequestError("bad_request", f"instancer {item['id']!r}: one RGBA colour per instance is needed")
    floats = {}
    for entry in item.get("floats", ()):
        values = array_from(blobs, entry["data"], np.float32)
        if len(values) != count or not entry.get("name") or entry["name"] in (TRANSFORM, COLOR):
            raise RequestError("bad_request", f"instancer {item['id']!r}: float {entry.get('name')!r} needs one value per instance and a name of its own")
        floats[entry["name"]] = values

    parts = scene.instancers.get(item["id"])
    if parts is None:
        parts = _create(scene, item["id"], mesh)
    prototype, points, holder = parts["prototype"], parts["points"], parts["holder"]
    if prototype.data != mesh:
        prototype.data = mesh
    assign_materials(scene, prototype, item)
    set_visibility(holder, item)

    if len(points.vertices) != count:
        points.clear_geometry()
        points.vertices.add(count)
    # The plugin sends rows; Blender stores a matrix attribute column by column.
    rows = matrices.reshape(count, 4, 4)
    points.vertices.foreach_set("co", np.ascontiguousarray(rows[:, :3, 3]).ravel())
    _attribute(points, TRANSFORM, 'FLOAT4X4').data.foreach_set("value", np.ascontiguousarray(rows.transpose(0, 2, 1)).ravel())
    if colors is not None:
        _attribute(points, COLOR, 'FLOAT_COLOR').data.foreach_set("color", colors)
    elif COLOR in points.attributes:
        points.attributes.remove(points.attributes[COLOR])
    # Point attributes become the instances' own, which an Attribute node of type INSTANCER reads.
    for name, values in floats.items():
        _attribute(points, name, 'FLOAT').data.foreach_set("value", values)
    stale = [a.name for a in points.attributes
             if a.name not in floats and a.name not in (TRANSFORM, COLOR) and not a.is_required and not a.is_internal]
    for name in stale:
        points.attributes.remove(points.attributes[name])
    points.update()
    holder.update_tag()


def remove_instancer(scene, item_id):
    """Remove everything an instancer made, if `item_id` is one."""
    parts = scene.instancers.pop(item_id, None)
    if parts is None:
        return
    scene.objects.pop(item_id, None)
    bpy.data.objects.remove(parts["holder"])
    bpy.data.objects.remove(parts["prototype"])
    bpy.data.meshes.remove(parts["points"])
    bpy.data.node_groups.remove(parts["group"])


def _attribute(mesh, name, data_type):
    """The point attribute of that name, made when missing."""
    attribute = mesh.attributes.get(name)
    if attribute is None:
        attribute = mesh.attributes.new(name, data_type, 'POINT')
    return attribute


def _create(scene, item_id, mesh):
    """The prototype, the points, the object that shows them and its Geometry Nodes group
    (Instance on Points, then Set Instance Transform from the matrix attribute)."""
    prototype = bpy.data.objects.new(item_id + ".prototype", mesh)
    points = bpy.data.meshes.new(item_id + ".points")
    holder = scene.link(item_id, points)

    group = bpy.data.node_groups.new(item_id, 'GeometryNodeTree')
    group.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    group.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    group.is_modifier = True
    nodes, links = group.nodes, group.links
    source = nodes.new('NodeGroupInput')
    target = nodes.new('NodeGroupOutput')
    info = nodes.new('GeometryNodeObjectInfo')
    info.inputs['Object'].default_value = prototype
    info.inputs['As Instance'].default_value = True
    instance = nodes.new('GeometryNodeInstanceOnPoints')
    matrix = nodes.new('GeometryNodeInputNamedAttribute')
    matrix.data_type = 'FLOAT4X4'
    matrix.inputs['Name'].default_value = TRANSFORM
    place = nodes.new('GeometryNodeSetInstanceTransform')
    links.new(source.outputs[0], instance.inputs['Points'])
    links.new(info.outputs['Geometry'], instance.inputs['Instance'])
    links.new(instance.outputs['Instances'], place.inputs['Instances'])
    links.new(matrix.outputs['Attribute'], place.inputs['Transform'])
    links.new(place.outputs['Instances'], target.inputs[0])
    modifier = holder.modifiers.new("instancer", 'NODES')
    modifier.node_group = group

    parts = {"prototype": prototype, "points": points, "holder": holder, "group": group}
    scene.instancers[item_id] = parts
    return parts
