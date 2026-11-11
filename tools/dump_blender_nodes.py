# SPDX-License-Identifier: GPL-3.0-or-later
"""Asks Blender for every shader node it has and writes them as JSON. Runs inside Blender:

    blender -b --factory-startup --python tools\\dump_blender_nodes.py -- <output.json>

For each node type that can be put into a material's node tree: its type name, its label,
its colour tag, its input and output sockets (identifier, name, kind, default) and its own
properties (the dropdowns and switches on the node itself), which of those settings show and hide
which sockets, and the node's place in Blender's Add menu. tools\\make_nodes.py turns the
file into the table the plugin builds its Cinema 4D nodes from.
"""

import json
import sys

import bpy

SKIP_PROPERTIES = {"rna_type", "type", "location", "width", "width_hidden", "height", "dimensions", "name", "label", "inputs", "outputs",
                   "internal_links", "parent", "use_custom_color", "color", "select", "show_options", "show_preview", "hide", "mute",
                   "show_texture", "bl_idname", "bl_label", "bl_description", "bl_icon", "bl_static_type", "bl_width_default",
                   "bl_width_min", "bl_width_max", "bl_height_default", "bl_height_min", "bl_height_max", "warning_propagation",
                   "location_absolute", "color_tag"}


def plain(value):
    if hasattr(value, "__len__") and not isinstance(value, str):
        return [float(v) for v in value]
    if isinstance(value, (bool, int, float, str)):
        return value
    return None


def describe(node):
    entry = {"type": node.bl_idname, "label": node.bl_label, "color_tag": getattr(node, "color_tag", ""), "inputs": [], "outputs": [], "properties": []}
    for socket in node.inputs:
        if not socket.enabled and socket.identifier.startswith("__"):
            continue
        entry["inputs"].append({"identifier": socket.identifier, "name": socket.name, "kind": socket.bl_idname, "enabled": socket.enabled,
                                "default": plain(socket.default_value) if hasattr(socket, "default_value") else None})
    for socket in node.outputs:
        entry["outputs"].append({"identifier": socket.identifier, "name": socket.name, "kind": socket.bl_idname, "enabled": socket.enabled})
    for prop in node.bl_rna.properties:
        if prop.identifier in SKIP_PROPERTIES or prop.is_readonly or prop.is_hidden:
            continue
        if prop.type == "ENUM":
            try:
                items = [[item.identifier, item.name] for item in prop.enum_items]
            except Exception:   # noqa: BLE001
                items = []
            if items:
                entry["properties"].append({"identifier": prop.identifier, "name": prop.name, "kind": "ENUM", "items": items,
                                            "default": getattr(node, prop.identifier)})
        elif prop.type in ("BOOLEAN", "INT", "FLOAT", "STRING") and not getattr(prop, "is_array", False):
            entry["properties"].append({"identifier": prop.identifier, "name": prop.name, "kind": prop.type,
                                        "default": plain(getattr(node, prop.identifier))})
    entry["show"] = availability(node, entry)
    return entry


def availability(node, entry):
    """Which sockets a setting of the node shows and hides: {"in:<identifier>": {property: [values]}}.

    Blender switches sockets on and off with the dropdowns on the node (Mix: one set of inputs per
    data type). Each dropdown and switch is tried value by value, the others left at their defaults;
    a socket no single setting brings up is looked for with two settings at once.
    """
    def state():
        result = {"in:" + s.identifier: s.enabled for s in node.inputs}
        result.update({"out:" + s.identifier: s.enabled for s in node.outputs})
        return result

    choices = {}
    for prop in entry["properties"]:
        if prop["kind"] == "ENUM":
            choices[prop["identifier"]] = [item[0] for item in prop["items"]]
        elif prop["kind"] == "BOOLEAN":
            choices[prop["identifier"]] = [False, True]
    defaults = {name: getattr(node, name) for name in choices}

    def put(values):
        for name, value in defaults.items():
            try:
                setattr(node, name, values.get(name, value))
            except Exception:   # noqa: BLE001
                pass

    base = state()
    shown = {}
    for name, values in choices.items():
        seen = {}
        for value in values:
            put({name: value})
            for socket, enabled in state().items():
                seen.setdefault(socket, []).append(enabled)
        for socket, flags in seen.items():
            if any(flags) and not all(flags):
                shown.setdefault(socket, {})[name] = [value for value, flag in zip(values, flags) if flag]
    put({})
    # Sockets that are off and that no single setting switches on: two settings together.
    names = list(choices)
    for socket in [s for s, enabled in base.items() if not enabled and s not in shown]:
        for i, first in enumerate(names):
            for second in names[i + 1:]:
                if len(choices[first]) * len(choices[second]) > 400:
                    continue
                hits = []
                for a in choices[first]:
                    for b in choices[second]:
                        put({first: a, second: b})
                        if state().get(socket):
                            hits.append((a, b))
                if hits:
                    shown[socket] = {first: sorted({a for a, _ in hits}, key=choices[first].index),
                                     second: sorted({b for _, b in hits}, key=choices[second].index)}
                    break
            if socket in shown:
                break
    put({})
    return shown


def menus():
    """Where Blender's Add menu of the Shader Editor keeps each node: {"ShaderNodeMath": "Utilities/Math"}."""
    import os
    import re
    path = os.path.join(bpy.utils.system_resource("SCRIPTS"), "startup", "bl_ui", "node_add_menu_shader.py")
    paths = {"NODE_MT_shader_node_input_constant_base": "Input/Constant", "NODE_MT_shader_node_math_base": "Utilities/Math",
             "NODE_MT_shader_node_vector_base": "Utilities/Vector"}
    result, current, label = {}, None, None
    for line in open(path, encoding="utf-8"):
        match = re.match(r"class (\w+)\(", line)
        if match:
            current, label = match.group(1), None
        match = re.match(r'\s+bl_label = "(.*)"', line)
        if match and current and label is None:
            label = paths.get(current, match.group(1))
        for name in re.findall(r'"(ShaderNode\w+)"', line):
            if label and (name not in result or label == "Utilities/Math"):
                result[name] = label
    return result


def main():
    output = sys.argv[sys.argv.index("--") + 1]
    material = bpy.data.materials.new("probe")
    material.use_nodes = True     # still needed in 5.2 for a node tree to exist
    tree = material.node_tree
    nodes = []
    for name in sorted(dir(bpy.types)):
        if not name.startswith("ShaderNode") or name in ("ShaderNode", "ShaderNodeTree", "ShaderNodeCustomGroup", "ShaderNodeGroup"):
            continue
        try:
            node = tree.nodes.new(name)
        except Exception:   # noqa: BLE001  (not for this kind of tree, or abstract)
            continue
        nodes.append(describe(node))
        tree.nodes.remove(node)
    with open(output, "w", encoding="utf-8") as file:
        json.dump({"blender": bpy.app.version_string, "nodes": nodes, "menus": menus()}, file, indent=1)
    print("NODES", len(nodes))


main()
