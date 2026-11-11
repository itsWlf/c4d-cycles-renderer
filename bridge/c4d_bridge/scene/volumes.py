# SPDX-License-Identifier: GPL-3.0-or-later
"""Volume builder: a .vdb file shown as a volume object."""

import os

import bpy

from .common import place
from .objects import assign_materials


def build_volume(scene, item, blobs):
    path = item["path"]
    if not os.path.isfile(path):
        scene.warn(f"The volume file {path} was not found; the volume is left out.")
    old = scene.volumes.pop(item["id"], None)
    volume = bpy.data.volumes.new(item["id"])
    volume.filepath = path
    # The file is in Cinema 4D's units and the object's matrix scales it to metres. Density must
    # not follow that scale: it is meant per metre of the scene.
    volume.render.space = 'WORLD'
    # One slot per material; the object fills them, as for meshes.
    for _ in item.get("materials", ()):
        volume.materials.append(None)
    obj = scene.link(item["id"], volume)
    if old is not None:
        bpy.data.volumes.remove(old)
    scene.volumes[item["id"]] = volume
    place(obj, item)
    assign_materials(scene, obj, item)
