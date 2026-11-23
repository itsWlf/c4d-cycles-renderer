# SPDX-License-Identifier: GPL-3.0-or-later
"""Proxy builder: what another .blend file holds, linked and shown at a place of this scene.

A `proxy` item names a file and what to take from it: one collection, or what one of its scenes
shows. The file is linked (`bpy.data.libraries.load(path, link=True)`), never appended: its data
stays the file's, is in memory once however many proxies show it, and a proxy is an empty that
instances the linked collection (one `matrix`) or a Geometry Nodes instancer of it (`matrices`,
many copies). All measured in Blender 5.0 to 5.3 (docs/reference/blender.md, "Linked files"):

- Linking the same file again makes no second library and no second copy of anything.
- Blender converts the colours of what it links to this file's working colour space when it
  links, so a file made in Blender's default space renders the same colours here. A working space
  switched afterwards leaves them as they were: relink_all() links again then.
- A file that changed on disk is not noticed, and linking again gives the old data while the
  library is there. The item's `stamp` (the plugin sends the file's date) says when to link anew.
- `bpy.data.libraries.remove(library)` frees a library with everything linked from it; nothing is
  left behind. Removing the linked objects and collections alone leaves the library and its scene.
- What is linked follows this scene's frame; there is one frame for everything (set_frame()).

What a scene "shows" is its collections as its first view layer has them: a collection excluded
there is left out, which a plain collection instance would not do, so a scene is shown through
collections made here that hold the linked ones (_wrap()). The same goes for a file taken without
its lights. The file's world, camera and render settings are not taken.
"""

import math
import os

import bpy
import numpy as np
from mathutils import Vector

from ..errors import RequestError
from .common import array_from, place
from .objects import set_visibility

# The point attribute that holds a copy's matrix (as instancers.py; not the built-in name).
_TRANSFORM = "c4d_transform"
# Object types that have a size of their own, for the box blend_info() answers.
_SOLID = {'MESH', 'CURVE', 'CURVES', 'SURFACE', 'FONT', 'META', 'VOLUME', 'POINTCLOUD', 'GREASEPENCIL'}


class _File:
    """One linked file: the collections taken from it so far."""

    def __init__(self, path, stamp):
        self.path = path
        self.stamp = stamp
        self.takes = {}         # (scene name, collection name, lights) -> collection, or None when it is not there
        self.lacking = {}       # the same key -> why there is none, in a sentence for the user
        self.made = []          # collections made here (they are not the file's)
        self.names = None       # {"scenes": [...], "collections": [...]}, sorted, as the file lists them


def _key(path):
    """One spelling per file."""
    return os.path.normcase(os.path.abspath(path))


def _load(file, kind, name):
    """Link one scene or collection of the file by name (None: only read the names). Returns it, or None."""
    with bpy.data.libraries.load(file.path, link=True) as (source, target):
        file.names = {"scenes": sorted(source.scenes), "collections": sorted(source.collections)}
        if name is not None and name in getattr(source, kind):
            setattr(target, kind, [name])
    linked = getattr(target, kind) if name is not None else None
    return linked[0] if linked else None


def _excludes(layer_collection):
    """Whether the view layer leaves out this collection or one below it."""
    return layer_collection.exclude or any(_excludes(child) for child in layer_collection.children)


def _wrap(file, layer_collection, name):
    """A collection that shows what the view layer shows of `layer_collection`: the linked one itself
    when nothing in it is excluded, else one made here with the same content less what is excluded."""
    if not _excludes(layer_collection):
        return layer_collection.collection
    made = bpy.data.collections.new(name)
    file.made.append(made)
    if not layer_collection.exclude:
        for obj in layer_collection.collection.objects:
            made.objects.link(obj)
    for child in layer_collection.children:
        if child.exclude and not child.children:
            continue
        made.children.link(_wrap(file, child, name + "." + child.name))
    return made


def _members(collection):
    """The objects a collection instance renders: its own and those of the collections below it that
    are not switched off for rendering."""
    yield from collection.objects
    for child in collection.children:
        if not child.hide_render:
            yield from _members(child)


def _without_lights(file, collection, name):
    """A collection made here that holds what `collection` shows, less its lights."""
    made = bpy.data.collections.new(name)
    file.made.append(made)
    made.instance_offset = collection.instance_offset
    seen = set()
    for obj in _members(collection):
        if obj.type != 'LIGHT' and obj not in seen:
            seen.add(obj)
            made.objects.link(obj)
    return made


