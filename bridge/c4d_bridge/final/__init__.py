# SPDX-License-Identifier: GPL-3.0-or-later
"""Final driver: renders one frame with a normal blocking render and returns its pixels.

The engine, samples, passes and every other render setting were set before by a `settings`
request; a `render` request carries the resolution and pixel aspect, the names of the passes
wanted back and optionally the region to render.
"""

import os
import shutil
import tempfile
import time

import bpy
import numpy as np

from ..errors import RequestError
from ..scene.common import MOTION_SHUTTER_FRAMES

_captured = {}
_wanted = []
_installed = False


def _install_cycles_readback():
    """Cycles keeps a final render's pixels out of reach of Python, except inside the engine
    object while a render finishes. Wrap that moment once and copy the passes wanted."""
    global _installed
    if _installed:
        return
    import cycles
    original = cycles.CyclesRender.render_frame_finish

    def render_frame_finish(self):
        original(self)
        result = self.get_result()
        width, height = result.resolution_x, result.resolution_y
        passes = result.layers[0].passes
        for name in ["Combined"] + _wanted:
            source = passes.find_by_name(name, "")
            if source is None:
                continue
            pixels = np.empty(width * height * source.channels, dtype=np.float32)
            source.rect.foreach_get(pixels)
            _captured[name] = pixels.reshape(height, width, source.channels)

    cycles.CyclesRender.render_frame_finish = render_frame_finish
    _installed = True


def _render_cycles(wanted, scene=None):
    """Returns {pass name: array}, rows bottom-up; "Combined" is always there.
    Renders the current scene, or `scene` when one is given."""
    _install_cycles_readback()
    _captured.clear()
    _wanted[:] = wanted
    if scene is None:
        bpy.ops.render.render()
    else:
        bpy.ops.render.render(scene=scene.name)
    images = dict(_captured)
    _captured.clear()
    if "Combined" not in images:
        raise RequestError("internal", "the render finished without delivering pixels")
    return images


def _render_through_file(scene, wanted):
    """Eevee offers no in-memory access to a final frame: render to an uncompressed float EXR
    in a temporary folder and read it back. Returns {pass name: array}, rows bottom-up."""
    import OpenImageIO as oiio

    directory = tempfile.mkdtemp(prefix="c4d_bridge_frame_")
    path = os.path.join(directory, "frame.exr")
    settings = scene.render.image_settings
    if wanted:
        # One file with every pass. The media type must be set before the format.
        settings.media_type = 'MULTI_LAYER_IMAGE'
        settings.file_format = 'OPEN_EXR_MULTILAYER'
    else:
        if hasattr(settings, "media_type"):
            settings.media_type = 'IMAGE'
        settings.file_format = 'OPEN_EXR'
        settings.color_mode = 'RGBA'
    settings.color_depth = '32'
    settings.exr_codec = 'NONE'
    # Write the working colour space unchanged; the plugin expects render-space values.
    linear = getattr(settings, "linear_colorspace_settings", None)
    if linear is not None:
        try:
            linear.name = bpy.data.colorspace.working_space
        except TypeError:
            pass
    scene.render.filepath = path
    images = {}
    try:
        bpy.ops.render.render(write_still=True)
        source = oiio.ImageInput.open(path)
        if source is None:
            raise RequestError("internal", f"the rendered frame could not be read: {oiio.geterror()}")
        try:
            part = 0
            while source.seek_subimage(part, 0):
                spec = source.spec()
                pixels = np.asarray(source.read_image(part, 0, 0, spec.nchannels, "float"), dtype=np.float32)
                pixels = pixels.reshape(spec.height, spec.width, spec.nchannels)
                # Channels are named "<layer>.<pass>.<channel>" in a multilayer file and plainly
                # "R", "G", "B", "A" otherwise. Collect them per pass, in the file's order.
                groups = {}
                for index, channel in enumerate(spec.channelnames):
                    pieces = channel.split(".")
                    name = pieces[-2] if len(pieces) >= 2 else "Combined"
                    groups.setdefault(name, []).append(index)
                for name, indices in groups.items():
                    # The file's rows run top-down; this module works bottom-up like Cycles.
                    images[name] = pixels[::-1, :, indices]
                part += 1
        finally:
            source.close()
    finally:
        shutil.rmtree(directory, ignore_errors=True)
    if "Combined" not in images:
        raise RequestError("internal", "the rendered file holds no combined image")
    return images


