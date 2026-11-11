# SPDX-License-Identifier: GPL-3.0-or-later
"""Native materials: Blender materials the plugin stores as they are and the user edits here.

A "Cycles Native Material" of Cinema 4D holds a Blender material as bytes: a .blend file that
contains only that material (protocol.md, "Native materials"). Nothing is translated.

  * A `material` item with `"native": true` brings the bytes; build() loads the material.
  * `edit_material` opens a window of this Blender that is nothing but a Shader Editor on the
    material. Every change is written back, as the same kind of bytes, into the folder the
    plugin named, together with a small preview picture; the plugin reads them from there.
    Blender has no way to send a message the plugin did not ask for, and needs none: the files
    are replaced whole (os.replace), and a state file counts their versions.

While a material is open in an editor window, that window is the truth: bytes the plugin sends
for it are ignored (they are what this module wrote a moment ago, or older), and clearing the
scene leaves the material and what it uses alone.

Undo in an editor window (Ctrl+Z) is Blender's global undo: it reads the changed datablocks
anew, so every reference held here goes stale. The entries therefore find their datablocks
again after each undo and redo (_after_undo), by the identifier Blender keeps across both. And
because nothing made from Python is an undo step, an undo step is pushed when an editor opens;
undoing past it would take the material itself away, so the timer steps forward again
(_tick_entry).
"""

import hashlib
import os
import sys
import tempfile
import time
import traceback

import bpy
import numpy as np

from ..errors import RequestError

SAVE_AFTER_SECONDS = 0.25       # a change is written once the material has been left alone this long
CHECK_SECONDS = 0.5             # how often an open material is compared with what was saved (see _fingerprint)
PREVIEW_TIMEOUT_SECONDS = 30.0
EMPTY_PREVIEW_SECONDS = 3.0     # how long a picture without alpha must stay to count as the finished one
_TICK_SECONDS = 0.1
MAX_REDO_TRIES = 16             # steps forward after an undo that took an open material away


class _Entry:
    def __init__(self, item_id, material, digest):
        self.item_id = item_id
        self.material = material
        self.digest = digest            # of the bytes the material was loaded from or last saved as
        self.window = None              # the editor window while one is open
        self.scene = None               # the editor's own scene and the object that carries the material
        self.holder = None
        self.folder = None              # where changes are written; None until the material is edited
        self.dirty_since = None
        self.versions = {"blend": 0, "preview": 0, "closed": 0}
        self.preview_asked = None       # when a preview was asked for and has not arrived
        self.preview_before = None
        self.test_change = None
        self.fingerprint = None         # of the material as last saved
        self.checked_at = 0.0
        self.opened_at = None           # for the picture a test may ask for (CYCLESRENDER_EDITOR_SHOT)
        self.last_error = None          # the last failure of the timer for this material, to report it once
        self.uids = {}                  # session_uid of material, scene and holder: to find them after an undo
        self.redo_tries = 0             # steps forward taken since an undo took the material away


_entries = {}
_installed = False


# ------------------------------------------------------------------------------------------------
# Bytes <-> material
def _load(data, name):
    """The material in the bytes, or None."""
    if not data:
        return None
    handle, path = tempfile.mkstemp(suffix=".blend", prefix="cyclesrender_native_")
    try:
        with os.fdopen(handle, "wb") as file:
            file.write(data)
        with bpy.data.libraries.load(path, link=False) as (source, target):
            target.materials = list(source.materials)[:1]
        material = target.materials[0] if target.materials else None
    except (OSError, RuntimeError):
        material = None
    finally:
        try:
            os.remove(path)
        except OSError:
            pass
    if material is not None:
        material.name = name
        material.use_fake_user = False
    return material


def _new(name):
    """What a native material is before it has been edited: Blender's default material."""
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    return material


