# SPDX-License-Identifier: Apache-2.0
"""Writes what the plugin builds its Cinema 4D nodes of Blender's shader nodes from.

    python tools\\make_nodes.py [path to blender.exe ...]

It starts every supported Blender (VERSIONS; by default from the installer's usual place, or the
executables named) once to ask it for its nodes (tools\\dump_blender_nodes.py), takes the union,
and writes
    plugin/cyclesrender/source/nodespace/cycles_nodes_table.h                           the table
    plugin/cyclesrender/res/nodes/com.wolfnimations.cyclesrender.nodes.db.json          data and layout
    plugin/cyclesrender/res/nodes/com.wolfnimations.cyclesrender.nodes.db.en-US.json    names
    docs/reference/cycles-nodes.md                                                      the list of nodes by version
Run it again for a new Blender version; do not edit the four by hand.

Versions. Every node, socket, setting and dropdown option carries the first supported version
that has it ("since"); the plugin leaves out what the Blender in use does not have (materials/
cycles_node_material.cpp) and the Node Editor marks a node that Blender lacks. The tool prints
what each version adds, and fails when a version brings a node that NODES_BY_VERSION and
SKIP_NODES do not name (so a new Blender cannot add a node unnoticed), when something a version
had is gone from a later one (the table cannot say "until"), and when VERSIONS differs from the
plugin's SUPPORTED_VERSIONS. What a node looks like (label, defaults, which settings show which
socket, its folder) comes from the newest version that has it; a default that differs between
versions is printed.

The table: every node of a material's node tree is one entry with its Blender type, its label,
its colour tag, its input and output sockets and the properties on the node itself (and the values
Blender keeps elsewhere: an output's value, a Color Ramp's stops, the curves of the curve nodes,
each as settings of the node; see OUTPUT_VALUES, RAMP_MAX_STOPS and CURVES below). The plugin
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
NODE_LIST = os.path.join(HERE, "..", "docs", "reference", "cycles-nodes.md")
INSTALL_CPP = os.path.join(HERE, "..", "plugin", "cyclesrender", "source", "session", "blender_install.cpp")
DATABASE = "com.wolfnimations.cyclesrender.nodes"

# The supported Blender versions, oldest first: the same as SUPPORTED_VERSIONS in the plugin's
# session/blender_install.cpp (checked when the tool runs) and SUPPORTED_BLENDER in the bridge.
VERSIONS = ["5.0", "5.1", "5.2", "5.3"]
BLENDER = r"C:\Program Files\Blender Foundation\Blender {}\blender.exe"

# The nodes each version brought, each looked at by hand: in the node space, or left out in
# SKIP_NODES with the reason. "*" stands for every node of that version. The tool fails on a node
# of a dumped Blender that the entry of the version that brought it does not name: add the node
# here (and to SKIP_NODES if it cannot work in a material the plugin sends) after reading what it
# does in Blender's manual.
NODES_BY_VERSION = {
    "5.0": "*",     # the table of 0.1.0 (made from Blender 5.2.3) was gone through node by node
    "5.1": ["ShaderNodeRaycast"],
    "5.2": [],
    # Eevee only, all four; Light Evaluation, Light Info and Shadow Raycast work inside the input
    # tree of a Light Accumulation (measured: built from Python they render in Eevee; Cycles
    # renders a Light Accumulation black).
    "5.3": ["ShaderNodeLightAccumulation", "ShaderNodeLightEvaluation", "ShaderNodeLightInfo", "ShaderNodeShadowRaycast",
            "ShaderNodeLightIterInternalInput", "ShaderNodeLightIterInternalOutput"],
}

# Left out, with the reason (docs/reference/cycles-nodes.md lists them).
SKIP_NODES = {
    "ShaderNodeOutputLight": "the output of a light's node tree, not of a material's",
    "ShaderNodeOutputWorld": "the output of the world's node tree, not of a material's",
    "ShaderNodeOutputLineStyle": "Freestyle's line styles, which the plugin does not render",
    "ShaderNodeUVAlongStroke": "Freestyle's line styles, which the plugin does not render",
    "ShaderNodeScript": "an OSL script: needs the script's text or file in Blender, which the plugin does not send",
    "ShaderNodeTexIES": "needs an IES file or text in Blender, which the plugin does not send",
    "ShaderNodeMixRGB": "Blender's old Mix node, kept for old files; Mix (ShaderNodeMix) does the same",
    "ShaderNodeEeveeSpecular": "Eevee's old Specular BSDF, kept by Blender for old files",
    "ShaderNodeOutputAOV": "writes a custom AOV, which needs AOVs in the view layer that the plugin does not make",
    "ShaderNodeLightIterInternalInput": "inserted by Blender itself around the input tree of a Light Accumulation when the "
                                        "material is compiled; not in Blender's Add menu, does nothing alone",
    "ShaderNodeLightIterInternalOutput": "inserted by Blender itself around the input tree of a Light Accumulation when the "
                                         "material is compiled; not in Blender's Add menu, does nothing alone",
}
# Sockets left out of every node, with the reason. "LightIndex" (5.3, on Attribute, Vector
# Transform and the material lighting nodes) is the light being evaluated: Blender fills it in
# itself inside the input tree of a Light Accumulation, and it shows on no node.
SKIP_SOCKETS = {"LightIndex"}
# The buttons under a file setting (nodespace/path_commands.cpp registers the commands by these ids).
PATH_COMMANDS = [("com.wolfnimations.cyclesrender.command.path.relative", "Make Relative"),
                 ("com.wolfnimations.cyclesrender.command.path.absolute", "Make Absolute")]
# Properties that are no simple value. An image is replaced by a file and a colour space (below).
# ("mode" was left out up to 0.2.0, which hid the Mode of Combine Color and Separate Color, RGB,
# HSV or HSL; Light Evaluation of 5.3 has one too. The only other nodes with a "mode", Script and
# IES Texture, are left out as a whole.)
SKIP_PROPERTIES = {"target", "is_active_output", "image", "object", "script", "ies", "filepath", "bytecode", "bytecode_hash"}

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

# The curve nodes. Each curve of a node is a setting with Cinema 4D's spline control (a port of
# type maxon::SplineMapper); the plugin samples the spline and the points travel as the node's
# `mapping` (protocol.md, "Node graphs"). At first as in Blender: a straight line across the area.
# The control knows only the square 0..1 (it rewrites a spline of another range into it on the
# first edit, measured), so a spline always lives in that square, and the square stands for the
# area of Blender's curve: 0..1, or -1..1 for Vector Curves.
# node type -> (Blender's area: min x, min y, max x, max y; [(index in Blender's mapping.curves, port, label)])
# Blender lists the curves of RGB Curves as R, G, B and then C (combined); its tabs show C first.
CURVES = {
    "ShaderNodeFloatCurve": ([0.0, 0.0, 1.0, 1.0], [(0, "prop.curve", "Curve")]),
    "ShaderNodeRGBCurve": ([0.0, 0.0, 1.0, 1.0], [(3, "prop.curve_c", "C"), (0, "prop.curve_r", "R"), (1, "prop.curve_g", "G"),
                                                  (2, "prop.curve_b", "B")]),
    "ShaderNodeVectorCurve": ([-1.0, -1.0, 1.0, 1.0], [(0, "prop.curve_x", "X"), (1, "prop.curve_y", "Y"), (2, "prop.curve_z", "Z")]),
}
# What the curve does left and right of its first and last point; Blender's default is EXTRAPOLATED.
CURVE_EXTEND = "HORIZONTAL|Horizontal;EXTRAPOLATED|Extrapolated"

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
              "e": ("net.maxon.datatype.id", "net.maxon.datatype.id", "net.maxon.ui.enum"),
              "k": ("net.maxon.interface.splinemapper-C", "net.maxon.interface.splinemapper-C", "net.maxon.ui.splinemapper")}

# A knot of a spline (maxon::SplineMapperKnot) as Cinema 4D writes it: interpolation 0 is Bezier,
# 1 linear, 2 cubic; the flags hold the knot's id in their upper 16 bits.
SPLINE_BEZIER = 0


def spline_knot(x, y, tangent_x, tangent_y, knot_id):
    return {"position": {"x": x, "y": y}, "interpolation": SPLINE_BEZIER, "tangentLeft": {"x": -tangent_x, "y": -tangent_y},
            "tangentRight": {"x": tangent_x, "y": tangent_y}, "flags": knot_id << 16}


def straight_spline(stored):
    """A spline of two Bezier knots in the corners of the square 0..1, the only range Cinema 4D's
    control shows, its tangents along the line: a straight line, which bends as soon as a knot or
    tangent is moved. The same as CreateCyclesCurve() in nodespace/cycles_nodes.cpp."""
    third = 1.0 / 3.0
    return value(stored, {"referenceDataType": stored, "_knots": [spline_knot(0.0, 0.0, third, third, 1), spline_knot(1.0, 1.0, third, third, 2)],
                          "_min": {"x": 0.0, "y": 0.0}, "_max": {"x": 1.0, "y": 1.0}, "_tension": 1.0,
                          "_linkTangent": False, "_linkPosition": False, "_snap": False})


def default_value(kind, default):
    stored = PORT_TYPES[kind][1]
    if stored is None or default is None:
        return None
    if kind == "k":
        return straight_spline(stored)     # the default of a curve setting is Blender's area, for the table
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

    def __init__(self, asset, label, menu, version_dependent=False):
        self.asset = asset
        self.label = label
        self.menu = menu
        self.data = [include()]
        # A preview is the whole material's ball, whatever the node (Cinema 4D cannot compute
        # these nodes): it belongs on the material's thumbnail, and is folded away on the nodes.
        # Except on a node that not every supported Blender has (version_dependent: newer than
        # the oldest version): its preview is a card that says whether the Blender chosen in the
        # Preferences has the node (VersionCard() in nodespace/cycles_nodespace.cpp). Whether a
        # node shows a preview cannot follow the Preferences (measured 2026-11-16: a description
        # is read once, the port description provider is not asked about a node's own
        # attributes, a value set in InstantiateImpl is overridden by the description), so these
        # nodes always show it; the card's picture follows the chosen Blender.
        self.data.append(entry("attribute", "net.maxon.node.base.displaypreview", {
            D + "data.base.datatype": value("net.maxon.datatype.id", "bool"), D + "data.base.defaultvalue": value("bool", version_dependent)}))
        # ... and out of the Node Editor altogether: neither the "Preview" tab of a selected node
        # (it must stay for the card: hiding it hides the picture on the node too, measured) nor
        # the switch that would show it on the node.
        hidden = {D + "ui.base.show": value("net.maxon.interface.cstring-C", "false")}
        self.ui = [include(), entry("attribute", "net.maxon.node.base.displaypreview", hidden)]
        if not version_dependent:
            self.ui.append(entry("group", "net.maxon.node.base.preview", hidden))
            self.ui.append(entry("attribute", "net.maxon.node.base.nodepreviewimage", hidden))
        self.strings = [include()]

    def group(self, identifier, label, is_open, on_node):
        self.data.append(entry("group", identifier))
        self.ui.append(entry("group", identifier, {D + "ui.base.groupid": value("net.maxon.datatype.internedid", "net.maxon.node.base.group.inputs"),
                                                   D + "ui.base.groupdefaultopen": value("bool", is_open),
                                                   D + "ui.base.showgroupinportlist": value("bool", on_node)}))
        self.strings.append(entry("group", identifier, name_string(label)))

    def port(self, identifier, label, kind, default, output=False, group=None, hidden=False, constant=False, show=None, options=None,
             preview=False):
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
        if preview:
            # The port the node's preview is asked for (the version card): without it Cinema 4D
            # asked only for nodes with inputs and a colour output (measured).
            data["net.maxon.node.attribute.ispreviewport"] = value("bool", True)
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

    def buttons(self, identifier, label, commands):
        """A row of buttons in the Attribute Manager, each a command of the plugin (registered under
        its id in maxon::CommandClasses, nodespace/path_commands.cpp), as Maxon's own Image node has
        its "Reload Image" and the like: a group of two columns in the layout and a "command" entry
        with the command type "button" per button; nothing in the data."""
        self.data.append(entry("group", identifier))
        self.ui.append(entry("group", identifier, {D + "ui.base.groupid": value("net.maxon.datatype.internedid", "net.maxon.node.base.group.inputs"),
                                                   D + "ui.base.groupcolumns": value("int64", len(commands)),
                                                   D + "ui.base.grouptitlebar": value("bool", False)}))
        self.strings.append(entry("group", identifier, name_string(label)))
        for command, text in commands:
            self.ui.append(entry("command", command, {D + "ui.base.commandtype": value("net.maxon.datatype.id", "button"),
                                                      D + "ui.base.groupid": value("net.maxon.datatype.internedid", identifier)}))
            self.strings.append(entry("command", command, name_string(text)))

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


def property_row(identifier, port, label, kind, items, default, since):
    """A row of the plugin's table for a setting; a colour or vector default goes as "r g b" text,
    the area of Blender's curve that a curve setting stands for as "min_x min_y max_x max_y"."""
    text = default if isinstance(default, str) else (" ".join(repr(float(v)) for v in default) if isinstance(default, list) else "")
    number = float(default) if isinstance(default, (int, float, bool)) else 0.0
    return f'\t{{ {c_string(identifier)}, {c_string(port)}, {c_string(label)}, \'{kind}\', {c_string(items)}, {c_string(text)}, {number!r}, {since} }},'


