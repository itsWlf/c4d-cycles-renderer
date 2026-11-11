# SPDX-License-Identifier: Apache-2.0
"""Writes what the plugin builds its Cinema 4D nodes of Blender's shader nodes from.

    python tools\\make_nodes.py [path to blender.exe]

It starts Blender once to ask it for its nodes (tools\\dump_blender_nodes.py) and writes
    plugin/cyclesrender/source/nodespace/cycles_nodes_table.h                           the table
    plugin/cyclesrender/res/nodes/com.wolfnimations.cyclesrender.nodes.db.json          data and layout
    plugin/cyclesrender/res/nodes/com.wolfnimations.cyclesrender.nodes.db.en-US.json    names
Run it again for a new Blender version; do not edit the three by hand.

The table: every node of a material's node tree is one entry with its Blender type, its label,
its colour tag, its input and output sockets and the properties on the node itself. The plugin
makes each a node of the "Cycles" node space (nodespace/cycles_nodespace.cpp) and writes a
graph of them back as Blender nodes, one to one (materials/cycles_node_material.cpp).

The two .json files are a description database of Cinema 4D, as its Resource Editor would
write it. It decides what a node looks like: the settings of a Blender node (its dropdowns and
switches) are hidden on the node and shown in the Attribute Manager, a socket shows only for
the settings Blender shows it for, Blender's panels are foldable groups, dropdowns show
Blender's labels, and the node sits in the folder of Blender's Add menu.

Ids of ports in Cinema 4D are derived from Blender's identifiers ("Base Color" -> "in.base_color")
and are stored in scene files: a socket Blender renames in a later version becomes a new port.
"""

import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(HERE, "..", "plugin", "cyclesrender", "source", "nodespace", "cycles_nodes_table.h")
NODES_DIR = os.path.join(HERE, "..", "plugin", "cyclesrender", "res", "nodes")
DATABASE = "com.wolfnimations.cyclesrender.nodes"
BLENDER = r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"

# Left out: they belong to other kinds of node trees, or need data the plugin cannot give them.
SKIP_NODES = {"ShaderNodeOutputLight", "ShaderNodeOutputWorld", "ShaderNodeOutputLineStyle", "ShaderNodeUVAlongStroke", "ShaderNodeScript",
              "ShaderNodeTexIES", "ShaderNodeMixRGB", "ShaderNodeEeveeSpecular", "ShaderNodeOutputAOV"}
# Properties that are no simple value. An image is replaced by a file and a colour space (below).
SKIP_PROPERTIES = {"target", "is_active_output", "image", "object", "script", "ies", "filepath", "bytecode", "bytecode_hash", "mode"}

# The settings Blender keeps on a material next to its node tree. Cinema 4D lets only its own
# material type carry a node graph, so they cannot be parameters of a material type of ours:
# they are value-only ports of the Material Output node, and travel as the material's `props`.
# (identifier of the Blender material property, label, kind, options, default) - defaults are Blender's.
MATERIAL_SETTINGS = [
    ("displacement_method", "Displacement", "e", "BUMP|Bump Only;DISPLACEMENT|Displacement Only;BOTH|Displacement and Bump", "BUMP"),
    ("cycles.emission_sampling", "Emission Sampling", "e", "NONE|None;AUTO|Auto;FRONT|Front;BACK|Back;FRONT_BACK|Front and Back", "AUTO"),
    ("use_transparent_shadow", "Transparent Shadows", "b", "", True),
    ("cycles.use_bump_map_correction", "Bump Map Correction", "b", "", True),
    ("pass_index", "Pass Index", "i", "", 0),
    ("cycles.volume_sampling", "Volume Sampling", "e", "DISTANCE|Distance;EQUIANGULAR|Equiangular;MULTIPLE_IMPORTANCE|Multiple Importance", "MULTIPLE_IMPORTANCE"),
    ("cycles.volume_interpolation", "Volume Interpolation", "e", "LINEAR|Linear;CUBIC|Cubic", "LINEAR"),
    ("cycles.volume_step_rate", "Volume Step Rate", "f", "", 1.0),
    ("surface_render_method", "Eevee Render Method", "e", "DITHERED|Dithered;BLENDED|Blended", "DITHERED"),
    ("use_transparency_overlap", "Eevee Transparency Overlap", "b", "", True),
    ("use_backface_culling", "Eevee Backface Culling Camera", "b", "", False),
    ("use_backface_culling_shadow", "Eevee Backface Culling Shadow", "b", "", False),
    ("use_backface_culling_lightprobe_volume", "Eevee Backface Culling Light Probe", "b", "", True),
    ("use_raytrace_refraction", "Eevee Raytraced Transmission", "b", "", False),
    ("thickness_mode", "Eevee Thickness", "e", "SPHERE|Sphere;SLAB|Slab", "SPHERE"),
    ("use_thickness_from_shadow", "Eevee Thickness From Shadow", "b", "", False),
    ("max_vertex_displacement", "Eevee Max Displacement (m)", "f", "", 0.0),
    ("volume_intersection_method", "Eevee Volume Intersection", "e", "FAST|Fast;ACCURATE|Accurate", "FAST"),
]