def _bytes(material):
    handle, path = tempfile.mkstemp(suffix=".blend", prefix="cyclesrender_native_")
    os.close(handle)
    try:
        # Files a node names (images) by their full path: the bytes are read back by other
        # Blenders, from other folders.
        bpy.data.libraries.write(path, {material}, fake_user=True, compress=True, path_remap="ABSOLUTE")
        with open(path, "rb") as file:
            return file.read()
    finally:
        for leftover in (path, path + "1"):
            try:
                os.remove(leftover)
            except OSError:
                pass


def _remember(entry):
    """Note the identifiers of the entry's datablocks; they stay the same across undo and redo."""
    entry.uids = {key: data.session_uid for key, data in (("material", entry.material), ("scene", entry.scene), ("holder", entry.holder))
                  if data is not None}


def _by_uid(collection, uid):
    return next((data for data in collection if data.session_uid == uid), None) if uid is not None else None


def _after_undo(*_args):
    """After an undo or redo: Blender has read the changed datablocks anew. Find them again, and
    save what the material is now."""
    for entry in _entries.values():
        if not entry.uids:
            continue
        entry.material = _by_uid(bpy.data.materials, entry.uids.get("material"))
        entry.scene = _by_uid(bpy.data.scenes, entry.uids.get("scene"))
        entry.holder = _by_uid(bpy.data.objects, entry.uids.get("holder"))
        if entry.material is not None and entry.folder is not None:
            entry.redo_tries = 0
            entry.dirty_since = time.monotonic() - SAVE_AFTER_SECONDS


def _alive(entry):
    """False once the material was removed behind this module's back."""
    try:
        return entry.material is not None and entry.material.name in bpy.data.materials and \
            bpy.data.materials[entry.material.name] == entry.material
    except ReferenceError:
        return False


# ------------------------------------------------------------------------------------------------
# The scene builder's side
def build(scene, item, blobs):
    """A `material` item with "native": the material of the item's bytes."""
    item_id = item["id"]
    name = item.get("name") or item_id
    index = item.get("blend")
    data = bytes(blobs[index]) if index is not None else b""
    digest = hashlib.sha1(data).hexdigest()
    entry = _entries.get(item_id)
    if entry is not None and not _alive(entry):
        entry = None
    if entry is not None and (entry.window is not None or entry.digest == digest):
        # Open in an editor (the editor is ahead of what the plugin has), or unchanged.
        scene.materials[item_id] = entry.material
        return
    material = _load(data, name)
    if material is None:
        if data:
            scene.warn(f"Material {name!r}: its stored Blender material could not be read; a default material is used.")
        material = _new(name)
    if entry is not None:
        entry.material.user_remap(material)
        bpy.data.materials.remove(entry.material)
        entry.material = material
        entry.digest = digest
    else:
        _entries[item_id] = _Entry(item_id, material, digest)
    scene.materials[item_id] = material


def protected():
    """Datablocks that clearing the scene must leave alone: what open editors show."""
    keep = set()
    for entry in _entries.values():
        if entry.window is None or not _alive(entry):
            continue
        keep.add(entry.material)
        if entry.holder is not None:
            keep.add(entry.holder)
            keep.add(entry.holder.data)
        trees = [entry.material.node_tree] if entry.material.node_tree else []
        while trees:
            tree = trees.pop()
            for node in tree.nodes:
                image = getattr(node, "image", None)
                if image is not None:
                    keep.add(image)
                group = getattr(node, "node_tree", None)
                if group is not None and group not in keep:
                    keep.add(group)
                    trees.append(group)
    return keep


def after_clear():
    """The scene was cleared: only what protected() named is still there."""
    for item_id in [item_id for item_id, entry in _entries.items() if entry.window is None]:
        del _entries[item_id]


def forget(item_id):
    """The plugin removed the item; an open editor keeps its material."""
    entry = _entries.get(item_id)
    if entry is not None and entry.window is None:
        del _entries[item_id]


