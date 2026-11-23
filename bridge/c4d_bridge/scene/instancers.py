# SPDX-License-Identifier: GPL-3.0-or-later
"""Instancer builder: many copies of one mesh, or of several meshes that belong together, each copy
with its own matrix, colour and numbers.

One instancer is three datablocks: the prototype object (the mesh with its materials, in no
collection, so it does not render by itself), a mesh of loose points carrying one matrix, one
colour and the named numbers (`floats`) per instance, and the object that shows them through a
Geometry Nodes modifier. The points' attributes become the instances' own.

With `parts` (since 0.3.0) a copy shows several meshes, each at its own place in the copy. They
are joined into one prototype, so that Blender has one instance per copy and not one per copy and
mesh: what an update costs in Blender grows with the number of instances (measured: about 2
microseconds each in Cycles), and twenty meshes on 120,000 copies took 5.2 s as twenty instancers
and take what one mesh takes as one. The prototype is then an object whose modifier takes every
part's mesh at its place (Object Info, relative), sets the part's materials on it and joins them.
The materials have to be set by nodes: they sit on the part's object, not on the mesh (so that
instances of one mesh can differ), and geometry taken from an object leaves those behind.
"""

import bpy
import numpy as np

from ..errors import RequestError
from .common import array_from, matrix_from
from .objects import assign_materials, set_visibility

# Not "instance_transform": that is the name of the instances' own built-in attribute, and a point
# attribute of that name is dropped when the points become instances.
TRANSFORM = "c4d_transform"
COLOR = "mograph_color"


def build_instancer(scene, item, blobs):
    """An `instancer` item: its datablocks made once, then the copies' matrices, colours and
    numbers written into the points."""
    sources = _sources(scene, item)
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
    if parts is not None and parts["joined"] != ("parts" in item):
        # One mesh before and several now, or the other way round: made anew.
        remove_instancer(scene, item["id"])
        parts = None
    if parts is None:
        parts = _create(scene, item["id"], None if "parts" in item else sources[0][0])
    points, holder = parts["points"], parts["holder"]
    if "parts" in item:
        _join(scene, item, parts, sources)
    else:
        prototype = parts["prototype"]
        if prototype.data != sources[0][0]:
            prototype.data = sources[0][0]
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
    prototype_data = parts["prototype"].data if parts["joined"] else None
    bpy.data.objects.remove(parts["prototype"])
    if prototype_data is not None:
        bpy.data.meshes.remove(prototype_data)
    bpy.data.meshes.remove(parts["points"])
    bpy.data.node_groups.remove(parts["group"])
    _remove_join(parts)


def _sources(scene, item):
    """What a copy shows: [(mesh, the fields that name its materials, its matrix in the copy or None)]."""
    if "parts" not in item:
        mesh = scene.meshes.get(item.get("mesh"))
        if mesh is None:
            raise RequestError("bad_request", f"instancer {item['id']!r} refers to unknown mesh {item.get('mesh')!r}")
        return [(mesh, item, None)]
    sources = []
    for part in item["parts"]:
        mesh = scene.meshes.get(part.get("mesh"))
        if mesh is None:
            raise RequestError("bad_request", f"instancer {item['id']!r} refers to unknown mesh {part.get('mesh')!r}")
        matrix = part.get("matrix")
        if not isinstance(matrix, (list, tuple)) or len(matrix) != 16:
            raise RequestError("bad_request", f"instancer {item['id']!r}: every part needs a matrix of 16 numbers")
        sources.append((mesh, part, matrix))
    if not sources:
        raise RequestError("bad_request", f"instancer {item['id']!r}: parts must name at least one mesh")
    return sources


def _attribute(mesh, name, data_type):
    """The point attribute of that name, made when missing."""
    attribute = mesh.attributes.get(name)
    if attribute is None:
        attribute = mesh.attributes.new(name, data_type, 'POINT')
    return attribute


