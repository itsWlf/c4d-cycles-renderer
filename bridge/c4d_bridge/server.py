# SPDX-License-Identifier: GPL-3.0-or-later
"""The bridge server: one socket thread, one queue, and all bpy work on Blender's main thread.

The socket thread only moves bytes into the queue. Requests are handled on the main thread:
in a background Blender by a blocking loop (there is no event loop to hook into), in a
windowed Blender by a timer on Blender's own loop so the viewport keeps rendering.
"""

import json
import os
import queue
import socket
import threading
import traceback

import bpy

from . import BRIDGE_VERSION, final, protocol
from .errors import RequestError
from .render import devices, settings
from .scene import Scene

CONNECT_TIMEOUT_SECONDS = 60.0

EXIT_OK = 0
EXIT_CONNECTION_LOST = 2
EXIT_NO_CLIENT = 3


class Bridge:
    """One bridge per Blender: the listening socket, the one client, and the request handlers.

    To add an operation, write a method that returns (reply fields, reply blobs), or raises
    RequestError to answer with an error, and enter it in self.handlers. Describe it in
    protocol/protocol.md in the same change.
    """

    def __init__(self, handshake_path, token):
        self.handshake_path = handshake_path
        self.token = token
        self.mode = "final" if bpy.app.background else "viewport"
        self.inbox = queue.Queue()
        self.sock = None
        self.send_lock = threading.Lock()
        self.authenticated = False
        self.exit_code = None  # set when the bridge should quit
        self.scene = Scene()
        self.handlers = {
            "hello": self.handle_hello,
            "ping": self.handle_ping,
            "shutdown": self.handle_shutdown,
            "scene": self.handle_scene,
            "settings": self.handle_settings,
            "get": self.handle_get,
            "render": self.handle_render,
            "material_preview": self.handle_material_preview,
            "edit_material": self.handle_edit_material,
            "close_material_editor": self.handle_close_material_editor,
            "view_start": lambda header, blobs: (self._viewport().start(int(header["width"]), int(header["height"])), ()),
            "view_stop": lambda header, blobs: (self._viewport().stop(), ()),
            "view_pause": lambda header, blobs: (self._viewport().pause(header.get("paused", True)), ()),
            "view_size": lambda header, blobs: (self._viewport().resize(int(header["width"]), int(header["height"])), ()),
            "view_pass": lambda header, blobs: (self._viewport().set_pass(header["pass"]), ()),
            "view_camera": lambda header, blobs: (self._viewport().set_view(header), ()),
            "view_region": lambda header, blobs: (self._viewport().set_region(header.get("region")), ()),
            "view_window": lambda header, blobs: (self._viewport().show_window(header.get("visible", True)), ()),
            "view_test_pattern": lambda header, blobs: (
                self._viewport().test_pattern(int(header["width"]), int(header["height"])), ()),
        }
        self.viewport = None  # the live viewport driver; only in a windowed Blender

    # --- socket thread --------------------------------------------------------

    def listen(self):
        """Open the listening socket and publish the port in the handshake file."""
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(CONNECT_TIMEOUT_SECONDS)
        write_handshake(self.handshake_path, {
            "protocol": protocol.VERSION,
            "port": self.listener.getsockname()[1],
            "pid": os.getpid(),
            "blender": bpy.app.version_string,
            "blender_version": list(bpy.app.version),
            "mode": self.mode,
            "bridge": BRIDGE_VERSION,
        })
        threading.Thread(target=self._socket_thread, name="c4d-bridge-socket", daemon=True).start()

    def _socket_thread(self):
        """Never touches bpy: accepts the one client and queues what it sends."""
        try:
            self.sock, _address = self.listener.accept()
        except OSError:
            self.inbox.put(("closed", EXIT_NO_CLIENT))
            return
        finally:
            self.listener.close()
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            while True:
                self.inbox.put(("message", protocol.receive_message(self.sock)))
        except (protocol.ConnectionClosed, protocol.ProtocolError, OSError):
            self.inbox.put(("closed", EXIT_CONNECTION_LOST))

    def send(self, header, blobs=()):
        with self.send_lock:
            protocol.send_message(self.sock, header, blobs)

    # --- main thread ----------------------------------------------------------

    def process(self, item):
        """Handle one queue item on the main thread."""
        kind, payload = item
        if kind == "closed":
            if self.exit_code is None:
                self.exit_code = payload
            return
        header, blobs = payload
        request_id = header.get("id")
        try:
            op = header.get("op")
            if not self.authenticated and op != "hello":
                raise RequestError("not_authenticated", "send hello first")
            handler = self.handlers.get(op)
            if handler is None:
                raise RequestError("unknown_op", f"unknown operation {op!r}")
            fields, reply_blobs = handler(header, blobs)
            self.send({"op": "reply", "id": request_id, "ok": True, **fields}, reply_blobs)
        except RequestError as error:
            self._send_error(request_id, error.code, str(error))
            if error.code == "bad_token":
                # Close only the sending side, so the client still receives the error reply.
                # The bridge quits when the client then closes the connection.
                self.sock.shutdown(socket.SHUT_WR)
        except Exception:
            self._send_error(request_id, "internal", traceback.format_exc())

    def _send_error(self, request_id, code, message):
        try:
            self.send({"op": "reply", "id": request_id, "ok": False, "error": {"code": code, "message": message}})
        except OSError:
            pass

    def run_blocking(self):
        """Background Blender: serve until told to quit. Returns the exit code."""
        while self.exit_code is None:
            self.process(self.inbox.get())
        return self.exit_code

    def run_on_timer(self, show_window=False):
        """Windowed Blender: prepare the viewport, then serve from Blender's own loop."""
        from .viewport import Viewport

        self.viewport = Viewport(self.token)

        def setup_begin():
            self.viewport.setup_begin()
            # The screen is swapped by the step above; the rest must run in a later loop pass.
            bpy.app.timers.register(setup_finish, first_interval=0.1, persistent=True)

        def setup_finish():
            self.viewport.setup_finish(show_window)
            bpy.app.timers.register(tick, first_interval=0.0, persistent=True)

        def tick():
            while self.exit_code is None:
                try:
                    self.process(self.inbox.get_nowait())
                except queue.Empty:
                    return 0.001
            if self.exit_code == EXIT_OK:
                bpy.ops.wm.quit_blender()
            else:
                os._exit(self.exit_code)  # nothing worth saving; report why Blender is going away
            return None

        bpy.app.timers.register(setup_begin, first_interval=0.0, persistent=True)

    def _viewport(self):
        if self.viewport is None or not self.viewport.ready:
            raise RequestError("bad_request", "this operation needs a windowed Blender (viewport mode)")
        return self.viewport

    def _scene_changed(self):
        if self.viewport is not None and self.viewport.ready:
            self.viewport.touch()

    # --- handlers: return (reply fields, reply blobs) -------------------------

    def handle_hello(self, header, blobs):
        if header.get("protocol") != protocol.VERSION:
            raise RequestError("bad_request", f"this bridge speaks protocol {protocol.VERSION}")
        if header.get("token") != self.token:
            raise RequestError("bad_token", "wrong token")
        self.authenticated = True
        return {
            "protocol": protocol.VERSION,
            "blender": bpy.app.version_string,
            "blender_version": list(bpy.app.version),
            "mode": self.mode,
            "bridge": BRIDGE_VERSION,
            "cycles_devices": devices.cycles_devices(),
        }, ()

    def handle_ping(self, header, blobs):
        fields = {key: value for key, value in header.items() if key not in ("op", "id")}
        return fields, blobs

    def handle_shutdown(self, header, blobs):
        self.exit_code = EXIT_OK
        return {}, ()

    def handle_scene(self, header, blobs):
        count, warnings = self.scene.apply(header, blobs)
        self._scene_changed()
        return {"count": count, "warnings": warnings}, ()

    def handle_settings(self, header, blobs):
        reply = settings.apply(header)
        # A property set from Python notifies nobody; the tag makes a live viewport pick it up.
        bpy.context.scene.update_tag()
        self._scene_changed()
        return reply

    def handle_get(self, header, blobs):
        return settings.read(header)

    def handle_edit_material(self, header, blobs):
        from .scene import native
        self._viewport()    # a window needs a windowed Blender
        return native.edit(header)

    def handle_close_material_editor(self, header, blobs):
        from .scene import native
        return native.close(header)

    def handle_render(self, header, blobs):
        return final.render(header)

    def handle_material_preview(self, header, blobs):
        from .final import preview
        return preview.render(self.scene, header)


def write_handshake(path, content):
    """Write the handshake file so that a reader never sees it half-written."""
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(content, handle)
    os.replace(temporary, path)
