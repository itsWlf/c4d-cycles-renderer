# SPDX-License-Identifier: GPL-3.0-or-later
"""Volume builder: a .vdb file shown as a volume object."""

import os

import bpy

from .common import place
from .objects import assign_materials

# Files of VDB format 225 and newer render empty in the Cycles of Blender 5.0 and 5.1, and render in
# 5.2 and 5.3; Eevee shows them in all four; files of format 224 render everywhere (measured
# 2026-11-16 with a fog ball and a smoke density grid of format 225 and a fire simulation of 224).
# Blender 5.0's OpenVDB 12 also says "unsupported VDB file format" (it reads up to 224) on its
# console, 5.1's OpenVDB 13 says nothing. Nothing tells Python, so the file's header is read.
_FIRST_FORMAT_EMPTY_IN_CYCLES = 225
_CYCLES_READS_IT_FROM = (5, 2, 0)
_VDB_MAGIC = 0x56444220


def _check_format(scene, path):
    """Warn when this Blender's Cycles renders the file empty."""
    try:
        with open(path, "rb") as file:
            header = file.read(12)
    except OSError:
        return
    if len(header) < 12 or int.from_bytes(header[:8], "little") != _VDB_MAGIC:
        return
    file_format = int.from_bytes(header[8:12], "little")
    if file_format >= _FIRST_FORMAT_EMPTY_IN_CYCLES and tuple(bpy.app.version) < _CYCLES_READS_IT_FROM:
        scene.warn(f"The volume file {path} has VDB format {file_format}: the Cycles of Blender {bpy.app.version_string} "
                   f"renders it empty (Eevee shows it; Blender 5.2 or newer renders it in Cycles).")


def build_volume(scene, item, blobs):
    path = item["path"]
    if not os.path.isfile(path):
        scene.warn(f"The volume file {path} was not found; the volume is left out.")
    else:
        _check_format(scene, path)
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
