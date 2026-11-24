# SPDX-License-Identifier: GPL-3.0-or-later
"""The file Blender writes itself from a final frame: the `render` request's `file` field.

Blender's File Output node, put into the compositor's tree for the one render and taken out
again, writes a multilayer OpenEXR with the passes asked for, Cryptomatte with its manifest among
them, from the same render whose pixels the final driver hands back. Measured on 5.0 to 5.3,
Cycles and Eevee (docs/reference/blender.md, "Files Blender writes"): the picture is the same to
the bit with and without the node, the file's picture equals it, and the node costs the writing
of the file. With a tree of the `compositor` item the file's `Combined` is the composite, as the
picture is; without one a tree that only passes the render on is attached for the render.

The node is given a temporary name in the file's folder and the file is moved to the path asked
for afterwards: Blender would read "#" in a name as the frame number and "{...}" as a template.
"""

import os
import uuid

import bpy

# The Render Layers' output for the picture; every other pass's output has the pass's name.
PICTURE = "Combined"
PICTURE_OUTPUT = "Image"
# Asked for by these names, written as Blender's layers of each (CryptoObject00, 01, ...).
CRYPTOMATTE = ("CryptoObject", "CryptoMaterial", "CryptoAsset")
_ITEM_TYPES = {'VALUE': 'FLOAT', 'VECTOR': 'VECTOR', 'RGBA': 'RGBA'}


class FileOutput:
    """One render's File Output node: attach() before the render, finish() after it, always detach()."""

    def __init__(self, scene, spec):
        self.scene = scene
        self.path = os.path.abspath(str(spec["path"]))
        self.depth = str(spec.get("depth", "32"))
        self.codec = str(spec.get("codec", "ZIP"))
        self.passes = [str(name) for name in spec.get("passes", [PICTURE])]
        self.warnings = []
        self.written = []
        self._tree = None
        self._own_tree = False
        self._added = []
        self._compositing = None
        self._temporary = None

    # ------------------------------------------------------------------------------------------
    def attach(self):
        scene = self.scene
        folder = os.path.dirname(self.path)
        try:
            os.makedirs(folder, exist_ok=True)
        except OSError as error:
            self.warnings.append(f"The file \"{self.path}\" cannot be written: {error}")
            return False
        tree = scene.compositing_node_group if scene.render.use_compositing else None
        self._compositing = scene.render.use_compositing
        if tree is None:
            # Nothing composites: a tree that passes the render on, for this render only.
            tree = bpy.data.node_groups.new("c4d_bridge_files", "CompositorNodeTree")
            tree.interface.new_socket("Image", in_out="OUTPUT", socket_type="NodeSocketColor")
            layers = tree.nodes.new("CompositorNodeRLayers")
            output = tree.nodes.new("NodeGroupOutput")
            tree.links.new(layers.outputs[PICTURE_OUTPUT], output.inputs[0])
            self._previous_tree = scene.compositing_node_group
            scene.compositing_node_group = tree
            scene.render.use_compositing = True
            self._own_tree = True
        self._tree = tree
        layers = next((node for node in tree.nodes if node.bl_idname == "CompositorNodeRLayers"), None)
        if layers is None:
            layers = tree.nodes.new("CompositorNodeRLayers")
            self._added.append(layers)
        # The picture as the tree ends: the composite where there are effects.
        picture = layers.outputs[PICTURE_OUTPUT]
        output = next((node for node in tree.nodes if node.bl_idname == "NodeGroupOutput"), None)
        if output is not None and output.inputs and output.inputs[0].is_linked:
            picture = output.inputs[0].links[0].from_socket

        node = tree.nodes.new("CompositorNodeOutputFile")
        self._added.append(node)
        self._set_format(node.format)
        # "#" is the frame number in a file name and "{...}" a template; the folder's braces are
        # doubled, the name is one of the bridge's own and the file is moved afterwards.
        node.directory = folder.replace("{", "{{").replace("}", "}}") + os.sep
        token = "c4d_bridge_" + uuid.uuid4().hex
        node.file_name = token
        if hasattr(node, "use_file_extension"):     # from 5.2; before, the extension is always added
            node.use_file_extension = True
        self._temporary = os.path.join(folder, token + ".exr")

        for name in self.passes:
            sources = self._sources(layers, picture, name)
            if not sources:
                self.warnings.append(f"The pass \"{name}\" is not available in this render engine; the file \"{os.path.basename(self.path)}\" is written without it.")
                continue
            for item_name, socket in sources:
                item = node.file_output_items.new(_ITEM_TYPES.get(socket.type, 'RGBA'), item_name)
                dimensions = getattr(socket, "dimensions", None)
                if dimensions and hasattr(item, "vector_socket_dimensions"):
                    item.vector_socket_dimensions = dimensions
                tree.links.new(socket, node.inputs[item_name])
                self.written.append(item_name)
        return True

    def _set_format(self, settings):
        settings.media_type = 'MULTI_LAYER_IMAGE'
        settings.file_format = 'OPEN_EXR_MULTILAYER'
        for prop, value, label in (("color_depth", self.depth, "colour depth"), ("exr_codec", self.codec, "codec")):
            try:
                setattr(settings, prop, value)
            except TypeError:
                self.warnings.append(f"Blender {bpy.app.version_string} does not offer the {label} \"{value}\" for OpenEXR; "
                                     f"the file is written with \"{getattr(settings, prop)}\".")
        # The values as rendered, in the working colour space, as the picture the plugin gets.
        try:
            settings.linear_colorspace_settings.name = bpy.data.colorspace.working_space
        except (AttributeError, TypeError):
            pass

    def _sources(self, layers, picture, name):
        """[(layer name in the file, output socket)] for one pass asked for."""
        if name == PICTURE:
            return [(PICTURE, picture)]
        if name in CRYPTOMATTE:
            return [(socket.name, socket) for socket in layers.outputs if socket.enabled and socket.name.startswith(name)]
        socket = layers.outputs.get(name)
        return [(name, socket)] if socket is not None and socket.enabled else []

    # ------------------------------------------------------------------------------------------
    def finish(self):
        """After the render: the file moved to its path. Returns the path, or None."""
        if self._temporary is None:
            return None
        if not os.path.exists(self._temporary):
            self.warnings.append(f"Blender wrote no file for \"{self.path}\".")
            return None
        try:
            os.replace(self._temporary, self.path)
        except OSError as error:
            self.warnings.append(f"The file \"{self.path}\" could not be written: {error}")
            return None
        self._temporary = None
        return self.path

    def detach(self):
        """Take out everything attach() put in, also after a failed render."""
        tree = self._tree
        if tree is not None:
            for node in self._added:
                try:
                    tree.nodes.remove(node)
                except (ReferenceError, RuntimeError):
                    pass
            if self._own_tree:
                self.scene.compositing_node_group = self._previous_tree
                self.scene.render.use_compositing = self._compositing
                bpy.data.node_groups.remove(tree)
        self._tree = None
        self._added = []
        if self._temporary and os.path.exists(self._temporary):
            try:
                os.remove(self._temporary)
            except OSError:
                pass
