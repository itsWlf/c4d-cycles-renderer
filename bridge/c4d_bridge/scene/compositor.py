# SPDX-License-Identifier: GPL-3.0-or-later
"""The `compositor` item: effects on the rendered picture, built as the scene's compositor tree.

The plugin describes the tree as it describes a material: nodes and links in Blender's own names
(protocol/protocol.md, "The compositor"). Two things go beyond a material's graph:

- A node may name a `group`: a node group of Blender's bundled asset library (its compositor
  "essentials", the effects Blender's own Add menu offers). It is appended from Blender's own file
  when first used and removed again with the tree, together with the groups it brought along.
- An item sent again with the same nodes, properties and links only has its values set. Blender
  then shows the change at the next redraw without starting the render's samples anew (measured,
  docs/reference/blender.md, "The compositor"); another tree is attached only when the structure
  differs.
- `view_layer` names render passes the tree reads (view layer properties such as `use_pass_z`):
  they are switched on before the tree is built (the Render Layers node has an output only for a
  pass that is on) and kept on while the tree is attached, also when a `settings` request switches
  them off (`after_settings`); with the tree they go back to what they were.

The final driver and the viewport driver read `active()` to know whether a composite exists.
Nothing here knows about Cinema 4D.
"""

import json
import os

import bpy

from ..errors import RequestError
from . import materials

TREE_NAME = "c4d_compositor"
# Blender's bundled compositor groups, below its data files folder.
ASSET_FILE = os.path.join("assets", "nodes", "compositing_nodes_essentials.blend")


class _State:
    """What the bridge has attached to the scene."""

    def __init__(self, item_id, tree, signature, appended, precision, item=None, passes=None):
        self.item_id = item_id
        self.tree = tree
        self.signature = signature      # the structure the tree was built from
        self.appended = appended        # {name of a node group appended for it: the asset asked for, or None}
        self.precision = precision      # the scene's compositor precision before it was attached
        self.item = item                # the item it was built from (built again when a pass it reads went)
        self.passes = passes or {}      # {view layer property switched on for it: its value before}


def active(scene):
    """True while the scene's picture goes through a compositor tree of this bridge."""
    return getattr(scene, "compositor", None) is not None


def build_compositor(scene, item, blobs):
    """A `compositor` item: the tree built anew, or only its values set when its structure is the
    one already attached."""
    nodes, links = item.get("nodes", ()), item.get("links", ())
    wanted = list(item.get("view_layer", ()))
    signature = _signature(nodes, links, wanted)
    state = getattr(scene, "compositor", None)
    # The passes the tree reads, on before its nodes are made (a pass that is off has no output to link).
    passes = dict(state.passes) if state is not None else {}
    _use_passes(scene, wanted, passes)
    if state is not None and state.signature == signature and _alive(state.tree):
        state.item_id = item["id"]
        state.item = item
        state.passes = passes
        made = {node.name: node for node in state.tree.nodes}
        for spec in nodes:
            node = made.get(spec["name"])
            if node is not None:
                _set_inputs(scene, node, spec, item["id"])
        scene.compositor_action = "updated"
        return

    appended = dict(state.appended) if state is not None else {}
    precision = state.precision if state is not None else bpy.context.scene.render.compositor_precision
    tree = bpy.data.node_groups.new(TREE_NAME, "CompositorNodeTree")
    # The tree's one output is the picture.
    tree.interface.new_socket("Image", in_out="OUTPUT", socket_type="NodeSocketColor")
    made, missing = {}, set()
    for spec in nodes:
        owner = f"{item['id']}/{spec['name']}"
        group = None
        if "group" in spec:
            group = _bundled_group(spec["group"], appended)
            if group is None:
                scene.warn(f"Blender {bpy.app.version_string} has no compositor effect named \"{spec['group']}\"; "
                           "it is left out.")
                missing.add(spec["name"])
                continue
        try:
            node = tree.nodes.new(spec.get("type", "CompositorNodeGroup"))
        except RuntimeError:
            scene.warn(f"Blender {bpy.app.version_string} has no compositor node of the type \"{spec.get('type')}\"; "
                       "it is left out.")
            missing.add(spec["name"])
            continue
        node.name = spec["name"]
        made[spec["name"]] = node
        if group is not None:
            node.node_tree = group
        for name, value in spec.get("props", {}).items():
            materials._set_property(node, name, value, owner)
        _set_inputs(scene, node, spec, item["id"])

    # A node that is left out is passed over: what it would have given is what its first input got.
    passed = {}
    for from_node, from_socket, to_node, _to_socket in links:
        if to_node in missing and to_node not in passed:
            passed[to_node] = (from_node, from_socket)
    for from_node, from_socket, to_node, to_socket in links:
        if to_node in missing:
            continue
        while from_node in missing:
            if from_node not in passed:
                break
            from_node, from_socket = passed[from_node]
        if from_node in missing:
            continue
        try:
            source, target = made[from_node], made[to_node]
        except KeyError as unknown:
            raise RequestError("bad_request", f"{item['id']!r}: a link names the unknown node {unknown}") from unknown
        tree.links.new(materials._find_socket(source.outputs, from_socket, f"{item['id']}/{from_node}"),
                       materials._find_socket(target.inputs, to_socket, f"{item['id']}/{to_node}"))

    blender_scene = bpy.context.scene
    blender_scene.compositing_node_group = tree
    blender_scene.render.use_compositing = True
    # The viewport's compositor calculates with half floats unless told otherwise, the final
    # frame's with whole ones: with FULL the two pictures agree (measured).
    blender_scene.render.compositor_precision = 'FULL'
    if state is not None and _alive(state.tree) and state.tree != tree:
        bpy.data.node_groups.remove(state.tree)
    scene.compositor = _State(item["id"], tree, signature, appended, precision, item, passes)
    scene.compositor_action = "built"
    _remove_unused_groups(scene.compositor)