# ------------------------------------------------------------------------------------------------
# The editor window
def _window_handles():
    if sys.platform != "win32":
        return []
    import ctypes
    user32 = ctypes.windll.user32
    pid = os.getpid()
    found = []
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def callback(hwnd, _lparam):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(ctypes.c_void_p(hwnd), name, 64)
        if owner.value == pid and name.value == "GHOST_WindowClass":
            found.append(hwnd)
        return True

    user32.EnumWindows(callback_type(callback), 0)
    return found


def _place_window(hwnd, title):
    """Give the new window a size of its own, its title, and bring it to the front."""
    if not hwnd or sys.platform != "win32":
        return
    import ctypes
    user32 = ctypes.windll.user32
    user32.SetWindowPos.argtypes = [ctypes.c_void_p, ctypes.c_void_p] + [ctypes.c_int] * 4 + [ctypes.c_uint]
    user32.ShowWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user32.SetForegroundWindow.argtypes = [ctypes.c_void_p]
    user32.SetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
    user32.ShowWindow(hwnd, 1)                              # SW_SHOWNORMAL
    user32.SetWindowPos(hwnd, None, 0, 0, 1200, 760, 0x0002 | 0x0004)   # no move, keep the z order
    user32.SetWindowTextW(hwnd, title)
    user32.SetForegroundWindow(hwnd)


def _open_window(entry, title):
    wm = bpy.context.window_manager
    main = wm.windows[0]
    scene = bpy.data.scenes.new("Material Editor")
    # The editor's previews (and nothing else) render in this scene: Cycles needs no shaders compiled.
    scene.render.engine = "CYCLES"
    mesh = bpy.data.meshes.new("Material Editor")
    holder = bpy.data.objects.new("Material Editor", mesh)
    scene.collection.objects.link(holder)
    mesh.materials.append(entry.material)

    before = set(_window_handles())
    # A second window of Blender's: closing it ends nothing while the main window (hidden,
    # rendering) exists. Window > New Window and New Main Window both refuse while the window they
    # start from shows one maximised area, as the IPR's does; "Duplicate Area into New Window"
    # does not mind, and gives a window with exactly one area. That area is turned into the
    # Shader Editor before Blender draws it once, so no second viewport render starts.
    count = len(wm.windows)
    source = max(main.screen.areas, key=lambda a: a.width * a.height)
    with bpy.context.temp_override(window=main, area=source, region=source.regions[-1]):
        bpy.ops.screen.area_dupli("INVOKE_DEFAULT")
    if len(wm.windows) <= count:
        raise RequestError("internal", "Blender did not open a window for the material editor")
    window = wm.windows[-1]
    area = max(window.screen.areas, key=lambda a: a.width * a.height)
    area.ui_type = "ShaderNodeTree"
    window.scene = scene
    window.view_layer.objects.active = holder
    space = area.spaces.active
    # The header with its menus; the sidebar stays a key press away (N).
    if hasattr(space, "show_region_header"):
        space.show_region_header = True
    if hasattr(space, "show_region_ui"):
        space.show_region_ui = False
    new = [hwnd for hwnd in _window_handles() if hwnd not in before]
    _place_window(new[0] if new else None, title)
    entry.window, entry.scene, entry.holder = window, scene, holder
    entry.opened_at = time.monotonic()
    _remember(entry)
    # The state to which the first Ctrl+Z in the window returns. Without it that would be the
    # state before the material and this window's scene were made.
    try:
        with bpy.context.temp_override(window=window, area=area):
            bpy.ops.ed.undo_push(message="Open Material Editor")
    except RuntimeError:
        pass


def _close_editor(entry):
    """The window is gone: take the editor's scene away, keep the material."""
    for data, collection in ((entry.holder, bpy.data.objects), (entry.scene, bpy.data.scenes)):
        try:
            if data is None:
                continue
            mesh = data.data if collection is bpy.data.objects else None
            collection.remove(data)
            if mesh is not None and mesh.users == 0:
                bpy.data.meshes.remove(mesh)
        except (ReferenceError, RuntimeError):
            pass
    entry.window = entry.scene = entry.holder = None


