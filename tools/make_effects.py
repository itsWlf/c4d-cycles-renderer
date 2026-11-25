# SPDX-License-Identifier: Apache-2.0
"""Writes the table the Cycles Camera tag's Imager tab is built from: Blender's compositor effects.

    python tools\\make_effects.py [path to blender.exe ...]

It starts every supported Blender (VERSIONS; by default from the installer's usual place, or the
executables named) once to ask it for the compositor effects it ships (tools\\dump_blender_effects.py:
the node groups of its bundled asset library that take a picture and give one) and for the plain
compositor nodes PLAIN_NODES names, takes the union, and writes
    plugin/cyclesrender/source/tags/imager_effects_table.h      the table
Run it again for a new Blender version; do not edit the file by hand.

Versions. Every effect, setting and menu option carries the first supported version that has it
("since"); the plugin leaves out what the Blender in use does not have (scene/imager_export.cpp) and
names it in the console. What an effect looks like (labels, defaults, ranges, panels) comes from the
newest version that has it; a default that differs between versions is printed. The tool fails when
something a version had is gone from a later one (the table cannot say "until"), and when VERSIONS
differs from the plugin's SUPPORTED_VERSIONS.

Two sources. A bundled node group is stored in a layer by Blender's name of the group (the layer's source
IMAGERLAYER_SOURCE_BLENDER); a plain compositor node by its node type (IMAGERLAYER_SOURCE_NODE, which
res/description/tcyclescamera.h reserved for it). The menu shows each by its label (a group's name, a
node's name in Blender's Add menu), in alphabetical order.

Steps and sliders. The table says how the tab shows each number: its step and the range of its slider
(none when both are 0). For Blender's own sockets the rule is the tab's since F11 step 2 (a factor and an
angle step by 0.01 and by a degree, other numbers by 0.1; a slider between Blender's limits for a factor,
and for a number bounded on both sides within SLIDER_SPAN); PLAIN_NODES may give a socket's range itself
where the node's declared range is not readable from Python.

Numbers stored in scene files. A layer of the Imager tab keeps a setting's value under the setting's
number (res/description/tcyclescamera.h, "The Imager tab"), which is made from Blender's own
identifier of the socket and so stays what it is for as long as Blender keeps the socket:
"Socket_<n>" is 1000 + n; any other identifier is 10000 + a hash of it. Two settings of one effect
with the same number stop the tool. The effect itself is stored by Blender's name of the group.
"""

import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(HERE, "..", "plugin", "cyclesrender", "source", "tags", "imager_effects_table.h")
INSTALL_CPP = os.path.join(HERE, "..", "plugin", "cyclesrender", "source", "session", "blender_install.cpp")

# The supported Blender versions, oldest first: the same as SUPPORTED_VERSIONS in the plugin's
# session/blender_install.cpp (checked when the tool runs) and VERSIONS in tools/make_nodes.py.
VERSIONS = ["5.0", "5.1", "5.2", "5.3"]
BLENDER = r"C:\Program Files\Blender Foundation\Blender {}\blender.exe"

# Blender's socket types -> the kind the plugin shows and sends: f a number, i a whole number, b a
# switch, c a colour (sent as r, g, b), v a vector (two or three numbers, as many as the socket
# takes), m a menu (sent as the option's name).
KINDS = {
    "NodeSocketFloat": "f", "NodeSocketFloatFactor": "f", "NodeSocketFloatAngle": "f", "NodeSocketFloatDistance": "f",
    "NodeSocketFloatPercentage": "f", "NodeSocketFloatUnsigned": "f",
    "NodeSocketInt": "i", "NodeSocketIntUnsigned": "i", "NodeSocketIntFactor": "i", "NodeSocketIntPercentage": "i",
    "NodeSocketBool": "b", "NodeSocketColor": "c", "NodeSocketMenu": "m",
}
# Blender's subtypes that change how a number is shown: F a factor (a slider between its limits),
# A an angle (radians, shown in degrees), N a plain number.
SUBTYPES = {"FACTOR": "F", "ANGLE": "A", "PERCENTAGE": "F"}
# A limit beyond this is no limit (Blender's is the largest float).
NO_LIMIT = 1.0e30
# The widest range a number is shown as a slider over (as tools/make_nodes.py's SLIDER_SPAN): wider ones keep a field.
SLIDER_SPAN = 100.0
ONE_DEGREE = 0.017453292519943295