def _take(warn, file, scene_name, collection_name, lights, name):
    """The collection a proxy shows, linked when first asked for. None, after a call of `warn` with
    a sentence for the user, when the file lacks what was named (`name`: what the user calls the proxy)."""
    key = (scene_name, collection_name, lights)
    if key not in file.takes:
        file.takes[key] = _link(file, key)
    if file.takes[key] is None:
        warn(f"{file.lacking[key]}; the proxy \"{name}\" shows nothing.")
    return file.takes[key]


def _link(file, key):
    """Link what `key` names from the file. Returns the collection to show, or None with file.lacking[key] set."""
    scene_name, collection_name, lights = key
    label = os.path.basename(file.path)
    taken = None
    if collection_name is not None:
        taken = _load(file, "collections", collection_name)
        if taken is None:
            file.lacking[key] = (f"The Blender file {file.path} has no collection named \"{collection_name}\" "
                                 f"(it has: {', '.join(file.names['collections']) or 'none'})")
    else:
        if scene_name is None:
            # Nothing named: the file's first scene by name. Which scene was open when the file was saved is
            # not something linking tells.
            _load(file, "scenes", None)
            scene_name = file.names["scenes"][0] if file.names["scenes"] else None
        linked = _load(file, "scenes", scene_name) if scene_name is not None else None
        if linked is None:
            file.lacking[key] = (f"The Blender file {file.path} has no scene named \"{scene_name}\" "
                                 f"(it has: {', '.join(file.names['scenes']) or 'none'})")
        else:
            taken = bpy.data.collections.new(f"{label}.{scene_name}")
            file.made.append(taken)
            for obj in linked.collection.objects:
                taken.objects.link(obj)
            for child in linked.view_layers[0].layer_collection.children:
                if child.exclude and not child.children:
                    continue
                taken.children.link(_wrap(file, child, f"{label}.{child.name}"))
    if taken is not None and not lights:
        taken = _without_lights(file, taken, f"{label}.{collection_name or scene_name}.no_lights")
    return taken


def _free(scene, key):
    """Let go of a file: its library with everything linked from it, and what was made here around it."""
    file = scene.proxy_files.pop(key, None)
    if file is None:
        return
    for made in file.made:
        try:
            bpy.data.collections.remove(made)
        except ReferenceError:
            pass
    for library in [library for library in bpy.data.libraries if _key(bpy.path.abspath(library.filepath)) == key]:
        bpy.data.libraries.remove(library)


def _file(scene, path, stamp):
    """The record of a file, linked anew when its stamp is another than the one it was linked with."""
    key = _key(path)
    file = scene.proxy_files.get(key)
    if file is not None and file.stamp == stamp:
        return file
    # New, or changed on disk. Removing the library empties every proxy that shows it: each is pointed
    # at the new data here (the one being built once more by its builder, which costs nothing).
    _free(scene, key)
    file = _File(path, stamp)
    scene.proxy_files[key] = file
    for entry in scene.proxies.values():
        if entry["key"] == key:
            _show(scene, entry, file)
    return file


def _show(scene, entry, file):
    """Point a proxy's holder at what it takes from `file`. Returns that collection, or None."""
    taken = _take(scene.warn, file, entry["scene"], entry["collection"], entry["lights"], entry["name"])
    if entry["group"] is None:
        entry["holder"].instance_type = 'COLLECTION' if taken is not None else 'NONE'
        entry["holder"].instance_collection = taken
    else:
        entry["info"].inputs['Collection'].default_value = taken
        entry["holder"].update_tag()
    return taken


def _group(item_id):
    """The Geometry Nodes group of a proxy with many copies: the collection as one instance on every
    point, placed by the points' matrix attribute (as instancers.py does for a mesh)."""
    group = bpy.data.node_groups.new(item_id, 'GeometryNodeTree')
    group.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    group.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    group.is_modifier = True
    nodes, links = group.nodes, group.links
    source = nodes.new('NodeGroupInput')
    target = nodes.new('NodeGroupOutput')
    info = nodes.new('GeometryNodeCollectionInfo')     # the whole collection as one instance, where the file has it
    instance = nodes.new('GeometryNodeInstanceOnPoints')
    matrix = nodes.new('GeometryNodeInputNamedAttribute')
    matrix.data_type = 'FLOAT4X4'
    matrix.inputs['Name'].default_value = _TRANSFORM
    transform = nodes.new('GeometryNodeSetInstanceTransform')
    links.new(source.outputs[0], instance.inputs['Points'])
    links.new(info.outputs[0], instance.inputs['Instance'])
    links.new(instance.outputs['Instances'], transform.inputs['Instances'])
    links.new(matrix.outputs['Attribute'], transform.inputs['Transform'])
    links.new(transform.outputs['Instances'], target.inputs[0])
    return group, info