def edit(header):
    """`edit_material`: open (or bring forward) the Shader Editor window of one native material."""
    item_id = header.get("material")
    folder = header.get("folder")
    if not item_id or not folder:
        raise RequestError("bad_request", "edit_material needs `material` and `folder`")
    os.makedirs(folder, exist_ok=True)
    title = header.get("name") or item_id
    entry = _entries.get(item_id)
    if entry is not None and not _alive(entry):
        entry = None
    if entry is None:
        # Not in the scene Blender holds (the IPR is not running): from the file the plugin wrote.
        data = b""
        path = os.path.join(folder, item_id + ".blend")
        if os.path.isfile(path):
            with open(path, "rb") as file:
                data = file.read()
        material = _load(data, title) or _new(title)
        entry = _entries[item_id] = _Entry(item_id, material, hashlib.sha1(data).hexdigest())
    entry.folder = folder
    entry.test_change = header.get("test_change")
    _install()
    already = entry.window is not None and entry.window in bpy.context.window_manager.windows[:]
    if not already:
        if entry.window is not None:
            _close_editor(entry)
        entry.versions["closed"] = 0
        _open_window(entry, "Cycles Native Material - " + title)
    # The plugin gets the material as it is now at once, with a preview.
    entry.dirty_since = time.monotonic() - SAVE_AFTER_SECONDS
    return {"opened": not already, "windows": len(bpy.context.window_manager.windows)}, ()


def close(header):
    """`close_material_editor`: close the editor window of one material, or of all without `material`.
    What follows is the same as when the user closes it: a last save, then the state says "closed"."""
    wanted = header.get("material")
    windows = bpy.context.window_manager.windows[:]
    count = 0
    for entry in _entries.values():
        if entry.window is None or entry.window not in windows or (wanted and entry.item_id != wanted):
            continue
        with bpy.context.temp_override(window=entry.window):
            bpy.ops.wm.window_close()
        count += 1
    return {"closed": count}, ()


# ------------------------------------------------------------------------------------------------
# Writing changes back
def _replace(path, data):
    temporary = path + ".tmp"
    with open(temporary, "wb") as file:
        file.write(data)
    os.replace(temporary, path)


def _write_state(entry):
    text = " ".join(f"{key}={value}" for key, value in entry.versions.items())
    _replace(os.path.join(entry.folder, entry.item_id + ".state"), text.encode("ascii"))


def _describe(value, depth=0):
    """A property's value as text that changes when the value does."""
    if value is None or isinstance(value, (bool, int, float, str)):
        return repr(value)
    if isinstance(value, bpy.types.Image):
        # The file counts, not the name: choosing another file for an image is an edit.
        return f"image:{value.name}:{value.filepath}:{value.source}:{value.colorspace_settings.name}:{value.alpha_mode}"
    if isinstance(value, bpy.types.ID):
        return f"id:{type(value).__name__}:{value.name}"
    if isinstance(value, bpy.types.ColorRamp):
        return "ramp:" + value.interpolation + value.color_mode + ";".join(
            f"{element.position:.6g}:" + ",".join(f"{c:.6g}" for c in element.color) for element in value.elements)
    if isinstance(value, bpy.types.CurveMapping):
        return "curves:" + "|".join(";".join(f"{point.location[0]:.6g},{point.location[1]:.6g},{point.handle_type}" for point in curve.points)
                                    for curve in value.curves)
    if isinstance(value, bpy.types.bpy_struct):
        if depth >= 2:
            return type(value).__name__
        return type(value).__name__ + "{" + ",".join(
            f"{prop.identifier}={_describe(getattr(value, prop.identifier, None), depth + 1)}" for prop in value.bl_rna.properties
            if prop.identifier != "rna_type" and prop.type != "COLLECTION" and not isinstance(getattr(value, prop.identifier, None), bpy.types.ID)) + "}"
    try:
        return "[" + ",".join(f"{v:.6g}" if isinstance(v, float) else repr(v) for v in value) + "]"
    except TypeError:
        return repr(value)