# Nodes that keep their value on an output socket, where Cinema 4D has no control: the value is a
# setting of the node here, and travels as the node's `outputs`.
# node type -> (identifier of the output socket, kind, label, default) - defaults are Blender's.
OUTPUT_VALUES = {
    "ShaderNodeValue": ("Value", "f", "Value", 0.5),
    "ShaderNodeRGB": ("Color", "c", "Color", [0.5, 0.5, 0.5]),
    "ShaderNodeNormal": ("Normal", "v", "Normal", [0.0, 0.0, 1.0]),
}

# The Color Ramp. Cinema 4D's node descriptions have no gradient control, so the stops are plain
# settings: how many there are, and a position, colour and alpha for each. They travel as the
# node's `color_ramp`. At first as in Blender: black at 0, white at 1; further stops are white at 0.5.
RAMP_MAX_STOPS = 8
RAMP_INTERPOLATIONS = "EASE|Ease;CARDINAL|Cardinal;LINEAR|Linear;B_SPLINE|B-Spline;CONSTANT|Constant"

# Blender's foldable panels on a node: (title, open at first, identifier of the first input in it).
# A panel runs to the next one; panels come after the loose inputs. Blender does not tell Python
# about them: they are read from source/blender/nodes/shader/nodes/node_shader_*.cc (add_panel).
PANELS = {
    "ShaderNodeBsdfPrincipled": [("Diffuse", False, "Diffuse Roughness"), ("Subsurface", False, "Subsurface Weight"),
                                 ("Specular", False, "Specular IOR Level"), ("Transmission", False, "Transmission Weight"),
                                 ("Coat", False, "Coat Weight"), ("Sheen", False, "Sheen Weight"), ("Emission", False, "Emission Color"),
                                 ("Thin Film", False, "Thin Film Thickness")],
    "ShaderNodeBsdfGlass": [("Thin Film", False, "Thin Film Thickness")],
    "ShaderNodeBsdfMetallic": [("Thin Film", False, "Thin Film Thickness")],
    "ShaderNodeVolumeCoefficients": [("Absorption", True, "Absorption Coefficients"), ("Scatter", True, "Scatter Coefficients"),
                                     ("Emission", True, "Emission Coefficients")],
}

# The folders of the nodes in the Asset Browser: Blender's Add menu below "Cycles Nodes".
CATEGORY_ROOT = "com.wolfnimations.cyclesrender.assetcategory.nodes"
CATEGORY_ROOT_LABEL = "Cycles Nodes"
# For nodes Blender's menu does not list (older nodes kept for old files), by colour tag.
MENU_BY_TAG = {"SHADER": "Shader", "TEXTURE": "Texture", "INPUT": "Input", "OUTPUT": "Output", "COLOR": "Color", "VECTOR": "Utilities/Vector",
               "CONVERTER": "Utilities/Math"}

SOCKET_KINDS = {"NodeSocketShader": "s", "NodeSocketColor": "c", "NodeSocketString": "t", "NodeSocketBool": "b", "NodeSocketInt": "i"}


def socket_kind(name):
    if name in SOCKET_KINDS:
        return SOCKET_KINDS[name]
    if name.startswith("NodeSocketVector") or name == "NodeSocketRotation":
        return "v"
    if name.startswith("NodeSocketFloat"):
        return "f"
    return None


def port_id(prefix, identifier):
    return prefix + re.sub(r"[^a-z0-9]+", "_", identifier.lower()).strip("_")