def _drop(scene, item_id, entry):
    """Remove what a proxy made in Blender (not its file)."""
    holder = scene.objects.pop(item_id, None)
    if holder is not None:
        bpy.data.objects.remove(holder)
    if entry["group"] is not None:
        bpy.data.node_groups.remove(entry["group"])
        bpy.data.meshes.remove(entry["points"])


def build_proxy(scene, item, blobs):
    """A `proxy` item: the holder made once, then pointed at what it takes from its file and placed."""
    item_id = item["id"]
    path = item["path"]
    many = "matrices" in item
    old = scene.proxies.get(item_id)
    if not os.path.isfile(path):
        scene.warn(f"The Blender file {path} was not found; the proxy \"{item.get('name') or item_id}\" shows nothing.")
        remove_proxy(scene, item_id)
        return
    if old is not None and (old["group"] is not None) != many:
        # One place became many copies or the other way round: another kind of holder.
        scene.proxies.pop(item_id)
        _drop(scene, item_id, old)
        old = None

    key = _key(path)
    entry = old or {"id": item_id, "group": None, "info": None, "points": None, "key": None}
    previous = entry["key"]
    entry.update(key=key, scene=item.get("scene"), collection=item.get("collection"), lights=bool(item.get("lights", True)),
                 frame=item.get("frame"), name=item.get("name") or item_id)
    if old is None and many:
        entry["points"] = bpy.data.meshes.new(item_id + ".points")
        entry["holder"] = scene.link(item_id, entry["points"])
        entry["group"], entry["info"] = _group(item_id)
        entry["holder"].modifiers.new("proxy", 'NODES').node_group = entry["group"]
    elif old is None:
        entry["holder"] = scene.link(item_id, None)
    scene.proxies[item_id] = entry

    taken = _show(scene, entry, _file(scene, path, item.get("stamp")))
    holder = entry["holder"]
    if many:
        matrices = array_from(blobs, item["matrices"], np.float32)
        if len(matrices) % 16:
            raise RequestError("bad_request", f"proxy {item_id!r}: matrices must be 16 numbers each")
        count = len(matrices) // 16
        points = entry["points"]
        if len(points.vertices) != count:
            points.clear_geometry()
            points.vertices.add(count)
        # The plugin sends rows; Blender stores a matrix attribute column by column.
        rows = matrices.reshape(count, 4, 4)
        points.vertices.foreach_set("co", np.ascontiguousarray(rows[:, :3, 3]).ravel())
        attribute = points.attributes.get(_TRANSFORM) or points.attributes.new(_TRANSFORM, 'FLOAT4X4', 'POINT')
        attribute.data.foreach_set("value", np.ascontiguousarray(rows.transpose(0, 2, 1)).ravel())
        points.update()
        holder.update_tag()
        if taken is not None and entry["lights"] and any(obj.type == 'LIGHT' for obj in taken.all_objects):
            scene.warn(f"The lights of the Blender file {path} are not copied with the {count} copies of the proxy \"{entry['name']}\": "
                       f"Blender's instances on points carry geometry only.")
    else:
        place(holder, item)
    set_visibility(holder, item)
    if previous is not None and previous != key:
        _release(scene, previous)


def _release(scene, key):
    """Free a file no proxy shows any more."""
    if not any(entry["key"] == key for entry in scene.proxies.values()):
        _free(scene, key)


def remove_proxy(scene, item_id):
    """Remove a proxy, if `item_id` is one; its file is freed when it was the last to show it."""
    entry = scene.proxies.pop(item_id, None)
    if entry is None:
        return
    _drop(scene, item_id, entry)
    _release(scene, entry["key"])


def clear(scene):
    """Free every linked file, before the scene's own data goes (Scene.clear)."""
    for key in list(scene.proxy_files):
        _free(scene, key)
    scene.proxies.clear()