# Plain compositor nodes offered as effects, by node type, with what the table cannot read from Blender.
# The Exposure node (0.3.0, F13 W6): Blender declares its Exposure input from -10 to 10, but the socket's
# property reports no limit and the node takes any value (measured on 5.0, 5.2 and 5.3: 12 stops give
# 4096 times the picture, internal/f13/probe_exposure_blender.py). Blender's view has the same stops as
# its Exposure (`ColorManagedViewSettings.exposure`: limits -32 to 32, a slider -10 to 10, steps of
# 0.01): the layer takes those, so that a value typed beyond the slider's end is kept, as on the Cycles
# Camera tag's Color tab.
PLAIN_NODES = {
    "CompositorNodeExposure": {"catalog": "Compositing-Color",
                               "settings": {"Exposure": {"min": -32.0, "max": 32.0, "slider": (-10.0, 10.0), "step": 0.01}}},
    # The Glare node (0.3.0, F13): Bloom, Ghosts, Streaks and the rest, in every supported version with the same
    # sockets (measured on 5.0 and 5.3). Its kernel inputs take a picture wired in, which a layer cannot give.
    "CompositorNodeGlare": {"catalog": "Compositing-Filter", "exclude": ["Kernel Data Type", "Float Kernel", "Color Kernel"]},
}

# Nodes the table reads something of without offering them: the options of a menu a built effect takes over.
HELPER_NODES = ["CompositorNodeFilter"]

