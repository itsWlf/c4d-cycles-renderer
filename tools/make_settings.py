# SPDX-License-Identifier: Apache-2.0
"""Writes the Render Settings page of Cycles Render from the one definition below.

    python tools\\make_settings.py

Generated (do not edit by hand; edit PAGE below and run this again):
    plugin/cyclesrender/res/description/vpcyclesrender.h       parameter ids
    plugin/cyclesrender/res/description/vpcyclesrender.res     layout of the page
    plugin/cyclesrender/res/strings_en-US/description/vpcyclesrender.str
    plugin/cyclesrender/source/videopost/render_settings_table.h   what each parameter sets in Blender

The page mimics Blender's Render properties: Blender's panels as foldable groups, Blender's
names and Blender's defaults (read from a new scene in Blender 5.2.3). Parameter ids are
stored in scene files: never renumber one and never reuse a retired number.
Retired: 1900 (a test colour), 2003 (the old Eevee tab), 2190 to 2192 (the Color Settings tab).

Each entry:
    B/I/F(id, SYMBOL, label, default, path, ...)   a checkbox, whole number or number
    C(id, SYMBOL, label, default index, [(SUFFIX, label, Blender identifier), ...], path)
    group(id, SYMBOL, label, [entries], open=..., engine=...)   a foldable group; top level: a tab
    row(a, b)                                       two entries side by side
Options:
    path2    a second Blender property that takes the same value
    engine   "CYCLES" or "EEVEE": shown for that engine only
    enable   (SYMBOL, value): greyed out unless that parameter has that value
    path=None: not sent as a property; the plugin reads it (GetExportSettings)
    advanced True: left out by the Simple layout (the switch at the top of the first tab)
"""

import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "plugin", "cyclesrender")


def _entry(kind, id, symbol, label, default, path=None, **options):
    entry = {"kind": kind, "id": id, "symbol": symbol, "label": label, "default": default, "path": path,
             "path2": None, "engine": None, "enable": None, "min": None, "max": None, "step": None, "unit": None, "advanced": False}
    unknown = set(options) - set(entry) - {"options"}
    assert not unknown, unknown
    entry.update(options)
    return entry


def B(id, symbol, label, default, path=None, **options):
    return _entry("BOOL", id, symbol, label, default, path, **options)


def I(id, symbol, label, default, path=None, **options):  # noqa: E743
    return _entry("INT", id, symbol, label, default, path, **options)


def F(id, symbol, label, default, path=None, **options):
    return _entry("FLOAT", id, symbol, label, default, path, **options)


def C(id, symbol, label, default, choices, path=None, **options):
    return _entry("CHOICE", id, symbol, label, default, path, options=choices, **options)


def color(id, symbol, label):
    return {"kind": "COLOR", "id": id, "symbol": symbol, "label": label, "engine": None, "enable": None}


def text(id, symbol, label):
    return {"kind": "TEXT", "id": id, "symbol": symbol, "label": label, "engine": None, "enable": None}


def group(id, symbol, label, children, open=True, engine=None, advanced=False):
    return {"kind": "GROUP", "id": id, "symbol": symbol, "label": label, "children": children, "open": open,
            "engine": engine, "enable": None, "advanced": advanced}


def row(*children):
    return {"kind": "ROW", "children": list(children)}


RESOLUTIONS = [("1", "1:1", "1"), ("2", "1:2", "2"), ("4", "1:4", "4"), ("8", "1:8", "8"), ("16", "1:16", "16")]
PASSES_OPTIONS = [("NONE", "None", "RGB"), ("ALBEDO", "Albedo", "RGB_ALBEDO"), ("ALBEDO_NORMAL", "Albedo and Normal", "RGB_ALBEDO_NORMAL")]
PREFILTERS = [("NONE", "None", "NONE"), ("FAST", "Fast", "FAST"), ("ACCURATE", "Accurate", "ACCURATE")]
QUALITIES = [("HIGH", "High", "HIGH"), ("BALANCED", "Balanced", "BALANCED"), ("FAST", "Fast", "FAST")]

# The render passes: checkboxes only; their table (Blender's pass names) is in render_settings_map.cpp.
# A fourth element names the one engine that has the pass: the page hides it for the other.
DATA_PASSES = [(1700, "DEPTH", "Depth"), (1701, "MIST", "Mist"), (1702, "POSITION", "Position"), (1703, "NORMAL", "Normal"), (1704, "UV", "UV", "CYCLES"),
               (1717, "VECTOR", "Vector (Motion)"), (1718, "OBJECT_INDEX", "Object Index", "CYCLES"), (1725, "MATERIAL_INDEX", "Material Index", "CYCLES"),
               (1724, "SAMPLE_COUNT", "Sample Count", "CYCLES")]
LIGHT_PASSES = [(1705, "DIFFUSE_DIRECT", "Diffuse Direct"), (1706, "DIFFUSE_INDIRECT", "Diffuse Indirect", "CYCLES"), (1707, "DIFFUSE_COLOR", "Diffuse Color"),
                (1708, "GLOSSY_DIRECT", "Glossy Direct"), (1709, "GLOSSY_INDIRECT", "Glossy Indirect", "CYCLES"), (1710, "GLOSSY_COLOR", "Glossy Color"),
                (1711, "TRANSMISSION_DIRECT", "Transmission Direct", "CYCLES"), (1712, "TRANSMISSION_INDIRECT", "Transmission Indirect", "CYCLES"),
                (1713, "TRANSMISSION_COLOR", "Transmission Color", "CYCLES"), (1720, "VOLUME_DIRECT", "Volume Direct"),
                (1721, "VOLUME_INDIRECT", "Volume Indirect", "CYCLES"), (1714, "EMISSION", "Emission"), (1715, "ENVIRONMENT", "Environment"),
                (1716, "AO", "Ambient Occlusion"), (1722, "SHADOW", "Shadow", "EEVEE"), (1723, "TRANSPARENT", "Transparent", "EEVEE"),
                (1719, "SHADOW_CATCHER", "Shadow Catcher", "CYCLES")]


def passes(entries):
    return [{"kind": "PASS", "id": entry[0], "symbol": "CYCLES_PASS_" + entry[1], "label": entry[2],
             "engine": entry[3] if len(entry) > 3 else None, "enable": None}
            for entry in entries]


POOLS = [(size, size + " MB", size) for size in ("16", "32", "64", "128", "256", "512", "1024")]
ADV = True   # shorthand below: advanced=ADV marks what the Simple layout leaves out

