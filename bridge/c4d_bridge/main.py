# SPDX-License-Identifier: GPL-3.0-or-later
"""Entry point of the bridge. Blender runs this file with --python.

  blender -b --factory-startup --python bridge/c4d_bridge/main.py -- --handshake <file> --token <text>
  blender    --factory-startup --python bridge/c4d_bridge/main.py -- --handshake <file> --token <text>

In a background Blender the script serves requests until it is told to quit and Blender then
exits with the bridge's exit code. In a windowed Blender it installs itself on Blender's
loop and returns.
"""

import argparse
import os
import sys

import bpy

# This file is run as a script, so make the package it belongs to importable.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from c4d_bridge import SUPPORTED_BLENDER  # noqa: E402
from c4d_bridge import server  # noqa: E402

EXIT_REFUSED = 4


def parse_arguments():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(prog="c4d_bridge")
    parser.add_argument("--handshake", required=True, help="file the bridge writes its port to")
    parser.add_argument("--token", required=True, help="secret the client must present")
    parser.add_argument("--show-window", action="store_true", help="keep Blender's window visible")
    return parser.parse_args(argv)


def main():
    arguments = parse_arguments()

    if tuple(bpy.app.version[:2]) not in SUPPORTED_BLENDER:
        server.write_handshake(arguments.handshake, {
            "error": f"Blender {bpy.app.version_string} is not supported by this bridge.",
        })
        sys.exit(EXIT_REFUSED)

    bridge = server.Bridge(arguments.handshake, arguments.token)
    bridge.listen()

    if bpy.app.background:
        sys.exit(bridge.run_blocking())

    from c4d_bridge.viewport import window
    if not arguments.show_window:
        window.show(window.find(), False)  # hidden at once; it keeps rendering while hidden
    bridge.run_on_timer(arguments.show_window)


main()
