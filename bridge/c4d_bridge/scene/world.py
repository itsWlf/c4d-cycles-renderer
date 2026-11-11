# SPDX-License-Identifier: GPL-3.0-or-later
"""World builder: the environment the scene sits in, as a node graph."""

import bpy

from .materials import build_nodes


def build_world(scene, item, blobs):
    world = bpy.context.scene.world
    if world is None:
        world = bpy.data.worlds.new("World")
        bpy.context.scene.world = world
    if world.node_tree is None:
        world.use_nodes = True
    build_nodes(scene, world.node_tree, item.get("nodes", ()), item.get("links", ()), item.get("id", "world"))