_NODE_SKIP = {"rna_type", "inputs", "outputs", "internal_links", "dimensions", "select", "location", "location_absolute", "width", "height",
              "parent", "show_options", "show_preview", "show_texture", "hide", "name", "label", "color", "use_custom_color", "type",
              "bl_idname", "bl_label", "bl_description", "bl_icon", "bl_static_type", "bl_width_default", "bl_width_min", "bl_width_max",
              "bl_height_default", "bl_height_min", "bl_height_max", "warning_propagation", "color_tag", "width_hidden"}
_MATERIAL_SETTINGS = ("surface_render_method", "displacement_method", "use_backface_culling", "use_backface_culling_shadow", "use_transparent_shadow",
                      "use_transparency_overlap", "use_raytrace_refraction", "thickness_mode", "use_thickness_from_shadow", "max_vertex_displacement",
                      "volume_intersection_method", "pass_index", "diffuse_color", "roughness", "metallic")


def _fingerprint(material):
    """Everything about a material that renders, as one hash: every node with every setting and
    every input's value, the links, what a node's image, colour ramp or curves hold, node groups
    with their content, and the material's own settings.

    Blender's change notice (depsgraph_update_post) is what normally reports an edit, at once. This
    is the net below it: whatever the notice does not cover is still found half a second later,
    because the material no longer equals what was saved.
    """
    parts = [f"{name}={_describe(getattr(material, name, None))}" for name in _MATERIAL_SETTINGS]
    cycles = getattr(material, "cycles", None)
    if cycles is not None:
        parts.append(_describe(cycles))
    trees, seen = ([material.node_tree] if material.node_tree else []), set()
    while trees:
        tree = trees.pop()
        if tree.name in seen:
            continue
        seen.add(tree.name)
        parts.append("tree:" + tree.name)
        for node in tree.nodes:
            parts.append(f"node:{node.name}:{node.bl_idname}:{node.mute}")
            for prop in node.bl_rna.properties:
                if prop.identifier in _NODE_SKIP or prop.type == "COLLECTION":
                    continue
                value = getattr(node, prop.identifier, None)
                parts.append(f"{prop.identifier}={_describe(value)}")
                if isinstance(value, bpy.types.NodeTree):
                    trees.append(value)
            for socket in node.inputs:
                if hasattr(socket, "default_value"):
                    parts.append(f"in:{socket.identifier}={_describe(socket.default_value)}:{socket.enabled}")
            for socket in node.outputs:     # the Value and RGB nodes keep their value there
                if hasattr(socket, "default_value"):
                    parts.append(f"out:{socket.identifier}={_describe(socket.default_value)}")
        for link in tree.links:
            parts.append(f"link:{link.from_node.name}:{link.from_socket.identifier}>{link.to_node.name}:{link.to_socket.identifier}:{link.is_muted}")
    return hashlib.sha1("\n".join(parts).encode("utf-8", "replace")).hexdigest()


def _save(entry):
    data = _bytes(entry.material)
    entry.dirty_since = None
    entry.fingerprint = _fingerprint(entry.material)
    entry.checked_at = time.monotonic()
    digest = hashlib.sha1(data).hexdigest()
    if digest != entry.digest or entry.versions["blend"] == 0:
        entry.digest = digest
        _replace(os.path.join(entry.folder, entry.item_id + ".blend"), data)
        entry.versions["blend"] += 1
        _write_state(entry)
    _ask_preview(entry)


def _preview_pixels(material):
    preview = material.preview
    if preview is None or preview.image_size[0] == 0:
        return None
    data = np.empty(preview.image_size[0] * preview.image_size[1] * 4, dtype=np.float32)
    preview.image_pixels_float.foreach_get(data)
    return data.reshape(preview.image_size[1], preview.image_size[0], 4)