def _create(scene, item_id, mesh):
    """The prototype, the points, the object that shows them and its Geometry Nodes group
    (Instance on Points, then Set Instance Transform from the matrix attribute). The prototype
    carries `mesh`; without one it is an object of its own that joins the item's parts (_join)."""
    joined = mesh is None
    prototype = bpy.data.objects.new(item_id + ".prototype", bpy.data.meshes.new(item_id + ".joined") if joined else mesh)
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

    parts = {"prototype": prototype, "points": points, "holder": holder, "group": group, "joined": joined,
             "part_objects": [], "join_group": None, "signature": None, "setters": []}
    scene.instancers[item_id] = parts
    return parts


def _remove_join(parts):
    """The objects and the node group that join an instancer's parts."""
    for obj in parts["part_objects"]:
        bpy.data.objects.remove(obj)
    parts["part_objects"] = []
    if parts["join_group"] is not None:
        bpy.data.node_groups.remove(parts["join_group"])
        parts["join_group"] = None
    parts["signature"] = None
    parts["setters"] = []


def _join(scene, item, parts, sources):
    """The prototype of an instancer with `parts`: every part's mesh at its place, with its
    materials, joined into one geometry. Built anew only when the parts are others than before (their
    meshes, places, number of material slots); another material of a part is set in place."""
    signature = tuple((mesh.name_full, len(mesh.materials), tuple(round(float(v), 9) for v in matrix)) for mesh, _fields, matrix in sources)
    if signature != parts["signature"]:
        _remove_join(parts)
        group = bpy.data.node_groups.new(item["id"] + ".join", 'GeometryNodeTree')
        group.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
        group.is_modifier = True
        nodes, links = group.nodes, group.links
        target = nodes.new('NodeGroupOutput')
        join = nodes.new('GeometryNodeJoinGeometry')
        links.new(join.outputs[0], target.inputs[0])
        for index, (mesh, _fields, matrix) in enumerate(sources):
            obj = bpy.data.objects.new(f"{item['id']}.part{index}", mesh)       # in no collection
            obj.matrix_world = matrix_from({"matrix": matrix})
            parts["part_objects"].append(obj)
            info = nodes.new('GeometryNodeObjectInfo')
            info.inputs['Object'].default_value = obj
            info.inputs['As Instance'].default_value = False
            # Relative to the prototype, which stays at the origin: the part's place in a copy.
            info.transform_space = 'RELATIVE'
            last = info.outputs['Geometry']
            for slot in range(len(mesh.materials)):
                named = nodes.new('GeometryNodeInputNamedAttribute')
                named.data_type = 'INT'
                named.inputs['Name'].default_value = "material_index"
                equal = nodes.new('FunctionNodeCompare')
                equal.data_type = 'INT'
                equal.operation = 'EQUAL'
                links.new(named.outputs['Attribute'], _socket(equal, 'A'))
                _socket(equal, 'B').default_value = slot
                setter = nodes.new('GeometryNodeSetMaterial')
                links.new(last, setter.inputs['Geometry'])
                links.new(equal.outputs['Result'], setter.inputs['Selection'])
                last = setter.outputs['Geometry']
                parts["setters"].append((index, slot, setter))
            links.new(last, join.inputs[0])
        prototype = parts["prototype"]
        modifier = prototype.modifiers.get("join") or prototype.modifiers.new("join", 'NODES')
        modifier.node_group = group
        parts["join_group"] = group
        parts["signature"] = signature
    for obj, (mesh, fields, _matrix) in zip(parts["part_objects"], sources):
        if obj.data != mesh:
            obj.data = mesh
        assign_materials(scene, obj, {"kind": "instancer", "id": item["id"], "materials": fields.get("materials", ())})
    for index, slot, setter in parts["setters"]:
        slots = parts["part_objects"][index].material_slots
        material = slots[slot].material if slot < len(slots) else None
        socket = setter.inputs['Material']
        if socket.default_value != material:
            socket.default_value = material


def _socket(node, name):
    """The whole-number input of that name of a Compare node (it has one per data type)."""
    for socket in node.inputs:
        if socket.name == name and socket.type == 'INT':
            return socket
    raise RuntimeError(f"the Compare node has no whole-number input {name!r}")