def category_id(path):
    return CATEGORY_ROOT + "".join("." + re.sub(r"[^a-z0-9]+", "", part.lower()) for part in path.split("/") if part)


def c_string(text):
    return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"') + '"'


def numbers(value, count=4):
    values = list(value) if isinstance(value, list) else [value if isinstance(value, (int, float)) else 0.0]
    values = [float(v) for v in values][:count] + [0.0] * (count - len(values))
    return "{ " + ", ".join(repr(v) for v in values) + " }"


# ------------------------------------------------------------------------------------------------
# Cinema 4D's description database, as the JSON its Resource Editor would write.
D = "net.maxon.description."


def value(kind, content, array=False):
    if array:
        return {"dataType": kind, "isArray": True, "content": content}
    return {"dataType": kind, "content": content}


def dictionary(pairs):
    fast = []
    for key in sorted(pairs):
        fast += [key, pairs[key]]
    return {"referenceDataType": "net.maxon.interface.datadictionary-C",
            "_impl": {"_mode": 1, "_keyType": "net.maxon.datatype.internedid", "_fastData": fast}}


def entry(command, identifier, more=None):
    pairs = {D + "base.command": value("net.maxon.datatype.id", command),
             D + "base.identifier": value("net.maxon.datatype.internedid", identifier)}
    pairs.update(more or {})
    return dictionary(pairs)


def include():
    return dictionary({D + "base.command": value("net.maxon.datatype.id", "include"),
                       D + "base.identifier": value("net.maxon.datatype.internedid", "net.maxon.node.base"),
                       D + "data.base.generateignoreheader": value("bool", True),
                       D + "data.base.include": value("net.maxon.datatype.id", "net.maxon.node.base")})


def definition(info, entries):
    return {"referenceClassType": "net.maxon.class.datadescriptiondefinition",
            "_info": dictionary(info) if info else {"referenceDataType": "net.maxon.interface.datadictionary-C", "_impl": {"_mode": 0}},
            "_description": entries}


def name_string(content):
    return {D + "string.base.translatedstring": value("net.maxon.interface.string-C", content)}


# kind of a port -> (data type, type of the default as stored, widget)
PORT_TYPES = {"f": ("float", "float64", "net.maxon.ui.number"), "i": ("int", "int64", "net.maxon.ui.number"),
              "b": ("bool", "bool", "net.maxon.ui.bool"),
              "c": ("net.maxon.parametrictype.col<3,float>", "net.maxon.parametrictype.col<3,float64>", "net.maxon.ui.color"),
              "s": ("net.maxon.parametrictype.col<3,float>", None, "net.maxon.ui.color"),
              "v": ("net.maxon.parametrictype.vec<3,float>", "net.maxon.parametrictype.vec<3,float64>", "net.maxon.ui.vector"),
              "t": ("net.maxon.interface.string-C", "net.maxon.interface.string-C", "net.maxon.ui.string"),
              "u": ("net.maxon.interface.url-C", None, "net.maxon.ui.url"),
              "e": ("net.maxon.datatype.id", "net.maxon.datatype.id", "net.maxon.ui.enum")}


def default_value(kind, default):
    stored = PORT_TYPES[kind][1]
    if stored is None or default is None:
        return None
    first = default[0] if isinstance(default, list) else default
    if kind == "c":
        return value(stored, {"r": float(default[0]), "g": float(default[1]), "b": float(default[2])})
    if kind == "v":
        v = (list(default) if isinstance(default, list) else [default]) + [0.0, 0.0, 0.0]
        return value(stored, {"x": float(v[0]), "y": float(v[1]), "z": float(v[2])})
    if kind == "f":
        return value(stored, float(first))
    if kind == "i":
        return value(stored, int(first))
    if kind == "b":
        return value(stored, bool(first))
    return value(stored, str(default))


def condition(rule, property_ports):
    """{"data_type": ["RGBA", "ROTATION"]} -> (prop.data_type=="RGBA" || prop.data_type=="ROTATION")"""
    parts = []
    for name, values in (rule or {}).items():
        if name not in property_ports:
            return None
        port, kind = property_ports[name]
        if kind == "b":
            terms = [port if v else "!" + port for v in values]
        else:
            terms = [f'{port}=="{v}"' for v in values]
        parts.append("(" + " || ".join(terms) + ")")
    return " && ".join(parts) if parts else None


