# SPDX-License-Identifier: GPL-3.0-or-later
"""Applies a `settings` request.

The plugin owns the table that maps Cinema 4D parameters to Blender properties and sends
ready-made (property path, value) pairs. Paths are relative to the scene, for example
`cycles.samples` or `render.film_transparent`; `view_layer` holds such pairs relative to the
view layer (the render passes). Three settings need more than an assignment and travel as
named fields instead: `engine`, `device` and `working_space`.
"""

import bpy

from ..errors import RequestError
from . import devices

# Engine names of the protocol -> Blender's engine identifiers.
ENGINES = {"CYCLES": "CYCLES", "EEVEE": "BLENDER_EEVEE"}


def apply(header):
    """Handle a `settings` request. Returns (reply fields, blobs)."""
    scene = bpy.context.scene
    warnings = []

    engine = header.get("engine")
    if engine is not None:
        if engine not in ENGINES:
            raise RequestError("bad_request", f"unknown engine {engine!r}")
        scene.render.engine = ENGINES[engine]

    working_space = header.get("working_space")
    if working_space is not None:
        _set_working_space(working_space, warnings)

    for path, value in header.get("values", ()):
        owner_path, _dot, name = path.rpartition(".")
        try:
            owner = scene.path_resolve(owner_path) if owner_path else scene
            if not hasattr(owner, name):
                raise AttributeError(name)
            if isinstance(value, str):
                # A choice among named options. Which ones exist can depend on the machine (the
                # OptiX denoiser needs a suitable graphics card): keep Blender's own, and say so.
                try:
                    setattr(owner, name, value)
                except TypeError:
                    warnings.append(f"Blender does not offer \"{value}\" for {name.replace('_', ' ')} on this computer; "
                                    f"\"{getattr(owner, name)}\" is used.")
                continue
            setattr(owner, name, value)
        except (AttributeError, ValueError, TypeError) as error:
            raise RequestError("bad_request", f"cannot set scene.{path} to {value!r} ({error})") from error

    # Properties of the view layer: which render passes are computed.
    layer = bpy.context.view_layer or scene.view_layers[0]
    for path, value in header.get("view_layer", ()):
        owner_path, _dot, name = path.rpartition(".")
        try:
            owner = layer.path_resolve(owner_path) if owner_path else layer
            if not hasattr(owner, name):
                raise AttributeError(name)
            setattr(owner, name, value)
        except (AttributeError, ValueError, TypeError) as error:
            raise RequestError("bad_request", f"cannot set view_layer.{path} to {value!r} ({error})") from error

    device = header.get("device")
    if device is not None:
        used = devices.use_cycles_device(scene, device)
        if used != device:
            warnings.append(f"Cycles has no {device} device here; rendering on the {used}.")
    return {"warnings": warnings, "engine": scene.render.engine, "device": scene.cycles.device}, ()


def read(header):
    """Handle a `get` request: report the current value of scene properties, by path."""
    scene = bpy.context.scene
    values = {}
    for path in header.get("paths", ()):
        try:
            value = scene.path_resolve(path)
        except ValueError as error:
            raise RequestError("bad_request", f"scene.{path} does not exist ({error})") from error
        if hasattr(value, "__len__") and not isinstance(value, str):
            value = list(value)
        values[path] = value
    return {"values": values, "working_space": bpy.data.colorspace.working_space}, ()


def _set_working_space(name, warnings):
    current = bpy.data.colorspace.working_space
    if current == name:
        return
    try:
        bpy.ops.wm.set_working_color_space(working_space=name, convert_colors=False)
    except (TypeError, RuntimeError) as error:
        warnings.append(f"Blender could not switch its working colour space to {name!r}; it stays "
                        f"{current!r}, so colours may differ. ({error})")
