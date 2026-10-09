#include "ScriptApiImpl.h"
#include "ScriptSystem.h"
#include "PhysicsWorld.h"
#include "EntityActivation.h"
#include "IxModuleApi.h"
#include "NativeBackend.h"
#include "SceneManager.h"
#include "SunShadow.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <iostream>
#include <limits>
#include <type_traits>

namespace ph = ixtreeme::physics;
int main()
{
    static_assert(IXTREEME_MODULE_API_VERSION == 7);
    static_assert(sizeof(ixscript::QueryFilter) == 16 && sizeof(ixscript::SphereOverlapHit) == 8);
    static_assert(std::is_trivially_copyable_v<ixscript::QueryFilter>);
    int errors=0;
    const auto check=[&](bool ok,const char* text){if(!ok){std::cerr<<text<<'\n';++errors;}};
    const auto shadow = MakeSunShadowProjection(1,2,64,16,200,2048);
    check(std::abs(shadow.m[14] + (64-200)*shadow.m[10])<1e-6f &&
        std::abs(shadow.m[14] + (64+200)*shadow.m[10]-1)<1e-6f,"shadow depth endpoints preserved");
    for (float x : {-15000.1f,-30.2f,0.0f,40.7f,15000.1f}) {
        const auto moved=MakeSunShadowProjection(x,2,64,16,200,2048);
        for(int i=0;i<16;++i) if(i!=12) check(moved.m[i]==shadow.m[i],"shadow movement changes translation only");
        const float shift=(moved.m[12]-shadow.m[12])*1024;
        check(shift==std::round(shift),"shadow shift remains whole texels");
    }
    const auto oldModule = +[](ixscript::IModuleRegistrar* registrar) -> std::uint32_t {
        registrar->RegisterScript("Abi6Rejected", +[]() -> ixscript::NativeScript* { return nullptr; });
        return 6;
    };
    check(!ixscript::InvokeGameModule(oldModule).versionOk,"ABI 6 rejected");
    const auto names = ixscript::NativeBackend::RegisteredNames();
    check(std::find(names.begin(),names.end(),"Abi6Rejected")==names.end(),"rejected ABI stages no factories");
    ScriptApiImpl api;
    unsigned calls=0;
    api.raycastFiltered=[&](const float*,const float* dir,float,const ixscript::QueryFilter& f){
        ++calls; check(std::abs(dir[0]-1)<0.0001f,"normalized ray");
        check(f.ignoreEntityId==5,"ignore forwarded");
        ixscript::RaycastHit hit; hit.hit=true; hit.entityId=7; return hit;
    };
    ixscript::QueryFilter f{4,0,5,0};
    check(api.RaycastFiltered(0,0,0,2,0,0,10,&f).entityId==7,"filtered ray callback");
    check(!api.RaycastFiltered(0,0,0,0,0,0,10,&f).hit,"zero direction");
    check(!api.RaycastFiltered(0,0,0,1,0,0,std::numeric_limits<float>::infinity(),&f).hit,"nonfinite ray");
    f.reserved=1; check(!api.RaycastFiltered(0,0,0,1,0,0,10,&f).hit,"reserved filter rejected"); f.reserved=0;
    check(calls==1,"invalid rays never reach physics");
    api.overlapSphere=[](const float*,float,const ixscript::QueryFilter&){
        return std::vector<ixscript::SphereOverlapHit>{{9,0},{0,0},{7,0},{9,1},{2,0}};
    };
    ixscript::SphereOverlapHit hits[1024]; unsigned cut=99;
    check(api.OverlapSphere(0,0,0,3,&f,hits,3,&cut)==3 && cut==1,"capacity/truncation");
    check(hits[0].entityId==0 && hits[1].entityId==2 && hits[2].entityId==7,"stable entity ordering");
    check(api.OverlapSphere(0,0,0,3,&f,hits,1024,&cut)==4 && !cut && hits[3].flags==1,"dedup/merged flags");
    check(!api.OverlapSphere(0,0,0,3,&f,nullptr,0,&cut) && cut,"zero capacity probe");
    check(!api.OverlapSphere(0,0,0,-1,&f,hits,3,&cut) && !cut,"invalid overlap");
    check(!api.OverlapSphere(0,0,0,1,&f,nullptr,1,&cut) && !cut,"null output");
    auto ordinaryOverlap = api.overlapSphere;
    api.overlapSphere=[](const float*,float,const ixscript::QueryFilter&) {
        std::vector<ixscript::SphereOverlapHit> out;
        for(unsigned i=0;i<1100;++i) out.push_back({1100-i,0});
        return out;
    };
    check(api.OverlapSphere(0,0,0,1,&f,hits,UINT32_MAX,&cut)==1024 && cut && hits[0].entityId==1,
        "capacity clamped before caller buffer writes");
    api.overlapSphere=ordinaryOverlap;
    api.SetEntityEnabled(99,false);
    check(api.deferredOps.size()==1 && !api.deferredOps[0].enabled,"enable deferred");
    api.deferredOps.clear();
    std::vector<MeshSceneEntity> meshes(3); std::vector<PointLight> points(1); std::vector<SpotLight> spots;
    meshes[0].id=1; meshes[0].enabled=false;
    points[0].id=1; points[0].parent={"mesh_entity",1};
    meshes[1].id=2; meshes[1].parent={"point_light",1};
    meshes[2].id=3; meshes[2].enabled=false; meshes[2].parent={"mesh_entity",2};
    ResolveEntityActivation(meshes,points,spots);
    check(!points[0].effectiveEnabled && !meshes[1].effectiveEnabled && meshes[1].enabled,"mixed parent inheritance");
    meshes[0].enabled=true; ResolveEntityActivation(meshes,points,spots);
    check(points[0].effectiveEnabled && meshes[1].effectiveEnabled && !meshes[2].effectiveEnabled,"local child flag preserved");
    meshes[0].parent={"mesh_entity",2}; ResolveEntityActivation(meshes,points,spots);
    check(!meshes[0].effectiveEnabled && !meshes[1].effectiveEnabled,"cycles inactive");
    SceneData scene; scene.meshEntities=meshes;
    const auto sceneFile=std::filesystem::temp_directory_path() /
        ("ix-abi7-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".scene");
    auto& scenes=SceneManager::Instance(); scenes.RestoreSceneSnapshot(scene,sceneFile.string(),false);
    check(scenes.SaveSceneAs(sceneFile.string()) && scenes.LoadScene(sceneFile.string()),"scene roundtrip");
    check(!scenes.GetCurrentScene().meshEntities[2].enabled && scenes.GetCurrentScene().meshEntities[1].enabled,
        "local enabled serialized independently of inherited state");
    scenes.CloseScene(); std::filesystem::remove(sceneFile);
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    ph::PhysicsWorld world; check(world.Create(),"physics created");
    const float zero[3]={0,0,0}; world.SetGravity(zero);
    ph::PhysicsBodyDesc desc; desc.bodyType=ph::BodyType::Dynamic;
    desc.rigidbody.useGravity=false; desc.rigidbody.linearDamping=0;
    desc.collider.layer=ph::PhysicsLayer::DynamicObject;
    auto body=world.CreateBody(desc); check(body!=0,"body created");
    ph::PhysicsQueryFilter q; q.layerMask=1u<<2;
    check(world.OverlapSphere(zero,2,q).size()==1,"layer overlap");
    q.layerMask=1u<<3; check(world.OverlapSphere(zero,2,q).empty(),"wrong layer"); q.layerMask=1u<<2;
    q.ignoreBody=body; check(world.OverlapSphere(zero,2,q).empty(),"ignore body"); q.ignoreBody=0;
    const float velocity[3]={2,0,0}; world.SetLinearVelocity(body,velocity);
    world.SetBodyEnabled(body,false);
    check(world.OverlapSphere(zero,2,q).empty(),"disabled absent from queries");
    world.Step(1.0f/60); ph::PhysicsTransform transform;
    check(world.GetBodyTransform(body,transform) && std::abs(transform.position[0])<0.0001,"disabled body frozen");
    float v[3]; check(world.GetLinearVelocity(body,v) && v[0]==2,"disabled velocity retained");
    world.SetBodyEnabled(body,true); world.SetBodyEnabled(body,true);
    check(world.OverlapSphere(zero,2,q).size()==1,"same body resumed");
    world.Step(1.0f/60); check(world.GetBodyTransform(body,transform) && transform.position[0]>0,"resumed velocity applied");
    desc.transform.position[0]=2;
    const auto joined=world.CreateBody(desc);
    check(world.CreateFixedJoint({body,joined})!=0,"joint created");
    world.SetBodyEnabled(body,false); world.Step(1.0f/60);
    world.SetBodyEnabled(body,true); world.Step(1.0f/60);
    world.DestroyBody(joined); world.Step(1.0f/60); // removes constraints that referenced the body
    desc.bodyType=ph::BodyType::Static; desc.collider.trigger=true; desc.transform.position[0]=5;
    const auto trigger=world.CreateBody(desc); const float center[3]={5,0,0};
    check(world.OverlapSphere(center,1,q).size()==1,"trigger included"); q.hitTriggers=false;
    check(world.OverlapSphere(center,1,q).empty(),"trigger excluded");
    world.SetBodyEnabled(trigger,false); world.DestroyBody(trigger); world.SetBodyEnabled(body,false);
    world.Destroy(); // removed bodies may be destroyed safely
#endif
    // Run actual VM bindings. Each marks success only after checking its result layout.
    api.resolveScriptSource=[](const std::string& id){ return id=="lua" ? std::string(R"(
function OnStart(self)
 local hits,cut=OverlapSphere(0,0,0,1,255,true,0,3)
 if #hits==3 and cut and hits[1].entityId==0 then SetEntityEnabled(107,false) end
 local hit,id=RaycastFiltered(0,0,0,2,0,0,10,4,false,5)
 if hit and id==7 then SetEntityEnabled(108,false) end
end
)") : std::string(R"(
class Script {
 void OnStart() {
  bool cut; array<SphereOverlapHit>@ hits=OverlapSphere(0,0,0,1,255,true,0,3,cut);
  if(hits.length()==3 && cut && hits[0].entityId==0) SetEntityEnabled(107,false);
  RaycastHit h;
  if(RaycastFiltered(0,0,0,2,0,0,10,4,false,5,h) && h.entityId==7) SetEntityEnabled(108,false);
 }
}
)"); };
    ixscript::ScriptSystem scripts(api);
    for(auto backend:{ixscript::ScriptBackendType::Lua,ixscript::ScriptBackendType::AngelScript}) {
        api.deferredOps.clear(); ixscript::ScriptComponent comp; comp.backend=backend;
        comp.scriptAssetId=backend==ixscript::ScriptBackendType::Lua ? "lua" : "as";
        auto script=scripts.CreateInstance(1,comp); check(bool(script),"VM script compiled");
        if(script) script->OnStart();
        check(api.deferredOps.size()==2 && api.deferredOps[0].id==107 && api.deferredOps[1].id==108,"VM query results and deferred enable");
    }
    return errors ? 1 : 0;
}
