# SPDX-License-Identifier: Apache-2.0
"""Builds examples/cycles_render_examples.c4d: one scene that shows what Cycles Render translates.

Runs inside Cinema 4D with the plugin loaded: from the Script Manager, or without a window
through the local test runner (which is not part of the repository):

    .\\tests\\c4d\\run_c4d_test.ps1 -Headless -Script tools\\make_example_scene.py -ModulePath build\\plugin\\bin\\Release\\plugins -TimeoutSec 900

Ten stations stand in two rows on a floor, each a group named after what it shows:

    front row   Classic Materials | Redshift Materials | Cycles Nodes | MoGraph Cloner | Hair
    back row    Volume            | Instances on a Matrix | Light Linking | Deformer | Extruded Text

The scene renders with Cycles Render (Render Settings), through the camera "Overview", lit by
a Redshift area light and the plain world of the render settings. With the test library at
hand (tests/c4d/lib) the script also renders examples/cycles_render_examples.png.
Needs Redshift for the Redshift station (it is skipped with a note otherwise).
"""

import math
import os

import c4d
import maxon

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUTPUT = os.path.normpath(os.path.join(ROOT, "examples", "cycles_render_examples.c4d"))
PREVIEW = os.path.normpath(os.path.join(ROOT, "examples", "cycles_render_examples.png"))

CYCLES_RENDER = 1073816
CYCLES_OBJECT_TAG = 1073899
CYCLES_SPACE = maxon.Id("com.wolfnimations.cyclesrender.nodespace")
CYCLES_NODE = "com.wolfnimations.cyclesrender.node."
REDSHIFT_SPACE = maxon.Id("com.redshift3d.redshift4c4d.class.nodespace")
REDSHIFT_STANDARD = "com.redshift3d.redshift4c4d.nodes.core.standardmaterial"

doc = c4d.documents.BaseDocument()
built, skipped = [], {}


# --- helpers ------------------------------------------------------------------------------------------
def add(obj, name, parent=None, position=(0, 0, 0), rotation=(0, 0, 0)):
    obj.SetName(name)
    obj.SetRelPos(c4d.Vector(*position))
    obj.SetRelRot(c4d.Vector(*[math.radians(a) for a in rotation]))
    if parent is None:
        doc.InsertObject(obj)
        last = doc.GetFirstObject()
        while last.GetNext():
            last = last.GetNext()
        if last != obj:
            obj.Remove()
            doc.InsertObject(obj, pred=last)
    else:
        obj.InsertUnderLast(parent)
    return obj


def station(name, x, z):
    return add(c4d.BaseObject(c4d.Onull), name, position=(x, 0, z))


def ball(parent, name, x, radius=45.0, y=None):
    sphere = c4d.BaseObject(c4d.Osphere)
    sphere[c4d.PRIM_SPHERE_RAD] = radius
    sphere[c4d.PRIM_SPHERE_SUB] = 48
    add(sphere, name, parent, position=(x, radius if y is None else y, 0))
    sphere.MakeTag(c4d.Tphong)
    return sphere


def assign(obj, material):
    tag = obj.MakeTag(c4d.Ttexture)
    tag[c4d.TEXTURETAG_MATERIAL] = material
    tag[c4d.TEXTURETAG_PROJECTION] = c4d.TEXTURETAG_PROJECTION_UVW
    return tag


def classic(name, color=(0.8, 0.8, 0.8), reflection=False, luminance=None):
    material = c4d.BaseMaterial(c4d.Mmaterial)
    material.SetName(name)
    material[c4d.MATERIAL_USE_COLOR] = True
    material[c4d.MATERIAL_COLOR_COLOR] = c4d.Vector(*color)
    material[c4d.MATERIAL_USE_REFLECTION] = reflection
    if luminance is not None:
        material[c4d.MATERIAL_USE_LUMINANCE] = True
        material[c4d.MATERIAL_LUMINANCE_COLOR] = c4d.Vector(*luminance)
    doc.InsertMaterial(material)
    return material


