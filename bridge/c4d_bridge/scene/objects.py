# SPDX-License-Identifier: GPL-3.0-or-later
"""Object builder: one instance of a mesh in the scene."""

from ..errors import RequestError
from .common import place


def build_object(scene, item, blobs):
    mesh = scene.meshes.get(item["mesh"])
    if mesh is None:
        raise RequestError("bad_request", f"object {item['id']!r} refers to unknown mesh {item['mesh']!r}")
    obj = scene.link(item["id"], mesh)
    place(obj, item)
    assign_materials(scene, obj, item)
    set_visibility(obj, item)


def set_visibility(obj, item):
    """Which rays see the object: {"camera", "shadow", "diffuse", "glossy", "transmission",
    "volume_scatter", "holdout", "shadow_catcher"}."""
    visibility = item.get("visibility", {})
    obj.visible_camera = visibility.get("camera", True)
    obj.visible_shadow = visibility.get("shadow", True)
    obj.visible_diffuse = visibility.get("diffuse", True)
    obj.visible_glossy = visibility.get("glossy", True)
    obj.visible_transmission = visibility.get("transmission", True)
    obj.visible_volume_scatter = visibility.get("volume_scatter", True)
    obj.is_holdout = visibility.get("holdout", False)
    obj.is_shadow_catcher = visibility.get("shadow_catcher", False)


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
        slot.link = 'OBJECT'
        slot.material = material