def table_rule(rule, enabled, property_ports):
    """The same rule for the plugin's table: "prop.data_type=RGBA|ROTATION&prop.clamp=1", "!" for never, "" for always."""
    parts = []
    for name, values in (rule or {}).items():
        if name not in property_ports:
            return ""
        port, kind = property_ports[name]
        parts.append(port + "=" + "|".join(("1" if v else "0") if kind == "b" else str(v) for v in values))
    if not parts:
        return "" if enabled else "!"
    return "&".join(parts)


class Description:
    """The three descriptions of one node: data, layout, names."""

    def __init__(self, asset, label, menu):
        self.asset = asset
        self.label = label
        self.menu = menu
        self.data = [include()]
        # A preview is the whole material's ball, whatever the node (Cinema 4D cannot compute
        # these nodes): it belongs on the material's thumbnail, and is folded away on the nodes.
        self.data.append(entry("attribute", "net.maxon.node.base.displaypreview", {
            D + "data.base.datatype": value("net.maxon.datatype.id", "bool"), D + "data.base.defaultvalue": value("bool", False)}))
        # ... and out of the Node Editor altogether: neither the "Preview" tab of a selected node
        # nor the switch that would show it on the node.
        hidden = {D + "ui.base.show": value("net.maxon.interface.cstring-C", "false")}
        self.ui = [include(), entry("group", "net.maxon.node.base.preview", hidden),
                   entry("attribute", "net.maxon.node.base.nodepreviewimage", hidden),
                   entry("attribute", "net.maxon.node.base.displaypreview", hidden)]
        self.strings = [include()]

    def group(self, identifier, label, is_open, on_node):
        self.data.append(entry("group", identifier))
        self.ui.append(entry("group", identifier, {D + "ui.base.groupid": value("net.maxon.datatype.internedid", "net.maxon.node.base.group.inputs"),
                                                   D + "ui.base.groupdefaultopen": value("bool", is_open),
                                                   D + "ui.base.showgroupinportlist": value("bool", on_node)}))
        self.strings.append(entry("group", identifier, name_string(label)))

    def port(self, identifier, label, kind, default, output=False, group=None, hidden=False, constant=False, show=None, options=None):
        data_type, _stored, widget = PORT_TYPES[kind]
        data = {D + "data.base.classification": value("net.maxon.datatype.id", "output" if output else "input"),
                D + "data.base.datatype": value("net.maxon.datatype.id", data_type)}
        stored = None if output else default_value(kind, default)
        if stored is not None:
            data[D + "data.base.defaultvalue"] = stored
        if options:
            data[D + "data.base.enum"] = value("(net.maxon.datatype.id,net.maxon.datatype.data)",
                                               [{"_0": name, "_1": value("net.maxon.datatype.id", name)} for name, _label in options], array=True)
        if constant:
            data["net.maxon.node.attribute.constantparameter"] = value("bool", True)
        if hidden:
            data["net.maxon.node.attribute.hideportinnodegraph"] = value("bool", True)
        self.data.append(entry("attribute", identifier, data))
        ui = {D + "ui.base.groupid": value("net.maxon.datatype.internedid",
                                           group or ("net.maxon.node.base.group.outputs" if output else "net.maxon.node.base.group.inputs")),
              D + "ui.base.guitypeid": value("net.maxon.datatype.id", widget)}
        if show:
            ui[D + "ui.base.show"] = value("net.maxon.interface.cstring-C", show)
        self.ui.append(entry("attribute", identifier, ui))
        self.strings.append(entry("attribute", identifier, name_string(label)))
        for name, option_label in options or []:
            self.strings.append(entry("string", f"{identifier}#enum#{name}", name_string(option_label)))

    def finish(self):
        self.strings.append(entry("exportedstring", "net.maxon.object.base.name", name_string(self.label)))
        return (definition({D + "data.info.classification": value("net.maxon.datatype.id", "node")}, self.data),
                definition({D + "ui.info.menucategory": value("net.maxon.datatype.id", category_id(self.menu))}, self.ui),
                definition({}, self.strings))


