# Releases

This folder is where release packages are built. Only this README is in the repository: the packages themselves are uploaded to the repository's Releases page, not committed.

## Layout

One subfolder per version, named after the version number:

```
release/
  0.1.0/
    CyclesRender-0.1.0-C4D2027-win64.zip   the Cinema 4D plugin, for upload
    cycles_render_bridge-0.1.0.zip         the Blender bridge (an extension), for upload
    cinema4d/cyclesrender/                 the plugin unpacked: copy this folder into Cinema 4D's plugins folder
    blender/cycles_render_bridge/          the bridge unpacked
    INSTALL.md                             the installation steps, for the release notes
  0.2.0/
    ...
```

Every release has both parts, built from the same sources and carrying the same version number. Each part can be added to its application as it is:

- **`cycles_render_bridge-<version>.zip`** is installed in Blender 5.2 with *Edit > Preferences > Get Extensions > Install from Disk…*. Nothing has to be set up: Blender does not even need to be running afterwards, and the extension does not need to stay enabled. The Cinema 4D plugin starts Blender with it when it renders.
- **`CyclesRender-<version>-C4D2027-win64.zip`** is unpacked into a plugins folder of Cinema 4D 2027. On its first start the plugin finds Blender 5.2 in its usual place and the bridge in Blender's extensions folder.

## Building a release

```powershell
.\tools\make_release.ps1 -Version 0.1.0
```

The script

1. checks that the version number stands in all three places: `plugin/cyclesrender/source/version.h`, `bridge/blender_manifest.toml` and `bridge/c4d_bridge/__init__.py`;
2. builds the plugin in Release configuration (`-SkipBuild` uses the build that is there);
3. writes `release/<version>/` as shown above, replacing what a previous run left there.

## Checklist for a release

1. Set the new version number in the three places named above, and add the release to `CHANGELOG.md`.
2. Run the test baseline (`.\tools\run_tests.ps1`); it must pass with no Blender left running.
3. Build the release (`.\tools\make_release.ps1 -Version <version>`).
4. Check the packages. First by script, in a throw-away Blender profile (the commands are in `docs/development.md`, "Releasing"). Then as a user would: install the bridge .zip in Blender, put the plugin folder into Cinema 4D's plugins folder, start Cinema 4D without any `g_cyclesRender…` setting, render a frame and open the IPR.
5. Commit, tag the commit `v<version>`, and create the release on GitHub with the two .zip files attached and the text of `INSTALL.md` in the notes.

## Compatibility between the two parts

Install both parts from the same release. A plugin and a bridge of different releases still work together as long as they speak the same protocol version; the plugin says so in Cinema 4D's console when the version numbers differ. When the protocol versions differ, the bridge refuses the connection and the plugin reports that.
