"""Original, editable low-poly prop library. Run Blender --background --python ... -- <asset-dir>."""
import bpy, math, random, sys
from pathlib import Path
from mathutils import Vector

OUT = Path(sys.argv[sys.argv.index('--') + 1])
MODELS = OUT / 'Models'
MODELS.mkdir(parents=True, exist_ok=True)
random.seed(20261010)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)

def material(name, color, texture=None, roughness=.85, metallic=0, emission=0):
    m = bpy.data.materials.new('BWH_' + name)
    m.use_nodes = True
    p = m.node_tree.nodes.get('Principled BSDF')
    p.inputs['Base Color'].default_value = (*color, 1)
    p.inputs['Roughness'].default_value = roughness
    p.inputs['Metallic'].default_value = metallic
    if texture:
        t = m.node_tree.nodes.new('ShaderNodeTexImage')
        t.image = bpy.data.images.load(str(OUT / 'Textures' / texture), check_existing=True)
        m.node_tree.links.new(t.outputs['Color'], p.inputs['Base Color'])
    if emission:
        p.inputs['Emission Color'].default_value = (*color, 1)
        p.inputs['Emission Strength'].default_value = emission
    return m

wood = material('WeatheredTimber', (.48,.42,.33), 'wood.png')
stone = material('MossyStone', (.42,.46,.40), 'stone.png')
roof = material('SlateRoof', (.18,.22,.24), 'slate.png')
iron = material('RustedIron', (.18,.11,.07), roughness=.72, metallic=.65)
bark = material('DeadWood', (.23,.20,.14), 'wood.png')
leaf = material('Reed', (.18,.25,.11), roughness=.95)
lamp = material('LanternGlow', (1,.38,.07), roughness=.4, emission=4)
wax = material('CandleWax', (.71,.66,.44))
dark = material('WindowDark', (.015,.025,.03))
red = material('OldFabric', (.27,.055,.04))

objects = []
def register(obj, mat):
    obj.data.materials.append(mat)
    objects.append(obj)
    return obj

def box(pos, dims, mat=wood, rot=(0,0,0), bevel=0):
    bpy.ops.mesh.primitive_cube_add(size=1, location=pos, rotation=rot)
    obj = bpy.context.object
    obj.dimensions = dims
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if bevel:
        b = obj.modifiers.new('WornEdges', 'BEVEL'); b.width=bevel; b.segments=1
        bpy.ops.object.modifier_apply(modifier=b.name)
    return register(obj, mat)

def cylinder(pos, radius, depth, mat=wood, verts=10, rot=(0,0,0)):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=radius, depth=depth, location=pos, rotation=rot)
    return register(bpy.context.object, mat)

def beam(a, b, radius=.07, mat=wood, verts=8):
    a,b=Vector(a),Vector(b)
    obj=cylinder((a+b)/2, radius, (b-a).length, mat, verts)
    obj.rotation_euler=(b-a).to_track_quat('Z','Y').to_euler()
    return obj

def text(value, pos, size=.3, mat=wood):
    bpy.ops.object.text_add(location=pos, rotation=(math.pi/2,0,math.pi))
    obj=bpy.context.object; obj.data.body=value; obj.data.size=size; obj.data.extrude=.004
    bpy.ops.object.convert(target='MESH')
    return register(bpy.context.object,mat)

def export(name):
    bpy.ops.object.select_all(action='DESELECT')
    for obj in objects: obj.select_set(True)
    bpy.context.view_layer.objects.active=objects[0]
    bpy.ops.object.join()
    obj=bpy.context.object; obj.name=name
    bpy.context.scene.cursor.location=(0,0,0)
    bpy.ops.object.origin_set(type='ORIGIN_CURSOR')
    # Triangulate now: physics, LOD generation and render all see identical surfaces.
    mod=obj.modifiers.new('Triangles','TRIANGULATE')
    bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.ops.export_scene.gltf(filepath=str(MODELS/(name+'.glb')), export_format='GLB', use_selection=True,
                              export_yup=True, export_materials='EXPORT', export_texcoords=True,
                              export_normals=True, export_cameras=False, export_lights=False)
    print('BWH_MODEL',name,len(obj.data.polygons),'triangles',flush=True)
    obj.hide_set(True); obj.hide_render=True
    objects.clear()

def gable(width,depth,height,peak):
    angle=math.atan2(peak-height,width/2)
    slope=math.hypot(width/2,peak-height)
    for sign in (-1,1):
        box((sign*width/4,0,(height+peak)/2), (slope+.4,depth+.8,.18),roof,(0,sign*angle,0))
    beam((0,-depth/2-.45,peak+.12),(0,depth/2+.45,peak+.12),.1,iron)