def redshift(name, values):
    material = c4d.BaseMaterial(c4d.Mmaterial)
    material.SetName(name)
    graph = material.GetNodeMaterialReference().CreateDefaultGraph(REDSHIFT_SPACE)
    doc.InsertMaterial(material)
    nodes = []
    maxon.GraphModelHelper.FindNodesByAssetId(graph, maxon.Id(REDSHIFT_STANDARD), True, nodes)
    inputs = nodes[0].GetInputs()
    with graph.BeginTransaction() as transaction:
        for port, value in values.items():
            inputs.FindChild(REDSHIFT_STANDARD + "." + port).SetPortValue(maxon.Color(*value) if isinstance(value, tuple) else value)
        transaction.Commit()
    return material


def cycles_nodes(name):
    """A node material in the Cycles space: (material, graph, Principled BSDF, Material Output)."""
    material = c4d.BaseMaterial(c4d.Mmaterial)
    material.SetName(name)
    graph = material.GetNodeMaterialReference().CreateDefaultGraph(CYCLES_SPACE)
    doc.InsertMaterial(material)
    found = {}
    for key in ("bsdfprincipled", "outputmaterial"):
        nodes = []
        maxon.GraphModelHelper.FindNodesByAssetId(graph, maxon.Id(CYCLES_NODE + key), True, nodes)
        found[key] = nodes[0]
    return material, graph, found["bsdfprincipled"], found["outputmaterial"]


def attempt(name, build):
    try:
        build()
        built.append(name)
    except Exception as error:   # noqa: BLE001
        skipped[name] = repr(error)[:200]


# --- the stage ----------------------------------------------------------------------------------------
floor = c4d.BaseObject(c4d.Oplane)
floor[c4d.PRIM_PLANE_WIDTH], floor[c4d.PRIM_PLANE_HEIGHT] = 3600.0, 2400.0
floor[c4d.PRIM_PLANE_SUBW], floor[c4d.PRIM_PLANE_SUBH] = 1, 1
add(floor, "Floor", position=(0, 0, 200))
assign(floor, classic("Floor", (0.35, 0.35, 0.37)))

key = c4d.BaseObject(c4d.Orslight)
key[c4d.REDSHIFT_LIGHT_TYPE] = 3                                   # area
key[c4d.REDSHIFT_LIGHT_PHYSICAL_INTENSITY] = 14.0
key[c4d.REDSHIFT_LIGHT_PHYSICAL_AREA_SIZEX] = 900.0
key[c4d.REDSHIFT_LIGHT_PHYSICAL_AREA_SIZEY] = 600.0
add(key, "Key Light (Redshift Area)", position=(-500, 900, -500), rotation=(-40, -55, 0))

fill = c4d.BaseObject(c4d.Olight)
fill[c4d.LIGHT_TYPE] = c4d.LIGHT_TYPE_OMNI
fill[c4d.LIGHT_BRIGHTNESS] = 0.5
fill[c4d.LIGHT_COLOR] = c4d.Vector(0.75, 0.85, 1.0)
add(fill, "Fill Light (classic)", position=(700, 500, -700))

camera = c4d.BaseObject(c4d.Orscamera)
camera[c4d.RSCAMERAOBJECT_FOCAL_LENGTH] = 30.0
add(camera, "Overview", position=(0, 760, -1700), rotation=(0, -22, 0))
view = doc.GetRenderBaseDraw() or doc.GetActiveBaseDraw()
if view is not None:
    view.SetSceneCamera(camera)

FRONT, BACK = 0.0, 420.0
COLUMNS = (-840.0, -420.0, 0.0, 420.0, 840.0)


# --- front row ------------------------------------------------------------------------------------------
def classic_station():
    group = station("Classic Materials", COLUMNS[0], FRONT)
    assign(ball(group, "Diffuse", -105), classic("Classic Red", (0.8, 0.12, 0.1)))
    assign(ball(group, "Mirror", 0), classic("Classic Mirror", (0.05, 0.05, 0.05), reflection=True))
    checks = classic("Classic Checkerboard", (1, 1, 1))
    shader = c4d.BaseShader(c4d.Xcheckerboard)
    shader[c4d.CHECKERBOARDSHADER_SCALEX], shader[c4d.CHECKERBOARDSHADER_SCALEY] = 8.0, 4.0
    checks.InsertShader(shader)
    checks[c4d.MATERIAL_COLOR_SHADER] = shader
    assign(ball(group, "Shader", 105), checks)