def write_database(descriptions, folders):
    data, ui, strings = [], [], []
    # A folder's name is found like a node's: as the name in a description under the folder's id.
    for path, label, _parent in folders:
        strings += [category_id(path), definition({}, [entry("exportedstring", "net.maxon.object.base.name", name_string(label))])]
    for description in descriptions:
        d, u, s = description.finish()
        data += [description.asset, d]
        ui += [description.asset, u]
        strings += [description.asset, s]
    os.makedirs(NODES_DIR, exist_ok=True)
    with open(os.path.join(NODES_DIR, DATABASE + ".db.json"), "w", encoding="utf-8", newline="\n") as file:
        json.dump({"identification": "net.maxon.fileid.descriptiondefinition",
                   "content": {"_descriptionsVersion": 6, "_descriptions": data, "_guiDescriptionsVersion": 6, "_guiDescriptions": ui}}, file, separators=(",", ":"))
    with open(os.path.join(NODES_DIR, DATABASE + ".db.en-US.json"), "w", encoding="utf-8", newline="\n") as file:
        json.dump({"identification": "net.maxon.fileid.descriptiondefinition", "content": {"_dbVersion": 6, "_db": strings}}, file, separators=(",", ":"))


def property_row(identifier, port, label, kind, items, default):
    """A row of the plugin's table for a setting; a colour or vector default goes as "r g b" text."""
    text = default if isinstance(default, str) else (" ".join(repr(float(v)) for v in default) if isinstance(default, list) else "")
    number = float(default) if isinstance(default, (int, float, bool)) else 0.0
    return f'\t{{ {c_string(identifier)}, {c_string(port)}, {c_string(label)}, \'{kind}\', {c_string(items)}, {c_string(text)}, {number!r} }},'


def options_of(items):
    return [tuple(item.split("|", 1)) for item in items.split(";") if item]