def wall_side(x,depth,height):
    # Open windows with shutters: all geometry remains traversable at doorways.
    box((x,0,.65),(.18,depth,1.3))
    box((x,0,height-.5),(.18,depth,1))
    for y in (-depth/2+.65,0,depth/2-.65):
        box((x,y,height/2),(.2,1.3,height-1.3))
    for y in (-depth/4,depth/4):
        for dy in (-.78,.78): box((x,y+dy,2),(.23,.24,1.4))
        box((x,y,1.35),(.4,1.7,.16))
        box((x,y,2),(.08,.045,1.3),iron)

def house(width=8,depth=10,height=3.3):
    box((0,0,.08),(width+.3,depth+.3,.16))
    wall_side(-width/2,depth,height); wall_side(width/2,depth,height)
    box((0,-depth/2,height/2),(width,.2,height))
    for sign in (-1,1): box((sign*(width/4+.55),depth/2,height/2),(width/2-1.1,.18,height))
    box((0,depth/2,height-.35),(2.2,.18,.7))
    for x in (-width/2,width/2):
        for y in (-depth/2,depth/2): box((x,y,height/2),(.3,.3,height))
    gable(width,depth,height,height+2)

house()
# Staggered boarded shutters and front porch.
for y in (-2.4,2.4): box((4.15,y,2.05),(.1,1.5,.2),wood,(.22,0,0))
box((0,6.0,.1),(4.5,2,.2))
for x in (-2,2): cylinder((x,6.6,1.5),.11,3)
box((0,6,3.1),(4.8,2.3,.15),roof)
box((2.6,-3.3,4.25),(.9,.9,2.3),stone)
box((-1,-2,.7),(2,1,.14)); cylinder((-1,-2,.36),.09,.65)
for x in (-1.8,-.2): box((x,-2,.36),(.12,.12,.65))
box((2,-3,.42),(1.6,3,.65)); box((2,-3,.82),(1.6,3,.15),red)
box((-3,-3,1.3),(1,.6,2.4)); export('Cabin')

# Chapel with open entrance, two aisle rows, altar and a broken bell tower.
house(10,16,4.7)
for x in (-3,3):
    for y in (-4,-1,2,5):
        box((x,y,.6),(2.2,.7,.12)); box((x,y-.28,1.02),(2.2,.12,.8))
        for dx in (-.85,.85): box((x+dx,y,.3),(.12,.5,.6))
box((0,-6,.75),(3.8,1.5,1.5),stone)
box((0,-7,2.7),(.2,.15,2),wood); box((0,-7,3.2),(1.2,.15,.2),wood)
for x in (-1,1): cylinder((x,-6,1.8),.1,.6,wax)
for x in (-1.6,1.6):
    for y in (5.3,7.6): box((x,y,7.1),(.32,.32,5.3))
box((0,6.45,7.5),(3.7,2.8,.25),wood)
for y in (5.3,7.6): beam((-1.6,y,7.6),(1.6,y,9.1),.1)
cylinder((0,6.4,8.6),.55,.9,iron,12)
beam((0,6.4,9.5),(0,6.4,12.9),.09,iron)
beam((-.65,6.4,12.1),(.65,6.4,12.1),.09,iron)
gable(3.7,3.1,9.7,11.7)
# Shift the small roof from origin to the belfry.
for obj in objects[-3:]: obj.location.y += 6.45
export('Chapel')

# Open boathouse, dock and deck: water-facing silhouette without blocking entrances.
box((0,0,.08),(7,9,.16))
for x in (-3.4,3.4):
    for y in (-4.4,0,4.4): cylinder((x,y,1.55),.13,3.1)
    box((x,0,.65),(.15,9,1.3))
box((0,-4.4,1.45),(7,.16,2.9))
gable(7,9,3.1,5.1)
for x in (-2,2): box((x,-1.5,.8),(1,.65,1.6))
export('Boathouse')

for y in range(20): box((0,(y-9.5)*.45,.08),(3,.42,.16))
for x in (-1.3,1.3):
    beam((x,-4.6,-.3),(x,4.6,-.3),.12)
    for y in (-4,-1,2,4): cylinder((x,y,-.9),.13,2)
export('Dock')

for y in range(28): box((0,(y-13.5)*.35,.08),(3.2,.32,.16))
for x in (-1.5,1.5):
    beam((x,-5,-.25),(x,5,-.25),.14)
    for y in (-4.6,-2,2,4.6): cylinder((x,y,.65),.09,1.3)
    beam((x,-4.7,1.3),(x,4.7,1.3),.08)
export('Footbridge')

box((0,0,.17),(1.2,.75,.34),stone,bevel=.08)
box((0,0,1),(.75,.23,1.6),stone,bevel=.07)
cylinder((0,0,1.75),.375,.23,stone,10,(math.pi/2,0,0))
box((0,.13,1.2),(.055,.035,.5),iron); box((0,.13,1.3),(.28,.035,.055),iron)
export('Gravestone')