# The layout follows the project's design mockups: per engine a Simple and an Advanced layout of
# the first tab, and the second tab. Where a mockup and Blender disagree about a default,
# Blender's default is used.
PAGE = [
    group(2100, "CYCLES_TAB_RENDER", "Render Settings", [
        {"kind": "ENGINE"},     # ids 1000 and 1001: sent as named fields, not as properties
        {"kind": "DEVICE"},
        {"kind": "LAYOUT"},     # id 1002: Simple or Advanced; changes what is shown, nothing in Blender

        # ---------------------------------------------------------------- Cycles
        group(2000, "CYCLES_GROUP_SAMPLING", "Sampling", [
            group(2110, "CYCLES_GROUP_SAMPLING_RENDER", "Render", [
                row(B(1102, "CYCLES_ADAPTIVE_SAMPLING", "Noise Threshold", True, "cycles.use_adaptive_sampling"),
                    F(1103, "CYCLES_ADAPTIVE_THRESHOLD", "Noise Threshold", 0.01, "cycles.adaptive_threshold", min=0.0, max=1.0, step=0.001,
                      enable=("CYCLES_ADAPTIVE_SAMPLING", 1))),
                row(I(1100, "CYCLES_SAMPLES", "Max Samples", 4096, "cycles.samples", min=1, max=16777216),
                    I(1104, "CYCLES_ADAPTIVE_MIN_SAMPLES", "Min Samples", 0, "cycles.adaptive_min_samples", min=0, max=4096,
                      enable=("CYCLES_ADAPTIVE_SAMPLING", 1))),
                F(1105, "CYCLES_TIME_LIMIT", "Time Limit (s)", 0.0, "cycles.time_limit", min=0.0, step=1.0),
                B(1106, "CYCLES_DENOISE", "Denoise", True, "cycles.use_denoising"),
                group(2111, "CYCLES_GROUP_DENOISE_RENDER", "Denoise Settings", [
                    C(1108, "CYCLES_DENOISER", "Denoiser", 0, [("OPENIMAGEDENOISE", "OpenImageDenoise", "OPENIMAGEDENOISE"), ("OPTIX", "OptiX", "OPTIX")],
                      "cycles.denoiser", enable=("CYCLES_DENOISE", 1)),
                    C(1109, "CYCLES_DENOISE_PASSES", "Passes", 2, PASSES_OPTIONS, "cycles.denoising_input_passes", enable=("CYCLES_DENOISE", 1), advanced=ADV),
                    C(1110, "CYCLES_DENOISE_PREFILTER", "Prefilter", 2, PREFILTERS, "cycles.denoising_prefilter", enable=("CYCLES_DENOISE", 1), advanced=ADV),
                    C(1111, "CYCLES_DENOISE_QUALITY", "Quality", 0, QUALITIES, "cycles.denoising_quality", enable=("CYCLES_DENOISE", 1)),
                    B(1112, "CYCLES_DENOISE_GPU", "Use GPU", False, "cycles.denoising_use_gpu", enable=("CYCLES_DENOISE", 1), advanced=ADV),
                ], open=False),
            ]),
            group(2112, "CYCLES_GROUP_SAMPLING_VIEWPORT", "Viewport", [
                row(B(1113, "CYCLES_IPR_ADAPTIVE_SAMPLING", "Noise Threshold", True, "cycles.use_preview_adaptive_sampling"),
                    F(1114, "CYCLES_IPR_ADAPTIVE_THRESHOLD", "Noise Threshold", 0.1, "cycles.preview_adaptive_threshold", min=0.0, max=1.0, step=0.001,
                      enable=("CYCLES_IPR_ADAPTIVE_SAMPLING", 1))),
                row(I(1101, "CYCLES_IPR_SAMPLES", "Max Samples", 1024, "cycles.preview_samples", min=1, max=16777216),
                    I(1115, "CYCLES_IPR_ADAPTIVE_MIN_SAMPLES", "Min Samples", 0, "cycles.preview_adaptive_min_samples", min=0, max=4096,
                      enable=("CYCLES_IPR_ADAPTIVE_SAMPLING", 1))),
                B(1107, "CYCLES_IPR_DENOISE", "Denoise", False, "cycles.use_preview_denoising"),
                group(2113, "CYCLES_GROUP_DENOISE_VIEWPORT", "Denoise Settings", [
                    C(1116, "CYCLES_IPR_DENOISER", "Denoiser", 0,
                      [("AUTO", "Automatic", "AUTO"), ("OPENIMAGEDENOISE", "OpenImageDenoise", "OPENIMAGEDENOISE"), ("OPTIX", "OptiX", "OPTIX")],
                      "cycles.preview_denoiser", enable=("CYCLES_IPR_DENOISE", 1)),
                    C(1117, "CYCLES_IPR_DENOISE_PASSES", "Passes", 1, PASSES_OPTIONS, "cycles.preview_denoising_input_passes", enable=("CYCLES_IPR_DENOISE", 1),
                      advanced=ADV),
                    C(1118, "CYCLES_IPR_DENOISE_PREFILTER", "Prefilter", 1, PREFILTERS, "cycles.preview_denoising_prefilter", enable=("CYCLES_IPR_DENOISE", 1),
                      advanced=ADV),
                    C(1119, "CYCLES_IPR_DENOISE_QUALITY", "Quality", 1, QUALITIES, "cycles.preview_denoising_quality", enable=("CYCLES_IPR_DENOISE", 1)),
                    row(B(1121, "CYCLES_IPR_DENOISE_GPU", "Use GPU", True, "cycles.preview_denoising_use_gpu", enable=("CYCLES_IPR_DENOISE", 1), advanced=ADV),
                        I(1120, "CYCLES_IPR_DENOISE_START", "Start Sample", 1, "cycles.preview_denoising_start_sample", min=0, max=16777216,
                          enable=("CYCLES_IPR_DENOISE", 1), advanced=ADV)),
                ], open=False),
            ]),
            group(2114, "CYCLES_GROUP_GUIDING", "Path Guiding", [
                B(1122, "CYCLES_GUIDING", "Path Guiding (CPU only)", False, "cycles.use_guiding"),
                I(1123, "CYCLES_GUIDING_TRAINING_SAMPLES", "Training Samples", 128, "cycles.guiding_training_samples", min=0, max=1000000,
                  enable=("CYCLES_GUIDING", 1)),
                row(B(1124, "CYCLES_GUIDING_SURFACE", "Surface", True, "cycles.use_surface_guiding", enable=("CYCLES_GUIDING", 1)),
                    B(1125, "CYCLES_GUIDING_VOLUME", "Volume", True, "cycles.use_volume_guiding", enable=("CYCLES_GUIDING", 1))),
            ], open=False, advanced=ADV),
            group(2115, "CYCLES_GROUP_LIGHTS", "Lights", [
                B(1126, "CYCLES_LIGHT_TREE", "Light Tree", True, "cycles.use_light_tree"),
                F(1127, "CYCLES_LIGHT_THRESHOLD", "Light Threshold", 0.01, "cycles.light_sampling_threshold", min=0.0, max=1.0, step=0.001),
            ], open=False, advanced=ADV),
            group(2116, "CYCLES_GROUP_SAMPLING_ADVANCED", "Advanced", [
                row(I(1128, "CYCLES_SEED", "Seed", 0, "cycles.seed", min=0, max=2147483647),
                    B(1129, "CYCLES_ANIMATED_SEED", "Animate Seed", False, "cycles.use_animated_seed")),
                C(1130, "CYCLES_SAMPLING_PATTERN", "Pattern", 0,
                  [("AUTOMATIC", "Automatic", "AUTOMATIC"), ("TABULATED_SOBOL", "Tabulated Sobol", "TABULATED_SOBOL"), ("BLUE_NOISE", "Blue-Noise", "BLUE_NOISE")],
                  "cycles.sampling_pattern"),
                F(1131, "CYCLES_SCRAMBLING_DISTANCE", "Scrambling Distance", 1.0, "cycles.scrambling_distance", min=0.0, max=1.0, step=0.01),
                row(B(1132, "CYCLES_SCRAMBLING_AUTO", "Automatic", False, "cycles.auto_scrambling_distance"),
                    B(1133, "CYCLES_SCRAMBLING_VIEWPORT", "Viewport", False, "cycles.preview_scrambling_distance")),
                row(I(1134, "CYCLES_MIN_LIGHT_BOUNCES", "Min Light Bounces", 0, "cycles.min_light_bounces", min=0, max=1024),
                    I(1135, "CYCLES_MIN_TRANSPARENT_BOUNCES", "Min Transparent Bounces", 0, "cycles.min_transparent_bounces", min=0, max=1024)),
            ], open=False, advanced=ADV),
        ], engine="CYCLES"),

        group(2001, "CYCLES_GROUP_LIGHT_PATHS", "Light Paths", [
            group(2120, "CYCLES_GROUP_BOUNCES", "Max Bounces", [
                I(1200, "CYCLES_MAX_BOUNCES", "Total", 12, "cycles.max_bounces", min=0, max=1024),
                row(I(1201, "CYCLES_DIFFUSE_BOUNCES", "Diffuse", 4, "cycles.diffuse_bounces", min=0, max=1024),
                    I(1202, "CYCLES_GLOSSY_BOUNCES", "Glossy", 4, "cycles.glossy_bounces", min=0, max=1024)),
                row(I(1203, "CYCLES_TRANSMISSION_BOUNCES", "Transmission", 12, "cycles.transmission_bounces", min=0, max=1024),
                    I(1204, "CYCLES_VOLUME_BOUNCES", "Volume", 0, "cycles.volume_bounces", min=0, max=1024)),
                I(1205, "CYCLES_TRANSPARENT_BOUNCES", "Transparent", 8, "cycles.transparent_max_bounces", min=0, max=1024),
            ]),
            group(2121, "CYCLES_GROUP_CLAMPING", "Clamping", [
                row(F(1206, "CYCLES_CLAMP_DIRECT", "Direct Light", 0.0, "cycles.sample_clamp_direct", min=0.0, step=0.1),
                    F(1207, "CYCLES_CLAMP_INDIRECT", "Indirect Light", 10.0, "cycles.sample_clamp_indirect", min=0.0, step=0.1)),
            ], open=False, advanced=ADV),
            group(2122, "CYCLES_GROUP_CAUSTICS", "Caustics", [
                F(1210, "CYCLES_FILTER_GLOSSY", "Filter Glossy", 1.0, "cycles.blur_glossy", min=0.0, max=10.0, step=0.01),
                row(B(1208, "CYCLES_CAUSTICS_REFLECTIVE", "Reflective", True, "cycles.caustics_reflective"),
                    B(1209, "CYCLES_CAUSTICS_REFRACTIVE", "Refractive", True, "cycles.caustics_refractive")),
            ], open=False, advanced=ADV),
            group(2123, "CYCLES_GROUP_FAST_GI", "Fast GI Approximation", [
                B(1211, "CYCLES_FAST_GI", "Fast GI Approximation", False, "cycles.use_fast_gi"),
                C(1212, "CYCLES_FAST_GI_METHOD", "Method", 0, [("REPLACE", "Replace", "REPLACE"), ("ADD", "Add", "ADD")], "cycles.fast_gi_method",
                  enable=("CYCLES_FAST_GI", 1)),
                row(I(1213, "CYCLES_FAST_GI_BOUNCES_VIEWPORT", "Viewport Bounces", 1, "cycles.ao_bounces", min=0, max=1024, enable=("CYCLES_FAST_GI", 1)),
                    I(1214, "CYCLES_FAST_GI_BOUNCES_RENDER", "Render Bounces", 1, "cycles.ao_bounces_render", min=0, max=1024, enable=("CYCLES_FAST_GI", 1))),
            ], open=False, advanced=ADV),
        ], engine="CYCLES"),

        group(2130, "CYCLES_GROUP_VOLUMES", "Volumes", [
            B(1803, "CYCLES_VOLUME_BIASED", "Biased", False, "cycles.volume_biased"),
            row(F(1800, "CYCLES_VOLUME_STEP_RATE", "Step Rate Render", 1.0, "cycles.volume_step_rate", min=0.1, max=10.0, step=0.1, advanced=ADV),
                F(1801, "CYCLES_VOLUME_STEP_RATE_VIEWPORT", "Viewport", 1.0, "cycles.volume_preview_step_rate", min=0.1, max=10.0, step=0.1, advanced=ADV)),
            I(1802, "CYCLES_VOLUME_MAX_STEPS", "Max Steps", 1024, "cycles.volume_max_steps", min=2, max=65536),
        ], engine="CYCLES"),

        # They act on adaptive subdivision, which the plugin does not export yet (it sends the
        # mesh Cinema 4D has subdivided): shown because the mockup has them, without effect so far.
        group(2131, "CYCLES_GROUP_SUBDIVISION", "Subdivisions", [
            row(F(1830, "CYCLES_DICING_RATE", "Dicing Rate Render", 1.0, "cycles.dicing_rate", min=0.5, max=1000.0, step=0.5),
                F(1831, "CYCLES_DICING_RATE_VIEWPORT", "Viewport", 8.0, "cycles.preview_dicing_rate", min=0.5, max=1000.0, step=0.5)),
            F(1832, "CYCLES_DICING_OFFSCREEN", "Offscreen Scale", 4.0, "cycles.offscreen_dicing_scale", min=1.0, max=25.0, step=0.5),
            I(1833, "CYCLES_MAX_SUBDIVISIONS", "Max Subdivisions", 12, "cycles.max_subdivisions", min=0, max=16),
        ], engine="CYCLES", advanced=ADV),

        # ---------------------------------------------------------------- Eevee
        group(2140, "EEVEE_GROUP_SAMPLING", "Sampling", [
            group(2152, "EEVEE_GROUP_SAMPLING_RENDER", "Render", [
                I(1400, "EEVEE_SAMPLES", "Samples", 64, "eevee.taa_render_samples", min=1, max=100000),
            ]),
            group(2153, "EEVEE_GROUP_SAMPLING_VIEWPORT", "Viewport", [
                I(1401, "EEVEE_IPR_SAMPLES", "Samples", 16, "eevee.taa_samples", min=1, max=100000),
                row(B(1404, "EEVEE_VIEWPORT_DENOISING", "Temporal Reprojection", True, "eevee.use_taa_reprojection", advanced=ADV),
                    B(1427, "EEVEE_JITTERED_SHADOWS", "Jittered Shadows", False, "eevee.use_shadow_jitter_viewport", advanced=ADV)),
            ]),
            group(2141, "EEVEE_GROUP_SHADOWS", "Shadows", [
                B(1403, "EEVEE_SHADOWS", "Shadows", True, "eevee.use_shadows"),
                row(I(1405, "EEVEE_SHADOW_RAYS", "Rays", 1, "eevee.shadow_ray_count", min=1, max=4, enable=("EEVEE_SHADOWS", 1)),
                    I(1406, "EEVEE_SHADOW_STEPS", "Steps", 6, "eevee.shadow_step_count", min=1, max=16, enable=("EEVEE_SHADOWS", 1))),
                row(B(1428, "EEVEE_VOLUME_SHADOWS", "Volume Shadows", False, "eevee.use_volumetric_shadows"),
                    I(1429, "EEVEE_VOLUME_SHADOW_STEPS", "Steps", 16, "eevee.volumetric_shadow_samples", min=1, max=128, enable=("EEVEE_VOLUME_SHADOWS", 1))),
                F(1407, "EEVEE_SHADOW_RESOLUTION", "Resolution", 1.0, "eevee.shadow_resolution_scale", min=0.0, max=1.0, step=0.01,
                  enable=("EEVEE_SHADOWS", 1)),
            ]),
            group(2154, "EEVEE_GROUP_SAMPLING_ADVANCED", "Advanced", [
                F(1408, "EEVEE_LIGHT_THRESHOLD", "Light Threshold", 0.01, "eevee.light_threshold", min=0.0, max=1.0, step=0.001),
            ], advanced=ADV),
        ], engine="EEVEE"),
        group(2146, "EEVEE_GROUP_LIGHT_PATHS", "Light Paths", [
            group(2147, "EEVEE_GROUP_INTENSITY", "Intensity", [
                row(F(1430, "EEVEE_DIRECT_INTENSITY", "Direct Light", 1.0, "eevee.direct_light_intensity", min=0.0, max=3.0, step=0.05),
                    F(1431, "EEVEE_INDIRECT_INTENSITY", "Indirect Light", 1.0, "eevee.indirect_light_intensity", min=0.0, max=3.0, step=0.05)),
            ]),
            group(2142, "EEVEE_GROUP_CLAMPING", "Clamping - Surface", [
                row(F(1409, "EEVEE_CLAMP_SURFACE_DIRECT", "Direct Light", 0.0, "eevee.clamp_surface_direct", min=0.0, step=0.1),
                    F(1410, "EEVEE_CLAMP_SURFACE_INDIRECT", "Indirect Light", 10.0, "eevee.clamp_surface_indirect", min=0.0, step=0.1)),
            ], advanced=ADV),
            group(2148, "EEVEE_GROUP_CLAMPING_VOLUME", "Clamping - Volume", [
                row(F(1411, "EEVEE_CLAMP_VOLUME_DIRECT", "Direct Light", 0.0, "eevee.clamp_volume_direct", min=0.0, step=0.1),
                    F(1412, "EEVEE_CLAMP_VOLUME_INDIRECT", "Indirect Light", 0.0, "eevee.clamp_volume_indirect", min=0.0, step=0.1)),
            ], advanced=ADV),
        ], engine="EEVEE"),
        group(2145, "EEVEE_GROUP_VOLUMES", "Volumes", [
            C(1423, "EEVEE_VOLUME_RESOLUTION", "Resolution", 3, RESOLUTIONS, "eevee.volumetric_tile_size"),
            row(I(1424, "EEVEE_VOLUME_STEPS", "Steps", 64, "eevee.volumetric_samples", min=1, max=256),
                F(1425, "EEVEE_VOLUME_DISTRIBUTION", "Distribution", 0.8, "eevee.volumetric_sample_distribution", min=0.0, max=1.0, step=0.01, advanced=ADV)),
            I(1426, "EEVEE_VOLUME_MAX_DEPTH", "Max Depth", 16, "eevee.volumetric_ray_depth", min=1, max=16),
            # Distances of Blender's scene, which is in metres whatever the document's unit is.
            group(2149, "EEVEE_GROUP_VOLUME_RANGE", "Custom Range", [
                B(1432, "EEVEE_VOLUME_CUSTOM_RANGE", "Custom Range", False, "eevee.use_volume_custom_range"),
                row(F(1433, "EEVEE_VOLUME_START", "Start (m)", 0.1, "eevee.volumetric_start", min=0.001, step=0.1, enable=("EEVEE_VOLUME_CUSTOM_RANGE", 1)),
                    F(1434, "EEVEE_VOLUME_END", "End (m)", 100.0, "eevee.volumetric_end", min=0.001, step=1.0, enable=("EEVEE_VOLUME_CUSTOM_RANGE", 1))),
            ], advanced=ADV),
        ], engine="EEVEE"),
        group(2143, "EEVEE_GROUP_RAYTRACING", "Raytracing", [
            B(1402, "EEVEE_RAYTRACING", "Raytracing", False, "eevee.use_raytracing"),
            C(1413, "EEVEE_RAYTRACING_METHOD", "Method", 1, [("PROBE", "Light Probe", "PROBE"), ("SCREEN", "Screen-Trace", "SCREEN")],
              "eevee.ray_tracing_method", enable=("EEVEE_RAYTRACING", 1)),
            C(1414, "EEVEE_RAYTRACING_RESOLUTION", "Resolution", 1, RESOLUTIONS, "eevee.ray_tracing_options.resolution_scale",
              enable=("EEVEE_RAYTRACING", 1)),
            group(2150, "EEVEE_GROUP_SCREEN_TRACING", "Screen Tracing", [
                row(F(1435, "EEVEE_SCREEN_PRECISION", "Precision", 0.25, "eevee.ray_tracing_options.screen_trace_quality", min=0.0, max=1.0, step=0.01,
                      enable=("EEVEE_RAYTRACING", 1)),
                    F(1436, "EEVEE_SCREEN_THICKNESS", "Thickness (m)", 0.1, "eevee.ray_tracing_options.screen_trace_thickness", min=0.001, step=0.05,
                      enable=("EEVEE_RAYTRACING", 1))),
                row(B(1437, "EEVEE_BACKFACE_HIT", "Backface", True, "eevee.ray_tracing_options.use_backface_hit", enable=("EEVEE_RAYTRACING", 1)),
                    F(1438, "EEVEE_BACKFACE_SCALE", "Backface", 0.25, "eevee.ray_tracing_options.backface_radiance_scale", min=0.0, max=1.0, step=0.01,
                      enable=("EEVEE_BACKFACE_HIT", 1))),
            ], advanced=ADV),
            group(2151, "EEVEE_GROUP_RAY_DENOISING", "Denoising", [
                B(1416, "EEVEE_RAYTRACING_DENOISE", "Denoise", True, "eevee.ray_tracing_options.use_denoise", enable=("EEVEE_RAYTRACING", 1)),
                row(B(1439, "EEVEE_DENOISE_SPATIAL", "Spatial Reuse", True, "eevee.ray_tracing_options.denoise_spatial", enable=("EEVEE_RAYTRACING_DENOISE", 1),
                      advanced=ADV),
                    B(1440, "EEVEE_DENOISE_TEMPORAL", "Temporal Accumulation", True, "eevee.ray_tracing_options.denoise_temporal",
                      enable=("EEVEE_RAYTRACING_DENOISE", 1), advanced=ADV)),
                B(1441, "EEVEE_DENOISE_BILATERAL", "Bilateral Filter", True, "eevee.ray_tracing_options.denoise_bilateral",
                  enable=("EEVEE_RAYTRACING_DENOISE", 1), advanced=ADV),
            ]),
            group(2144, "EEVEE_GROUP_FAST_GI", "Fast GI Approximation", [
                B(1442, "EEVEE_FAST_GI", "Fast GI Approximation", True, "eevee.use_fast_gi", enable=("EEVEE_RAYTRACING", 1)),
                C(1417, "EEVEE_FAST_GI_METHOD", "Method", 1,
                  [("AO", "Ambient Occlusion", "AMBIENT_OCCLUSION_ONLY"), ("GI", "Global Illumination", "GLOBAL_ILLUMINATION")],
                  "eevee.fast_gi_method", enable=("EEVEE_RAYTRACING", 1)),
                F(1415, "EEVEE_RAYTRACING_MAX_ROUGHNESS", "Threshold", 0.5, "eevee.ray_tracing_options.trace_max_roughness", min=0.0, max=1.0, step=0.01,
                  enable=("EEVEE_RAYTRACING", 1), advanced=ADV),
                C(1418, "EEVEE_FAST_GI_RESOLUTION", "Resolution", 1, RESOLUTIONS, "eevee.fast_gi_resolution", enable=("EEVEE_RAYTRACING", 1)),
                row(I(1419, "EEVEE_FAST_GI_RAYS", "Rays", 2, "eevee.fast_gi_ray_count", min=1, max=16, enable=("EEVEE_RAYTRACING", 1)),
                    I(1420, "EEVEE_FAST_GI_STEPS", "Steps", 8, "eevee.fast_gi_step_count", min=1, max=64, enable=("EEVEE_RAYTRACING", 1))),
                F(1421, "EEVEE_FAST_GI_PRECISION", "Precision", 0.25, "eevee.fast_gi_quality", min=0.0, max=1.0, step=0.01, enable=("EEVEE_RAYTRACING", 1),
                  advanced=ADV),
                row(F(1443, "EEVEE_FAST_GI_DISTANCE", "Distance (m)", 0.0, "eevee.fast_gi_distance", min=0.0, max=100.0, step=0.1, enable=("EEVEE_RAYTRACING", 1),
                      advanced=ADV),
                    F(1444, "EEVEE_FAST_GI_THICKNESS", "Thickness (m)", 0.1, "eevee.fast_gi_thickness_near", min=0.0, max=100.0, step=0.05,
                      enable=("EEVEE_RAYTRACING", 1), advanced=ADV)),
                F(1422, "EEVEE_FAST_GI_BIAS", "Bias", 0.05, "eevee.fast_gi_bias", min=0.0, max=0.5, step=0.01, enable=("EEVEE_RAYTRACING", 1), advanced=ADV),
            ]),
        ], engine="EEVEE"),
    ]),

    group(2004, "CYCLES_GROUP_SCENE", "General Settings", [
        group(2002, "CYCLES_GROUP_FILM", "Film", [
            F(1302, "CYCLES_FILM_EXPOSURE", "Exposure", 1.0, "cycles.film_exposure", min=0.0, max=1024.0, step=0.01, engine="CYCLES"),
            B(1300, "CYCLES_TRANSPARENT_FILM", "Transparent", False, "render.film_transparent"),
            row(B(1304, "CYCLES_TRANSPARENT_GLASS", "Transparent Glass", False, "cycles.film_transparent_glass", engine="CYCLES",
                  enable=("CYCLES_TRANSPARENT_FILM", 1)),
                F(1305, "CYCLES_TRANSPARENT_ROUGHNESS", "Roughness Threshold", 0.1, "cycles.film_transparent_roughness", min=0.0, max=1.0, step=0.01,
                  engine="CYCLES", enable=("CYCLES_TRANSPARENT_GLASS", 1))),
            F(1308, "EEVEE_FILTER_SIZE", "Filter Size (px)", 1.5, "render.filter_size", min=0.01, max=10.0, step=0.1, engine="EEVEE"),
            row(B(1306, "EEVEE_OVERSCAN", "Overscan", False, "eevee.use_overscan", engine="EEVEE"),
                F(1307, "EEVEE_OVERSCAN_SIZE", "Overscan (%)", 3.0, "eevee.overscan_size", min=0.0, max=10.0, step=0.5, engine="EEVEE",
                  enable=("EEVEE_OVERSCAN", 1))),
            group(2161, "CYCLES_GROUP_PIXEL_FILTER", "Pixel Filter", [
                C(1303, "CYCLES_FILTER_TYPE", "Type", 2,
                  [("BOX", "Box", "BOX"), ("GAUSSIAN", "Gaussian", "GAUSSIAN"), ("BLACKMAN_HARRIS", "Blackman-Harris", "BLACKMAN_HARRIS")],
                  "cycles.pixel_filter_type"),
                F(1301, "CYCLES_FILTER_WIDTH", "Width (px)", 1.5, "cycles.filter_width", min=0.01, max=10.0, step=0.1),
            ], engine="CYCLES"),
        ]),
        group(2165, "EEVEE_GROUP_DOF", "Depth of Field", [
            F(1445, "EEVEE_BOKEH_MAX_SIZE", "Max Size (px)", 100.0, "eevee.bokeh_max_size", min=0.0, max=200.0, step=1.0),
            row(F(1446, "EEVEE_BOKEH_THRESHOLD", "Sprite Threshold", 1.0, "eevee.bokeh_threshold", min=0.0, max=10.0, step=0.1),
                F(1447, "EEVEE_BOKEH_NEIGHBOR", "Neighbor Rejection", 10.0, "eevee.bokeh_neighbor_max", min=0.0, max=40.0, step=0.5)),
            row(B(1448, "EEVEE_BOKEH_JITTER", "Jitter Camera", False, "eevee.use_bokeh_jittered"),
                F(1449, "EEVEE_BOKEH_OVERBLUR", "Over-Blur (%)", 5.0, "eevee.bokeh_overblur", min=0.0, max=20.0, step=0.5, enable=("EEVEE_BOKEH_JITTER", 1))),
        ], engine="EEVEE"),
        # The shutter and its position are not sent: the plugin reads the scene at the moments the
        # shutter opens and closes (cycles_videopost.cpp).
        group(2005, "CYCLES_GROUP_MOTION_BLUR", "Motion Blur", [
            B(1600, "CYCLES_MOTION_BLUR", "Motion Blur", False, "render.use_motion_blur"),
            C(1604, "CYCLES_MOTION_POSITION", "Position", 1, [("START", "Start on Frame", "START"), ("CENTER", "Center on Frame", "CENTER"), ("END", "End on Frame", "END")],
              None, enable=("CYCLES_MOTION_BLUR", 1)),
            F(1601, "CYCLES_MOTION_SHUTTER", "Shutter (frames)", 0.5, None, min=0.01, max=2.0, step=0.05, enable=("CYCLES_MOTION_BLUR", 1)),
            C(1605, "CYCLES_ROLLING_SHUTTER", "Rolling Shutter", 0, [("NONE", "None", "NONE"), ("TOP", "Top-Bottom", "TOP")], "cycles.rolling_shutter_type",
              engine="CYCLES", enable=("CYCLES_MOTION_BLUR", 1)),
            F(1606, "CYCLES_ROLLING_DURATION", "Rolling Duration", 0.1, "cycles.rolling_shutter_duration", min=0.0, max=1.0, step=0.01, engine="CYCLES",
              enable=("CYCLES_ROLLING_SHUTTER", 1)),
            F(1607, "EEVEE_MOTION_BLEEDING", "Bleeding Bias", 100.0, "eevee.motion_blur_depth_scale", min=0.01, max=1000.0, step=1.0, engine="EEVEE",
              enable=("CYCLES_MOTION_BLUR", 1)),
            row(I(1608, "EEVEE_MOTION_MAX_BLUR", "Max Blur (px)", 32, "eevee.motion_blur_max", min=0, max=512, engine="EEVEE", enable=("CYCLES_MOTION_BLUR", 1)),
                I(1603, "EEVEE_MOTION_BLUR_STEPS", "Steps", 1, "eevee.motion_blur_steps", min=1, max=64, engine="EEVEE", enable=("CYCLES_MOTION_BLUR", 1))),
        ]),
        group(2181, "CYCLES_GROUP_CURVES", "Curves", [
            C(1810, "CYCLES_CURVES_SHAPE", "Shape", 0,
              [("RIBBONS", "Rounded Ribbons", "RIBBONS"), ("THICK", "3D Curves", "THICK"), ("THICK_LINEAR", "Linear 3D Curves", "THICK_LINEAR")],
              "cycles_curves.shape", engine="CYCLES"),
            I(1811, "CYCLES_CURVES_SUBDIVISIONS", "Curve Subdivisions", 2, "cycles_curves.subdivisions", min=0, max=24, engine="CYCLES"),
            C(1450, "EEVEE_CURVES_SHAPE", "Shape", 0, [("STRAND", "Strand", "STRAND"), ("STRIP", "Strip", "STRIP"), ("CYLINDER", "Cylinder", "CYLINDER")],
              "render.hair_type", engine="EEVEE"),
            I(1451, "EEVEE_CURVES_SUBDIVISIONS", "Additional Subdivisions", 0, "render.hair_subdiv", min=0, max=3, engine="EEVEE"),
        ]),
        group(2160, "CYCLES_GROUP_PERFORMANCE", "Performance", [
            B(1452, "EEVEE_HQ_NORMALS", "High Quality Normals", False, "render.use_high_quality_normals", engine="EEVEE"),
            group(2162, "CYCLES_GROUP_THREADS", "Threads", [
                C(1820, "CYCLES_THREADS_MODE", "Threads Mode", 0, [("AUTO", "Auto-Detect", "AUTO"), ("FIXED", "Fixed", "FIXED")], "render.threads_mode"),
                I(1821, "CYCLES_THREADS", "Threads", 8, "render.threads", min=1, max=1024, enable=("CYCLES_THREADS_MODE", 1)),
            ], engine="CYCLES"),
            group(2163, "CYCLES_GROUP_MEMORY", "Memory", [
                row(B(1822, "CYCLES_AUTO_TILE", "Use Tiling", True, "cycles.use_auto_tile"),
                    I(1823, "CYCLES_TILE_SIZE", "Tile Size", 2048, "cycles.tile_size", min=8, max=8192, enable=("CYCLES_AUTO_TILE", 1))),
            ], engine="CYCLES"),
            group(2164, "CYCLES_GROUP_ACCELERATION", "Acceleration Structure", [
                row(B(1824, "CYCLES_SPATIAL_SPLITS", "Use Spatial Splits", False, "cycles.debug_use_spatial_splits"),
                    B(1825, "CYCLES_COMPACT_BVH", "Use Compact BVH", False, "cycles.debug_use_compact_bvh")),
            ], open=False, engine="CYCLES"),
            group(2166, "EEVEE_GROUP_MEMORY", "Memory", [
                C(1453, "EEVEE_SHADOW_POOL", "Shadow Pool", 5, POOLS + [("1536", "1536 MB", "1536"), ("2048", "2048 MB", "2048")], "eevee.shadow_pool_size"),
                C(1454, "EEVEE_LIGHT_PROBE_POOL", "Light Probe Pool", 0, POOLS, "eevee.gi_irradiance_pool_size"),
            ], engine="EEVEE"),
        ]),
    ]),

    # What the plugin itself needs to know to read the Cinema 4D scene. These are the plugin's own
    # settings, not Blender's, which is why they have a tab of their own.
    group(2180, "CYCLES_TAB_OBJECT", "Object Settings", [
        group(2170, "CYCLES_GROUP_CONVERSION", "Scene Conversion", [
            F(1500, "CYCLES_LIGHT_MULTIPLIER", "Light Multiplier", 1.0, None, min=0.0, step=1.0, unit="PERCENT"),
            B(1501, "CYCLES_DEPTH_OF_FIELD", "Depth of Field", False, None),
            I(1504, "CYCLES_BAKE_RESOLUTION", "Shader Bake Resolution", 1024, None, min=16, max=8192),
        ]),
        group(2171, "CYCLES_GROUP_WORLD", "Default World (without a Sky object)", [
            color(1502, "CYCLES_WORLD_COLOR", "Color"),
            F(1503, "CYCLES_WORLD_STRENGTH", "Strength", 1.0, None, min=0.0, step=0.1),
        ]),
    ]),

    group(2006, "CYCLES_GROUP_PASSES", "AOVs", [
        group(2195, "CYCLES_GROUP_PASSES_DATA", "Data", passes(DATA_PASSES)),
        group(2196, "CYCLES_GROUP_PASSES_LIGHT", "Light", passes(LIGHT_PASSES)),
    ]),
    # Directly right of the AOVs tab (the maintainer, 2026-11-12).
    # Blender's Output tab, Format panel: the resolution, its percentage and the pixel aspect, as
    # Blender shows them. Cinema 4D's Output page keeps the values that render (Width, Height,
    # Pixel Aspect): these settings are stored here and write those values, and show them back
    # when they were changed there (videopost/output_settings.cpp). Nothing of it goes to Blender
    # as a property (path None): the plugin sends the size and pixel aspect with each frame.
    group(2200, "CYCLES_TAB_OUTPUT", "Output", [
        group(2201, "CYCLES_GROUP_FORMAT", "Format", [
            row(I(1850, "CYCLES_OUTPUT_RESOLUTION_X", "Resolution X", 1920, None, min=1, max=65536),
                I(1851, "CYCLES_OUTPUT_RESOLUTION_Y", "Y", 1080, None, min=1, max=65536)),
            row(I(1852, "CYCLES_OUTPUT_PERCENTAGE", "%", 100, None, min=1, max=1000),
                {"kind": "TEXT", "id": 1855, "symbol": "CYCLES_OUTPUT_RESULT", "label": "", "engine": None, "enable": None}),
            row(F(1853, "CYCLES_OUTPUT_ASPECT_X", "Aspect X", 1.0, None, min=1.0, max=200.0, step=0.1),
                F(1854, "CYCLES_OUTPUT_ASPECT_Y", "Y", 1.0, None, min=1.0, max=200.0, step=0.1)),
            text(1856, "CYCLES_OUTPUT_NOTE", "Width, Height and Pixel Aspect of Cinema 4D's Output page, in Blender's terms."),
        ]),
    ]),

]


