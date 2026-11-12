# SPDX-License-Identifier: GPL-3.0-or-later
"""Image and material builders, and the generic node-graph builder they share with the world.

The plugin describes a node graph in Blender's own terms: node type names, property names,
socket identifiers. Nothing here knows about Cinema 4D materials.
"""

import os

import bpy

from ..errors import RequestError
from . import native


def build_image(scene, item, blobs):
    path = item["path"]
    if not os.path.isfile(path):
        scene.warn(f"Texture file not found: {path}")
        scene.images[item["id"]] = None
        return
    # One image per item. The plugin sends one item per file and colour space, so one file can
    # come as two items (a texture as colour and as bump heights, or a bitmap whose colour
    # profile differs from another's); with check_existing both would share one image, and the
    # later item's colour space would apply to both.
    image = scene.images.get(item["id"])
    if image is not None and image.filepath != path:
        if image not in native.protected():
            bpy.data.images.remove(image)
        image = None
    if image is None:
        image = bpy.data.images.load(path, check_existing=False)
    colorspace = item.get("colorspace")
    if colorspace:
        try:
            image.colorspace_settings.name = colorspace
        except TypeError:
            scene.warn(f"Blender has no colour space named {colorspace!r}; {os.path.basename(path)} keeps "
                       f"{image.colorspace_settings.name!r}.")
    if "alpha_mode" in item:
        image.alpha_mode = item["alpha_mode"]
    scene.images[item["id"]] = image


def build_material(scene, item, blobs):
    if item.get("native"):
        # A Blender material stored as it is: nothing to build.
        native.build(scene, item, blobs)
        return
    material = scene.materials.get(item["id"])
    if material is None:
        material = bpy.data.materials.new(item["id"])
        scene.materials[item["id"]] = material
    if material.node_tree is None:
        material.use_nodes = True
    build_nodes(scene, material.node_tree, item.get("nodes", ()), item.get("links", ()), item["id"])
    for name, value in item.get("props", {}).items():
        _set_property(material, name, value, item["id"])


def build_nodes(scene, tree, nodes, links, owner):
    """Replace the content of `tree` by the described nodes and links.

    nodes: [{"name", "type", "props": {property: value}, "inputs": {socket: value}, "outputs": {socket: value}}]
    links: [[from node, output socket, to node, input socket]]
    A socket is named by its identifier or its visible name; an integer is its index.
    """
    tree.nodes.clear()
    made = {}
    for spec in nodes:
        try:
            node = tree.nodes.new(spec["type"])
        except RuntimeError as error:
            raise RequestError("bad_request", f"{owner!r}: unknown node type {spec['type']!r}") from error
        node.name = spec["name"]
        made[spec["name"]] = node
        # Properties first: some of them (a Mix node's data type) decide which sockets exist.
        for name, value in spec.get("props", {}).items():
            if name == "image":
                node.image = scene.images.get(value)
            elif name == "color_ramp":
                _set_ramp(node, value, f"{owner}/{spec['name']}")
            else:
                _set_property(node, name, value, f"{owner}/{spec['name']}")
        for key, value in spec.get("inputs", {}).items():
            socket = _find_socket(node.inputs, key, f"{owner}/{spec['name']}")
            _set_default(socket, _resolve(scene, value, f"{owner}/{spec['name']}"))
        # The Value, RGB and Normal nodes keep their value on an output socket.
        for key, value in spec.get("outputs", {}).items():
            _set_default(_find_socket(node.outputs, key, f"{owner}/{spec['name']}"), value)
    for from_node, from_socket, to_node, to_socket in links:
        try:
            source, target = made[from_node], made[to_node]
        except KeyError as missing:
            raise RequestError("bad_request", f"{owner!r}: a link names the unknown node {missing}") from missing
        tree.links.new(_find_socket(source.outputs, from_socket, f"{owner}/{from_node}"),
                       _find_socket(target.inputs, to_socket, f"{owner}/{to_node}"))


# Numbers that only Blender knows: {"image": "<image id>", "is": "<what>"} stands for a property of
# a loaded image. The plugin uses it where a graph must step by one texel.
_IMAGE_VALUES = {
    "texel_width": lambda image: 1.0 / max(1, image.size[0]),
    "texel_height": lambda image: 1.0 / max(1, image.size[1]),
}


def _resolve(scene, value, owner):
    """Replace image-dependent placeholders inside a socket value by numbers."""
    if isinstance(value, list):
        return [_resolve(scene, element, owner) for element in value]
    if isinstance(value, dict):
        compute = _IMAGE_VALUES.get(value.get("is"))
        if compute is None:
            raise RequestError("bad_request", f"{owner}: unknown image value {value.get('is')!r}")
        image = scene.images.get(value.get("image"))
        return compute(image) if image is not None else 0.0
    return value


def _set_ramp(node, value, owner):
    """{"interpolation": ..., "elements": [[position, r, g, b, a], ...]} for a Color Ramp node."""
    ramp = getattr(node, "color_ramp", None)
    stops = value.get("elements", ())
    if ramp is None or not stops:
        raise RequestError("bad_request", f"{owner}: a colour ramp needs a Color Ramp node and at least one stop")
    _set_property(ramp, "interpolation", value.get("interpolation", "LINEAR"), owner)
    # A new ramp has two stops, and its last one cannot be removed.
    while len(ramp.elements) > 1:
        ramp.elements.remove(ramp.elements[-1])
    for index, (position, *color) in enumerate(stops):
        element = ramp.elements[0] if index == 0 else ramp.elements.new(position)
        element.position = position
        element.color = color


def _set_property(target, name, value, owner):
    try:
        # "cycles.emission_sampling": a property of a part of the target.
        while "." in name:
            part, name = name.split(".", 1)
            target = getattr(target, part)
        setattr(target, name, value)
    except (AttributeError, TypeError, ValueError) as error:
        raise RequestError("bad_request", f"{owner}: cannot set {name!r} to {value!r} ({error})") from error


def _find_socket(sockets, key, owner):
    if isinstance(key, int):
        if 0 <= key < len(sockets):
            return sockets[key]
        raise RequestError("bad_request", f"{owner}: no socket number {key}")
    for socket in sockets:
        if socket.identifier == key:
            return socket
    for socket in sockets:
        if socket.name == key and socket.enabled:
            return socket
    raise RequestError("bad_request", f"{owner}: no socket named {key!r}")


def _set_default(socket, value):
    if not hasattr(socket, "default_value"):
        return      # a shader socket: it has no value of its own
    current = socket.default_value
    if isinstance(value, (list, tuple)) and hasattr(current, "__len__"):
        value = list(value)
        # Colours are RGBA in Blender; the plugin may send RGB.
        if len(current) == 4 and len(value) == 3:
            value.append(1.0)
        # Any other length (a 2D vector) takes as many numbers as it has.
        value = (value + [0.0] * len(current))[:len(current)]
    elif isinstance(value, (list, tuple)):
        value = value[0] if value else 0.0
    if isinstance(current, bool):
        value = bool(value)
    elif isinstance(current, int) and isinstance(value, float):
        value = int(value)
    try:
        socket.default_value = value
    except (TypeError, ValueError) as error:
        raise RequestError("bad_request", f"{socket.node.bl_idname}: socket {socket.identifier!r} cannot take {value!r} ({error})") from error
