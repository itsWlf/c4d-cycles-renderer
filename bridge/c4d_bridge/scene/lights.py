# SPDX-License-Identifier: GPL-3.0-or-later
"""Light builder."""

import bpy

from .common import matrix_from

# Optional fields that map one to one onto properties of the light datablock. Which ones a light
# has depends on its type (spot_size only on a spot, spread only on an area, ...).
_DIRECT = ("spot_size", "spot_blend", "shape", "size", "size_y", "angle", "shadow_soft_size",
           "use_shadow", "spread", "use_soft_falloff", "exposure", "normalize", "use_temperature",
           "temperature", "diffuse_factor", "specular_factor", "transmission_factor", "volume_factor",
           "use_custom_distance", "cutoff_distance", "use_shadow_jitter", "shadow_jitter_overblur",
           "shadow_filter_radius", "shadow_maximum_resolution", "use_absolute_resolution")

# Fields of the item's `cycles` object: properties of `light.cycles` (Cycles' Settings panel).
_CYCLES = ("max_bounces", "use_multiple_importance_sampling", "is_caustics_light", "is_portal")

# Fields of the item's `visibility` object: the light object's ray visibility.
_VISIBILITY = {"camera": "visible_camera", "diffuse": "visible_diffuse", "glossy": "visible_glossy",
               "transmission": "visible_transmission", "volume_scatter": "visible_volume_scatter"}


def _set(scene, owner, name, value, what):
    if not hasattr(owner, name):
        scene.warn(f"Light {what}: Blender's {owner.bl_rna.identifier} has no property \"{name}\"; it was left out.")
        return
    try:
        setattr(owner, name, value)
    except (TypeError, ValueError) as error:
        scene.warn(f"Light {what}: {name} = {value!r} was refused by Blender ({error}).")


def build_light(scene, item, blobs):
    light = bpy.data.lights.new(item["id"], item.get("type", "POINT"))
    light.energy = item.get("energy", 1000.0)
    light.color = item.get("color", (1.0, 1.0, 1.0))
    for name in _DIRECT:
        if name in item:
            _set(scene, light, name, item[name], item["id"])
    for name, value in item.get("cycles", {}).items():
        if name in _CYCLES:
            _set(scene, light.cycles, name, value, item["id"])
    obj = scene.link(item["id"], light)
    obj.matrix_world = matrix_from(item)
    for name, value in item.get("visibility", {}).items():
        if name in _VISIBILITY:
            setattr(obj, _VISIBILITY[name], bool(value))
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