def _view_layer():
    return bpy.context.view_layer or bpy.context.scene.view_layers[0]


def _use_passes(scene, wanted, passes):
    """Switch on the view layer properties in `wanted`; `passes` records each one this switched on
    (with its value before) and loses the ones no longer wanted, which go back to that value."""
    layer = _view_layer()
    for name in list(passes):
        if name not in wanted:
            try:
                setattr(layer, name, passes.pop(name))
            except (AttributeError, TypeError):
                pass
    for name in wanted:
        try:
            current = getattr(layer, name)
        except AttributeError:
            scene.warn(f"Blender {bpy.app.version_string} has no render pass \"{name}\" for the compositor; the effect that reads it gets nothing.")
            continue
        if not current:
            setattr(layer, name, True)
            passes.setdefault(name, False)


def after_settings(scene):
    """A `settings` request has set the view layer's passes: those the tree reads are switched on
    again, and the tree is built anew when one had gone (its links to it went with it). A pass the
    settings switch on themselves is theirs from now on."""
    state = getattr(scene, "compositor", None)
    if state is None or not state.item or not state.item.get("view_layer"):
        return False
    layer = _view_layer()
    gone = False
    for name in state.item["view_layer"]:
        try:
            current = getattr(layer, name)
        except AttributeError:
            continue
        if current and name not in state.passes:
            continue
        if current:
            # On through the settings now: no longer the tree's to switch off.
            state.passes.pop(name, None)
            continue
        gone = True
    if not gone:
        return False
    state.signature = None
    build_compositor(scene, state.item, ())
    return True


def remove_compositor(scene, item_id):
    """Take the compositor away again when `item_id` is its item. Leaves no node group behind."""
    state = getattr(scene, "compositor", None)
    if state is None or state.item_id != item_id:
        return
    detach(scene)


def detach(scene, removed=False):
    """No compositor: the scene's picture is the render again. With `removed` the datablocks are
    gone already (the scene was cleared) and only the scene's settings are put back."""
    state = getattr(scene, "compositor", None)
    scene.compositor = None
    if state is None:
        return
    scene.compositor_action = "removed"
    blender_scene = bpy.context.scene
    blender_scene.compositing_node_group = None
    try:
        blender_scene.render.compositor_precision = state.precision
    except TypeError:
        pass
    _use_passes(scene, (), state.passes)
    if removed:
        return
    if _alive(state.tree):
        bpy.data.node_groups.remove(state.tree)
    state.tree = None
    _remove_unused_groups(state)


def before_clear(scene):
    """Scene.clear() removes every node group: let go of the tree first, so that the scene never
    points at a removed one."""
    if getattr(scene, "compositor", None) is not None:
        bpy.context.scene.compositing_node_group = None


def _alive(tree):
    """False once a tree was removed behind the bridge's back (a cleared scene)."""
    try:
        return tree is not None and tree.name in bpy.data.node_groups and bpy.data.node_groups[tree.name] == tree
    except ReferenceError:
        return False


def _signature(nodes, links, passes=()):
    """What decides whether a tree can be kept: its nodes with their types, groups and properties,
    its links and the passes it reads. The values of its inputs do not count."""
    return json.dumps([[[spec.get("name"), spec.get("type"), spec.get("group"), spec.get("props", {})] for spec in nodes],
                       [list(link) for link in links], sorted(passes)], sort_keys=True)


def _set_inputs(scene, node, spec, item_id):
    """The values of a node's unlinked inputs. A value Blender does not take (a menu option this
    version lacks) is named and Blender's own stays."""
    owner = f"{item_id}/{spec['name']}"
    for key, value in spec.get("inputs", {}).items():
        try:
            socket = materials._find_socket(node.inputs, key, owner)
            materials._set_default(socket, value)
        except RequestError as error:
            scene.warn(f"The compositor effect \"{spec.get('group') or spec.get('type')}\" does not take {value!r} for \"{key}\" "
                       f"in Blender {bpy.app.version_string}; Blender's own value is used. ({error})")


def _bundled_group(name, appended):
    """The node group of that name from Blender's own asset file, appended when it is not there
    yet; None when this Blender ships none of that name. `appended` maps the name of every node
    group appended so far to the asset it was asked for as (None for one that came along)."""
    for data_name, asset in appended.items():
        if asset == name and data_name in bpy.data.node_groups:
            return bpy.data.node_groups[data_name]
    path = os.path.join(bpy.utils.system_resource('DATAFILES'), ASSET_FILE)
    if not os.path.isfile(path):
        return None
    before = set(bpy.data.node_groups)
    with bpy.data.libraries.load(path, link=False, assets_only=True) as (source, target):
        if name not in source.node_groups:
            return None
        target.node_groups = [name]
    group = target.node_groups[0]
    if group is None:
        return None
    # The group may bring others along (the ones it uses): all of them go again with the tree.
    for data in bpy.data.node_groups:
        if data not in before:
            # An asset keeps itself alive (a fake user); here it lives only as long as the tree uses it.
            if data.asset_data is not None:
                data.asset_clear()
            data.use_fake_user = False
            appended[data.name] = name if data == group else None
    return group


def _remove_unused_groups(state):
    """Remove the appended groups nothing uses any more, also those only a removed group used."""
    while True:
        unused = [bpy.data.node_groups[name] for name in sorted(state.appended)
                  if name in bpy.data.node_groups and bpy.data.node_groups[name].users == 0]
        if not unused:
            break
        for group in unused:
            del state.appended[group.name]
            bpy.data.node_groups.remove(group)
    state.appended = {name: asset for name, asset in state.appended.items() if name in bpy.data.node_groups}
