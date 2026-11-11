# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the picture a Cycles Native Material shows before it has been opened in Blender.

    python tools\\make_native_placeholder.py [path to blender.exe]

It is Blender's own preview of its default material (the un-edited node setup: a Principled
BSDF wired into the Material Output), in the format the bridge writes previews in
(protocol.md, "Native materials"):
    plugin/cyclesrender/res/native_default.preview
Blender's window shows for a moment: its preview job needs one. Run it again for a new Blender
version if the default material's look changed.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(HERE, "..", "plugin", "cyclesrender", "res", "native_default.preview")
BLENDER = r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"


def in_blender(output):
    import time

    import bpy
    import numpy as np

    state = {"material": None, "asked": 0.0}

    def pixels(material):
        preview = material.preview
        if preview is None or preview.image_size[0] == 0:
            return None
        data = np.empty(preview.image_size[0] * preview.image_size[1] * 4, dtype=np.float32)
        preview.image_pixels_float.foreach_get(data)
        return data.reshape(preview.image_size[1], preview.image_size[0], 4)

    def tick():
        if state["material"] is None:
            bpy.context.scene.render.engine = "CYCLES"
            material = bpy.data.materials.new("Material")
            material.use_nodes = True
            state["material"] = material
            state["asked"] = time.monotonic()
            with bpy.context.temp_override(id=material):
                bpy.ops.ed.lib_id_generate_preview()
            return 0.1
        image = pixels(state["material"])
        if image is None or not image[..., 3].any():
            if time.monotonic() - state["asked"] > 60.0:
                print("PLACEHOLDER failed: no preview after 60 s")
                bpy.ops.wm.quit_blender()
                return None
            return 0.1
        alpha = image[..., 3]
        rows = np.where((alpha > 0.5).any(axis=1))[0]
        columns = np.where((alpha > 0.5).any(axis=0))[0]
        image = image[rows[0]:rows[-1] + 1, columns[0]:columns[-1] + 1]
        image = np.clip(image[::-1, :, :3] * image[::-1, :, 3:4], 0.0, 1.0)
        with open(output, "wb") as file:
            file.write(np.array([image.shape[1], image.shape[0]], dtype="<i4").tobytes())
            file.write((image * 255.0 + 0.5).astype(np.uint8).tobytes())
        print("PLACEHOLDER", image.shape[1], image.shape[0])
        bpy.ops.wm.quit_blender()
        return None

    bpy.app.timers.register(tick, first_interval=1.0)


if "bpy" in sys.modules or os.path.basename(sys.argv[0]).lower().startswith("blender"):
    in_blender(sys.argv[sys.argv.index("--") + 1])
elif __name__ == "__main__":
    blender = sys.argv[1] if len(sys.argv) > 1 else BLENDER
    done = subprocess.run([blender, "--factory-startup", "--no-window-focus", "--python", os.path.abspath(__file__), "--", os.path.abspath(OUTPUT)],
                          capture_output=True, text=True, timeout=180)
    print([line for line in done.stdout.splitlines() if line.startswith("PLACEHOLDER")] or done.stdout[-800:])