# Effects the plugin builds of several of Blender's nodes (scene/imager_built.cpp; the layer's source
# IMAGERLAYER_SOURCE_BUILT). Their settings are the plugin's own and are written here: identifier, label, kind
# (Blender's socket type, as the dump names them), default, limits, the slider and step where wanted, and for a
# menu its options, or ("options_from": node type, socket identifier, the options taken) a menu of a node
# above, so that an option a Blender lacks says so. Every node the builder uses must exist in every version
# ("nodes", checked). The identifiers are stored in scene files (as numbers made from them): never rename one.
BUILT_EFFECTS = {
    "Edges": {
        "label": "Edges", "catalog": "Compositing-Filter",
        "description": "Lines where the depth, the normals or the picture change, in a colour over the picture",
        "nodes": ["CompositorNodeFilter", "ShaderNodeMath", "ShaderNodeVectorMath", "ShaderNodeMapRange", "CompositorNodeDilateErode",
                  "CompositorNodeRGBToBW", "ShaderNodeMix"],
        "settings": [
            {"identifier": "Source", "name": "Source", "kind": "NodeSocketMenu", "default": "Depth and Normal",
             "options": ["Depth and Normal", "Depth", "Normal", "Picture"],
             "description": "What the lines follow: a change of distance (outlines), of the surface's direction (creases), both, or of the "
                            "picture's brightness. The IPR with Cycles always follows the picture"},
            {"identifier": "Filter", "name": "Filter", "kind": "NodeSocketMenu", "default": "Sobel",
             "options_from": ["CompositorNodeFilter", "Type", ["Sobel", "Prewitt", "Kirsch", "Laplace"]],
             "description": "Blender's edge filter that finds the changes"},
            {"identifier": "Threshold", "name": "Threshold", "kind": "NodeSocketFloat", "default": 1.0, "min": 0.0, "max": 100.0,
             "slider": (0.0, 4.0), "step": 0.01,
             "description": "How large a change makes a line: 1 is a step of 10 % in distance, a crease of 30 degrees or a step of 25 % in brightness"},
            {"identifier": "Width", "name": "Width", "kind": "NodeSocketInt", "default": 0, "min": 0, "max": 50, "slider": (0, 10), "step": 1,
             "description": "Pixels the lines are widened by"},
            {"identifier": "Color", "name": "Color", "kind": "NodeSocketColor", "default": [0.0, 0.0, 0.0, 1.0],
             "description": "The lines' colour"},
            {"identifier": "Strength", "name": "Strength", "kind": "NodeSocketFloatFactor", "default": 1.0, "min": 0.0, "max": 1.0,
             "description": "How much of the lines' colour covers the picture"},
        ],
    },
    # Redshift's camera tone mapping, measured exact (docs/reference/c4d-sdk.md, "The Redshift camera's effects"):
    # per channel H x + (1 - H) x / (1 + x) with Desaturate Highlights, else on the luminance; then the saturation.
    "Tone Mapping": {
        "label": "Tone Mapping", "catalog": "Compositing-Color",
        "description": "Compresses the highlights as Redshift's camera does, then changes the saturation",
        "nodes": ["ShaderNodeMix", "ShaderNodeMath", "CompositorNodeSeparateColor", "CompositorNodeCombineColor"],
        "settings": [
            {"identifier": "Highlights", "name": "Highlights", "kind": "NodeSocketFloatFactor", "default": 0.2, "min": 0.0, "max": 1.0,
             "description": "How much of the highlights is kept: 0 compresses them fully (x / (1 + x)), 1 not at all"},
            {"identifier": "Desaturate Highlights", "name": "Desaturate Highlights", "kind": "NodeSocketBool", "default": True,
             "description": "Each channel compressed on its own (bright colours turn white); off, the colour's brightness is compressed and its hue kept"},
            {"identifier": "Saturation", "name": "Saturation", "kind": "NodeSocketFloat", "default": 0.0, "min": -1.0, "max": 1.0,
             "slider": (-1.0, 1.0), "step": 0.01, "description": "Moves every colour away from (or towards) its grey of the same brightness"},
        ],
    },
    # Redshift's camera vignetting, measured exact: every pixel times cos(angle of its ray to the camera's axis)^Strength.
    "Lens Vignetting": {
        "label": "Lens Vignetting", "catalog": "Compositing-Camera & Lens Effects",
        "description": "Darkens the picture as a lens does, by the cosine of each pixel's angle to the camera's axis to a power, as Redshift's camera Vignetting",
        "nodes": ["CompositorNodeImageCoordinates", "ShaderNodeVectorMath", "ShaderNodeMath", "ShaderNodeMix"],
        "settings": [
            {"identifier": "Strength", "name": "Strength", "kind": "NodeSocketFloat", "default": 4.0, "min": 0.0, "max": 1000.0,
             "slider": (0.0, 20.0), "step": 0.1, "description": "The power of the cosine: 4 is a lens's natural darkening, 0 none"},
            {"identifier": "Field of View", "name": "Field of View", "kind": "NodeSocketFloatAngle", "default": 0.9272952180016122,
             "min": 0.017453292519943295, "max": 3.12413936106985,
             "description": "The camera's angle of view across the picture's longer side"},
        ],
    },
    # Redshift's camera white point, measured exact: divides by the white point, keeping the brightness of grey.
    "White Balance": {
        "label": "White Balance", "catalog": "Compositing-Color",
        "description": "Makes the colour given here white, keeping the brightness of grey, as Redshift's camera white point",
        "nodes": ["ShaderNodeMix"],
        "settings": [
            {"identifier": "White Point", "name": "White Point", "kind": "NodeSocketColor", "default": [1.0, 1.0, 1.0, 1.0],
             "description": "The colour that becomes white"},
        ],
    },
    # Blender's Glare node with an intensity beyond its Strength of 1: the picture plus Intensity times what the glare
    # adds. Redshift's bloom, streak and flare are translated into it (look-alikes, measured in docs/reference/blender.md).
    "Lens Glare": {
        "label": "Lens Glare", "catalog": "Compositing-Filter",
        "description": "Blender's Glare node (bloom, ghosts, streaks and the rest) with an intensity that may exceed 1",
        "nodes": ["CompositorNodeGlare", "ShaderNodeMix"],
        "settings": [
            {"identifier": "Type", "name": "Type", "kind": "NodeSocketMenu", "default": "Bloom",
             "options_from": ["CompositorNodeGlare", "Type", ["Bloom", "Ghosts", "Streaks", "Fog Glow", "Simple Star"]],
             "description": "The kind of glare, Blender's Glare node's"},
            {"identifier": "Threshold", "name": "Threshold", "kind": "NodeSocketFloat", "default": 1.0, "min": 0.0, "max": 1000.0,
             "slider": (0.0, 10.0), "step": 0.1, "description": "The brightness from which a pixel glares"},
            {"identifier": "Smoothness", "name": "Smoothness", "kind": "NodeSocketFloatFactor", "default": 0.1, "min": 0.0, "max": 1.0,
             "description": "How softly pixels near the threshold glare"},
            {"identifier": "Size", "name": "Size", "kind": "NodeSocketFloatFactor", "default": 0.5, "min": 0.0, "max": 1.0,
             "description": "The size of a bloom or fog glow relative to the picture: 1 covers it all"},
            {"identifier": "Intensity", "name": "Intensity", "kind": "NodeSocketFloat", "default": 1.0, "min": 0.0, "max": 20.0,
             "slider": (0.0, 4.0), "step": 0.01, "description": "How much of the glare is added: 1 is the Glare node's own at full strength"},
            {"identifier": "Streaks", "name": "Streaks", "kind": "NodeSocketInt", "default": 4, "min": 1, "max": 16, "slider": (1, 16), "step": 1,
             "description": "The number of streaks"},
            {"identifier": "Streaks Angle", "name": "Streaks Angle", "kind": "NodeSocketFloatAngle", "default": 0.0,
             "description": "The angle of the first streak to the horizontal"},
            {"identifier": "Iterations", "name": "Iterations", "kind": "NodeSocketInt", "default": 3, "min": 2, "max": 5, "slider": (2, 5), "step": 1,
             "description": "The number of ghosts, or the reach of streaks"},
            {"identifier": "Fade", "name": "Fade", "kind": "NodeSocketFloatFactor", "default": 0.9, "min": 0.75, "max": 1.0,
             "description": "How slowly streaks fade out"},
            {"identifier": "Saturation", "name": "Saturation", "kind": "NodeSocketFloatFactor", "default": 1.0, "min": 0.0, "max": 1.0,
             "description": "The saturation of the glare"},
            {"identifier": "Tint", "name": "Tint", "kind": "NodeSocketColor", "default": [1.0, 1.0, 1.0, 1.0],
             "description": "The glare's colour"},
        ],
    },
}