def options_of(items):
    """"ID|Label;ID|Label|503" -> [(ID, Label), ...]; a third part is the version code of an option
    that came after its setting (version_code)."""
    return [tuple(item.split("|")[:2]) for item in items.split(";") if item]


# ------------------------------------------------------------------------------------------------
# Versions.

def version_code(version):
    """"5.3" -> 503, as the table and the plugin compare versions (major * 100 + minor)."""
    major, minor = version.split(".")[:2]
    return int(major) * 100 + int(minor)


def plugin_versions():
    """The versions in SUPPORTED_VERSIONS of the plugin's session/blender_install.cpp."""
    text = open(INSTALL_CPP, encoding="utf-8").read()
    block = text[text.index("SUPPORTED_VERSIONS[]"):]
    block = block[:block.index("};")]
    return [f"{major}.{minor}" for major, minor in re.findall(r"\{\s*(\d+),\s*(\d+),", block)]


def dump_all(executables):
    """Asks each Blender for its nodes: {"5.2": dump, ...}, oldest first."""
    dumps = {}
    for blender in executables:
        out = os.path.join(tempfile.gettempdir(), "cyclesrender_blender_nodes_%d.json" % len(dumps))
        if os.path.exists(out):
            os.remove(out)
        subprocess.run([blender, "-b", "--factory-startup", "--python", os.path.join(HERE, "dump_blender_nodes.py"), "--", out],
                       check=True, stdout=subprocess.DEVNULL)
        data = json.load(open(out, encoding="utf-8"))
        version = ".".join(data["blender"].split(".")[:2])
        if version in dumps:
            sys.exit(f"Two of the executables are Blender {version}.")
        dumps[version] = data
    return dict(sorted(dumps.items(), key=lambda item: version_code(item[0])))


