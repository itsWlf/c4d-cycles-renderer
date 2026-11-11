# SPDX-License-Identifier: GPL-3.0-or-later
"""Camera builder."""

import bpy

from .common import place


def build_camera(scene, item, blobs):
    # A camera that exists is changed in place: moving the view is the most frequent edit of all.
    existing = scene.objects.get(item["id"])
    camera = existing.data if existing is not None and existing.type == 'CAMERA' else bpy.data.cameras.new(item["id"])
    camera.type = item.get("type", "PERSP")
    camera.sensor_fit = 'HORIZONTAL'
    camera.sensor_width = item.get("sensor_width", 36.0)
    camera.lens = item.get("lens", 50.0)
    camera.ortho_scale = item.get("ortho_scale", 6.0)
    camera.shift_x = item.get("shift_x", 0.0)
    camera.shift_y = item.get("shift_y", 0.0)
    camera.clip_start = item.get("clip_start", 0.1)
    camera.clip_end = item.get("clip_end", 1000.0)

    dof = item.get("dof")
    camera.dof.use_dof = dof is not None
    if dof is not None:
        camera.dof.focus_distance = dof["focus_distance"]
        camera.dof.aperture_fstop = dof["fstop"]

    obj = scene.link(item["id"], camera)
    place(obj, item)
    if item.get("active"):
        bpy.context.scene.camera = obj