def built_effects(nodes):
    """The built effects as the dump describes effects, for one Blender whose helper and plain nodes are `nodes`
    ({type: description}); None for one whose nodes this Blender lacks."""
    effects = []
    for name, spec in BUILT_EFFECTS.items():
        missing = [kind for kind in spec["nodes"] if kind not in nodes]
        if missing:
            sys.exit(f"{name}: Blender lacks {', '.join(missing)} (the table has no 'since' for a built effect's nodes)")
        inputs = []
        for setting in spec["settings"]:
            record = {"identifier": setting["identifier"], "name": setting["name"], "kind": setting["kind"], "panel": "",
                      "default": setting["default"], "hide_value": False, "description": setting.get("description", ""),
                      "min": setting.get("min"), "max": setting.get("max"), "subtype": "NONE"}
            if "options_from" in setting:
                kind, identifier, taken = setting["options_from"]
                offered = next(s for s in nodes[kind]["inputs"] if s["identifier"] == identifier).get("options", [])
                record["options"] = [option for option in taken if option in offered]
            elif "options" in setting:
                record["options"] = list(setting["options"])
            inputs.append(record)
        effects.append({"name": name, "label": spec["label"], "description": spec["description"], "catalog": spec["catalog"],
                        "image_in": "Image", "image_out": "Image", "outputs": [], "inputs": inputs, "source": "built"})
    return effects