def _rgba(image):
    """Any pass as four channels: a single value becomes grey, a missing alpha becomes 1."""
    height, width, channels = image.shape
    if channels >= 4:
        return image[:, :, :4]
    out = np.ones((height, width, 4), dtype=np.float32)
    if channels == 1:
        out[:, :, :3] = image
    else:
        out[:, :, :channels] = image
    return out


def _full_frame(image, scene, width, height):
    """A region render may deliver only the region; put it into a black frame of the full size."""
    if image.shape[0] == height and image.shape[1] == width:
        return image
    render = scene.render
    x0 = int(round(render.border_min_x * width))
    y0 = int(round(render.border_min_y * height))
    full = np.zeros((height, width, image.shape[2]), dtype=np.float32)
    h = min(image.shape[0], height - y0)
    w = min(image.shape[1], width - x0)
    full[y0:y0 + h, x0:x0 + w] = image[:h, :w]
    return full


def render(header):
    """Handle a `render` request. Returns (reply fields, blobs)."""
    scene = bpy.context.scene
    if scene.camera is None:
        raise RequestError("bad_request", "the scene has no active camera")

    width, height = int(header["width"]), int(header["height"])
    scene.render.resolution_x = width
    scene.render.resolution_y = height
    scene.render.resolution_percentage = 100
    # Width of a pixel over its height. Blender keeps it as two numbers of at least 1 each.
    aspect = float(header.get("pixel_aspect", 1.0))
    if aspect <= 0.0:
        raise RequestError("bad_request", "pixel_aspect must be positive")
    scene.render.pixel_aspect_x = aspect if aspect >= 1.0 else 1.0
    scene.render.pixel_aspect_y = 1.0 if aspect >= 1.0 else 1.0 / aspect
    # Across the frames of a sequence Cycles keeps what did not change (acceleration structure,
    # loaded textures) instead of building it again for every frame.
    scene.render.use_persistent_data = True
    # Motion blur: the plugin gives where things are when the shutter opens and closes, and the
    # scene builders key that one frame before and after the current frame (scene/common.py).
    scene.render.motion_blur_shutter = MOTION_SHUTTER_FRAMES
    scene.render.motion_blur_position = 'CENTER'

    # The region, as fractions of the image measured from its lower left corner.
    region = header.get("region")
    scene.render.use_border = region is not None
    scene.render.use_crop_to_border = False
    if region is not None:
        scene.render.border_min_x, scene.render.border_min_y = region[0], region[1]
        scene.render.border_max_x, scene.render.border_max_y = region[2], region[3]

    wanted = list(header.get("passes", ()))
    started = time.perf_counter()
    if scene.render.engine == 'CYCLES':
        images = _render_cycles(wanted)
    else:
        images = _render_through_file(scene, wanted)
    seconds = time.perf_counter() - started

    # Blender's rows run bottom-up; the protocol sends the top row first.
    def blob(image):
        return np.ascontiguousarray(_full_frame(_rgba(image), scene, width, height)[::-1], dtype=np.float32)

    blobs = [blob(images["Combined"])]
    delivered, warnings = [], []
    for name in wanted:
        if name in images:
            delivered.append(name)
            blobs.append(blob(images[name]))
        else:
            warnings.append(f"The render pass \"{name}\" is not available in this render engine.")
    return {
        "width": width,
        "height": height,
        "channels": 4,
        "pixels": 0,
        "passes": delivered,
        "warnings": warnings,
        "seconds": seconds,
        "engine": scene.render.engine,
    }, blobs
