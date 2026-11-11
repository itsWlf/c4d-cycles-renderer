# SPDX-License-Identifier: GPL-3.0-or-later
"""The viewport driver: live rendering for the IPR window.

Blender's window is turned into one borderless 3D viewport in Rendered mode, hidden from the
user. Blender then does what it does for its own users: it restarts the render the moment the
scene changes and refines it sample by sample. After every redraw the driver copies the
viewport's float pixels into the shared-memory frame block (frame_block.py), where the plugin
picks them up. Mechanisms and their measurements: docs/reference/blender.md.
"""

import ctypes
import math
import re
import time
import traceback

import bpy
import gpu
from mathutils import Matrix

from ..errors import RequestError
from . import window
from .frame_block import DONE, PAUSED, RENDERING, STOPPED, FrameBlock

_SAMPLE = re.compile(r"Sample (\d+)(?:/(\d+))?")
_STATUS_OFFSET = 64   # RenderEngine.text inside Blender's struct; found on 5.0 and 5.2.3
_CAMERA_ZOOM_FILLING_THE_REGION = 50.0 * (2.0 - math.sqrt(2.0))
# After a change Cycles keeps saying "Rendering Done" about the previous render until its
# render thread has picked the change up (a resize alone is held back 0.2 s). So "done" is
# believed only when this much time has passed since the last change.
_SETTLE_SECONDS = 0.6

# After the region has taken a new size the view is set once more, this much later; see
# Viewport._after_resize.
_RESIZE_NUDGE_SECONDS = 0.25


def free_view_lens(lens, sensor_width, width, height):
    """The focal length to give the viewport's own view so that it frames like a camera with
    this lens whose sensor width spans the image width.

    Blender's free view behaves like a 36 mm sensor fitted to the larger side of the region,
    seen through half its nominal focal length (source: BKE_camera_params_from_view3d, zoom 2).
    """
    fit = 1.0 if width >= height else width / height
    return 2.0 * lens * 36.0 / sensor_width * fit


# Blender accepts 1 to 250 mm for the viewport's focal length.
FREE_VIEW_LENS_RANGE = (1.0, 250.0)


