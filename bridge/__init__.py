# SPDX-License-Identifier: GPL-3.0-or-later
"""Cycles Render Bridge: the Blender side of the Cycles Render plugin for Cinema 4D.

Blender sees this folder as an extension (blender_manifest.toml), and this file is the add-on
it loads. Installing the extension is all a user does. That puts the package `c4d_bridge`
into Blender's extensions folder, where the Cinema 4D plugin finds it and starts a Blender of
its own with `c4d_bridge/main.py` as start-up script.

That Blender runs with factory settings, so this add-on is not even loaded there. It exists
to tell someone who finds it in Blender's Preferences what it is, and to offer a button that
uninstalls it (for an update: uninstall, then install the new .zip). Enabling or disabling it
changes nothing, and it must not import `c4d_bridge`: the bridge is started by its script,
not by Blender's add-on system.
"""

import bpy

# "bl_ext.<repository>.<extension id>": where Blender put this extension.
_REPOSITORY, _EXTENSION_ID = (__package__.split(".") + ["", ""])[1:3]


def uninstall(repository=_REPOSITORY, extension_id=_EXTENSION_ID):
    """Remove this extension from Blender, files and all, as the Uninstall entry of Blender's
    own extension list does. Returns None, so that it can run as a timer.

    Called through a timer and not from the button itself: the operator behind the button is
    part of the code that is removed.
    """
    repositories = bpy.context.preferences.extensions.repos
    index = next((i for i, repo in enumerate(repositories) if repo.module == repository), -1)
    if index < 0:
        print(f"Cycles Render Bridge: the extension repository {repository!r} was not found; nothing was uninstalled.")
        return None
    window = bpy.context.window_manager.windows[0] if bpy.context.window_manager.windows else None
    if window is not None:
        with bpy.context.temp_override(window=window):
            bpy.ops.extensions.package_uninstall(repo_index=index, pkg_id=extension_id)
    else:
        bpy.ops.extensions.package_uninstall(repo_index=index, pkg_id=extension_id)
    return None


class CYCLESRENDERBRIDGE_OT_uninstall(bpy.types.Operator):
    """Remove the Cycles Render Bridge from Blender. Cinema 4D cannot render with Cycles Render until it is installed again"""

    bl_idname = "cycles_render_bridge.uninstall"
    bl_label = "Uninstall Cycles Render Bridge"
    bl_options = {"INTERNAL"}

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)

    def execute(self, context):
        bpy.app.timers.register(uninstall, first_interval=0.1)
        return {"FINISHED"}


class CyclesRenderBridgePreferences(bpy.types.AddonPreferences):
    """What the user sees when unfolding the add-on in Blender's Preferences."""

    bl_idname = __package__

    def draw(self, context):
        column = self.layout.column()
        column.label(text="The Blender side of Cycles Render, a render engine plugin for Cinema 4D.")
        column.label(text="There is nothing to set up here: Cinema 4D starts Blender when it renders.")
        column.label(text="Keep the extension installed. It works whether or not it is enabled.")
        column.separator()
        column.label(text="To update: uninstall here, then install the new .zip with Install from Disk.")
        column.operator(CYCLESRENDERBRIDGE_OT_uninstall.bl_idname, text="Uninstall", icon="TRASH")


_CLASSES = (CYCLESRENDERBRIDGE_OT_uninstall, CyclesRenderBridgePreferences)


def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