# ------------------------------------------------------------------------------------------------
def main():
    blender = sys.argv[1] if len(sys.argv) > 1 else BLENDER
    dump = os.path.join(tempfile.gettempdir(), "cyclesrender_blender_nodes.json")
    subprocess.run([blender, "-b", "--factory-startup", "--python", os.path.join(HERE, "dump_blender_nodes.py"), "--", dump],
                   check=True, stdout=subprocess.DEVNULL)
    data = json.load(open(dump, encoding="utf-8"))

    lines = ["// SPDX-License-Identifier: Apache-2.0",
             "// Generated by tools/make_nodes.py from Blender " + data["blender"] + ". Do not edit: run the script again.",
             "// Included by nodespace/cycles_nodes.cpp only.", "",
             "#ifndef CYCLES_NODES_TABLE_H__", "#define CYCLES_NODES_TABLE_H__", ""]
    entries = []
    descriptions = []
    menus_used = []
    for node in data["nodes"]:
        if node["type"] in SKIP_NODES:
            continue
        name = node["type"][len("ShaderNode"):]
        asset = "com.wolfnimations.cyclesrender.node." + name.lower()
        menu = data["menus"].get(node["type"]) or MENU_BY_TAG.get(node["color_tag"], "Utilities")
        if menu not in menus_used:
            menus_used.append(menu)
        description = Description(asset, node["label"], menu)
        inputs, outputs, properties = [], [], []
        in_ports, out_ports, prop_ports = [], [], []   # what the description needs of each

        labels = [s["name"] for s in node["inputs"]]
        for socket in node["inputs"]:
            kind = socket_kind(socket["kind"])
            if kind is None or (socket["identifier"] == "Weight" and not socket["enabled"]):
                continue
            label = socket["name"]
            numbered = any(re.search(r"_\d+$", s["identifier"]) for s in node["inputs"] if s["name"] == label)
            switched = any(node["show"].get("in:" + s["identifier"]) for s in node["inputs"] if s["name"] == label)
            if labels.count(label) > 1 and not numbered and not switched:
                # Sockets of one name for several kinds of data that show together.
                # Not for plain repeats (Math: Value, Value, Value), and not where a setting of the
                # node shows one of them at a time (Mix: A as number, vector or colour): those keep
                # Blender's name.
                suffix = socket["identifier"].split("_")[-1] if "_" in socket["identifier"] else {"f": "Float", "v": "Vector", "c": "Color"}.get(kind, "")
                label = f"{label} ({suffix})" if suffix and not suffix.isdigit() else label
            in_ports.append((socket, kind, label))
        out_labels = [s["name"] for s in node["outputs"]]
        for socket in node["outputs"]:
            kind = socket_kind(socket["kind"])
            if kind is None:
                continue
            label = socket["name"]
            switched = any(node["show"].get("out:" + s["identifier"]) for s in node["outputs"] if s["name"] == label)
            if out_labels.count(label) > 1 and "_" in socket["identifier"] and not switched:
                label = f'{label} ({socket["identifier"].split("_")[-1]})'
            out_ports.append((socket, kind, label))
        for prop in node["properties"]:
            if prop["identifier"] in SKIP_PROPERTIES:
                continue
            kind = {"ENUM": "e", "BOOLEAN": "b", "INT": "i", "FLOAT": "f", "STRING": "t"}[prop["kind"]]
            items = ";".join(f"{identifier}|{label}" for identifier, label in prop.get("items", []))
            default = prop["default"]
            properties.append(f'\t{{ {c_string(prop["identifier"])}, {c_string(port_id("prop.", prop["identifier"]))}, {c_string(prop["name"])}, \'{kind}\', '
                              f'{c_string(items)}, {c_string(default if isinstance(default, str) else "")}, '
                              f'{float(default) if isinstance(default, (int, float, bool)) else 0.0!r} }},')
            prop_ports.append((prop["identifier"], port_id("prop.", prop["identifier"]), prop["name"], kind, items, default, None))
        if node["type"] == "ShaderNodeOutputMaterial":
            for identifier, label, kind, items, default in MATERIAL_SETTINGS:
                properties.append(f'\t{{ {c_string("material:" + identifier)}, {c_string(port_id("prop.material_", identifier))}, {c_string(label)}, \'{kind}\', '
                                  f'{c_string(items)}, {c_string(default if isinstance(default, str) else "")}, '
                                  f'{float(default) if not isinstance(default, str) else 0.0!r} }},')
                prop_ports.append(("material:" + identifier, port_id("prop.material_", identifier), label, kind, items, default, "group.material"))
        prop_show = {}      # port of a setting -> when it shows, for settings that are not always there

        def setting(identifier, port, label, kind, items, default, show=None):
            properties.append(property_row(identifier, port, label, kind, items, default))
            prop_ports.append((identifier, port, label, kind, items, default, None))
            if show:
                prop_show[port] = show

        if node["type"] in OUTPUT_VALUES:
            identifier, kind, label, default = OUTPUT_VALUES[node["type"]]
            setting("output:" + identifier, port_id("prop.output_", identifier), label, kind, "", default)
        if node["type"] == "ShaderNodeValToRGB":
            setting("ramp:interpolation", "prop.ramp_interpolation", "Interpolation", "e", RAMP_INTERPOLATIONS, "LINEAR")
            setting("ramp:stops", "prop.ramp_stops", "Stops", "e", ";".join(f"s{n}|{n}" for n in range(2, RAMP_MAX_STOPS + 1)), "s2")
            for n in range(1, RAMP_MAX_STOPS + 1):
                show = None if n <= 2 else "(" + " || ".join(f'prop.ramp_stops=="s{k}"' for k in range(n, RAMP_MAX_STOPS + 1)) + ")"
                position = {1: 0.0, 2: 1.0}.get(n, 0.5)
                setting(f"ramp:position_{n}", f"prop.ramp_position_{n}", f"Position {n}", "f", "", position, show)
                setting(f"ramp:color_{n}", f"prop.ramp_color_{n}", f"Color {n}", "c", "", [0.0, 0.0, 0.0] if n == 1 else [1.0, 1.0, 1.0], show)
                setting(f"ramp:alpha_{n}", f"prop.ramp_alpha_{n}", f"Alpha {n}", "f", "", 1.0, show)
        if node["type"] in ("ShaderNodeTexImage", "ShaderNodeTexEnvironment"):
            # The image: a file, and how its values are meant. The plugin makes the image from the two.
            properties.insert(0, '\t{ "image", "prop.image_file", "File", \'u\', "", "", 0.0 },')
            properties.insert(1, '\t{ "image.colorspace", "prop.image_colorspace", "Color Space", \'e\', "COLOR|Color (sRGB);DATA|Non-Color", "COLOR", 0.0 },')
            prop_ports.insert(0, ("image", "prop.image_file", "File", "u", "", None, None))
            prop_ports.insert(1, ("image.colorspace", "prop.image_colorspace", "Color Space", "e", "COLOR|Color (sRGB);DATA|Non-Color", "COLOR", None))

        # ---- the description. As on a Blender node: outputs, the node's settings, then the inputs.
        by_property = {identifier: (port, kind) for identifier, port, _label, kind, _items, _default, _group in prop_ports}
        for socket, kind, label in in_ports:
            inputs.append(f'\t{{ {c_string(socket["identifier"])}, {c_string(port_id("in.", socket["identifier"]))}, {c_string(label)}, \'{kind}\', '
                          f'{numbers(socket["default"])}, {c_string(socket["default"] if isinstance(socket["default"], str) else "")}, '
                          f'{c_string(table_rule(node["show"].get("in:" + socket["identifier"]), socket["enabled"], by_property))} }},')
        for socket, kind, label in out_ports:
            outputs.append(f'\t{{ {c_string(socket["identifier"])}, {c_string(port_id("out.", socket["identifier"]))}, {c_string(label)}, \'{kind}\', '
                           f'{numbers(0.0)}, "", {c_string(table_rule(node["show"].get("out:" + socket["identifier"]), socket["enabled"], by_property))} }},')

        def shown_when(key, socket):
            # A socket Blender never switches on (for no setting of the node) is not shown at all.
            rule = condition(node["show"].get(key), by_property)
            return rule if rule or socket["enabled"] else "false"

        for socket, kind, label in out_ports:
            description.port(port_id("out.", socket["identifier"]), label, kind, None, output=True, hidden=not socket["enabled"],
                             show=shown_when("out:" + socket["identifier"], socket))
        if any(group for *_rest, group in prop_ports):
            description.group("group.material", "Material Settings", True, False)
        for identifier, port, label, kind, items, default, group in prop_ports:
            # A setting: no wire can go in, and it does not show on the node.
            description.port(port, label, kind, default, group=group, hidden=True, constant=True, options=options_of(items),
                             show=prop_show.get(port))
        panels = {first: (f"group.{port_id('', title)}", title, is_open) for title, is_open, first in PANELS.get(node["type"], [])}
        group = None
        for socket, kind, label in in_ports:
            if socket["identifier"] in panels:
                group, title, is_open = panels[socket["identifier"]]
                description.group(group, title, is_open, True)
            show = shown_when("in:" + socket["identifier"], socket)
            # A socket Blender does not show for the node's default settings starts hidden on the node.
            description.port(port_id("in.", socket["identifier"]), label, kind, socket["default"], group=group,
                             hidden=not socket["enabled"], show=show)
        descriptions.append(description)

        for kind_name, rows, struct in (("INPUTS", inputs, "CyclesSocket"), ("OUTPUTS", outputs, "CyclesSocket"), ("PROPERTIES", properties, "CyclesProperty")):
            if rows:
                lines += [f"static const {struct} {name.upper()}_{kind_name}[] =", "{"] + rows + ["};", ""]

        def block(kind_name, rows):
            return f"{name.upper()}_{kind_name}, {len(rows)}" if rows else "nullptr, 0"
        entries.append(f'\t{{ {c_string(node["type"])}, {c_string(asset)}, {c_string(node["label"])}, '
                       f'{c_string(node["color_tag"])}, {block("INPUTS", inputs)}, {block("OUTPUTS", outputs)}, {block("PROPERTIES", properties)} }},')
    lines += ["static const CyclesNode CYCLES_NODES[] =", "{"] + entries + ["};", ""]

    # The folders: every menu path in use and its parents, parents first.
    folders = [("", CATEGORY_ROOT_LABEL, None)]
    for menu in menus_used:
        parts = menu.split("/")
        for depth in range(1, len(parts) + 1):
            path = "/".join(parts[:depth])
            if not any(path == known for known, _label, _parent in folders):
                folders.append((path, parts[depth - 1], "/".join(parts[:depth - 1])))
    lines += ["// The folders of the nodes in the Asset Browser: id, name, id of the folder it is in.",
              "static const CyclesNodeFolder CYCLES_NODE_FOLDERS[] =", "{"]
    for path, label, parent in folders:
        lines.append(f'\t{{ {c_string(category_id(path))}, {c_string(label)}, {c_string(category_id(parent)) if parent is not None else "nullptr"} }},')
    lines += ["};", "", "#endif // CYCLES_NODES_TABLE_H__", ""]

    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as file:
        file.write("\n".join(lines))
    write_database(descriptions, folders)
    print(f"{len(entries)} nodes written, {len(folders)} folders")


if __name__ == "__main__":
    main()