def redshift_station():
    group = station("Redshift Materials", COLUMNS[1], FRONT)
    assign(ball(group, "Glass", -105), redshift("Redshift Glass", {"refr_weight": 1.0, "refl_roughness": 0.0, "refr_color": (0.75, 0.95, 0.85)}))
    assign(ball(group, "Gold", 0), redshift("Redshift Gold", {"base_color": (0.95, 0.65, 0.2), "metalness": 1.0, "refl_roughness": 0.25}))
    assign(ball(group, "Car Paint", 105), redshift("Redshift Coat", {"base_color": (0.05, 0.1, 0.5), "refl_roughness": 0.6, "coat_weight": 1.0, "coat_roughness": 0.03}))


def nodes_station():
    group = station("Cycles Nodes", COLUMNS[2], FRONT)

    # 1. an Emission node wired into the output
    material, graph, shader, output = cycles_nodes("Cycles Emission")
    with graph.BeginTransaction() as transaction:
        emission = graph.AddChild(maxon.Id(), maxon.Id(CYCLES_NODE + "emission"))
        emission.GetInputs().FindChild("in.color").SetPortValue(maxon.Color(1.0, 0.45, 0.1))
        emission.GetInputs().FindChild("in.strength").SetPortValue(4.0)
        target = output.GetInputs().FindChild("in.surface")
        target.RemoveConnections(maxon.PORT_DIR.INPUT, maxon.Wires.All())
        emission.GetOutputs().FindChild("out.emission").Connect(target)
        transaction.Commit()
    assign(ball(group, "Emission", -105), material)

    # 2. a Noise Texture as the base colour of the Principled BSDF
    material, graph, shader, output = cycles_nodes("Cycles Noise")
    with graph.BeginTransaction() as transaction:
        noise = graph.AddChild(maxon.Id(), maxon.Id(CYCLES_NODE + "texnoise"))
        noise.GetInputs().FindChild("in.scale").SetPortValue(6.0)
        noise.GetInputs().FindChild("in.detail").SetPortValue(6.0)
        noise.GetOutputs().FindChild("out.color").Connect(shader.GetInputs().FindChild("in.base_color"))
        shader.GetInputs().FindChild("in.roughness").SetPortValue(0.35)
        transaction.Commit()
    assign(ball(group, "Noise Texture", 0), material)

    # 3. a Voronoi Texture driving the roughness of a metal
    material, graph, shader, output = cycles_nodes("Cycles Voronoi Metal")
    with graph.BeginTransaction() as transaction:
        voronoi = graph.AddChild(maxon.Id(), maxon.Id(CYCLES_NODE + "texvoronoi"))
        voronoi.GetInputs().FindChild("in.scale").SetPortValue(12.0)
        voronoi.GetOutputs().FindChild("out.distance").Connect(shader.GetInputs().FindChild("in.roughness"))
        shader.GetInputs().FindChild("in.base_color").SetPortValue(maxon.Color(0.85, 0.87, 0.9))
        shader.GetInputs().FindChild("in.metallic").SetPortValue(1.0)
        transaction.Commit()
    assign(ball(group, "Voronoi Roughness", 105), material)