class Viewport:
    def __init__(self, token):
        self.block = FrameBlock(token)
        self.window = self.area = self.region = self.space = None
        self.hwnd = None
        self.ready = False
        self.rendering = False
        self.paused = False
        self._timer = None
        self._framebuffer = None
        self._gpu_buffers = (0, None)      # (generation of the frame block, its two gpu Buffers)
        self._cycles_status = ""
        self._settle_until = 0.0           # "done" is not believed before this time
        self._done_since = 0.0             # when the progress text first said "done"
        self._frame_size = (0, 0)
        self._last_draw = 0.0
        self._finished_times = None        # start and end of the last render that ran to its end
        self._free_view = False            # looking through the viewport's own view, not the camera object
        self._settle_timer = False
        self._starting = False             # view_start was received, rendering has not begun yet
        self._start_size = (0, 0)
        self._resize_size = None           # the size a resize is under way to
        self._resize_deadline = 0.0
        self._resize_reached = 0.0         # when the region got that size
        self._resize_timer = False
        self._nudge = 1
        self._start_deadline = 0.0
        self._redraws = 0                  # Eevee: redraws since the last change
        self._last_sample = 0
        self._start_time = 0.0
        self._end_time = 0.0
        self.error = None                  # traceback of a failed draw callback, sent with the next reply

    # --- set-up: two steps, because the screen is swapped between them -----------------

    def setup_begin(self):
        preferences = bpy.context.preferences.view
        preferences.show_splash = False
        preferences.use_save_prompt = False
        preferences.render_display_type = 'NONE'
        self.window = bpy.context.window_manager.windows[0]
        area, region = self._find_view3d()
        with bpy.context.temp_override(window=self.window, area=area, region=region):
            bpy.ops.screen.screen_full_area(use_hide_panels=True)

    def setup_finish(self, show_window):
        self._refresh_area()
        self.hwnd = window.find()
        window.show(self.hwnd, show_window)
        window.lower_process_priority()
        self.space.overlay.show_overlays = False
        self.space.show_gizmo = False
        self.space.shading.type = 'SOLID'
        self.space.shading.use_scene_lights_render = True
        self.space.shading.use_scene_world_render = True
        bpy.context.scene.render.preview_pixel_size = '1'
        self._wrap_cycles()
        bpy.types.SpaceView3D.draw_handler_add(self._pre_view, (), 'WINDOW', 'PRE_VIEW')
        bpy.types.SpaceView3D.draw_handler_add(self._post_view, (), 'WINDOW', 'POST_VIEW')
        self.ready = True

    def _find_view3d(self):
        for area in self.window.screen.areas:
            if area.type == 'VIEW_3D':
                for region in area.regions:
                    if region.type == 'WINDOW':
                        return area, region
        raise RuntimeError("Blender's window has no 3D viewport")

    def _refresh_area(self):
        self.area, self.region = self._find_view3d()
        self.space = self.area.spaces.active

    def _override(self):
        return bpy.context.temp_override(window=self.window, area=self.area, region=self.region)

    # --- commands ---------------------------------------------------------------------

    def start(self, width, height):
        self.resize(width, height)
        if not self._free_view:
            self._look_through_camera()
        if self.paused:
            self.pause(False)
        # Rendering begins once the window has its new size. Starting at once would render the
        # old size first and start over a moment later.
        self._start_deadline = time.time() + 1.0
        self._start_size = (width, height)
        if not self._starting:
            self._starting = True
            bpy.app.timers.register(self._begin_rendering, first_interval=0.0)
        return self.describe()

    def _begin_rendering(self):
        if not self._starting:
            return None     # stopped again in the meantime
        if (self.region.width, self.region.height) != self._start_size and time.time() < self._start_deadline:
            return 0.005
        self._starting = False
        with self._override():
            self.space.shading.type = 'RENDERED'
        self._fast_loop(True)
        self.rendering = True
        self.touch()
        return None

    def stop(self):
        """Leave Rendered mode, which frees the render session and its memory."""
        self._starting = False
        if self.paused:
            self.pause(False)
        with self._override():
            self.space.shading.type = 'SOLID'
        self._fast_loop(False)
        self.rendering = False
        self.block.set_progress(STOPPED, 0, 0, "")
        return self.describe()

    def pause(self, paused):
        paused = bool(paused)
        with self._override():   # the property's update callback needs an area
            bpy.context.scene.cycles.preview_pause = paused
        now = time.time()
        if paused and not self.paused:
            self._end_time = self._end_time or now
            self.block.set_progress(PAUSED, self._last_sample, self._samples_wanted(), "Rendering Paused")
        elif not paused and self.paused:
            # The clock leaves out the time spent paused.
            if self._end_time:
                self._start_time += now - self._end_time
            self._end_time = 0.0
        self.paused = paused
        self.block.set_times(self._start_time, self._end_time)
        self._fast_loop(self.rendering and not paused)
        self.area.tag_redraw()
        return self.describe()

    def resize(self, width, height):
        if width < 16 or height < 16 or width > 16384 or height > 16384:
            raise RequestError("bad_request", f"unusable viewport size {width} x {height}")
        window.set_client_size(self.hwnd, width, height)
        # The camera frame fills the region when the render aspect equals the region's.
        render = bpy.context.scene.render
        render.resolution_x, render.resolution_y, render.resolution_percentage = width, height, 100
        self.touch()
        if self.rendering:
            self._resize_size = (width, height)
            self._resize_deadline = time.time() + 1.0
            self._resize_reached = 0.0
            if not self._resize_timer:
                self._resize_timer = True
                bpy.app.timers.register(self._after_resize, first_interval=0.005)
        return self.describe()

    def _after_resize(self):
        """Cycles takes a new region size a moment after the region has it. A view that was set in
        between can end up drawn around the centre of the old size: the right picture, shifted by
        half the difference (seen in 2 of 16 resizes followed at once by `view_camera`). Setting
        the view again once the size has arrived makes Cycles lay it out afresh."""
        if not self.rendering or self._resize_size is None:
            self._resize_timer = False
            return None
        now = time.time()
        if not self._resize_reached:
            if (self.region.width, self.region.height) != self._resize_size and now < self._resize_deadline:
                return 0.005
            self._resize_reached = now
        if now - self._resize_reached < _RESIZE_NUDGE_SECONDS:
            return _RESIZE_NUDGE_SECONDS - (now - self._resize_reached) + 0.005
        self._resize_timer = False
        self._resize_size = None
        # A change too small to see, alternating so that it never adds up.
        self._nudge = -self._nudge
        factor = 1.0 + 1e-5 * self._nudge
        if self._free_view:
            self.space.lens = self.space.lens * factor
        else:
            view = self.space.region_3d
            view.view_camera_zoom = view.view_camera_zoom * factor
        self.touch()
        return None

    def _look_through_camera(self):
        view = self.space.region_3d
        view.view_perspective = 'CAMERA'
        view.view_camera_zoom = _CAMERA_ZOOM_FILLING_THE_REGION
        view.view_camera_offset = (0.0, 0.0)

    def set_view(self, header):
        """Choose what the viewport looks through.

        {"camera": true}: the scene's camera object. Otherwise the viewport's own view, placed
        like a camera: `matrix` (a camera's world matrix), `lens`, `sensor_width`, `width` and
        `height` of the image, `clip_start`, `clip_end`. Moving this view changes no scene data,
        so Cycles need not go through the scene again; with many instances that is ten times
        faster than moving a camera object. A free view has no depth of field and no lens shift.
        """
        view = self.space.region_3d
        if header.get("camera"):
            self._free_view = False
            self._look_through_camera()
        else:
            values = header["matrix"]
            matrix = Matrix([values[0:4], values[4:8], values[8:12], values[12:16]])
            self._free_view = True
            view.view_perspective = 'PERSP'
            # The image size comes with the request: after a resize the region follows only later.
            self.space.lens = free_view_lens(header["lens"], header.get("sensor_width", 36.0),
                                             header.get("width", self.region.width), header.get("height", self.region.height))
            self.space.clip_start = header.get("clip_start", 0.01)
            self.space.clip_end = header.get("clip_end", 10000.0)
            view.view_matrix = matrix.inverted()
        self.touch()
        return self.describe()

    def set_region(self, region):
        """Render only a part of the image: [min x, min y, max x, max y] as fractions of the image
        from its lower left corner; None for all of it. The viewport's own view and the camera
        view each have their own border; the camera's frame fills the region, so both take the
        same numbers."""
        render = bpy.context.scene.render
        with self._override():
            self.space.use_render_border = region is not None
            render.use_border = region is not None
            if region is not None:
                self.space.render_border_min_x, self.space.render_border_min_y = region[0], region[1]
                self.space.render_border_max_x, self.space.render_border_max_y = region[2], region[3]
                render.border_min_x, render.border_min_y = region[0], region[1]
                render.border_max_x, render.border_max_y = region[2], region[3]
        self.touch()
        return self.describe()

    def set_pass(self, name):
        """Show one render pass. `name` is Blender's identifier for the active engine."""
        shading = self.space.shading
        try:
            with self._override():
                if bpy.context.scene.render.engine == 'CYCLES':
                    shading.cycles.render_pass = name
                    self.window.screen.update_tag()   # without it the old pass stays on screen
                else:
                    shading.render_pass = name
        except TypeError as error:
            raise RequestError("bad_request", f"the engine has no pass {name!r} ({error})") from error
        self.touch()
        return self.describe()

    def show_window(self, visible):
        window.show(self.hwnd, visible)
        return self.describe()

    def test_pattern(self, width, height):
        self.block.write_test_pattern(width, height)
        return self.describe()

    def touch(self):
        """Something changed that restarts the render: reset the clock and the sample count."""
        self._redraws = 0
        self._last_sample = 0
        if self._end_time:
            self._finished_times = (self._start_time, self._end_time)   # of the render that was complete
        self._start_time = time.time()
        self._end_time = 0.0
        self.block.set_times(self._start_time, 0.0)
        self._settle()
        if self.rendering and self.area:
            # Say so at once: the next redraw, which reports progress, may be a moment away.
            if not self.paused:
                self.block.set_progress(RENDERING, 0, self._samples_wanted(), "Updating")
            self.area.tag_redraw()

    def _settle(self):
        self._settle_until = time.time() + _SETTLE_SECONDS
        self._done_since = 0.0
        # The render may be finished again before then, and nothing would redraw to say so.
        if not self._settle_timer:
            self._settle_timer = True
            bpy.app.timers.register(self._redraw_once, first_interval=_SETTLE_SECONDS + 0.05)

    def _redraw_once(self):
        remaining = self._settle_until - time.time()
        if remaining > 0:
            return remaining + 0.05     # changes kept coming; look again when they have settled
        self._settle_timer = False
        if self.rendering and self.area:
            self.area.tag_redraw()
        return None

    def describe(self):
        fields = {"state_block": self.block.state_name, "rendering": self.rendering or self._starting, "paused": self.paused,
                  "width": self.region.width if self.region else 0, "height": self.region.height if self.region else 0}
        if self.error:
            fields["draw_error"], self.error = self.error, None
        return fields

    def _fast_loop(self, enabled):
        """A 2 ms window timer keeps Blender's loop from sleeping 5 ms between polls. It makes the
        loop spin, so it only runs while rendering."""
        manager = bpy.context.window_manager
        if enabled and self._timer is None:
            self._timer = manager.event_timer_add(0.002, window=self.window)
        elif not enabled and self._timer is not None:
            manager.event_timer_remove(self._timer)
            self._timer = None

    def _samples_wanted(self):
        scene = bpy.context.scene
        return scene.cycles.preview_samples if scene.render.engine == 'CYCLES' else scene.eevee.taa_samples

    # --- draw callbacks: read scene data, never write it ---------------------------------

    def _wrap_cycles(self):
        import cycles
        original = cycles.CyclesRender.view_draw
        viewport = self

        def view_draw(engine, context, depsgraph):
            original(engine, context, depsgraph)
            # Cycles' progress text has no Python property; it is read from the engine struct.
            raw = ctypes.string_at(engine.as_pointer() + _STATUS_OFFSET, 256).split(b"\0", 1)[0]
            readable = len(raw) < 200 and all(32 <= byte < 127 for byte in raw)
            viewport._cycles_status = raw.decode("ascii") if readable else ""

        cycles.CyclesRender.view_draw = view_draw

    def _pre_view(self):
        self._framebuffer = gpu.state.active_framebuffer_get()

    def _post_view(self):
        framebuffer = self._framebuffer
        if framebuffer is None or not self.rendering:
            return
        try:
            region = bpy.context.region
            width, height = region.width, region.height
            if width < 1 or height < 1:
                return
            if (width, height) != self._frame_size:
                self._frame_size = (width, height)
                self._settle()   # Cycles starts over shortly after a resize
            index, _array = self.block.begin(width, height)
            if self._gpu_buffers[0] != self.block.generation:
                # The gpu Buffers wrap the shared memory, so the pixels are read straight into it.
                self._gpu_buffers = (self.block.generation, tuple(
                    gpu.types.Buffer('FLOAT', (height, width, 4), array) for array in self.block.buffers))
            with framebuffer.bind():
                framebuffer.read_color(0, 0, width, height, 4, 0, 'FLOAT', data=self._gpu_buffers[1][index])
            self._update_progress()
            self.block.end(index)
        except Exception:
            self.error = traceback.format_exc()

    def _update_progress(self):
        scene = bpy.context.scene
        wanted = self._samples_wanted()
        if scene.render.engine == 'CYCLES':
            status = self._cycles_status
            match = _SAMPLE.search(status)
            done = "Done" in status
            if match:
                sample = int(match.group(1))
                wanted = int(match.group(2)) if match.group(2) else wanted
            else:
                sample = wanted if done else self._last_sample
        else:
            # Eevee reports nothing: every redraw since the last change is one sample, and it
            # stops redrawing once it has them all.
            self._redraws += 1
            sample = min(self._redraws, wanted) if wanted else self._redraws
            done = wanted > 0 and sample >= wanted
            status = "Rendering Done" if done else f"Sample {sample}/{wanted}"

        now = time.time()
        previous_draw, self._last_draw = self._last_draw, now
        if done:
            self._done_since = self._done_since or now
            if now < self._settle_until:
                done, sample, status = False, 0, "Updating"
        else:
            self._done_since = 0.0
        if sample < self._last_sample and not done:
            self._start_time, self._end_time = now, 0.0     # the render started over
        self._last_sample = sample
        if self.paused:
            state = PAUSED
        elif done:
            state = DONE
            if not self._end_time:
                # A render that ended while "done" was not yet believed ended with the redraw
                # before this one; this one only came to look.
                ended = previous_draw if previous_draw < self._settle_until else now
                if ended <= self._start_time and self._finished_times:
                    # Nothing was drawn since the change: it restarted nothing, and the finished
                    # render it found is still the one on screen.
                    self._start_time, self._end_time = self._finished_times
                else:
                    self._end_time = max(ended, self._start_time)
        else:
            state = RENDERING
            self._end_time = 0.0
        self.block.set_progress(state, sample, wanted, status)
        self.block.set_times(self._start_time, self._end_time)
