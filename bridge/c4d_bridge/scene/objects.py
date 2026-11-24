# SPDX-License-Identifier: GPL-3.0-or-later
"""Object builders: one instance of a mesh in the scene, and an empty a node names."""

from ..errors import RequestError
from .common import give_name, place


def build_object(scene, item, blobs):
    """An `object` item: an object that shows a mesh sent before, with its place, materials,
    visibility, properties and, when the item has one, a name (for Cryptomatte)."""
    mesh = scene.meshes.get(item["mesh"])
    if mesh is None:
        raise RequestError("bad_request", f"object {item['id']!r} refers to unknown mesh {item['mesh']!r}")
    obj = scene.link(item["id"], mesh)
    give_name(obj, item.get("name"))
    place(obj, item)
    assign_materials(scene, obj, item)
    set_visibility(obj, item)
    set_properties(obj, item)


def build_empty(scene, item, blobs):
    """An `empty` item (since 0.3.0): an object without data at a place, which a node of a material or the
    world names by the item's id (materials.py, resolve_objects). Sent again, it is the same object moved, so
    what names it keeps naming it."""
    obj = scene.link(item["id"], None)
    give_name(obj, item.get("name"))
    place(obj, item)


def set_properties(obj, item):
    """Custom properties of the object ({name: a number, or a list of three or four numbers for a
    vector or a colour with its alpha}), which a material reads with an Attribute node of type OBJECT
    or INSTANCER. Those set by an earlier request and not sent again are removed."""
    wanted = item.get("properties") or {}
    for name in obj.get(_SENT, "").split("\n"):
        if name and name not in wanted and name in obj:
            del obj[name]
    for name, value in wanted.items():
        if not name or "\n" in name or not _is_property_value(value):
            raise RequestError("bad_request", f"{item['kind']} {item['id']!r}: property {name!r} must be a number or a list of three or four numbers")
        # (Only what differs is set, as in set_visibility.)
        current = obj.get(name)
        if isinstance(value, list):
            if current is None or isinstance(current, (int, float, str)) or [float(v) for v in current] != [float(v) for v in value]:
                obj[name] = [float(v) for v in value]
        elif current != value or isinstance(current, float) != isinstance(value, float):
            obj[name] = value
    if wanted:
        if obj.get(_SENT) != "\n".join(wanted):
            obj[_SENT] = "\n".join(wanted)
    elif _SENT in obj:
        del obj[_SENT]


def _is_property_value(value):
    """A number, or a list of three or four numbers."""
    def number(entry):
        return isinstance(entry, (int, float)) and not isinstance(entry, bool)
    return number(value) or (isinstance(value, list) and len(value) in (3, 4) and all(number(entry) for entry in value))


# The names of the properties the plugin set on an object, one per line, kept on the object.
_SENT = "_c4d_bridge_properties"


def set_visibility(obj, item):
    """Which rays see the object: {"camera", "shadow", "diffuse", "glossy", "transmission",
    "volume_scatter", "holdout", "shadow_catcher"}, and "pass_index", what the Object Index pass
    shows for it."""
    visibility = item.get("visibility", {})
    # Only what differs is set: an item sent again for a new place alone (the plugin's updates while an
    # object is being moved) then tells Blender of a moved object and of nothing else to look at.
    for name, value in (("visible_camera", visibility.get("camera", True)), ("visible_shadow", visibility.get("shadow", True)),
                        ("visible_diffuse", visibility.get("diffuse", True)), ("visible_glossy", visibility.get("glossy", True)),
                        ("visible_transmission", visibility.get("transmission", True)),
                        ("visible_volume_scatter", visibility.get("volume_scatter", True)),
                        ("is_holdout", visibility.get("holdout", False)), ("is_shadow_catcher", visibility.get("shadow_catcher", False)),
                        ("pass_index", int(visibility.get("pass_index", 0)))):
        if getattr(obj, name) != value:
            setattr(obj, name, value)


def assign_materials(scene, obj, item):
    """Materials sit on the object, not on the mesh, so instances of one mesh can differ."""
    for index, material_id in enumerate(item.get("materials", ())):
        if index >= len(obj.material_slots) or material_id is None:
            continue
        material = scene.materials.get(material_id)
        if material is None:
            raise RequestError("bad_request",
                               f"{item['kind']} {item['id']!r} refers to unknown material {material_id!r}")
        slot = obj.material_slots[index]
        if slot.link != 'OBJECT':
            slot.link = 'OBJECT'
        if slot.material != material:
            slot.material = material