def cloner_station():
    group = station("MoGraph Cloner", COLUMNS[3], FRONT)
    cloner = c4d.BaseObject(c4d.Omgcloner)
    add(cloner, "Cloner", group, position=(0, 30, 0))
    cloner[c4d.ID_MG_MOTIONGENERATOR_MODE] = c4d.ID_MG_MOTIONGENERATOR_MODE_GRIDARRAY
    cloner[c4d.MG_GRID_MODE] = c4d.MG_GRID_MODE_ENDPOINT
    cloner[c4d.MG_GRID_RESOLUTION] = c4d.Vector(6, 3, 4)
    cloner[c4d.MG_GRID_SIZE] = c4d.Vector(280, 100, 170)
    cube = c4d.BaseObject(c4d.Ocube)
    cube[c4d.PRIM_CUBE_LEN] = c4d.Vector(34, 34, 34)
    cube[c4d.PRIM_CUBE_DOFILLET] = True
    cube[c4d.PRIM_CUBE_FRAD] = 5.0
    add(cube, "Clone", cloner)
    # Each clone takes a colour from a Random effector; the material shows it through the MoGraph Color shader.
    random = c4d.BaseObject(c4d.Omgrandom)
    random[c4d.ID_MG_BASEEFFECTOR_POSITION_ACTIVE] = False
    random[c4d.ID_MG_BASEEFFECTOR_COLOR_MODE] = c4d.ID_MG_BASEEFFECTOR_COLOR_MODE_EFFECTOR
    add(random, "Random Colours", group)
    effectors = c4d.InExcludeData()
    effectors.InsertObject(random, 1)
    cloner[c4d.ID_MG_MOTIONGENERATOR_EFFECTORLIST] = effectors
    material = classic("Clone Colour", (1, 1, 1))
    shader = c4d.BaseShader(c4d.Xmgcolor)
    material.InsertShader(shader)
    material[c4d.MATERIAL_COLOR_SHADER] = shader
    assign(cloner, material)


def hair_station():
    group = station("Hair", COLUMNS[4], FRONT)
    plane = c4d.BaseObject(c4d.Oplane)
    plane[c4d.PRIM_PLANE_WIDTH], plane[c4d.PRIM_PLANE_HEIGHT] = 240.0, 160.0
    plane[c4d.PRIM_PLANE_SUBW], plane[c4d.PRIM_PLANE_SUBH] = 6, 4
    doc.InsertObject(plane)
    scalp = c4d.utils.SendModelingCommand(c4d.MCOMMAND_CURRENTSTATETOOBJECT, [plane], doc=doc)[0]
    plane.Remove()
    add(scalp, "Scalp", group, position=(0, 2, 0))
    assign(scalp, classic("Scalp", (0.2, 0.12, 0.08)))
    hair = c4d.BaseObject(c4d.Ohair)
    add(hair, "Hair", group)
    hair[c4d.HAIRSTYLE_LINK] = scalp
    hair[c4d.HAIRSTYLE_COUNT] = 400
    hair[c4d.HAIRSTYLE_LENGTH] = 90.0
    hair[c4d.HAIRSTYLE_SEGMENTS] = 8
    hair[c4d.HAIRSTYLE_HAIR_COUNT] = 12000
    material = c4d.BaseMaterial(c4d.Mhair)
    material.SetName("Hair")
    material[c4d.HAIRMATERIAL_THICKNESS_ROOT], material[c4d.HAIRMATERIAL_THICKNESS_TIP] = 1.2, 0.2
    doc.InsertMaterial(material)
    hair.MakeTag(c4d.Thairmaterial)[c4d.HAIRMATERIAL_TAG_LINK] = material


# --- back row -------------------------------------------------------------------------------------------
def volume_station():
    group = station("Volume (Volume Builder + Cycles Object tag)", COLUMNS[0], BACK)
    builder = c4d.BaseObject(c4d.Ovolumebuilder)
    add(builder, "Fog", group, position=(0, 110, 0))
    source = c4d.BaseObject(c4d.Otorus)
    source[c4d.PRIM_TORUS_OUTERRAD], source[c4d.PRIM_TORUS_INNERRAD] = 85.0, 35.0
    source[c4d.PRIM_TORUS_SEG], source[c4d.PRIM_TORUS_CSUB] = 48, 24
    add(source, "Torus", builder, rotation=(0, 0, 90))
    builder[c4d.ID_VOLUMEBUILDER_VOLUMETYPE] = c4d.ID_VOLUMEBUILDER_FOG
    builder[c4d.ID_VOLUMEBUILDER_GRID_SIZE] = 4.0
    builder.AddSceneObject(source)
    tag = builder.MakeTag(CYCLES_OBJECT_TAG)
    tag[1010] = True                                    # Volume: Override Volume Shading
    tag[1000] = 25.0                                    # Density
    tag[1001] = c4d.Vector(0.6, 0.8, 1.0)               # Color


