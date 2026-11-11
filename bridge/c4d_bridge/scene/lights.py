# SPDX-License-Identifier: GPL-3.0-or-later
"""Light builder."""

import bpy

from .common import matrix_from

# Optional fields that map one to one onto properties of the light datablock.
_DIRECT = ("spot_size", "spot_blend", "shape", "size", "size_y", "angle", "shadow_soft_size",
           "use_shadow", "spread")


def build_light(scene, item, blobs):
    light = bpy.data.lights.new(item["id"], item.get("type", "POINT"))
    light.energy = item.get("energy", 1000.0)
    light.color = item.get("color", (1.0, 1.0, 1.0))
    for name in _DIRECT:
        if name in item:
            setattr(light, name, item[name])
    if "use_soft_falloff" in item and hasattr(light, "use_soft_falloff"):
        light.use_soft_falloff = item["use_soft_falloff"]
    scene.link(item["id"], light).matrix_world = matrix_from(item)
    scene.light_links[item["id"]] = item.get("linking")


def apply_links(scene):
    """Light linking: give every light with a list the collection of objects it lights or
    spares. Done after each request, because an object that was sent again is a new object."""
    for light_id in list(scene.light_links):
        obj = scene.objects.get(light_id)
        if obj is None:
            del scene.light_links[light_id]
        name = "links_" + light_id
        old = bpy.data.collections.get(name)
        if old is not None:
            bpy.data.collections.remove(old)
        spec = scene.light_links.get(light_id)
        if obj is None or not spec:
            continue
        collection = bpy.data.collections.new(name)
        for object_id in spec.get("objects", ()):
            member = scene.objects.get(object_id)
            if member is not None and member.name not in collection.objects:
                collection.objects.link(member)
        if spec.get("mode") == "EXCLUDE":
            for entry in collection.collection_objects:
                entry.light_linking.link_state = 'EXCLUDE'
        # The same list decides what this light lights and what casts its shadows: an object
        # Cinema 4D excludes from a light throws no shadow from it either (measured).
        obj.light_linking.receiver_collection = collection
        obj.light_linking.blocker_collection = collection
