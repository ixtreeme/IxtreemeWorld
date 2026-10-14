"""Deterministic landscape, original textures/sounds and an engine scene draft.
Usage: python prepare.py <existing-project-root> <staging-dir>
Then run blender_assets.py and BlackwaterHollowBuilder. Never overwrites Main.scene.
"""
import json, math, sys, wave
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT, STAGING = Path(sys.argv[1]), Path(sys.argv[2])
assert (ROOT/'project.ixproj').is_file(), 'Existing engine project required'
ASSETS=ROOT/'Assets/BlackwaterHollow'
for p in (STAGING, ASSETS/'Textures',ASSETS/'Audio',ASSETS/'Scripts',ASSETS/'UI'):
    p.mkdir(parents=True,exist_ok=True)
rng=np.random.default_rng(20261010)

# All authored assets here are original. Existing terrain maps stay in their original folders.
yy,xx=np.mgrid[0:512,0:512]
noise=rng.normal(0,1,(512,512))
grain=np.sin(xx*.22+np.sin(yy*.013)*2)*.06 + np.sin(xx*.91+yy*.021)*.025
planks=(xx//64%2)*.025 - (xx%64<3)*.16
wood=np.clip(.35+grain+planks+noise*.015,.02,1)
stone=np.clip(.36+.06*np.sin(xx*.027+np.sin(yy*.043)*2)+noise*.04,.1,.8)
slate=np.clip(.23+noise*.025-(yy%64<3)*.10-((xx+(yy//64%2)*32)%64<2)*.05,.03,.6)
soil=np.clip(.24+noise*.045+.035*np.sin(xx*.11)*np.sin(yy*.073),.08,.6)
for name,values,tint in [('wood',wood,(1.05,.94,.75)),('stone',stone,(.88,1,.91)),('slate',slate,(.82,.96,1)),('soil',soil,(1.05,.86,.63))]:
    rgb=np.stack([values*c for c in tint],axis=-1)
    Image.fromarray(np.uint8(np.clip(rgb,0,1)*255)).save(ASSETS/'Textures'/f'{name}.png')
Image.new('RGB',(64,64),(128,128,255)).save(ASSETS/'Textures/flat_normal.png')
Image.new('RGB',(64,64),(225,225,225)).save(ASSETS/'Textures/roughness.png')
r=np.sqrt(((xx-256)/256)**2+((yy-256)/256)**2)
alpha=np.clip(1-r,0,1)**2
sprite=np.full((512,512,4),255,np.uint8); sprite[:,:,3]=(alpha*255).astype(np.uint8)
Image.fromarray(sprite).resize((128,128)).save(ASSETS/'Textures/mist_sprite.png')

# Periodic FFT noise + periodic envelopes produce seamless beds without clicks at loop boundaries.
SR=22050
def noise_loop(seconds, cutoff, seed):
    n=int(seconds*SR); random=np.random.default_rng(seed)
    f=np.fft.rfftfreq(n,1/SR)
    spectrum=np.fft.rfft(random.normal(size=n)) / np.sqrt(1+(f/cutoff)**4)
    spectrum[0]=0
    v=np.fft.irfft(spectrum,n)
    return v/(np.std(v)+1e-9)
def wav(name,v):
    v=np.clip(v,-.92,.92)
    with wave.open(str(ASSETS/'Audio'/f'{name}.wav'),'wb') as f:
        f.setnchannels(1); f.setsampwidth(2); f.setframerate(SR)
        f.writeframes((v*32767).astype('<i2').tobytes())
    print('AUDIO',name,len(v)/SR,flush=True)
t=np.arange(SR*48)/SR
wind=noise_loop(48,550,13)
wind*=.065*(.65+.25*np.sin(math.tau*t/48)+.1*np.sin(math.tau*t/12))
wav('ForestWind',wind)
water=noise_loop(48,2800,14)*.035*(.75+.2*np.sin(math.tau*t/4)+.05*np.sin(math.tau*t/48))
wav('LakeLapping',water)
drone=.035*np.sin(math.tau*48*t)+.022*np.sin(math.tau*49.0625*t)+.015*np.sin(math.tau*72.125*t)
drone*=.6+.4*np.sin(math.pi*t/48)**2
wav('UneasyDrone',drone)
creak=np.zeros_like(t)
for start,freq in [(4,190),(19,260),(35,145)]:
    q=t-start; env=np.where((q>0)&(q<3),np.sin(np.clip(q,0,3)*math.pi/3)**2,0)
    creak+=.075*env*np.sin(math.tau*(freq*q+10*np.sin(q*3))) * (.6+.4*np.sin(q*32)**2)
wav('TimberCreak',creak)
crows=np.zeros_like(t)
for start in (6,7,25,26.2,39):
    q=t-start; env=np.where((q>0)&(q<.45),np.sin(np.clip(q,0,.45)*math.pi/.45)**2,0)
    crows+=.08*env*(np.sin(math.tau*(600*q-200*q*q))+.25*np.sin(math.tau*1500*q))*(.5+.5*np.sin(q*100)**2)
wav('DistantCrows',crows)
whisper=noise_loop(8,1800,15); q=np.arange(8*SR)/SR
wav('ForestWhisper',whisper*.028*np.sin(math.pi*q/8)**3*(.7+.3*np.sin(q*9)))
q=np.arange(SR*8)/SR
bell=sum(.10/(i+1)*np.sin(math.tau*f*q)*np.exp(-q*(.5+i*.17)) for i,f in enumerate((220,587,917,1331,1863)))
bell*=np.minimum(q*200,1)
wav('ChapelBell',bell)
q=np.arange(int(SR*.32))/SR
step=noise_loop(.32,1200,17)*.06*np.exp(-q*17)*np.minimum(q*120,1)
step+=.06*np.sin(math.tau*78*q)*np.exp(-q*28)*np.minimum(q*120,1)
wav('Footstep',step)

# Broad valley with nonrepeating detail, wet lowland, protected footpaths and dramatic border hills.
PATHS=[
    [(-170,-220),(-170,-150),(-150,-85),(-125,-40),(-75,-10),(-55,70),(-90,140),(-90,170),(-30,190)],
    [(-75,-10),(-35,-35),(20,-45),(90,-55),(170,-20),(205,65),(195,160),(150,220),(45,240),(-30,190)],
    [(-150,-85),(-245,-10),(-260,90),(-200,170),(-90,170)],
    [(170,-20),(230,-90),(240,-135)],
]
POIS=[('Kunyho',-150,-80,13,13),('Kapolna',-90,170,15,17),('Temeto',-25,194,20,21),
      ('Csonakhaz',20,-43,12,12),('Romos kapu',240,-135,10,10),('Kilato',185,165,12,12)]
def smooth(a,b,v):
    q=np.clip((v-a)/(b-a),0,1)
    return q*q*(3-2*q)
def base_height(x,z):
    h=7+2.4*np.sin(x*.013)*np.cos(z*.010)+1.3*np.sin(z*.024+x*.008)
    h+=.65*np.sin(x*.046+np.sin(z*.038))*np.cos(z*.052)
    h+=.2*np.sin(x*.21)*np.cos(z*.173)
    h+=45*np.exp(-((x+335)**2/180**2+(z-300)**2/155**2))
    h+=34*np.exp(-((x-345)**2/160**2+(z+320)**2/160**2))
    edge=np.maximum(np.abs(x),np.abs(z))
    h+=smooth(350,500,edge)*(25+18*np.sin(x*.008+z*.009)**2)
    lake=np.sqrt(((x-82)/82)**2+((z-83)/116)**2)
    bed=-4.7+2.8*np.minimum(lake,1)**2+.3*np.sin(x*.07)*np.cos(z*.07)
    h=h*(smooth(.83,1.26,lake))+bed*(1-smooth(.83,1.26,lake))
    return h
def path_distance(x,z):
    distance=np.full(np.broadcast(x,z).shape,np.inf)
    target=np.zeros_like(distance)
    for path in PATHS:
        for (ax,az),(bx,bz) in zip(path,path[1:]):
            dx,dz=bx-ax,bz-az
            u=np.clip(((x-ax)*dx+(z-az)*dz)/(dx*dx+dz*dz),0,1)
            px,pz=ax+u*dx,az+u*dz
            d=np.sqrt((x-px)**2+(z-pz)**2)
            hit=d<distance
            height=base_height(ax,az)*(1-u)+base_height(bx,bz)*u
            target=np.where(hit,height,target); distance=np.minimum(distance,d)
    return distance,target
def height(x,z):
    h=base_height(x,z)
    dist,road=path_distance(x,z); w=1-smooth(3.2,8,dist)
    h=h*(1-w)+road*w
    for _,px,pz,rx,rz in POIS:
        d=np.maximum(np.abs(x-px)/rx,np.abs(z-pz)/rz)
        w=1-smooth(.7,1.5,d)
        h=h*(1-w)+base_height(px,pz)*w
    return h
X,Z=np.meshgrid(np.linspace(-500,500,501),np.linspace(500,-500,501))
H=height(X,Z)
# Placement uses the same bilinear grid that the engine samples, avoiding per-prop noise evaluation.
authored_height=height
def height(x,z):
    if not (np.isscalar(x) and np.isscalar(z)): return authored_height(x,z)
    gx,gz=np.clip((x+500)/2,0,500),np.clip((500-z)/2,0,500)
    ix,iz=min(int(gx),499),min(int(gz),499); tx,tz=gx-ix,gz-iz
    return (H[iz,ix]*(1-tx)+H[iz,ix+1]*tx)*(1-tz)+(H[iz+1,ix]*(1-tx)+H[iz+1,ix+1]*tx)*tz
(H*100).astype('<f4').tofile(STAGING/'height_cm.f32')
cx,cz=X[:-1,:-1]+1,Z[:-1,:-1]-1
pd,_=path_distance(cx,cz)
trail=1-smooth(2.4,5.8,pd)
rock=smooth(.26,.62,np.hypot(*np.gradient(H,2))[:-1,:-1])
wet=1-smooth(1,4,H[:-1,:-1])
grass=(1-trail)*(1-rock)*(1-wet)
weights=np.stack((grass,trail*(1-rock),rock,wet*(1-trail)*(1-rock)),axis=-1)
weights/=np.maximum(weights.sum(axis=-1,keepdims=True),.001)
spl=(weights*255).astype(np.uint8)
spl[:,:,0]+=255-spl.sum(axis=-1,dtype=np.uint16).astype(np.uint8)
spl.tofile(STAGING/'splat_a.rgba')
# Same coordinate convention as WaterBodyIO: rows go from bbox-min to bbox-max in Z.
wx,wz=np.meshgrid(np.linspace(-12,178,190),np.linspace(-44,207,251))
mask=np.uint8((height(wx,wz)<.7)*255)
mask.tofile(STAGING/'lake.mask')

entities=[]; eid=0; lid=0
def entity(name,x=0,z=0,y=None,model=None,scale=1,yaw=0,group=None,collider=False):
    global eid
    eid+=1
    e=dict(type='mesh_entity',id=eid,name=name,position=[float(x),float(height(x,z) if y is None else y),float(z)],
           rotation=[0,float(yaw),0],scale=[scale]*3 if model else [0]*3,enabled=True,skinned=False,
           mesh_asset_id='',mesh_asset_path=f'Assets/BlackwaterHollow/Models/{model}.glb' if model else '')
    if group: e['parent']={'type':'mesh_entity','id':group}
    if model:
        e['lod_component']={'enabled':True,'override_asset_default':True,'config':{
            'level_count':4,'target_ratios':[1,.5,.22,.08],'distances':[0,35,85,160],'hysteresis_m':6}}
    if collider: e['collider']={'enabled':True,'shape':'mesh','layer':'static_world','friction':.8}
    entities.append(e)
    return e
def group(name):
    e=entity(name,y=0); e['scale']=[1]*3; return e['id']
forest=group('01 / Forest - shared models, LOD + baked impostors')
buildings=group('02 / Abandoned village and landmarks')
props=group('03 / Rocks, ground cover and details')
atmos=group('04 / Fog, particles and spatial sound')
play=group('05 / First person exploration')

for title,x,z,_,_ in POIS:
    if title=='Kunyho': entity('Abandoned ranger cabin',x,z,model='Cabin',group=buildings,collider=True)
    if title=='Kapolna': entity('Chapel of the lost',x,z,model='Chapel',group=buildings,collider=True)
    if title=='Csonakhaz': entity('Lake boathouse',x,z,model='Boathouse',yaw=math.pi,group=buildings,collider=True)
    if title=='Romos kapu': entity('Broken stone gateway',x,z,model='RuinedArch',group=buildings,collider=True)
entity('Forgotten second cabin',-205,155,model='Cabin',scale=.85,yaw=1.4,group=buildings,collider=True)
# Dock approaches shallow water from the south, deck at the shoreline height.
entity('Rotting lake jetty',40,-10,y=.90,model='Dock',group=buildings,collider=True)
entity('Marsh trail boardwalk',-55,50,y=float(height(-55,50))+.10,model='Footbridge',group=buildings,collider=True)
entity('Overlook above the eastern shore',185,165,model='LookoutDeck',group=buildings,collider=True)
for iz in range(5):
    for ix in range(6):
        x=-40+ix*5.4+float(rng.uniform(-.5,.5)); z=181+iz*5.4
        entity(f'Grave {iz*6+ix+1:02}',x,z,model='WoodenCross' if (ix+iz)%5==0 else 'Gravestone',
               scale=float(rng.uniform(.8,1.12)),yaw=float(rng.uniform(-.22,.22)),group=buildings,collider=True)
for x,z,yaw in [(-45,179,0),(-45,183,0),(-45,203,0),(-45,207,0),(-18,211,math.pi/2),(-14,211,math.pi/2)]:
    entity('Broken cemetery fence',x,z,model='Fence',yaw=yaw,group=buildings,collider=True)
for x,z in [(-167,-207),(-122,-36),(-70,13),(-100,136),(156,-39),(197,120)]:
    entity('Weathered trail sign',x,z,model='Signpost',group=props,collider=True)
for x,z in [(-148,-87),(-147,-87),(-204,148),(23,-45),(26,-45),(-95,176)]:
    entity('Supply crate',x,z,model='Crate',yaw=float(rng.uniform(-.4,.4)),group=props,collider=True)

def clear_distance(x,z):
    d,_=path_distance(x,z)
    for _,px,pz,rx,rz in POIS:
        d=np.minimum(d,np.maximum(np.abs(x-px)-rx*1.2,np.abs(z-pz)-rz*1.2))
    d=np.minimum(d,np.hypot(x+205,z-155)-12)
    return d
# Stratified jitter gives dense but irregular woodland. Wet lake bed and all paths stay clear.
tree_count=0
for z0 in np.arange(-475,480,12):
    for x0 in np.arange(-475,480,12):
        x,z=x0+float(rng.uniform(-4.5,4.5)),z0+float(rng.uniform(-4.5,4.5))
        if height(x,z)<1.1 or clear_distance(x,z)<6 or rng.random()<.13: continue
        variant=int(rng.choice(4,p=[.47,.24,.18,.11])); name=['PineTall','PineYoung','Oak','Aspen'][variant]
        scale=float(rng.uniform(.75,1.25))
        e=entity(f'{name} {tree_count+1:04}',x,z,scale=scale,yaw=float(rng.uniform(0,math.tau)),group=forest)
        e['scale']=[scale]*3
        e['mesh_asset_path']=f'Assets/BWH_{name}/BWH_{name}.glb'
        e['lod_component']={'enabled':True,'override_asset_default':True,'config':{
            'level_count':4,'target_ratios':[1,.45,.16,.06],'distances':[0,24,52,95],'hysteresis_m':5}}
        # Trunk collision only, not expensive foliage mesh collision.
        e['collider']={'enabled':True,'shape':'capsule','radius':.22,'height':8,'center':[0,4,0],'layer':'static_world','friction':.8}
        tree_count+=1

for i in range(480):
    x,z=map(float,rng.uniform(-450,450,2))
    if height(x,z)<1 or clear_distance(x,z)<4: continue
    s=float(rng.uniform(.6,2.2))
    entity(f'Mossy boulder {i:03}',x,z,model='Rock'+str(i%3+1),scale=s,yaw=float(rng.uniform(0,math.tau)),group=props,collider=True)
for i in range(95):
    x,z=map(float,rng.uniform(-280,280,2))
    if height(x,z)<1 or clear_distance(x,z)<4: continue
    entity('Fallen timber' if i%3 else 'Rotten stump',x,z,model='FallenLog' if i%3 else 'Stump',
           yaw=float(rng.uniform(0,math.tau)),group=props,collider=True)
for i in range(240):
    a=float(rng.uniform(0,math.tau)); x=82+math.cos(a)*float(rng.uniform(83,94)); z=83+math.sin(a)*float(rng.uniform(114,128))
    if .4<height(x,z)<3.5:
        entity('Lake reeds',x,z,model='Reeds',scale=float(rng.uniform(.7,1.25)),yaw=a,group=props)
for i in range(550):
    x,z=map(float,rng.uniform(-300,300,2))
    if height(x,z)>1 and clear_distance(x,z)>3:
        entity('Forest undergrowth',x,z,model='GrassTuft',scale=float(rng.uniform(.8,1.8)),yaw=float(rng.uniform(0,math.tau)),group=props)

# A warm line of sparse lanterns pulls the player along the main route without lighting the whole map.
def lantern(x,z):
    global lid
    e=entity('Old oil lantern',x,z,model='LanternPost',group=props,collider=True)
    lid+=1
    entities.append(dict(type='dynamic_light',light_type='point',id=lid,name='Amber pool of light',position=[x+.6,e['position'][1]+2.8,z],
                         color=[1,.43,.14],intensity=3,radius=12,enabled=True))
    return e
for x,z in [(-167,-215),(-172,-145),(-154,-91),(-117,-39),(-78,-11),(-62,61),(-93,147),(-40,177),(21,-51),(166,-29),(199,132)]:
    lantern(x,z)

def fog(name,x,z,y,extents,density):
    e=entity(name,x,z,y=y,group=atmos)
    e['fog_volume']={'enabled':True,'shape':'box','color':[.24,.33,.34],'density':density,'half_extents':extents,'edge_fade':min(extents)*.8}
fog('Low mist / still lake',82,83,2,[94,3.3,121],.015)
fog('Low mist / cemetery',-25,194,float(height(-25,194))+1.3,[24,2.5,25],.025)
fog('Low mist / western hollow',-220,35,float(height(-220,35))+1.4,[38,3,40],.014)
fog('Cold haze / chapel',-90,170,float(height(-90,170))+2,[16,3,19],.007)
fog('Low mist / arrival trail',-172,-167,float(height(-172,-167))+1,[17,2.5,31],.010)

def particles(name,x,z,y,kind='dust',extents=(12,2,12)):
    e=entity(name,x,z,y=y,group=atmos)
    mist=kind=='mist'; firefly=kind=='firefly'
    p={'enabled':True,'play_on_start':True,'loop':True,'gpu_simulation':True,
       'texture_asset_id':'Assets/BlackwaterHollow/Textures/mist_sprite.png','blend_mode':'Additive' if firefly else 'Alpha',
       'emission_rate':3 if mist else 5,'burst_count':20,'max_particles':100 if mist else 160,
       'start_lifetime_min':12,'start_lifetime_max':20,'start_speed_min':.015,'start_speed_max':.07,
       'start_size_min':2.8 if mist else .035,'start_size_max':5 if mist else .07,
       'size_over_life':[.4,1,1,.3],'direction':[.7,.05,.3],'cone_angle':170,'gravity':0,'drag':.7,
       'shape':'Box','shape_extents':list(extents),'turbulence_enabled':True,'turbulence_strength':.025 if mist else .08,
       'turbulence_scale':9 if mist else 3.5,'turbulence_speed':.3,'soft_particles':True,'soft_distance':1.3 if mist else .3}
    color=[.3,.4,.4] if mist else ([.65,.8,.22] if firefly else [.64,.68,.58])
    opacity=.035 if mist else .65 if firefly else .30
    p['color_over_life']=sum([color+[a] for a in (0,opacity,opacity,0)],[])
    e['particle_system']=p
for x,z in [(-170,-160),(-130,-55),(-75,25),(-100,145),(175,130),(-230,40)]:
    particles('Suspended forest dust',x,z,float(height(x,z))+2.8)
for x,z in [(40,7),(125,165),(-25,194)]:
    particles('Slow ground mist',x,z,float(max(.8,height(x,z)))+.6,'mist',(18,.4,16))
for x,z in [(15,-28),(-30,198)]: particles('Faint marsh lights',x,z,float(height(x,z))+1.5,'firefly',(9,1,9))

def sound(name,clip,x=0,z=0,volume=.6,spatial=True,minimum=5,maximum=75):
    e=entity(name,x,z,y=float(max(2,height(x,z)+2)),group=atmos)
    e['audio_source']={'clip_asset_id':f'Assets/BlackwaterHollow/Audio/{clip}.wav','enabled':True,'loop':True,'is_3d':spatial,
        'play_on_start':True,'volume':volume,'pitch':1,'min_distance':minimum,'max_distance':maximum,'bus':'SFX' if spatial else 'Music'}
sound('Wind through canopy','ForestWind',volume=.72,spatial=False)
sound('Uneasy distant resonance','UneasyDrone',volume=.3,spatial=False)
sound('Water against the jetty','LakeLapping',32,-12,.7,True,12,145)
sound('Cabin timbers creaking','TimberCreak',-150,-80,.55,True,3,36)
sound('Boathouse rope creaks','TimberCreak',20,-43,.45,True,3,34)
sound('Crows above cemetery','DistantCrows',-25,194,.65,True,10,110)
sound('Crows in west wood','DistantCrows',-235,60,.45,True,8,100)

player=entity('Player',-170,-220,y=float(height(-170,-220))+.15,group=play)
player['scale']=[1,1,1]
player['character_controller']={'enabled':True,'camera_mode':'first_person','walk_speed':3.7,'run_speed':6.4,'jump_height':.8,
    'capsule_radius':.3,'capsule_height':1.8,'eye_height':1.64,'slope_limit_deg':46,'step_height':.42,'mouse_sensitivity':.12}
player['audio_listener']={'enabled':True}
player['script']={'backend':'Lua','script_asset_id':'Assets/BlackwaterHollow/Scripts/Explore.lua','enabled':True,'parameters':{
    'footstep':'Assets/BlackwaterHollow/Audio/Footstep.wav','whisper':'Assets/BlackwaterHollow/Audio/ForestWhisper.wav',
    'bell':'Assets/BlackwaterHollow/Audio/ChapelBell.wav','spawn_y':str(player['position'][1])}}
lid+=1
entities.append(dict(type='dynamic_light',light_type='spot',id=lid,name='Player torch',parent={'type':'mesh_entity','id':player['id']},
    position=[0,1.52,.16],rotation=[-.08,0,0],color=[.85,.9,.78],intensity=4,radius=23,inner_cone_deg=14,outer_cone_deg=27,enabled=True))
cam={'type':'camera','id':1,'name':'Main Camera','position':[-170,player['position'][1]+1.64,-220],
     'rotation':[-.055,0,0],'fov':72,'near':.08,'far':1200}
entities.append(cam)

def relative(path): return path.relative_to(ROOT).as_posix()
forest_tex=ROOT/'Assets/fű/grass_with_rocks_01_1k'
path_tex=ROOT/'Assets/materials/Terrain'
palette=[]
for i in range(8):
    slot={'slot':i,'display_name':['Forest floor','Old trail','Mossy rock','Wet earth'][min(i,3)],'tiling_scale_x':.45,'tiling_scale_y':.45}
    if i==0:
        slot.update(texture_path=relative(forest_tex/'grass_with_rocks_01_color_1k.png'),normal_texture_path=relative(forest_tex/'grass_with_rocks_01_normal_gl_1k.png'),
                    roughness_texture_path=relative(forest_tex/'grass_with_rocks_01_roughness_1k.png'),ao_texture_path=relative(forest_tex/'grass_with_rocks_01_ambientocclusion_1k.png'),color_tint=[.30,.32,.28])
    elif i==1:
        slot.update(texture_path=relative(path_tex/'cobblestone_floor_09_diff_1k.png'),normal_texture_path=relative(path_tex/'cobblestone_floor_09_nor_gl_1k.png'),
                    roughness_texture_path=relative(path_tex/'cobblestone_floor_09_rough_1k.png'),color_tint=[.58,.5,.38])
    else:
        slot.update(texture_path=f'Assets/BlackwaterHollow/Textures/{"stone" if i==2 else "soil"}.png',normal_texture_path='Assets/BlackwaterHollow/Textures/flat_normal.png',
                    roughness_texture_path='Assets/BlackwaterHollow/Textures/roughness.png',color_tint=[.65,.73,.65] if i==2 else [.72,.70,.63])
    slot.update(normal_strength=.85,ao_strength=.8,roughness_strength=.9,metallic_strength=0)
    slot['tiling']=[slot.pop('tiling_scale_x'),slot.pop('tiling_scale_y')]
    slot['tint']=slot.pop('color_tint')
    palette.append(slot)
for e in entities:
    if 'lod_component' in e:
        config=e['lod_component']['config']
        ratios=config.pop('target_ratios'); distances=config.pop('distances')
        for i in range(4): config[f'target_ratio_{i}']=ratios[i]; config[f'distance_m_{i}']=distances[i]
scene={'version':1,'metadata':{'name':'Blackwater Hollow','author':'Codex / original environment'},
       'editor_camera':{'eye':[-176,float(height(-176,-215))+3.6,-229],'yaw':.12,'pitch':-.10},'main_camera_id':1,
       'environment':{'directional_light_color':[.60,.72,.76],'directional_light_intensity':.9,
            'directional_light_angle_x':17,'directional_light_angle_y':22,'ambient_color':[.15,.21,.23],'ambient_intensity':.9,
            'sky':{'mode':'procedural','zenith_color':[.055,.095,.15],'horizon_color':[.27,.35,.37],'ground_color':[.06,.085,.075],
                'sun_size_degrees':.65,'sun_glow':.35,'god_rays':{'enabled':True,'technique':2,'intensity':.45,'length':.6,'falloff':.975,'quality':0,
                    'volumetric_intensity':1.2,'volumetric_density':.002,'volumetric_anisotropy':.42,'volumetric_distance':85,'volumetric_quality':0}},
            'tone_mapping':{'mode':'filmic','exposure_ev':-.18},
            'fog':{'enabled':True,'color':[.24,.33,.34],'density':.0032,'start_distance':8,'max_distance':1000,'max_opacity':.95,
                   'height_enabled':True,'base_height':14,'height_falloff':.035,'affect_sky':True}},
       'terrain':{'exists':False},'entities':entities,'terrain_palette':palette}
(STAGING/'draft.scene').write_text(json.dumps(scene,ensure_ascii=False,indent=2),encoding='utf-8')

lua='''-- Original exploration ambience. Engine CharacterController owns movement and collision.
function OnStart(self)
    self.ui = UiOpen("BlackwaterHollow/UI/explore.rml")
    self.steps = 0; self.time = 0; self.seen = {}; self.bellUsed = false; self.eWasDown = false
    self.safe = {-170, tonumber(self.params.spawn_y), -220}
    self.landmarks = {{"Kunyhó",-150,-80},{"Kápolna",-90,170},{"Temető",-25,194},{"Csónakház",20,-43}}
    UiSetText(self.ui,"status","Kövesd a lámpásokat. Valami még őrzi ezt a völgyet.")
end
function OnUpdate(self,dt)
    self.time = self.time + dt
    local x,y,z = GetPosition(self.id)
    local state = GetCharacterState(self.id)
    if state and state.grounded and state.moving then
        self.steps = self.steps + dt
        local interval = state.running and .33 or .52
        if self.steps > interval then PlayOneShot(self.params.footstep); self.steps = 0 end
    else self.steps = .25 end
    if state and state.grounded and y > .85 and math.abs(x)<490 and math.abs(z)<490 then self.safe={x,y,z} end
    if y < -1.2 or math.abs(x)>495 or math.abs(z)>495 then
        SetPosition(self.id,self.safe[1],self.safe[2]+.15,self.safe[3])
        UiSetText(self.ui,"status","A mély víz és a sziklák elzárják az utat.")
    end
    local count = 0
    for i,p in ipairs(self.landmarks) do
        local d=(x-p[2])^2+(z-p[3])^2
        if d<18^2 and not self.seen[i] then
            self.seen[i]=true; UiSetText(self.ui,"status",p[1].." felfedezve")
            if i==3 then PlayOneShot(self.params.whisper) end
        end
        if self.seen[i] then count=count+1 end
    end
    UiSetText(self.ui,"progress","Felfedezett helyek: "..count.." / 4")
    local atChapel=(x+90)^2+(z-170)^2<13^2
    local e=IsKeyDown("E")
    UiSetText(self.ui,"prompt",atChapel and not self.bellUsed and "E — kondítsd meg a harangot" or "")
    if atChapel and e and not self.eWasDown and not self.bellUsed then
        PlayOneShot(self.params.bell); self.bellUsed=true
        UiSetText(self.ui,"status","A harang visszhangja elveszik az erdőben.")
    end
    self.eWasDown=e
end
function OnDestroy(self)
    if self.ui and self.ui~=0 then UiClose(self.ui) end
end
'''
(ASSETS/'Scripts/Explore.lua').write_text(lua,encoding='utf-8')
(ASSETS/'UI/explore.rml').write_text('''<rml><head><title>Blackwater Hollow</title><link type="text/rcss" href="explore.rcss"/></head>
<body><div id="title">BLACKWATER HOLLOW</div><div id="progress">Felfedezett helyek: 0 / 4</div>
<div id="status"></div><div id="crosshair">+</div><div id="prompt"></div>
<div id="controls">WASD · mozgás &nbsp; SHIFT · futás &nbsp; SPACE · ugrás &nbsp; Jobb egér · körülnézés &nbsp; E · interakció</div></body></rml>''',encoding='utf-8')
(ASSETS/'UI/explore.rcss').write_text('''body { font-family: Roboto; font-size: 17dp; color: #d4dad2; }
#title { position: absolute; left: 32dp; top: 26dp; letter-spacing: 4dp; font-size: 21dp; color: #b5c5be; }
#progress { position: absolute; left: 32dp; top: 60dp; font-size: 14dp; color: #a8b4ad; }
#status { position: absolute; left: 32dp; top: 85dp; font-size: 15dp; }
#crosshair { position: absolute; left: 50%; top: 50%; color: #ffffff66; font-size: 17dp; }
#prompt { position: absolute; bottom: 88dp; width: 100%; text-align: center; color: #e2c38a; }
#controls { position: absolute; bottom: 28dp; width: 100%; text-align: center; font-size: 14dp; color: #a6b4ac; }
''',encoding='utf-8')

# Grounded, render-independent checks of the actual authored route, not mirrored implementation tests.
max_slope=0; route_length=0
for route in PATHS:
    for a,b in zip(route,route[1:]):
        length=math.dist(a,b); route_length+=length
        u=np.linspace(0,1,int(length)+1)
        h=height(a[0]+u*(b[0]-a[0]),a[1]+u*(b[1]-a[1]))
        max_slope=max(max_slope,float(np.max(np.abs(np.diff(h)))/(length/(len(h)-1))))
assert max_slope<math.tan(math.radians(46)), f'Path too steep: {max_slope}'
report={'seed':20261010,'size_m':[1000,1000],'height_range_m':[float(H.min()),float(H.max())],
        'trees':tree_count,'mesh_entities':eid,'route_length_m':route_length,'route_max_slope_degrees':math.degrees(math.atan(max_slope)),
        'paths':PATHS,'landmarks':POIS,'audio_files':8,'terrain_chunks':64}
(STAGING/'authoring-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
image=np.stack([.13+.002*np.maximum(H,0),.19+.002*np.maximum(H,0),.15+.001*np.maximum(H,0)],-1)
image[H<.7]=[.06,.16,.20]
im=Image.fromarray(np.uint8(np.clip(image,0,1)*255)).resize((1200,1200))
draw=ImageDraw.Draw(im)
def mapxy(x,z): return ((x+500)*1.2,(500-z)*1.2)
font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',18)
titlefont=ImageFont.truetype('C:/Windows/Fonts/segoeuib.ttf',32)
for path in PATHS: draw.line([mapxy(*p) for p in path],fill=(171,152,108),width=4)
for name,x,z,_,_ in POIS:
    px,pz=mapxy(x,z); draw.ellipse((px-5,pz-5,px+5,pz+5),fill=(236,186,91)); draw.text((px+9,pz-20),name,font=font,fill=(226,225,209))
px,pz=mapxy(-170,-220); draw.ellipse((px-7,pz-7,px+7,pz+7),fill=(142,218,180)); draw.text((px+12,pz),'START',font=font,fill='white')
draw.text((30,24),'BLACKWATER HOLLOW',font=titlefont,fill=(211,225,216))
draw.text((30,70),'1000 x 1000 m  |  N ↑  |  Explorer route',font=font,fill=(170,193,181))
draw.line([(45,1140),(165,1140)],fill='white',width=3); draw.text((45,1109),'100 m',font=font,fill='white')
im.save(STAGING/'BlackwaterHollow_Map.png')
print(json.dumps(report,indent=2),flush=True)