# ------------------------------------------------------------------------------------------------
def walk(entries):
    for entry in entries:
        yield entry
        if entry["kind"] in ("GROUP", "ROW"):
            yield from walk(entry["children"])


def write(path, content):
    with open(os.path.join(ROOT, path), "w", encoding="utf-8", newline="\n") as file:
        file.write(content)


def number(value):
    return repr(float(value))


def shown_for(entry):
    """For which engine and layout an entry shows: entries that differ cannot share a grid."""
    if entry["kind"] == "ROW":
        return shown_for(entry["children"][0])
    return entry.get("engine"), bool(entry.get("advanced"))


def group_body(children, depth):
    """The settings of a group, laid out as one grid of two columns per run of settings.

    All rows of a run share the grid, so labels and fields line up from row to row, as they do in
    Blender's own panels. A setting that stands alone in its row is followed by an empty cell. A run ends at
    a sub-group, at a line of text, and where the engine or the layout (Simple, Advanced) a
    setting shows for changes: hiding a cell of a grid would shift every cell after it.
    """
    tab = "\t" * depth
    out = []
    run = []

    def flush():
        if not run:
            return
        cells = ""
        for child in run:
            if child["kind"] == "ROW":
                assert len(child["children"]) == 2, "a row has two settings"
                cells += "".join(res_entry(cell, depth + 1) for cell in child["children"])
            else:
                cells += res_entry(child, depth + 1) + f"{tab}\tSTATICTEXT {{ JOINENDSCALE; }}\n"
        out.append(f"{tab}GROUP\n{tab}{{\n{tab}\tCOLUMNS 2;\n\n{cells}{tab}}}\n")
        run.clear()

    for child in children:
        # A render pass is a checkbox of its own row, and some are hidden for one engine: outside a
        # grid hiding one shifts nothing.
        if child["kind"] in ("GROUP", "TEXT", "PASS"):
            flush()
            out.append(res_entry(child, depth))
            continue
        if run and shown_for(run[-1]) != shown_for(child):
            flush()
        run.append(child)
        if child["kind"] == "ROW" and len({shown_for(cell) for cell in child["children"]}) > 1:
            # The row's second setting may be hidden while its first shows: as the grid's last
            # cell it shifts nothing.
            flush()
    flush()
    return "".join(out)


