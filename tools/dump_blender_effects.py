# SPDX-License-Identifier: GPL-3.0-or-later
"""Asks Blender for the compositor effects it ships and writes them as JSON. Runs inside Blender:

    blender -b --factory-startup --offline-mode --python tools\\dump_blender_effects.py -- <output.json> [node type ...]

The effects are the node groups of Blender's bundled asset library that take a picture and give a
picture (datafiles/assets/nodes/compositing_nodes_essentials.blend; the groups that calculate with
vectors or frames are left out). For each: its name, its description, its catalog, the sockets the
picture goes in and comes out by, and every other input with its kind, default, range, subtype,
panel and, for a menu, its options. The plain compositor nodes named after the output file
("CompositorNodeExposure") are described the same way under "nodes", named by their type (a
socket's range is what its property reports, which is not always the range Blender declares for
the node: tools\\make_effects.py gives those it needs itself). tools\\make_effects.py turns the
files of all supported versions into the table the plugin's Imager tab is built from.
"""

import json
import os
import sys

import bpy

ASSET_FILE = os.path.join("assets", "nodes", "compositing_nodes_essentials.blend")


def plain(value):
    if isinstance(value, (bool, int, float, str)):
        return value
    if hasattr(value, "__len__"):
        return [float(v) for v in value]
    return None


def menu_options(socket):
    """The options of a menu socket. Blender tells them only in the error it raises for an option
    that does not exist."""
    try:
        socket.default_value = "\x00no such option"
    except TypeError as error:
        text = str(error)
        start = text.rfind(" not found in (")
        if start >= 0:
            # A tuple of Python strings: evaluate it as one.
            import ast
            try:
                options = ast.literal_eval(text[start + len(" not found in "):])
                return [options] if isinstance(options, str) else list(options)
            except (ValueError, SyntaxError):
                pass
    return []


def describe(group, tree):
    node = tree.nodes.new("CompositorNodeGroup")
    node.node_tree = group
    panels = {}
    for item in group.interface.items_tree:
        if item.item_type == "SOCKET":
            panels[item.identifier] = (item.parent.name if item.parent and item.parent.name else "", item)
    entry = {"name": group.name, "description": group.asset_data.description if group.asset_data else "",
             "catalog": group.asset_data.catalog_simple_name if group.asset_data else "",
             "image_in": None, "image_out": None, "outputs": [], "inputs": []}
    for socket in node.outputs:
        entry["outputs"].append({"identifier": socket.identifier, "name": socket.name, "kind": socket.bl_idname})
        if entry["image_out"] is None and socket.bl_idname == "NodeSocketColor":
            entry["image_out"] = socket.identifier
    for socket in node.inputs:
        panel, item = panels.get(socket.identifier, ("", None))
        if entry["image_in"] is None and socket.bl_idname == "NodeSocketColor":
            entry["image_in"] = socket.identifier
            continue
        record = {"identifier": socket.identifier, "name": socket.name, "kind": socket.bl_idname, "panel": panel,
                  "default": plain(socket.default_value) if hasattr(socket, "default_value") else None,
                  "hide_value": bool(getattr(socket, "hide_value", False)),
                  "description": item.description if item is not None else "",
                  "min": getattr(item, "min_value", None), "max": getattr(item, "max_value", None),
                  "subtype": getattr(item, "subtype", "NONE")}
        if socket.bl_idname == "NodeSocketMenu":
            default = socket.default_value
            record["options"] = menu_options(socket)
            record["default"] = default
        entry["inputs"].append(record)
    tree.nodes.remove(node)
    return entry


def describe_node(kind, tree):
    """A plain compositor node of type `kind` described as an effect, or None when this Blender lacks it."""
    try:
        node = tree.nodes.new(kind)
    except RuntimeError:
        return None
    entry = {"name": kind, "description": node.bl_rna.description, "catalog": "", "label": node.bl_rna.name,
             "image_in": None, "image_out": None, "outputs": [], "inputs": []}
    for socket in node.outputs:
        entry["outputs"].append({"identifier": socket.identifier, "name": socket.name, "kind": socket.bl_idname})
        if entry["image_out"] is None and socket.bl_idname == "NodeSocketColor":
            entry["image_out"] = socket.identifier
    for socket in node.inputs:
        if entry["image_in"] is None and socket.bl_idname == "NodeSocketColor":
            entry["image_in"] = socket.identifier
            continue
        prop = socket.bl_rna.properties.get("default_value") if hasattr(socket, "default_value") else None
        record = {"identifier": socket.identifier, "name": socket.name, "kind": socket.bl_idname, "panel": "",
                  "default": plain(socket.default_value) if hasattr(socket, "default_value") else None,
                  "hide_value": bool(getattr(socket, "hide_value", False)), "description": socket.description,
                  "min": getattr(prop, "hard_min", None), "max": getattr(prop, "hard_max", None),
                  "subtype": getattr(prop, "subtype", "NONE")}
        if socket.bl_idname == "NodeSocketMenu":
            default = socket.default_value
            record["options"] = menu_options(socket)
            record["default"] = default
        entry["inputs"].append(record)
    tree.nodes.remove(node)
    return entry


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:]
    out_path = arguments[0]
    path = os.path.join(bpy.utils.system_resource("DATAFILES"), ASSET_FILE)
    result = {"blender": bpy.app.version_string, "version": list(bpy.app.version), "file": ASSET_FILE.replace(os.sep, "/"),
              "found": os.path.isfile(path), "effects": []}
    if result["found"]:
        with bpy.data.libraries.load(path, link=False, assets_only=True) as (source, target):
            target.node_groups = list(source.node_groups)
        tree = bpy.data.node_groups.new("dump", "CompositorNodeTree")
        for group in sorted(target.node_groups, key=lambda g: g.name):
            if group is None or group.bl_idname != "CompositorNodeTree":
                continue
            entry = describe(group, tree)
            # An effect takes a picture and gives one.
            if entry["image_in"] and entry["image_out"]:
                result["effects"].append(entry)
    tree = bpy.data.node_groups.new("dump_nodes", "CompositorNodeTree")
    result["nodes"] = [entry for entry in (describe_node(kind, tree) for kind in arguments[1:]) if entry is not None]
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(result, handle, indent=1)


main()