def built_overrides(name):
    """A built effect's steps and sliders, as PLAIN_NODES gives a plain node's."""
    spec = BUILT_EFFECTS.get(name)
    if not spec:
        return {}
    return {s["identifier"]: {key: s[key] for key in ("min", "max", "slider", "step") if key in s} for s in spec["settings"] if "step" in s}


def c_string(text):
    return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def version_code(version):
    major, minor = version.split(".")[:2]
    return int(major) * 100 + int(minor)


def plugin_versions():
    text = open(INSTALL_CPP, encoding="utf-8").read()
    block = text[text.index("SUPPORTED_VERSIONS[]"):]
    block = block[:block.index("};")]
    return [f"{major}.{minor}" for major, minor in re.findall(r"\{\s*(\d+),\s*(\d+),", block)]


def dump_all(executables):
    """Asks each Blender for its effects: {"5.2": dump, ...}, oldest first."""
    dumps = {}
    for blender in executables:
        out = os.path.join(tempfile.gettempdir(), "cyclesrender_blender_effects_%d.json" % len(dumps))
        if os.path.exists(out):
            os.remove(out)
        subprocess.run([blender, "-b", "--factory-startup", "--offline-mode", "--python", os.path.join(HERE, "dump_blender_effects.py"), "--", out]
                       + sorted(set(PLAIN_NODES) | set(HELPER_NODES) | {k for spec in BUILT_EFFECTS.values() for k in spec["nodes"]}),
                       check=True, stdout=subprocess.DEVNULL)
        data = json.load(open(out, encoding="utf-8"))
        described = {node["name"]: node for node in data.get("nodes", [])}
        # The three sources as one list of effects; a plain node and a built effect are marked by their source.
        plain = []
        for node in data.get("nodes", []):
            if node["name"] not in PLAIN_NODES:
                continue
            node["source"] = "node"
            node["catalog"] = PLAIN_NODES[node["name"]]["catalog"]
            excluded = PLAIN_NODES[node["name"]].get("exclude", [])
            node["inputs"] = [socket for socket in node["inputs"] if socket["identifier"] not in excluded]
            plain.append(node)
        data["effects"] = data["effects"] + plain + built_effects(described)
        version = ".".join(data["blender"].split(".")[:2])
        if version in dumps:
            sys.exit(f"Two of the executables are Blender {version}.")
        dumps[version] = data
    return dict(sorted(dumps.items(), key=lambda item: version_code(item[0])))


def setting_number(identifier):
    """The number a layer stores a setting's value under (see the docstring)."""
    match = re.fullmatch(r"Socket_(\d+)", identifier)
    if match and int(match.group(1)) < 9000:
        return 1000 + int(match.group(1))
    value = 2166136261
    for byte in identifier.encode("utf-8"):
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return 10000 + value % 50000


def kind_of(socket):
    kind = socket["kind"]
    if kind.startswith("NodeSocketVector"):
        return "v"
    return KINDS.get(kind)


def numbers(value, count=4):
    values = list(value) if isinstance(value, list) else [value if isinstance(value, (bool, int, float)) else 0.0]
    values = [float(v) for v in values][:count] + [0.0] * (count - len(values))
    return "{ " + ", ".join(repr(v) for v in values) + " }"


def limit(value, fallback):
    if not isinstance(value, (int, float)) or abs(value) > NO_LIMIT:
        return fallback
    return float(value)


