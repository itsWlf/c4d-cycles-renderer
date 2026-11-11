# SPDX-License-Identifier: GPL-3.0-or-later
"""Cycles render devices.

Blender is started with factory settings, so no GPU device is enabled until the bridge
enables one on every launch.
"""

import bpy

# Tried in this order; the first back end that lists a device is used.
_GPU_BACKENDS = ('OPTIX', 'CUDA', 'HIP', 'ONEAPI', 'METAL')


def cycles_devices():
    """The render devices Cycles can use on this machine, as [{"name", "type"}]."""
    try:
        preferences = bpy.context.preferences.addons["cycles"].preferences
        preferences.refresh_devices()
        return [{"name": device.name, "type": device.type} for device in preferences.devices]
    except Exception:
        return []


def use_cycles_device(scene, device):
    """Select "CPU" or "GPU" for Cycles. Returns the one actually selected."""
    scene.cycles.device = 'CPU'
    if device != "GPU":
        return "CPU"
    preferences = bpy.context.preferences.addons["cycles"].preferences
    for backend in _GPU_BACKENDS:
        try:
            preferences.compute_device_type = backend
        except TypeError:
            continue  # this Blender build lacks the back end
        preferences.refresh_devices()
        if any(d.type == backend for d in preferences.devices):
            for d in preferences.devices:
                d.use = d.type == backend
            scene.cycles.device = 'GPU'
            return "GPU"
    return "CPU"
