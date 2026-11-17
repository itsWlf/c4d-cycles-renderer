# Releases

This folder is where release packages are built. Only this README is in the repository: the packages themselves are uploaded to the repository's Releases page, not committed.

## Layout

One subfolder per version, named after the version number:

```
release/
  0.1.0/
    CyclesRender-0.1.0-C4D2027-win64.zip   the Cinema 4D plugin, for upload (0.1.0: Cinema 4D 2027 only)
    cycles_render_bridge-0.1.0.zip         the Blender bridge (an extension), for upload
    cinema4d/cyclesrender/                 the plugin unpacked: copy this folder into Cinema 4D's plugins folder
    blender/cycles_render_bridge/          the bridge unpacked
    INSTALL.md                             the installation steps, for the release notes
  0.2.0/
    CyclesRender-0.2.0-C4D2024-2027-win64.zip   from 0.2.0 on: one package for Cinema 4D 2024 to 2027
    ...
```

Every release has both parts, built from the same sources and carrying the same version number. Each part can be added to its application as it is:

- **`cycles_render_bridge-<version>.zip`** is installed in Blender 5.0, 5.1, 5.2 or 5.3 with *Edit > Preferences > Get Extensions > Install from Disk…*. Nothing has to be set up: Blender does not even need to be running afterwards, and the extension does not need to stay enabled. The Cinema 4D plugin starts Blender with it when it renders.
- **`CyclesRender-<version>-C4D2024-2027-win64.zip`** is unpacked into a plugins folder of Cinema 4D 2024, 2025, 2026 or 2027; the same folder serves all four. It holds one build per version, `cyclesrender_2024.xdl64` to `cyclesrender_2027.xdl64`, and one `res`: each Cinema 4D loads the build of its own version and passes over the others without a message (`docs/reference/c4d-sdk.md`, "Cinema 4D versions"). On its first start the plugin finds a supported Blender (5.0 to 5.3; of several, the one the bridge is installed into, else Blender 5.2 (the long-term support release), else the newest) in its usual place and the bridge in Blender's extensions folder.

## Building a release

```powershell
.\tools\make_release.ps1 -Version 0.1.0
```

The script

1. checks that the version number stands in all three places: `plugin/cyclesrender/source/version.h`, `bridge/blender_manifest.toml` and `bridge/c4d_bridge/__init__.py`;
2. builds the plugin in Release configuration for every supported Cinema 4D version (`tools/build_plugin.ps1 -Version all`; `-SkipBuild` uses the builds that are there, and stops when one is missing);
3. writes `release/<version>/` as shown above, replacing what a previous run left there.

## Checklist for a release

1. Set the new version number in the three places named above, and add the release to `CHANGELOG.md`.
2. Run the test baseline (`.\tools\run_tests.ps1`: Cinema 4D 2027, Blender 5.2); for a major release also in each other Cinema 4D version (`-C4D 2024`, `2025`, `2026`) and on each other Blender (`-Blender 5.0`, `5.1`, `5.3`), the commands in `docs/development.md`, "Which versions, when". Every run must pass with no Blender left running.
3. Build the release (`.\tools\make_release.ps1 -Version <version>`).
4. Check the packages. First by script, in a throw-away Blender profile (the commands are in `docs/development.md`, "Releasing"). Then as a user would: install the bridge .zip in Blender, put the plugin folder into the plugins folder of each supported Cinema 4D, start it without any `g_cyclesRender…` setting, check that the console says "for Cinema 4D <its year>: module loaded", render a frame and open the IPR.
5. Commit, tag the commit `v<version>`, and create the release on GitHub with the two .zip files attached and the text of `INSTALL.md` in the notes.

## Compatibility between the two parts

Install both parts from the same release. A plugin and a bridge of different releases still work together as long as they speak the same protocol version; the plugin says so in Cinema 4D's console when the version numbers differ. When the protocol versions differ, the bridge refuses the connection and the plugin reports that.