box((0,0,.18),(.6,.6,.36),stone)
beam((0,0,.2),(0,0,2.1),.12,wood)
beam((-.7,0,1.55),(.7,0,1.55),.12,wood)
export('WoodenCross')

for x in (-1.8,1.8): cylinder((x,0,.75),.10,1.5)
for z in (.6,1.2): beam((-2,0,z),(2,0,z),.06)
export('Fence')

cylinder((0,0,1.4),.1,2.8)
box((0,0,2.65),(2.3,.12,.55),wood,bevel=.02)
text('BLACKWATER', (1,.07,2.51),.23,stone)
box((0,0,1.96),(2.4,.12,.45),wood)
text('CHAPEL  >',(.9,.07,1.83),.23,stone)
export('Signpost')

cylinder((0,0,1.7),.09,3.4,iron)
beam((0,0,3.1),(.6,0,3.1),.065,iron)
box((.6,0,2.78),(.30,.30,.4),lamp)
for z in (2.54,3.02): box((.6,0,z),(.46,.46,.07),iron)
for x in (.41,.79):
    for y in (-.19,.19): beam((x,y,2.54),(x,y,3.02),.022,iron,6)
export('LanternPost')

# Six authored variations share one rock material and modest triangle counts.
for variant in range(3):
    for i in range(3 if variant == 0 else 1):
        bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=2, radius=1, location=(random.uniform(-.6,.6),random.uniform(-.6,.6),.5))
        obj=bpy.context.object
        for v in obj.data.vertices: v.co *= random.uniform(.83,1.18)
        obj.scale=(1.4+variant*.5,.9+variant*.25,.7+variant*.25)
        register(obj,stone)
    export('Rock'+str(variant+1))

cylinder((0,0,.55),.50,1.1,bark,9)
for a in range(5):
    a*=math.tau/5
    beam((math.cos(a)*.85,math.sin(a)*.85,0),(0,0,.4),.15,bark)
export('Stump')

beam((-3,0,.38),(3,0,.42),.38,bark,12)
for x in (-1.3,1.2): beam((x,0,.5),(x+.5,-1.2,.85),.09,bark)
export('FallenLog')

for i in range(30):
    x,y=random.uniform(-.9,.9),random.uniform(-.9,.9)
    h=random.uniform(.5,1.5)
    beam((x,y,0),(x+.1,y+.1,h),.015,leaf,3)
    beam((x+.1,y+.1,h-.2),(x+.1,y+.1,h),.05,bark,5)
export('Reeds')

for i in range(20):
    x,y=random.uniform(-1,1),random.uniform(-1,1)
    for a in (0,2,4):
        a+=i
        beam((x,y,0),(x+.16*math.cos(a),y+.16*math.sin(a),random.uniform(.16,.5)),.022,leaf,3)
export('GrassTuft')

for x in (-.4,.4):
    for y in (-.4,.4): box((x,y,.5),(.15,.15,1))
for i in range(5):
    z=(i+.5)*.2
    for sign in (-1,1):
        box((sign*.48,0,z),(.12,1,.16))
        box((0,sign*.48,z),(1,.12,.16))
box((0,0,1.02),(1.1,1.1,.1)); export('Crate')

# Small ruined stone arch: an approach landmark with a real opening.
for sign in (-1,1):
    for i in range(7): box((sign*2,0,i*.42+.2),(.85,1,.38),stone,bevel=.05)
for i in range(9):
    a=(i+.5)*math.pi/9
    box((2*math.cos(a),0,2.9+2*math.sin(a)),(.65,1,.55),stone,(0,a-math.pi/2,0),.04)
export('RuinedArch')

box((0,0,.1),(7,7,.2))
for x in (-3.3,3.3):
    for y in (-3.3,0,3.3): cylinder((x,y,.65),.09,1.3)
    beam((x,-3.3,1.3),(x,3.3,1.3),.07)
beam((-3.3,-3.3,1.3),(3.3,-3.3,1.3),.07)
box((-2,0,.55),(.7,3,.12))
for y in (-1,1): box((-2,y,.25),(.5,.16,.5))
export('LookoutDeck')

for obj in bpy.context.scene.objects: obj.hide_set(False)
# Keep the editable source library neatly arranged rather than overlapping at the origin.
for i,obj in enumerate(bpy.context.scene.objects):
    if obj.type=='MESH': obj.location.x += (i%5)*24; obj.location.y += (i//5)*28
bpy.ops.file.pack_all()
bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'BlackwaterPropLibrary.blend'))
print('BWH_COMPLETE',flush=True)
