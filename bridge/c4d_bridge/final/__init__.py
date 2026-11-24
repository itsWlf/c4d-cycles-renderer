# SPDX-License-Identifier: GPL-3.0-or-later
"""Final driver: renders one frame with a normal blocking render and returns its pixels.

The engine, samples, passes and every other render setting were set before by a `settings`
request; a `render` request carries the resolution and pixel aspect, the names of the passes
wanted back, optionally the region to render and a file Blender writes itself from the same
render (files.py).
"""

import os
import shutil
import tempfile
import time

import bpy
import numpy as np

from ..errors import RequestError
from ..scene.common import MOTION_SHUTTER_FRAMES
from . import files

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


def _render_cycles(wanted, scene=None, write_still=False):
    """Returns {pass name: array}, rows bottom-up; "Combined" is always there.
    Renders the current scene, or `scene` when one is given. With `write_still` Blender also
    writes the picture to the scene's file path (the composite, see _render_composited)."""
    _install_cycles_readback()
    _captured.clear()
    _wanted[:] = wanted
    if scene is None:
        bpy.ops.render.render(write_still=write_still)
    else:
        bpy.ops.render.render(scene=scene.name, write_still=write_still)
    images = dict(_captured)
    _captured.clear()
    if "Combined" not in images:
        raise RequestError("internal", "the render finished without delivering pixels")
    return images


# What the picture is called in the images of this module as the engine made it, when "Combined"
# is the compositor's result.
UNCOMPOSITED = "Uncomposited"


def _file_settings(scene, directory, multilayer):
    """Make the scene write an uncompressed float EXR into `directory`; returns the file's path."""
    path = os.path.join(directory, "frame.exr")
    settings = scene.render.image_settings
    if multilayer:
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
    return path


def _read_file(path):
    """The passes of a rendered EXR as {pass name: array}, rows bottom-up. In a file with every
    pass the compositor's result is the layer "Composite": it is returned as "Combined", and the
    view layer's own picture as UNCOMPOSITED."""
    import OpenImageIO as oiio

    images, composite = {}, None
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
                if len(pieces) >= 3 and pieces[0] == "Composite":
                    name = "Composite"
                groups.setdefault(name, []).append(index)
            for name, indices in groups.items():
                # The file's rows run top-down; this module works bottom-up like Cycles.
                if name == "Composite":
                    composite = pixels[::-1, :, indices]
                else:
                    images[name] = pixels[::-1, :, indices]
            part += 1
    finally:
        source.close()
    if composite is not None:
        if "Combined" in images:
            images[UNCOMPOSITED] = images["Combined"]
        images["Combined"] = composite
    return images


def _render_through_file(scene, wanted):
    """Eevee offers no in-memory access to a final frame: render to an uncompressed float EXR
    in a temporary folder and read it back. Returns {pass name: array}, rows bottom-up."""
    directory = tempfile.mkdtemp(prefix="c4d_bridge_frame_")
    try:
        path = _file_settings(scene, directory, bool(wanted))
        bpy.ops.render.render(write_still=True)
        images = _read_file(path)
    finally:
        shutil.rmtree(directory, ignore_errors=True)
    if "Combined" not in images:
        raise RequestError("internal", "the rendered file holds no combined image")
    return images


def _render_composited(scene, wanted):
    """Cycles with a compositor tree: the passes are read in memory as always, and they are the
    render before the compositor. The composite exists only in Blender's render result, which
    Python cannot read; Blender writes it to a file and that is read back (what it costs:
    docs/reference/blender.md, "The compositor")."""
    directory = tempfile.mkdtemp(prefix="c4d_bridge_frame_")
    try:
        path = _file_settings(scene, directory, False)
        images = _render_cycles(wanted, write_still=True)
        composite = _read_file(path).get("Combined")
    finally:
        shutil.rmtree(directory, ignore_errors=True)
    if composite is None:
        raise RequestError("internal", "the composited frame could not be read")
    images[UNCOMPOSITED] = images["Combined"]
    images["Combined"] = composite
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
    # With a compositor tree (scene/compositor.py) the picture is the composite; the passes stay
    # as the engine rendered them.
    composited = scene.compositing_node_group is not None and scene.render.use_compositing
    # The file Blender writes itself, from this render (files.py); a tree it attaches only passes
    # the render on, so it is no composite.
    file_output = files.FileOutput(scene, header["file"]) if header.get("file") else None
    written = None
    started = time.perf_counter()
    try:
        if file_output is not None:
            file_output.attach()
        if scene.render.engine != 'CYCLES':
            images = _render_through_file(scene, wanted)
        elif composited:
            images = _render_composited(scene, [name for name in wanted if name != "Combined"])
        else:
            images = _render_cycles(wanted)
        if file_output is not None:
            written = file_output.finish()
    finally:
        if file_output is not None:
            file_output.detach()
    seconds = time.perf_counter() - started
    # Asked for as a pass, "Combined" is the picture before the compositor.
    uncomposited = images.get(UNCOMPOSITED, images["Combined"])

    # Blender's rows run bottom-up; the protocol sends the top row first.
    def blob(image):
        return np.ascontiguousarray(_full_frame(_rgba(image), scene, width, height)[::-1], dtype=np.float32)

    blobs = [blob(images["Combined"])]
    delivered, warnings = [], []
    for name in wanted:
        if name == "Combined":
            delivered.append(name)
            blobs.append(blob(uncomposited))
        elif name in images:
            delivered.append(name)
            blobs.append(blob(images[name]))
        else:
            warnings.append(f"The render pass \"{name}\" is not available in this render engine.")
    reply_file = {}
    if file_output is not None:
        warnings.extend(file_output.warnings)
        if written:
            reply_file = {"file": written, "file_passes": file_output.written}
    return {
        **reply_file,
        "width": width,
        "height": height,
        "channels": 4,
        "pixels": 0,
        "passes": delivered,
        "warnings": warnings,
        "seconds": seconds,
        "engine": scene.render.engine,
        "composited": composited,
    }, blobs