def matrix_station():
    group = station("Instances on a Matrix (Cycles Object tag)", COLUMNS[1], BACK)
    matrix = c4d.BaseObject(c4d.Omgmatrix)
    add(matrix, "Matrix", group, position=(0, 40, 0))
    matrix[c4d.ID_MG_MOTIONGENERATOR_MODE] = c4d.ID_MG_MOTIONGENERATOR_MODE_GRIDARRAY
    matrix[c4d.MG_GRID_MODE] = c4d.MG_GRID_MODE_ENDPOINT
    matrix[c4d.MG_GRID_RESOLUTION] = c4d.Vector(5, 1, 3)
    matrix[c4d.MG_GRID_SIZE] = c4d.Vector(280, 0, 160)
    plain = c4d.BaseObject(c4d.Omgplain)
    plain[c4d.ID_MG_BASEEFFECTOR_POSITION_ACTIVE] = False
    plain[c4d.ID_MG_BASEEFFECTOR_ROTATE_ACTIVE] = True
    plain[c4d.ID_MG_BASEEFFECTOR_ROTATION] = c4d.Vector(math.radians(35), 0, math.radians(12))
    add(plain, "Turn", group)
    effectors = c4d.InExcludeData()
    effectors.InsertObject(plain, 1)
    matrix[c4d.ID_MG_MOTIONGENERATOR_EFFECTORLIST] = effectors
    # The two objects that are instanced; they are switched off for the renderer themselves.
    sources = add(c4d.BaseObject(c4d.Onull), "Instanced Objects (render dot off)", group)
    sources[c4d.ID_BASEOBJECT_VISIBILITY_RENDER] = c4d.MODE_OFF
    sources[c4d.ID_BASEOBJECT_VISIBILITY_EDITOR] = c4d.MODE_OFF
    cone = c4d.BaseObject(c4d.Ocone)
    cone[c4d.PRIM_CONE_BRAD], cone[c4d.PRIM_CONE_HEIGHT] = 22.0, 70.0
    add(cone, "Cone", sources)
    assign(cone, classic("Instance Teal", (0.1, 0.6, 0.55)))
    pyramid = c4d.BaseObject(c4d.Oplatonic)
    pyramid[c4d.PRIM_PLATONIC_RAD] = 28.0
    add(pyramid, "Platonic", sources)
    assign(pyramid, classic("Instance Yellow", (0.9, 0.7, 0.1)))
    tag = matrix.MakeTag(CYCLES_OBJECT_TAG)
    tag[1300] = True                                    # Instances: enable
    listed = c4d.InExcludeData()
    listed.InsertObject(pyramid, 0)
    listed.InsertObject(cone, 0)
    tag[1301] = listed
    tag[1302] = 1.0                                     # Scale


def linking_station():
    group = station("Light Linking (Cycles Object tag)", COLUMNS[2], BACK)
    white = classic("White", (0.85, 0.85, 0.85))
    lit = ball(group, "Lit by the pink light", -70, radius=55.0)
    spared = ball(group, "Excludes the pink light (tag)", 70, radius=55.0)
    assign(lit, white)
    assign(spared, white)
    pink = c4d.BaseObject(c4d.Olight)
    pink[c4d.LIGHT_TYPE] = c4d.LIGHT_TYPE_OMNI
    pink[c4d.LIGHT_COLOR] = c4d.Vector(1.0, 0.1, 0.6)
    pink[c4d.LIGHT_BRIGHTNESS] = 4.0
    pink[c4d.LIGHT_DETAILS_FALLOFF] = c4d.LIGHT_DETAILS_FALLOFF_INVERSESQUARE
    pink[c4d.LIGHT_DETAILS_OUTERDISTANCE] = 220.0
    add(pink, "Pink Light", group, position=(0, 170, -150))
    tag = spared.MakeTag(CYCLES_OBJECT_TAG)
    tag[1200] = True                                    # Light Linking: enable
    tag[1201] = 0                                       # Exclude
    listed = c4d.InExcludeData()
    listed.InsertObject(pink, 0)
    tag[1202] = listed