def number_plan(kind, subtype, low, high, override):
    """(step, slider low, slider high) of a setting; the slider (0, 0) when it has none."""
    has_low, has_high = low > -NO_LIMIT, high < NO_LIMIT
    if override and "step" in override:
        slider = override.get("slider", (0.0, 0.0))
        return float(override["step"]), float(slider[0]), float(slider[1])
    if kind in ("f", "v"):
        step = ONE_DEGREE if subtype == "A" else (0.01 if subtype == "F" else 0.1)
        if kind == "f" and has_low and has_high and (subtype == "F" or high - low <= SLIDER_SPAN):
            return step, low, high
        return step, 0.0, 0.0
    if kind == "i":
        if has_low and has_high and high - low <= SLIDER_SPAN:
            return 1.0, low, high
        return 1.0, 0.0, 0.0
    return 0.0, 0.0, 0.0


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
        sys.exit(f"Dumped Blender {', '.join(dumps)}; every supported version is needed ({', '.join(VERSIONS)}).")

    problems = []
    since = {}      # ("effect", name) / ("setting", name, identifier) / ("option", name, identifier, option) -> version
    newest = {}     # name -> (version, effect)
    present = {}    # key -> versions that have it
    for version, dump in dumps.items():
        if not dump["found"]:
            problems.append(f"Blender {version} has no {dump['file']}")
        print(f"Blender {dump['blender']}: {len(dump['effects'])} effects: " + ", ".join(e["name"] for e in dump["effects"]))
        for effect in dump["effects"]:
            keys = [("effect", effect["name"])]
            for socket in effect["inputs"]:
                keys.append(("setting", effect["name"], socket["identifier"]))
                for option in socket.get("options", ()):
                    keys.append(("option", effect["name"], socket["identifier"], option))
            for key in keys:
                since.setdefault(key, version)
                present.setdefault(key, []).append(version)
            previous = newest.get(effect["name"])
            if previous:
                old = {s["identifier"]: s for s in previous[1]["inputs"]}
                for socket in effect["inputs"]:
                    before = old.get(socket["identifier"])
                    if before and (before["default"] != socket["default"] or before["name"] != socket["name"] or before["kind"] != socket["kind"]):
                        print(f"  {effect['name']} / {socket['name']}: {previous[0]} had {before['name']!r} {before['kind']} {before['default']!r}, "
                              f"{version} has {socket['name']!r} {socket['kind']} {socket['default']!r}")
                if (previous[1]["image_in"], previous[1]["image_out"]) != (effect["image_in"], effect["image_out"]):
                    problems.append(f"{effect['name']}: the picture's sockets changed between {previous[0]} and {version}")
            newest[effect["name"]] = (version, effect)
    # The table has no "until": whatever a version has, every later one must have.
    for key, versions in present.items():
        expected = VERSIONS[VERSIONS.index(versions[0]):]
        if versions != expected:
            problems.append(f"{' / '.join(key[1:])} ({key[0]}) is in {', '.join(versions)} but not in every later version")
    for key in sorted(since, key=lambda k: (version_code(since[k]), k)):
        if since[key] != VERSIONS[0]:
            print(f"  new in {since[key]}: {key[0]} {' / '.join(key[1:])}")

    lines = ["// SPDX-License-Identifier: Apache-2.0",
             "// Generated by tools/make_effects.py from Blender " + ", ".join(d["blender"] for d in dumps.values())
             + ". Do not edit: run the script again.",
             "// Included by tags/imager_effects.cpp only. \"since\": the first Blender that has it, major * 100 + minor.",
             "// A setting's number is stored in scene files (tools/make_effects.py says how it is made).", "",
             "#ifndef IMAGER_EFFECTS_TABLE_H__", "#define IMAGER_EFFECTS_TABLE_H__", ""]
    entries = []
    for name in sorted(newest, key=lambda n: (newest[n][1].get("label") or n).lower()):
        version, effect = newest[name]
        node = effect.get("source") == "node"
        built = effect.get("source") == "built"
        overrides = PLAIN_NODES.get(name, {}).get("settings", {}) or built_overrides(name)
        symbol = re.sub(r"[^A-Z0-9]+", "", name.upper())
        rows, seen = [], {}
        for socket in effect["inputs"]:
            kind = kind_of(socket)
            if socket["hide_value"]:
                # An input that only makes sense with something wired into it (a second picture).
                print(f"  left out: {name} / {socket['name']} ({socket['identifier']}): an input to wire, not a value")
                continue
            if kind is None:
                print(f"  left out: {name} / {socket['name']} ({socket['identifier']}): a {socket['kind']}")
                continue
            number = setting_number(socket["identifier"])
            if number in seen:
                problems.append(f"{name}: {socket['identifier']} and {seen[number]} get the same number {number}")
            seen[number] = socket["identifier"]
            default = socket["default"]
            options, option_since, chosen, count = "", "", 0, 0
            if kind == "m":
                listed = socket.get("options", [])
                if not listed:
                    problems.append(f"{name} / {socket['name']}: a menu without options")
                options = "\n".join(listed)
                option_since = ",".join(str(version_code(since[("option", name, socket["identifier"], option)])) for option in listed)
                chosen = listed.index(default) if default in listed else 0
                default = 0.0
            elif kind in ("v", "c"):
                count = len(default) if isinstance(default, list) else 3
            subtype = SUBTYPES.get(socket.get("subtype") or "NONE", "N") if kind in ("f", "i", "v") else "N"
            if socket["kind"] in ("NodeSocketFloatFactor", "NodeSocketVectorFactor2D", "NodeSocketVectorFactor"):
                subtype = "F"
            if socket["kind"] == "NodeSocketFloatAngle":
                subtype = "A"
            override = overrides.get(socket["identifier"])
            low, high = limit(socket.get("min"), -NO_LIMIT), limit(socket.get("max"), NO_LIMIT)
            if override:
                low, high = float(override.get("min", low)), float(override.get("max", high))
            step, slider_low, slider_high = number_plan(kind, subtype, low, high, override)
            rows.append("\t{ " + ", ".join([
                c_string(socket["identifier"]), str(number), c_string(socket["name"]), f"'{kind}'", f"'{subtype}'", numbers(default), str(count),
                repr(low), repr(high), repr(step), repr(slider_low), repr(slider_high),
                c_string(socket["panel"]), c_string(options), c_string(option_since), str(chosen),
                c_string(socket.get("description", "")), str(version_code(since[("setting", name, socket["identifier"])]))]) + " },")
        if not rows:
            problems.append(f"{name}: an effect without a setting")
        lines += [f"static const ImagerSetting {symbol}_SETTINGS[] =", "{"] + rows + ["};", ""]
        entries.append("\t{ " + ", ".join([c_string(name), c_string(effect.get("label") or name),
                                           "IMAGERLAYER_SOURCE_NODE" if node else ("IMAGERLAYER_SOURCE_BUILT" if built else "IMAGERLAYER_SOURCE_BLENDER"),
                                           c_string(effect.get("description", "")), c_string(effect.get("catalog", "")),
                                           c_string(effect["image_in"]), c_string(effect["image_out"]),
                                           f"{symbol}_SETTINGS", f"maxon::Int32(SIZEOF({symbol}_SETTINGS) / SIZEOF({symbol}_SETTINGS[0]))", str(version_code(since[("effect", name)]))]) + " },")
    lines += ["static const ImagerEffect IMAGER_EFFECTS[] =", "{"] + entries + ["};", "", "#endif // IMAGER_EFFECTS_TABLE_H__", ""]

    if problems:
        for problem in problems:
            print("PROBLEM:", problem)
        sys.exit(f"{len(problems)} problem(s); nothing was written.")
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))
    print(f"{len(entries)} effects -> {os.path.normpath(OUTPUT)}")


main()