def res_entry(entry, depth):
    tab = "\t" * depth
    kind = entry["kind"]
    if kind == "ENGINE":
        return (f"{tab}LONG CYCLES_ENGINE\n{tab}{{\n{tab}\tANIM OFF;\n{tab}\tCYCLE\n{tab}\t{{\n{tab}\t\tCYCLES_ENGINE_CYCLES;\n"
                f"{tab}\t\tCYCLES_ENGINE_EEVEE;\n{tab}\t}}\n{tab}}}\n")
    if kind == "LAYOUT":
        # Two buttons side by side, as in the mockup.
        return (f"{tab}LONG CYCLES_LAYOUT\n{tab}{{\n{tab}\tANIM OFF;\n{tab}\tCUSTOMGUI QUICKTABRADIO;\n{tab}\tCYCLE\n{tab}\t{{\n"
                f"{tab}\t\tCYCLES_LAYOUT_SIMPLE;\n{tab}\t\tCYCLES_LAYOUT_ADVANCED;\n{tab}\t}}\n{tab}}}\n")
    if kind == "DEVICE":
        return (f"{tab}LONG CYCLES_DEVICE\n{tab}{{\n{tab}\tANIM OFF;\n{tab}\tCYCLE\n{tab}\t{{\n{tab}\t\tCYCLES_DEVICE_CPU;\n"
                f"{tab}\t\tCYCLES_DEVICE_GPU;\n{tab}\t}}\n{tab}}}\n")
    if kind == "GROUP":
        inner = group_body(entry["children"], depth + 1)
        # A top-level group is a tab: only the first is open, or all would show at once.
        is_open = entry["open"] and (depth > 1 or entry is PAGE[0])
        return f"\n{tab}GROUP {entry['symbol']}\n{tab}{{\n{tab}\tDEFAULT {1 if is_open else 0};\n\n{inner}{tab}}}\n"
    symbol = entry["symbol"]
    if kind in ("BOOL", "PASS"):
        return f"{tab}BOOL {symbol} {{ ANIM OFF; }}\n"
    if kind == "COLOR":
        return f"{tab}COLOR {symbol} {{ ANIM OFF; }}\n"
    if kind == "TEXT":
        return f"{tab}STATICTEXT {symbol} {{ }}\n"
    if kind == "CHOICE":
        options = "".join(f"{tab}\t\t{symbol}_{suffix};\n" for suffix, _label, _name in entry["options"])
        return f"{tab}LONG {symbol}\n{tab}{{\n{tab}\tANIM OFF;\n{tab}\tCYCLE\n{tab}\t{{\n{options}{tab}\t}}\n{tab}}}\n"
    flags = ["ANIM OFF;"]
    if entry["unit"]:
        flags.append(f"UNIT {entry['unit']};")
    for key in ("min", "max", "step"):
        if entry[key] is not None:
            value = entry[key] if kind == "INT" else number(entry[key])
            flags.append(f"{key.upper()} {value};")
    return f"{tab}{'LONG' if kind == 'INT' else 'REAL'} {symbol} {{ {' '.join(flags)} }}\n"