class Union:
    """Every node of every dumped version, as the newest version that has it describes it, and
    the first version that has each node, socket, setting and option."""

    def __init__(self, dumps):
        self.dumps = dumps
        self.versions = list(dumps)
        self.since = {}         # ("node", type) / ("in"|"out"|"prop", type, identifier) / ("option", type, prop, item) -> version
        self.newest = {}        # type -> (version, node)
        self.menus = {}
        self.problems = []      # what makes the tool fail
        self.defaults = []      # (type, what, {version: default}) where versions differ
        seen = {}               # key -> versions that have it
        for version, data in dumps.items():
            for node in data["nodes"]:
                kind = node["type"]
                self.newest[kind] = (version, node)
                self.menus[kind] = data["menus"].get(kind)
                keys = [("node", kind)]
                keys += [("in", kind, s["identifier"]) for s in node["inputs"]]
                keys += [("out", kind, s["identifier"]) for s in node["outputs"]]
                for prop in node["properties"]:
                    keys.append(("prop", kind, prop["identifier"]))
                    keys += [("option", kind, prop["identifier"], item[0]) for item in prop.get("items", [])]
                for key in keys:
                    seen.setdefault(key, []).append(version)
        for key, versions in seen.items():
            self.since[key] = versions[0]
            # What a version had and a later one that has the node lacks: the table has no "until".
            node_versions = seen[("node", key[1])]
            later = [v for v in node_versions if version_code(v) > version_code(versions[0]) and v not in versions]
            if later:
                self.problems.append(f"{' '.join(key)}: in Blender {versions[0]}, gone in {', '.join(later)}; "
                                     f"the table cannot say that something ends with a version")
        # Defaults that differ between versions (the table takes the newest).
        for kind, (_version, _node) in sorted(self.newest.items()):
            for side in ("inputs", "properties"):
                values = {}
                for version, data in dumps.items():
                    for node in data["nodes"]:
                        if node["type"] != kind:
                            continue
                        for entry in node[side]:
                            values.setdefault(entry["identifier"], {})[version] = entry.get("default")
                for identifier, by_version in values.items():
                    if len({json.dumps(v) for v in by_version.values()}) > 1:
                        self.defaults.append((kind, identifier, by_version))

    def nodes(self):
        return [self.newest[kind][1] for kind in sorted(self.newest)]

    def code(self, *key):
        return version_code(self.since[key])

    def check_reviewed(self):
        """Every node is named for the version that brought it (NODES_BY_VERSION) or left out (SKIP_NODES)."""
        for version in self.versions:
            if version not in NODES_BY_VERSION:
                self.problems.append(f"Blender {version} is not in NODES_BY_VERSION: look at the nodes it brings and add it")
        for kind in sorted(self.newest):
            first = self.since[("node", kind)]
            reviewed = NODES_BY_VERSION.get(first)
            if reviewed == "*" and first == self.versions[0]:
                continue
            if reviewed is not None and kind in reviewed:
                continue
            self.problems.append(f"{kind} ({self.newest[kind][1]['label']}) came with Blender {first} and is named neither in "
                                 f"NODES_BY_VERSION[\"{first}\"] nor in SKIP_NODES: read what it does and add it")
        for version, kinds in NODES_BY_VERSION.items():
            if kinds == "*" or version not in self.versions:
                continue
            for kind in kinds:
                if self.since.get(("node", kind)) != version:
                    self.problems.append(f"NODES_BY_VERSION[\"{version}\"] names {kind}, which "
                                         + (f"came with Blender {self.since[('node', kind)]}" if ("node", kind) in self.since else "no Blender has"))

    def report(self):
        """What each version adds, in words."""
        lines = []
        for version in self.versions[1:]:
            added = sorted(key for key, first in self.since.items() if first == version)
            nodes = [key[1] for key in added if key[0] == "node"]
            # (The options of a setting that is new itself are not listed again.)
            rest = [key for key in added if key[0] != "node" and key[1] not in nodes
                    and not (key[0] == "option" and self.since.get(("prop", key[1], key[2])) == version)]
            lines.append(f"Blender {version} adds {len(nodes)} node(s) and {len(rest)} socket(s), setting(s) and option(s) to older nodes:")
            for kind in nodes:
                lines.append(f"  node {kind} ({self.newest[kind][1]['label']})" + (" - left out" if kind in SKIP_NODES else ""))
            for key in rest:
                lines.append("  " + {"in": "input", "out": "output", "prop": "setting", "option": "option"}[key[0]] + " " + " ".join(key[1:]))
        for kind, identifier, by_version in self.defaults:
            lines.append(f"Default of {kind} {identifier} differs: " + ", ".join(f"{v} {d}" for v, d in by_version.items())
                         + "; the table uses the newest")
        return lines