def deformer_station():
    group = station("Deformer", COLUMNS[3], BACK)
    column = c4d.BaseObject(c4d.Ocube)
    column[c4d.PRIM_CUBE_LEN] = c4d.Vector(60, 220, 60)
    column[c4d.PRIM_CUBE_SUBX], column[c4d.PRIM_CUBE_SUBY], column[c4d.PRIM_CUBE_SUBZ] = 6, 40, 6
    add(column, "Column", group, position=(0, 110, 0))
    column.MakeTag(c4d.Tphong)
    assign(column, classic("Deformed Green", (0.2, 0.6, 0.25)))
    twist = c4d.BaseObject(c4d.Otwist)
    twist[c4d.DEFORMOBJECT_SIZE] = c4d.Vector(80, 220, 80)
    twist[c4d.DEFORMOBJECT_ANGLE] = math.radians(200)
    add(twist, "Twist", column)


def text_station():
    group = station("Extruded Text", COLUMNS[4], BACK)
    extrude = c4d.BaseObject(c4d.Oextrude)
    extrude[c4d.EXTRUDEOBJECT_EXTRUSIONOFFSET] = 30.0
    add(extrude, "Extrude", group, position=(-150, 30, 0))
    text = c4d.BaseObject(c4d.Osplinetext)
    text[c4d.PRIM_TEXT_TEXT] = "Cycles"
    text[c4d.PRIM_TEXT_HEIGHT] = 110.0
    add(text, "Text", extrude)
    assign(extrude, classic("Text Orange", (0.95, 0.45, 0.05)))


for station_name, build in (("Classic Materials", classic_station), ("Redshift Materials", redshift_station), ("Cycles Nodes", nodes_station),
                            ("MoGraph Cloner", cloner_station), ("Hair", hair_station), ("Volume", volume_station),
                            ("Instances on a Matrix", matrix_station), ("Light Linking", linking_station), ("Deformer", deformer_station),
                            ("Extruded Text", text_station)):
    attempt(station_name, build)

# --- render settings -------------------------------------------------------------------------------------
rd = doc.GetActiveRenderData()
rd.SetName("Cycles Render")
rd[c4d.RDATA_XRES], rd[c4d.RDATA_YRES] = 1280, 720
rd[c4d.RDATA_PIXELASPECT] = 1.0
rd[c4d.RDATA_RENDERENGINE] = CYCLES_RENDER
videopost = c4d.documents.BaseVideoPost(CYCLES_RENDER)
rd.InsertVideoPost(videopost)
videopost[1100] = 256                                   # Max Samples (the noise threshold ends most pixels earlier)
videopost[1101] = 128                                   # Viewport (IPR) Max Samples
videopost[1107] = True                                  # denoise the IPR
videopost[1204] = 2                                     # Volume bounces
videopost[1502] = c4d.Vector(0.06, 0.08, 0.12)          # the default world: a dim blue
videopost[1503] = 1.0

doc.SetDocumentName(os.path.basename(OUTPUT))
doc.ExecutePasses(None, True, True, True, c4d.BUILDFLAGS_EXTERNALRENDERER)
os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
saved = c4d.documents.SaveDocument(doc, OUTPUT, c4d.SAVEDOCUMENTFLAGS_DONTADDTORECENTLIST, c4d.FORMAT_C4DEXPORT)

report = {"built": built, "skipped": skipped, "saved": bool(saved), "file": OUTPUT}

# --- a preview picture, when the test library is at hand ---------------------------------------------------
try:
    import sys
    sys.path.insert(0, os.path.join(ROOT, "tests", "c4d", "lib"))
    import crtest
    bitmap = c4d.bitmaps.MultipassBitmap(1280, 720, c4d.COLORMODE_RGBf)
    code = c4d.documents.RenderDocument(doc, rd.GetDataInstance(), bitmap, c4d.RENDERFLAGS_EXTERNAL | c4d.RENDERFLAGS_OCIO_RAW_RENDERING)
    report["render_result"] = code
    if code == c4d.RENDERRESULT_OK:
        crtest.Image.from_bitmap(bitmap).save_png(PREVIEW)
        report["preview"] = PREVIEW
except Exception as error:   # noqa: BLE001
    report["preview_error"] = repr(error)[:300]

try:
    result.update(report)                               # under the test runner
    result["passed"] = bool(saved) and not skipped
except NameError:
    print(report)
