# SPDX-License-Identifier: GPL-3.0-or-later
"""Scene builders: turn the `items` of a `scene` request into Blender data.

Each item kind has one builder function, registered in BUILDERS at the end of this file. The
plugin has already converted everything to Blender's coordinates, units and names, so a
builder only copies values. Item kinds and their fields are defined in protocol/protocol.md.
"""

import bpy

from ..errors import RequestError

# Datablock collections that hold what the plugin sends. Everything in them is removed by
# Scene.clear(), including what Blender's start-up file put there.
_OWNED_COLLECTIONS = ("objects", "meshes", "lights", "cameras", "materials", "images", "worlds", "node_groups", "actions", "volumes", "hair_curves", "pointclouds", "collections")


class Scene:
    """Blender data created for the plugin, by the plugin's item ids."""

    def __init__(self):
        self.meshes = {}
        self.objects = {}
        self.materials = {}
        self.images = {}
        self.instancers = {}
        self.volumes = {}
        self.curves = {}
        self.pointclouds = {}
        self.light_links = {}
        self.node_objects = {}      # material or world id -> (its node tree, {node name: item id}): objects nodes name (materials.py)
        self.proxies = {}           # proxy items by id (proxies.py)
        self.proxy_files = {}       # the .blend files they link
        self.compositor = None      # the compositor tree attached to the scene (compositor.py)
        self.compositor_action = None   # what the last request did to it: "built", "updated", "removed"
        self.warnings = []

    def clear(self):
        """Remove everything in the file that renders, including Blender's start-up scene."""
        # ... except what an open material editor shows (native.py).
        keep = native.protected()
        # Linked files first: a library goes with everything linked from it (proxies.py).
        proxies.clear(self)
        # The compositor's tree is a node group like the others below: the scene lets go of it first.
        compositor.before_clear(self)
        for name in _OWNED_COLLECTIONS:
            bpy.data.batch_remove([data for data in getattr(bpy.data, name) if data not in keep])
        native.after_clear()
        compositor.detach(self, removed=True)
        self.meshes.clear()
        self.objects.clear()
        self.materials.clear()
        self.images.clear()
        self.instancers.clear()
        self.volumes.clear()
        self.curves.clear()
        self.pointclouds.clear()
        self.light_links.clear()
        self.node_objects.clear()

    def apply(self, header, blobs):
        """Build every item of one request. Returns (item count, warnings)."""
        self.warnings = []
        self.compositor_action = None
        if header.get("clear"):
            self.clear()
        self.remove(header.get("remove", ()))
        items = header.get("items", [])
        # The scene's frame, which linked files play by, before anything is keyed around it.
        proxies.set_frame(self, header)
        for item in items:
            builder = BUILDERS.get(item.get("kind"))
            if builder is None:
                raise RequestError("bad_request", f"unknown scene item kind {item.get('kind')!r}")
            # Blob indices inside an item count from the item's first blob.
            first = item.get("blobs", 0)
            try:
                builder(self, item, blobs[first:] if first else blobs)
            except KeyError as missing:
                raise RequestError("bad_request",
                                   f"{item.get('kind')} {item.get('id')!r} lacks the field {missing}") from missing
        lights.apply_links(self)
        materials.resolve_objects(self)
        proxies.after_items(self, header)
        return len(items), self.warnings

    def warn(self, message):
        """Report something the user should know without failing the request."""
        self.warnings.append(message)

    def remove(self, item_ids):
        """Remove items by id, whatever their kind. Unknown ids are ignored."""
        # Objects first: removing a mesh that an object still uses would take the object along.
        for item_id in item_ids:
            instancers.remove_instancer(self, item_id)
            proxies.remove_proxy(self, item_id)
            compositor.remove_compositor(self, item_id)
            self._remove_object(item_id)
        for item_id in item_ids:
            native.forget(item_id)
            self.node_objects.pop(item_id, None)
            for collection, store in ((bpy.data.meshes, self.meshes), (bpy.data.materials, self.materials),
                                      (bpy.data.images, self.images), (bpy.data.volumes, self.volumes),
                                      (bpy.data.hair_curves, self.curves)):
                data = store.pop(item_id, None)
                if data is not None and data not in native.protected():
                    collection.remove(data)
            # A point cloud is a mesh in Blender 5.0 (pointclouds.py).
            data = self.pointclouds.pop(item_id, None)
            if data is not None:
                pointclouds.remove_data(data)

    def _remove_object(self, item_id):
        """Remove the object of an item, with the light or camera data only it uses."""
        old = self.objects.pop(item_id, None)
        if old is None:
            return
        data = old.data
        cameras.remove_shear_parent(old)
        bpy.data.objects.remove(old)
        # Lights and cameras belong to their object; meshes are items of their own.
        if isinstance(data, bpy.types.Light):
            bpy.data.lights.remove(data)
        elif isinstance(data, bpy.types.Camera):
            bpy.data.cameras.remove(data)

    def link(self, item_id, data):
        """Create or replace the object that carries `data` and put it in the scene."""
        old = self.objects.get(item_id)
        if old is not None and old.data == data:
            return old
        self._remove_object(item_id)
        obj = bpy.data.objects.new(item_id, data)
        bpy.context.scene.collection.objects.link(obj)
        self.objects[item_id] = obj
        return obj


from . import cameras, compositor, curves, instancers, lights, materials, meshes, native, objects, pointclouds, proxies, volumes, world  # noqa: E402

BUILDERS = {
    "mesh": meshes.build_mesh,
    "points": meshes.build_points,
    "object": objects.build_object,
    "empty": objects.build_empty,
    "instancer": instancers.build_instancer,
    "proxy": proxies.build_proxy,
    "volume": volumes.build_volume,
    "curves": curves.build_curves,
    "pointcloud": pointclouds.build_pointcloud,
    "camera": cameras.build_camera,
    "light": lights.build_light,
    "image": materials.build_image,
    "material": materials.build_material,
    "world": world.build_world,
    "compositor": compositor.build_compositor,
}
