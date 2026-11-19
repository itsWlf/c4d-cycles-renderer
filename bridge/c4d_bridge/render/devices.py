# SPDX-License-Identifier: GPL-3.0-or-later
"""Cycles render devices.

Blender is started with factory settings, so no GPU device is enabled until the bridge
enables one on every launch. What is set here lives in this Blender's memory only: with
--factory-startup Blender neither reads nor writes the user's preferences file
(G_FLAG_USERPREF_NO_SAVE_ON_EXIT, creator_args.cc), and the bridge never saves preferences.
"""

import bpy

# Tried in this order when no back end is asked for; the first back end that lists a device is used.
_GPU_BACKENDS = ('OPTIX', 'CUDA', 'HIP', 'ONEAPI', 'METAL')


def _preferences():
    """The Cycles add-on's preferences, where the devices are."""
    return bpy.context.preferences.addons["cycles"].preferences


def cycles_devices():
    """The render devices Cycles can use on this machine, of every back end and the CPU, as
    [{"name", "type", "id"}]. `id` is Blender's persistent id of the device."""
    try:
        preferences = _preferences()
        preferences.refresh_devices()
        return [{"name": device.name, "type": device.type, "id": device.id} for device in preferences.devices]
    except Exception:
        return []


def display_gpu():
    """The graphics card Blender draws with (and Eevee renders on), as its GPU module names it, or
    None where Blender draws with none (in the background)."""
    try:
        import gpu
        return gpu.platform.renderer_get() or None
    except Exception:
        return None


def _backend_has_device(preferences, backend):
    """Switch Blender to `backend` and say whether it lists a device of it."""
    try:
        preferences.compute_device_type = backend
    except TypeError:
        return False    # this Blender build lacks the back end
    preferences.refresh_devices()
    return any(d.type == backend for d in preferences.devices)


def use_cycles_device(scene, device, device_type=None, device_ids=None):
    """Select "CPU", "GPU" or "AUTO" for Cycles. "AUTO" is "GPU" when this computer has one, and
    the CPU otherwise, which is then said once as a warning (it is what was chosen, but the user
    should know where the render runs).

    device_type: a back end (OPTIX, CUDA, HIP, ONEAPI, METAL), "NONE" for no GPU, or None for the
    first that lists a device. device_ids: Blender's ids of the devices to use (of that back end,
    and the CPU next to it as in Blender's preferences); None for every device of the back end.
    Returns (device used, back end used or "NONE", names of the devices in use, warnings). Every
    case where "GPU" or "AUTO" ends on the CPU comes with a warning, except "AUTO" with "NONE",
    which is the user's own choice of no GPU."""
    warnings = []
    scene.cycles.device = 'CPU'
    if device not in ("GPU", "AUTO"):
        return "CPU", "NONE", [], warnings
    automatic = device == "AUTO"
    if device_type == "NONE":
        if not automatic:
            warnings.append("The Cycles Render preferences choose no GPU device type; rendering on the CPU.")
        return "CPU", "NONE", [], warnings
    preferences = _preferences()
    backend = None
    if device_type:
        if _backend_has_device(preferences, device_type):
            backend = device_type
        else:
            warnings.append(f"Cycles has no {device_type} device on this computer (preferences of Cycles Render); "
                            "the first device type that has one is used.")
    if backend is None:
        backend = next((candidate for candidate in _GPU_BACKENDS if _backend_has_device(preferences, candidate)), None)
    if backend is None:
        if automatic:
            warnings.append("Cycles found no GPU it can render on in this computer (Device: Automatic); rendering on the CPU.")
        else:
            warnings.append("Cycles found no GPU it can render on in this computer; rendering on the CPU.")
        return "CPU", "NONE", [], warnings
    preferences.compute_device_type = backend
    preferences.refresh_devices()
    candidates = [d for d in preferences.devices if d.type in (backend, 'CPU')]
    if device_ids is not None:
        wanted = set(device_ids)
        if not any(d.id in wanted for d in candidates):
            warnings.append(f"None of the devices chosen in the Cycles Render preferences is one of this computer's {backend} devices; "
                            f"every {backend} device is used.")
            device_ids = None
    for d in preferences.devices:
        if device_ids is None:
            d.use = d.type == backend
        else:
            d.use = d.type in (backend, 'CPU') and d.id in device_ids
    used = [d for d in preferences.devices if d.use]
    if not any(d.type == backend for d in used):
        # Only the CPU is ticked: Blender renders that on the CPU device itself.
        warnings.append(f"No {backend} device is ticked in the Cycles Render preferences; rendering on the CPU.")
        return "CPU", "NONE", [], warnings
    scene.cycles.device = 'GPU'
    return "GPU", backend, [d.name for d in used], warnings