def since_text(code):
    return f"{code // 100}.{code % 100}"


def write_node_list(union, skipped):
    """docs/reference/cycles-nodes.md: every node with the version it exists from."""
    blender = ", ".join(data["blender"] for data in union.dumps.values())
    lines = ["<!-- Generated by tools/make_nodes.py. Do not edit: run the script again. -->", "",
             "# Blender's shader nodes by version", "",
             f"Every shader node of the supported Blender versions ({blender}), the version it exists from, and whether "
             "it is in the Cycles node space of Cinema 4D. With an older Blender chosen in the Preferences, a node of a newer "
             "version is left out of what is sent, and so are its wires, with a line in the console; the Node Editor marks "
             "such a node (`docs/reference/c4d-sdk.md`, \"A node the chosen Blender does not have\").", "",
             "| Node | Blender type | From | In the node space |", "|---|---|---|---|"]
    for node in sorted(union.nodes(), key=lambda n: (union.code("node", n["type"]), n["label"])):
        kind = node["type"]
        where = "left out: " + SKIP_NODES[kind] if kind in SKIP_NODES else "yes, in " + (union.menus.get(kind) or MENU_BY_TAG.get(node["color_tag"], "Utilities"))
        lines.append(f"| {node['label']} | `{kind}` | {union.since[('node', kind)]} | {where} |")
    lines += ["", "## Added to nodes of an older version", "",
              "Sockets, settings and dropdown options a later version added to a node that an older version has already. "
              "With an older Blender each is left out of what is sent; the console names it where it matters (wired, or a "
              "value other than its default, which is what the older Blender does).", "",
              "| Node | Added | From |", "|---|---|---|"]
    names = {"in": "input", "out": "output", "prop": "setting", "option": "option"}
    for key, first in sorted(union.since.items(), key=lambda item: (version_code(item[1]), item[0])):
        if key[0] == "node" or first == union.versions[0] or union.since[("node", key[1])] == first or key[1] in SKIP_NODES:
            continue
        if key[0] == "option" and union.since.get(("prop", key[1], key[2])) == first:
            continue        # the options of a setting that is new itself
        if key[2] == "__extend__" or (key[0] == "prop" and key[2] in SKIP_PROPERTIES):
            continue        # not in the node space at all
        what = f"{names[key[0]]} {' of '.join(reversed(key[2:])) if key[0] == 'option' else key[2]}"
        note = " (not in the node space: " + skipped[key] + ")" if key in skipped else ""
        if key == ("prop", "ShaderNodeRaycast", "active_index"):
            note = " (which socket of the node's list of attributes is selected in Blender's sidebar; no effect on the render)"
        lines.append(f"| {union.newest[key[1]][1]['label']} | {what}{note} | {first} |")
    if union.defaults:
        lines += ["", "## Defaults that differ between versions", "",
                  "A new node in Cinema 4D takes the default of the newest version; the plugin sends every value, so a "
                  "material renders the same on every version.", "", "| Node | Socket or setting | Defaults |", "|---|---|---|"]
        for kind, identifier, by_version in union.defaults:
            lines.append(f"| {union.newest[kind][1]['label']} | {identifier} | "
                         + ", ".join(f"{v}: {round(d, 6) if isinstance(d, float) else d}" for v, d in by_version.items()) + " |")
    with open(NODE_LIST, "w", encoding="utf-8", newline="\n") as file:
        file.write("\n".join(lines) + "\n")