def _ask_preview(entry):
    """Start Blender's own preview job for the material (a ball, rendered in the background)."""
    pixels = _preview_pixels(entry.material)
    entry.preview_before = None if pixels is None else pixels.tobytes()
    override = {"id": entry.material}
    if entry.window is not None:
        override["window"] = entry.window
    try:
        with bpy.context.temp_override(**override):
            bpy.ops.ed.lib_id_generate_preview()
        entry.preview_asked = time.monotonic()
    except RuntimeError:
        entry.preview_asked = None


def _take_preview(entry):
    """Write the preview once the job has delivered a new picture."""
    pixels = _preview_pixels(entry.material)
    waited = time.monotonic() - entry.preview_asked
    # A picture without any alpha is what Blender shows while the job runs. It is also the
    # finished picture of a material that shows nothing (a Transparent BSDF, a Holdout): taken
    # as that once it has stayed for a while.
    empty = pixels is None or not pixels[..., 3].any()
    if pixels is None or pixels.tobytes() == entry.preview_before or (empty and waited < EMPTY_PREVIEW_SECONDS):
        if waited > PREVIEW_TIMEOUT_SECONDS:
            entry.preview_asked = None
        return
    entry.preview_asked = None
    # Only the ball: its bounding box, over black, top row first, 8 bits per channel as displayed.
    alpha = pixels[..., 3]
    rows = np.where((alpha > 0.5).any(axis=1))[0]
    columns = np.where((alpha > 0.5).any(axis=0))[0]
    if rows.size and columns.size:
        pixels = pixels[rows[0]:rows[-1] + 1, columns[0]:columns[-1] + 1]
    image = np.clip(pixels[::-1, :, :3] * pixels[::-1, :, 3:4], 0.0, 1.0)
    data = np.array([image.shape[1], image.shape[0]], dtype="<i4").tobytes() + (image * 255.0 + 0.5).astype(np.uint8).tobytes()
    _replace(os.path.join(entry.folder, entry.item_id + ".preview"), data)
    entry.versions["preview"] += 1
    _write_state(entry)


def _on_update(_scene, depsgraph):
    trees = {entry.material.node_tree: entry for entry in _entries.values() if entry.folder and _alive(entry)}
    materials = {entry.material: entry for entry in trees.values()}
    for update in depsgraph.updates:
        original = update.id.original
        entry = materials.get(original) or trees.get(original)
        if entry is not None:
            entry.dirty_since = time.monotonic()


def _apply_test_change(entry):
    """For tests: change the material as an edit in the window would (`test_change`: a base
    colour, {"image": file} for an Image Texture wired into the base colour,
    {"image_path": file} for another file in the image that is there, {"node": type} for any
    node wired in, or {"undo": true} for Ctrl+Z). Each change is an undo step, as a hand's is."""
    colour = entry.test_change
    entry.test_change = None
    if isinstance(colour, dict) and colour.get("undo"):
        with bpy.context.temp_override(window=entry.window):
            bpy.ops.ed.undo()
        return
    _change_for_test(entry, colour)
    with bpy.context.temp_override(window=entry.window):
        bpy.ops.ed.undo_push(message="Test Change")


def _change_for_test(entry, colour):
    tree = entry.material.node_tree
    node = next((n for n in tree.nodes if n.bl_idname == "ShaderNodeBsdfPrincipled"), None)
    if isinstance(colour, dict):
        # {"image": <file>}: an Image Texture with that file, wired into the base colour, as
        # when the user adds the node and opens a file.
        if colour.get("image"):
            texture = tree.nodes.new("ShaderNodeTexImage")
            texture.image = bpy.data.images.load(colour["image"], check_existing=True)
            tree.links.new(texture.outputs["Color"], node.inputs["Base Color"])
        if colour.get("image_path"):
            # Another file for the image that is there: a change of the image, not of the node tree.
            for texture in tree.nodes:
                if getattr(texture, "image", None) is not None:
                    texture.image.filepath = colour["image_path"]
                    texture.image.reload()
        if colour.get("node"):
            # Any node, wired as a user would: a shader into the output, anything else into the
            # base colour.
            added = tree.nodes.new(colour["node"])
            source = next((socket for socket in added.outputs if socket.enabled), None)
            output = next((n for n in tree.nodes if n.bl_idname == "ShaderNodeOutputMaterial"), None)
            if source is not None and source.type == "SHADER" and output is not None:
                tree.links.new(source, output.inputs["Surface"])
            elif source is not None and node is not None:
                tree.links.new(source, node.inputs["Base Color"])
        return
    if node is None:
        node = tree.nodes.new("ShaderNodeEmission")
        output = next(n for n in tree.nodes if n.bl_idname == "ShaderNodeOutputMaterial")
        tree.links.new(node.outputs[0], output.inputs["Surface"])
        node.inputs["Color"].default_value = (*colour, 1.0)
    else:
        node.inputs["Base Color"].default_value = (*colour, 1.0)