def main():
    entries = [entry for entry in walk(PAGE) if entry["kind"] not in ("ROW", "ENGINE", "DEVICE", "LAYOUT")]
    ids = {}
    symbols = {}
    for entry in entries:
        assert entry["id"] not in ids, ("id used twice", entry["id"], entry["symbol"], ids[entry["id"]])
        assert entry["symbol"] not in symbols, ("symbol used twice", entry["symbol"])
        assert entry["id"] not in (1000, 1001, 1002, 1900, 2003, 2190, 2191, 2192), entry["id"]
        ids[entry["id"]] = entry["symbol"]
        symbols[entry["symbol"]] = entry
    generated = "// Generated by tools/make_settings.py. Do not edit: change the definition there and run it again.\n"

    # --- ids
    lines = ["#ifndef VPCYCLESRENDER_H__", "#define VPCYCLESRENDER_H__", "", generated.rstrip(), "//",
             "// Parameter IDs of the Cycles Render settings. They are stored in scene files: never renumber,",
             "// and never reuse a retired number (1900 was a test colour, 2003 the old Eevee tab).", "enum", "{",
             "\tCYCLES_ENGINE = 1000,", "\t\tCYCLES_ENGINE_CYCLES = 0,", "\t\tCYCLES_ENGINE_EEVEE = 1,",
             "\tCYCLES_DEVICE = 1001,", "\t\tCYCLES_DEVICE_CPU = 0,", "\t\tCYCLES_DEVICE_GPU = 1,",
             "\tCYCLES_LAYOUT = 1002,", "\t\tCYCLES_LAYOUT_SIMPLE = 0,", "\t\tCYCLES_LAYOUT_ADVANCED = 1,", ""]
    for entry in sorted(entries, key=lambda e: e["id"]):
        lines.append(f"\t{entry['symbol']} = {entry['id']},")
        if entry["kind"] == "CHOICE":
            for index, (suffix, _label, _name) in enumerate(entry["options"]):
                lines.append(f"\t\t{entry['symbol']}_{suffix} = {index},")
    lines += ["", "\tCYCLES_SETTINGS_END_", "};", "", "#endif // VPCYCLESRENDER_H__", ""]
    write("res/description/vpcyclesrender.h", "\n".join(lines))

    # --- layout. The renderer's own first group stays empty, so that its tab does not show.
    body = "".join(res_entry(entry, 1) for entry in PAGE)   # the tabs; each lays out its own settings (group_body)
    write("res/description/vpcyclesrender.res",
          generated + "CONTAINER VPcyclesrender\n{\n\tNAME VPcyclesrender;\n\tINCLUDE VPbase;\n" + body + "}\n")

    # --- strings
    lines = ["STRINGTABLE VPcyclesrender", "{", '\tVPcyclesrender "Cycles Render";', "",
             '\tCYCLES_ENGINE "Render Engine";', '\tCYCLES_ENGINE_CYCLES "Cycles";', '\tCYCLES_ENGINE_EEVEE "Eevee";',
             '\tCYCLES_DEVICE "Device";', '\tCYCLES_DEVICE_CPU "CPU Compute";', '\tCYCLES_DEVICE_GPU "GPU Compute";',
             '\tCYCLES_LAYOUT "Settings";', '\tCYCLES_LAYOUT_SIMPLE "Simple Layout";', '\tCYCLES_LAYOUT_ADVANCED "Advanced Layout";', ""]
    for entry in entries:
        lines.append(f"\t{entry['symbol']} \"{entry['label']}\";")
        if entry["kind"] == "CHOICE":
            for suffix, label, _name in entry["options"]:
                lines.append(f"\t{entry['symbol']}_{suffix} \"{label}\";")
    lines += ["}", ""]
    write("res/strings_en-US/description/vpcyclesrender.str", "// Generated by tools/make_settings.py.\n" + "\n".join(lines))

    # --- the table for the plugin
    def text_or_null(value):
        return f'"{value}"' if value else "nullptr"

    lines = ["// SPDX-License-Identifier: Apache-2.0", generated.rstrip(), "// Included by render_settings_map.cpp only, after the types used here.", "",
             "#ifndef RENDER_SETTINGS_TABLE_H__", "#define RENDER_SETTINGS_TABLE_H__", "",
             "// Every setting with a number or a checkbox: id, type, default, Blender property (or nullptr), second property.",
             "static const Setting SETTINGS[] =", "{"]
    for entry in entries:
        if entry["kind"] in ("BOOL", "INT", "FLOAT"):
            default = number(entry["default"])
            lines.append(f"\t{{ {entry['symbol']}, SETTINGTYPE::{entry['kind']}, {default}, {text_or_null(entry['path'])}, {text_or_null(entry['path2'])} }},")
    lines += ["};", "", "// Settings that choose one of several named options; Blender receives the option's name.",
              "static const ChoiceSetting CHOICES[] =", "{"]
    most = max(len(entry["options"]) for entry in entries if entry["kind"] == "CHOICE")
    for entry in entries:
        if entry["kind"] == "CHOICE":
            names = [f'"{name}"' for _suffix, _label, name in entry["options"]] + ["nullptr"] * (most + 1 - len(entry["options"]))
            lines.append(f"\t{{ {entry['symbol']}, {entry['default']}, {text_or_null(entry['path'])}, {text_or_null(entry['path2'])}, {{ {', '.join(names)} }} }},")
    lines += ["};", f"static const maxon::Int32 CHOICE_OPTIONS = {most + 1};", "",
              "// Groups and settings that belong to one engine and are hidden for the other.",
              "static const EngineOnly ENGINE_ONLY[] =", "{"]
    for entry in entries:
        if entry["engine"]:
            lines.append(f"\t{{ {entry['symbol']}, CYCLES_ENGINE_{entry['engine']} }},")
    lines += ["};", "", "// Groups and settings the Simple layout leaves out.", "static const maxon::Int32 ADVANCED_ONLY[] =", "{"]
    for entry in entries:
        if entry.get("advanced"):
            lines.append(f"\t{entry['symbol']},")
    lines += ["};", "", "// Settings that are greyed out unless another setting has the given value.",
              "static const EnabledBy ENABLED_BY[] =", "{"]
    for entry in entries:
        if entry["enable"]:
            other, value = entry["enable"]
            assert other in symbols, other
            lines.append(f"\t{{ {entry['symbol']}, {other}, {value} }},")
    lines += ["};", "", "#endif // RENDER_SETTINGS_TABLE_H__", ""]
    write("source/videopost/render_settings_table.h", "\n".join(lines))
    print(f"{len(entries)} entries written")


if __name__ == "__main__":
    main()