# ------------------------------------------------------------------------------------------------
def main():
    supported = plugin_versions()
    if supported != VERSIONS:
        sys.exit(f"VERSIONS {VERSIONS} differs from SUPPORTED_VERSIONS {supported} in session/blender_install.cpp: change both")
    executables = sys.argv[1:] or [BLENDER.format(version) for version in VERSIONS]
    missing = [path for path in executables if not os.path.isfile(path)]
    if missing:
        sys.exit("Not found: " + ", ".join(missing) + ". Every supported Blender is needed, or name the executables.")
    dumps = dump_all(executables)
    if list(dumps) != VERSIONS:
        sys.exit(f"Dumped Blender {', '.join(dumps)}; every supported version is needed ({', '.join(VERSIONS)}), "
                 "else the versions a node exists from would be wrong.")
    union = Union(dumps)
    union.check_reviewed()
    for line in union.report():
        print(line)
    if union.problems:
        for problem in union.problems:
            print("PROBLEM:", problem)
        sys.exit(f"{len(union.problems)} problem(s); nothing was written.")
    skipped = {}            # (side, type, identifier) -> why a socket is not in the space

    lines = ["// SPDX-License-Identifier: Apache-2.0",
             "// Generated by tools/make_nodes.py from Blender " + ", ".join(d["blender"] for d in dumps.values())
             + ". Do not edit: run the script again.",
             "// Included by nodespace/cycles_nodes.cpp only. \"since\": the first Blender that has it, major * 100 + minor.", "",
             "#ifndef CYCLES_NODES_TABLE_H__", "#define CYCLES_NODES_TABLE_H__", ""]
    entries = []
    descriptions = []
    menus_used = []
    oldest = version_code(VERSIONS[0])
    for node in union.nodes():
        if node["type"] in SKIP_NODES:
            continue
        name = node["type"][len("ShaderNode"):]
        asset = "com.wolfnimations.cyclesrender.node." + name.lower()
        menu = union.menus.get(node["type"]) or MENU_BY_TAG.get(node["color_tag"], "Utilities")
        node_since = union.code("node", node["type"])
        if menu not in menus_used:
            menus_used.append(menu)
        description = Description(asset, node["label"], menu, node_since > oldest)
        inputs, outputs, properties = [], [], []
        in_ports, out_ports, prop_ports = [], [], []   # what the description needs of each

        labels = [s["name"] for s in node["inputs"]]
        for socket in node["inputs"]:
            kind = socket_kind(socket["kind"])
            if kind is None or (socket["identifier"] == "Weight" and not socket["enabled"]):
                continue
            if socket["identifier"] in SKIP_SOCKETS:
                skipped[("in", node["type"], socket["identifier"])] = "Blender fills it in itself"
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
            prop_since = union.code("prop", node["type"], prop["identifier"])
            # An option that came after its setting carries its version as a third part.
            items = ";".join(f"{identifier}|{label}" + (f"|{code}" if code > prop_since else "")
                             for identifier, label in prop.get("items", [])
                             for code in [union.code("option", node["type"], prop["identifier"], identifier)])
            default = prop["default"]
            properties.append(property_row(prop["identifier"], port_id("prop.", prop["identifier"]), prop["name"], kind, items, default, prop_since))
            prop_ports.append((prop["identifier"], port_id("prop.", prop["identifier"]), prop["name"], kind, items, default, None))
        if node["type"] == "ShaderNodeOutputMaterial":
            for identifier, label, kind, items, default in MATERIAL_SETTINGS:
                properties.append(property_row("material:" + identifier, port_id("prop.material_", identifier), label, kind, items, default, node_since))
                prop_ports.append(("material:" + identifier, port_id("prop.material_", identifier), label, kind, items, default, "group.material"))
        prop_show = {}      # port of a setting -> when it shows, for settings that are not always there

        def setting(identifier, port, label, kind, items, default, show=None):
            # Values the plugin keeps for Blender (an output's value, ramp stops, curves, frames): as old as the node.
            properties.append(property_row(identifier, port, label, kind, items, default, node_since))
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
        if node["type"] in CURVES:
            area, curves = CURVES[node["type"]]
            for index, port, label in curves:
                setting(f"curve:{index}", port, label, "k", "", area)
            setting("curve:extend", "prop.curve_extend", "Extend", "e", CURVE_EXTEND, "EXTRAPOLATED")
        if node["type"] in ("ShaderNodeTexImage", "ShaderNodeTexEnvironment"):
            # The image: a file, and how its values are meant. The plugin makes the image from the two.
            properties.insert(0, property_row("image", "prop.image_file", "File", "u", "", "", node_since))
            properties.insert(1, property_row("image.colorspace", "prop.image_colorspace", "Color Space", "e", "COLOR|Color (sRGB);DATA|Non-Color", "COLOR", node_since))
            prop_ports.insert(0, ("image", "prop.image_file", "File", "u", "", None, None))
            prop_ports.insert(1, ("image.colorspace", "prop.image_colorspace", "Color Space", "e", "COLOR|Color (sRGB);DATA|Non-Color", "COLOR", None))
            # The first and last frame of an image sequence. Cinema 4D's drop of an image file on the
            # Node Editor wants an image node with these two (NODESPACE::IMAGENODEPORTS) and fills
            # them for a sequence; Blender's sequences are not translated, so they never show and
            # are not sent (the exporter names a dropped sequence in the console).
            setting("image:frame_start", "prop.image_frame_start", "Start Frame", "i", "", 0, "false")
            setting("image:frame_end", "prop.image_frame_end", "End Frame", "i", "", 0, "false")

        # ---- the description. As on a Blender node: outputs, the node's settings, then the inputs.
        by_property = {identifier: (port, kind) for identifier, port, _label, kind, _items, _default, _group in prop_ports}
        for socket, kind, label in in_ports:
            inputs.append(f'\t{{ {c_string(socket["identifier"])}, {c_string(port_id("in.", socket["identifier"]))}, {c_string(label)}, \'{kind}\', '
                          f'{numbers(socket["default"])}, {c_string(socket["default"] if isinstance(socket["default"], str) else "")}, '
                          f'{c_string(table_rule(node["show"].get("in:" + socket["identifier"]), socket["enabled"], by_property))}, '
                          f'{union.code("in", node["type"], socket["identifier"])} }},')
        for socket, kind, label in out_ports:
            outputs.append(f'\t{{ {c_string(socket["identifier"])}, {c_string(port_id("out.", socket["identifier"]))}, {c_string(label)}, \'{kind}\', '
                           f'{numbers(0.0)}, "", {c_string(table_rule(node["show"].get("out:" + socket["identifier"]), socket["enabled"], by_property))}, '
                           f'{union.code("out", node["type"], socket["identifier"])} }},')

        def shown_when(key, socket):
            # A socket Blender never switches on (for no setting of the node) is not shown at all.
            rule = condition(node["show"].get(key), by_property)
            return rule if rule or socket["enabled"] else "false"

        for index, (socket, kind, label) in enumerate(out_ports):
            description.port(port_id("out.", socket["identifier"]), label, kind, None, output=True, hidden=not socket["enabled"],
                             show=shown_when("out:" + socket["identifier"], socket), preview=node_since > oldest and index == 0)
        if any(group for *_rest, group in prop_ports):
            description.group("group.material", "Material Settings", True, False)
        for identifier, port, label, kind, items, default, group in prop_ports:
            # A setting: no wire can go in, and it does not show on the node.
            description.port(port, label, kind, default, group=group, hidden=True, constant=True, options=options_of(items),
                             show=prop_show.get(port))
            if kind == "u":
                # Under a file: make its path relative to the document's folder, or absolute again.
                description.buttons("group.path", "Path", PATH_COMMANDS)
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
                       f'{c_string(node["color_tag"])}, {block("INPUTS", inputs)}, {block("OUTPUTS", outputs)}, {block("PROPERTIES", properties)}, '
                       f'{node_since} }},')
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
    write_node_list(union, skipped)
    print(f"{len(entries)} nodes written ({len(union.newest)} in Blender {', '.join(VERSIONS)}, {len(SKIP_NODES)} left out), {len(folders)} folders")


if __name__ == "__main__":
    main()