def relink_all(scene):
    """Link every file again: after the working colour space changed, since Blender converts linked
    colours only when it links."""
    for key in list(scene.proxy_files):
        old = scene.proxy_files[key]
        _free(scene, key)
        file = _File(old.path, old.stamp)
        scene.proxy_files[key] = file
        for entry in scene.proxies.values():
            if entry["key"] == key:
                _show(scene, entry, file)


def _whole(frame):
    return int(math.floor(frame + 0.5))


def set_frame(scene, header):
    """Blender's scene has one frame, and what is linked plays by it. Before the items of a request
    are built: the frame the proxies ask for (`frame`), the first one's when they differ, with a
    warning. Objects keyed around the old frame for motion blur (common.place) move along with it."""
    wanted = {item_id: entry["frame"] for item_id, entry in scene.proxies.items()}
    sent = False
    for item in header.get("items", ()):
        if item.get("kind") == "proxy":
            wanted[item.get("id")] = item.get("frame")
            sent = True
    frames = [_whole(frame) for frame in wanted.values() if frame is not None]
    if not frames:
        return
    frame = frames[0]
    if sent and any(other != frame for other in frames):
        scene.warn(f"The proxies ask for different frames of their Blender files ({', '.join(str(f) for f in sorted(set(frames)))}): "
                   f"Blender's scene has one time, so every linked file is at frame {frame}.")
    current = bpy.context.scene
    shift = frame - current.frame_current
    if shift == 0:
        return
    for action in bpy.data.actions:
        if action.library is not None:
            continue
        for layer in action.layers:
            for strip in layer.strips:
                for bag in strip.channelbags:
                    for curve in bag.fcurves:
                        for key in curve.keyframe_points:
                            key.co.x += shift
                            key.handle_left.x += shift
                            key.handle_right.x += shift
                        curve.update()
    current.frame_set(frame)


def after_items(scene, header):
    """After the items of a request are built: what was linked by it takes its place of the scene's frame.
    A live viewport shows newly linked animated objects where their file was saved until the frame is set
    (measured; a final render evaluates the frame by itself)."""
    if any(item.get("kind") == "proxy" for item in header.get("items", ())):
        current = bpy.context.scene
        current.frame_set(current.frame_current)


def info(scene, header):
    """The `blend_info` operation: what a .blend file offers to take, with the box each fills."""
    path = header["path"]
    if not os.path.isfile(path):
        raise RequestError("bad_request", f"The Blender file {path} was not found.")
    key = _key(path)
    # In a Blender that shows the file already, what is linked stays; else the file is let go again.
    held = key in scene.proxy_files
    file = scene.proxy_files.get(key) or _File(path, None)
    scene.proxy_files[key] = file
    reply = {"scenes": [], "collections": []}
    holder = bpy.data.objects.new("blend_info", None)
    try:
        try:
            _load(file, "scenes", None)
        except OSError as error:
            raise RequestError("bad_request", f"Blender could not read {path}: {error}") from error
        bpy.context.scene.collection.objects.link(holder)
        holder.instance_type = 'COLLECTION'
        for kind, names in (("scenes", file.names["scenes"]), ("collections", file.names["collections"])):
            for name in names:
                taken = _take(lambda message: None, file, name if kind == "scenes" else None, name if kind == "collections" else None,
                              True, "blend_info")
                entry = {"name": name}
                if taken is not None:
                    holder.instance_collection = taken
                    entry.update(_box(holder))
                reply[kind].append(entry)
    finally:
        bpy.data.objects.remove(holder)
        if not held:
            _free(scene, key)
    return reply, ()


def _box(holder):
    """{"objects", "min", "max"}: how many objects with a size the holder's instances are, and the box
    around them in Blender's axes and metres, at this Blender's frame (no box when there is none)."""
    depsgraph = bpy.context.evaluated_depsgraph_get()
    depsgraph.update()
    low, high, count = None, None, 0
    for instance in depsgraph.object_instances:
        if not instance.is_instance or instance.parent is None or instance.parent.original != holder:
            continue
        obj = instance.object
        if obj.type not in _SOLID or obj.original.hide_render:
            continue
        count += 1
        matrix = instance.matrix_world
        for corner in obj.bound_box:
            point = matrix @ Vector(corner)
            low = point.copy() if low is None else Vector(map(min, low, point))
            high = point.copy() if high is None else Vector(map(max, high, point))
    if low is None:
        return {"objects": 0}
    return {"objects": count, "min": [round(v, 6) for v in low], "max": [round(v, 6) for v in high]}