def _tick_entry(entry, now, windows):
    """What the timer does for one material that is, or was, open in an editor window."""
    if entry.folder is None:
        return
    if not _alive(entry):
        if entry.window is not None and entry.window in windows and entry.redo_tries < MAX_REDO_TRIES:
            # Undone to before the editor opened: the material is not there. One step forward
            # again per tick, until it is (_after_undo finds it).
            entry.redo_tries += 1
            try:
                with bpy.context.temp_override(window=entry.window):
                    bpy.ops.ed.redo()
            except RuntimeError:
                entry.redo_tries = MAX_REDO_TRIES
            return
        del _entries[entry.item_id]
        return
    if entry.window is not None and entry.window not in windows:
        # Closed by the user. What was changed last is written before the plugin is told.
        _close_editor(entry)
        _save(entry)
        entry.versions["closed"] = 1
        _write_state(entry)
        return
    shot = os.environ.get("CYCLESRENDER_EDITOR_SHOT")
    if shot and entry.opened_at is not None and now - entry.opened_at > 3.0 and not entry.test_change:
        # For tests and documents: a picture of the editor window, taken by Blender itself.
        entry.opened_at = None
        try:
            with bpy.context.temp_override(window=entry.window):
                bpy.ops.screen.screenshot(filepath=shot)
        except RuntimeError:
            pass
    if entry.test_change and entry.versions["blend"] > 0 and entry.dirty_since is None:
        _apply_test_change(entry)
        entry.dirty_since = now
    if entry.dirty_since is None and entry.fingerprint is not None and now - entry.checked_at >= CHECK_SECONDS:
        # The net below Blender's change notice: does the material still equal what was saved?
        entry.checked_at = now
        if _fingerprint(entry.material) != entry.fingerprint:
            entry.dirty_since = now - SAVE_AFTER_SECONDS
    if entry.dirty_since is not None and now - entry.dirty_since >= SAVE_AFTER_SECONDS:
        _save(entry)
    if entry.preview_asked is not None:
        _take_preview(entry)


def _tick():
    """The timer behind every open material: saves what changed, fetches finished previews.

    Blender drops a timer whose function raises, and with this one every later save would be
    lost without a word. So a failure for one material is caught here: it is printed to
    Blender's console (once per distinct error), and the timer and the other materials go on.
    """
    now = time.monotonic()
    windows = bpy.context.window_manager.windows[:]
    for entry in list(_entries.values()):
        try:
            _tick_entry(entry, now, windows)
        except Exception:
            report = traceback.format_exc()
            if report != entry.last_error:
                entry.last_error = report
                print(f"Cycles Render bridge: the editor of material {entry.item_id!r} failed in its timer:\n{report}", file=sys.stderr)
    return _TICK_SECONDS


def _install():
    global _installed
    if _installed:
        return
    _installed = True
    bpy.app.handlers.depsgraph_update_post.append(_on_update)
    bpy.app.handlers.undo_post.append(_after_undo)
    bpy.app.handlers.redo_post.append(_after_undo)
    bpy.app.timers.register(_tick, first_interval=_TICK_SECONDS, persistent=True)
