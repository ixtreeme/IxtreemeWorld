#include "GeometryFixtures.h"
#include "tools/material/MaterialGraphImages.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <limits>

using namespace material_tool;
namespace {
int failures=0;
void Check(bool ok,const std::string& what) {if(!ok){std::cerr<<"FAIL: "<<what<<'\n';++failures;}}
bool Near(float a,float b,float eps=.0001f){return std::abs(a-b)<eps;}
float Mean(const GeometryBake& bake,float GeometryTexel::*field) {float sum=0;unsigned count=0;for(size_t i=0;i<bake.texels.size();++i)if(bake.coverage[i]==1){sum+=bake.texels[i].*field;++count;}return sum/count;}
}
int main(int argc,char**) {
    GeometryBakeSettings settings;settings.resolution=32;settings.aoSamples=32;settings.rayDistance=3;
    GeometryBake plane,box,corner,inside;std::string error;
    Check(BakeGeometry(geometry_fixtures::Plane(),settings,plane,error),"plane: "+error);
    if(plane.texels.empty())return 1;
    Check(plane.coveredTexels==1024 && plane.boundaryEdges==4,"plane coverage/topology");
    GeometryTexel sample;Check(plane.Sample(.5f,.5f,sample),"plane center exists");
    Check(Near(sample.ao,1) && Near(sample.curvature,.5f) && Near(sample.cavity,0) && Near(sample.thickness,0),"open flat plane reference maps");
    Check(Near(sample.objectPosition[0],.03125f) && Near(sample.objectPosition[2],.03125f) && Near(sample.worldNormal[1],1),"barycentric position/normal");
    Check(BakeGeometry(geometry_fixtures::Box(),settings,box,error),"box: "+error);
    if(box.texels.empty())return 1;
    Check(box.boundaryEdges==0,"UV seams weld into closed box topology");
    Check(Near(Mean(box,&GeometryTexel::ao),1) && Near(Mean(box,&GeometryTexel::thickness),1),"convex box AO/thickness reference");
    Check(Mean(box,&GeometryTexel::curvature)>.6f && Near(Mean(box,&GeometryTexel::cavity),0),"convex curvature and independent cavity");
    Check(BakeGeometry(geometry_fixtures::Corner(),settings,corner,error),"corner: "+error);
    if(corner.texels.empty())return 1;
    Check(Mean(corner,&GeometryTexel::ao)<.9f && Mean(corner,&GeometryTexel::curvature)<.5f && Mean(corner,&GeometryTexel::cavity)>.05f,"concave corner occlusion/curvature/cavity");
    auto shortRays=settings;shortRays.rayDistance=.01f;GeometryBake shortCorner;
    Check(BakeGeometry(geometry_fixtures::Corner(),shortRays,shortCorner,error),"short rays");
    Check(Mean(shortCorner,&GeometryTexel::ao)>Mean(corner,&GeometryTexel::ao) && Near(Mean(shortCorner,&GeometryTexel::curvature),Mean(corner,&GeometryTexel::curvature)) && Near(Mean(shortCorner,&GeometryTexel::cavity),Mean(corner,&GeometryTexel::cavity)),"cavity/curvature do not derive from AO");
    Check(BakeGeometry(geometry_fixtures::Box(true),settings,inside,error),"inward box");
    Check(Near(Mean(inside,&GeometryTexel::ao),0) && Mean(inside,&GeometryTexel::curvature)<.4f && Mean(inside,&GeometryTexel::cavity)>.2f,"closed inward box reference");
    auto transformed=settings;transformed.rayDistance=10;transformed.objectToWorld={2,0,0,3, 0,3,0,4, 0,0,-4,5, 0,0,0,1};GeometryBake world;
    Check(BakeGeometry(geometry_fixtures::Box(),transformed,world,error),"nonuniform reflected transform");
    for(size_t i=0;i<world.texels.size();++i)if(world.coverage[i]==1){const auto& t=world.texels[i];Check(Near(t.worldPosition[0],t.objectPosition[0]*2+3) && Near(t.worldPosition[2],t.objectPosition[2]*-4+5),"world position transform");Check(Near(t.worldNormal[2],-t.objectNormal[2]),"inverse transpose reflected normal");Check(Near(t.thickness,std::abs(t.objectNormal[0])*2+std::abs(t.objectNormal[1])*3+std::abs(t.objectNormal[2])*4),"thickness measured in transformed world units");}
    auto smooth=geometry_fixtures::Plane();for(auto& v:smooth.vertices)v.normal={.577350269f,.577350269f,.577350269f};GeometryBake smoothWorld;
    Check(BakeGeometry(smooth,transformed,smoothWorld,error),"nonuniform transform of non-axis normals");smoothWorld.Sample(.5f,.5f,sample);
    const float length=std::sqrt(.25f+1.0f/9+1.0f/16);
    Check(Near(sample.worldNormal[0],.5f/length) && Near(sample.worldNormal[1],(1.0f/3)/length) && Near(sample.worldNormal[2],-.25f/length),"inverse transpose differs from position transform");
    transformed.objectToWorld[0]=0;Check(!BakeGeometry(geometry_fixtures::Plane(),transformed,world,error),"singular transform rejected");
    GeometryBake padded;Check(BakeGeometry(geometry_fixtures::Plane(.25f),settings,padded,error),"padded island");
    Check(padded.coveredTexels==256 && padded.coverage[4*32+4]==2 && padded.coverage[3*32+3]==0,"padding distance is bounded");
    Check(padded.texels[4*32+4].uv==padded.texels[8*32+8].uv,"padding retains source UV");
    const auto oldHash=plane.meshHash;auto invalid=geometry_fixtures::Plane();invalid.indices[0]=999;
    Check(!BakeGeometry(invalid,settings,plane,error) && plane.meshHash==oldHash,"bad indices preserve prior bake");
    invalid=geometry_fixtures::Plane();invalid.vertices[0].uv[0]=-1;Check(!BakeGeometry(invalid,settings,plane,error),"UV wrapping rejected");
    invalid=geometry_fixtures::Plane();invalid.vertices[0].position[0]=std::numeric_limits<float>::quiet_NaN();Check(!BakeGeometry(invalid,settings,plane,error),"nonfinite geometry rejected");
    invalid=geometry_fixtures::Plane();invalid.indices.insert(invalid.indices.end(),{0,1,2});Check(!BakeGeometry(invalid,settings,plane,error),"nonmanifold topology rejected");
    invalid=geometry_fixtures::Plane();auto duplicate=invalid;for(auto& v:duplicate.vertices)v.position[1]+=2;invalid.vertices.insert(invalid.vertices.end(),duplicate.vertices.begin(),duplicate.vertices.end());for(auto i:duplicate.indices)invalid.indices.push_back(i+4);Check(!BakeGeometry(invalid,settings,plane,error) && error.find("overlap")!=std::string::npos,"UV overlap rejected");
    unsigned calls=0;Check(!BakeGeometry(geometry_fixtures::Box(),settings,plane,error,{[&]{return ++calls>2;},{}}) && plane.meshHash==oldHash,"mid-bake cancellation is transactional");
    std::string bytes,again;GeometryBake decoded;
    Check(EncodeGeometryBake(box,bytes,error) && DecodeGeometryBake(bytes,decoded,error) && EncodeGeometryBake(decoded,again,error) && bytes==again,"portable exact cache roundtrip");
    bytes[150]^=1;Check(!DecodeGeometryBake(bytes,decoded,error) && decoded.meshHash==box.meshHash,"corrupt cache preserves valid destination");
    Check(!DecodeGeometryBake("IXGEOM02",decoded,error),"unsupported cache version");
    GeometryBake repeat;Check(BakeGeometry(geometry_fixtures::Corner(),settings,repeat,error),"deterministic repeated bake");
    Check(EncodeGeometryBake(corner,bytes,error) && EncodeGeometryBake(repeat,again,error) && bytes==again,"seed/sample results deterministic");
    auto mesh=geometry_fixtures::Plane();Check(GeometryMeshHash(mesh)==oldHash,"mesh content identity");mesh.vertices[0].position[0]+=.1f;Check(GeometryMeshHash(mesh)!=oldHash,"modified positions invalidate identity");
    auto graph=MaterialGraphDocument::WeatheredStone();MaterialGraphProgram program;BakedMaterialImages images;
    Check(program.Compile(graph,error),"Weathered Stone compile");
    Check(!BakeMaterialUV(program,{}, {},32,images,error),"geometry graph still rejects synthetic UV bake");
    Check(BakeMaterialOnGeometry(program,{}, {},corner,images,error) && images.size==32 && !images.color.empty(),"Weathered Stone real-mesh material bake");
    PreviewSettings preview;auto pixels=RenderMaterialPreview(program,{}, {},preview,32,&corner);Check(pixels.size()==4096,"real-geometry UV preview");
    auto channels=MaterialGraphDocument::Example();const auto ao=channels.AddNode(NodeKind::AO,{}),curvatureNode=channels.AddNode(NodeKind::Curvature,{}),cavityNode=channels.AddNode(NodeKind::Cavity,{});
    const auto output=std::find_if(channels.nodes.begin(),channels.nodes.end(),[](const auto& n){return n.kind==NodeKind::PbrOutput;})->id;
    const auto pins=NodePins(NodeKind::PbrOutput);
    for(size_t slot=0;slot<pins.size();++slot){if(std::string(pins[slot].name)=="AO")Check(channels.Connect(PinId(ao,0),PinId(output,slot),error),"wire real AO");if(std::string(pins[slot].name)=="Height")Check(channels.Connect(PinId(curvatureNode,0),PinId(output,slot),error),"wire real curvature");if(std::string(pins[slot].name)=="Metallic")Check(channels.Connect(PinId(cavityNode,0),PinId(output,slot),error),"wire real cavity");}
    Check(program.Compile(channels,error) && BakeMaterialOnGeometry(program,{}, {},corner,images,error),"existing geometry nodes consume real bake in the shared evaluator");
    const auto byte=[](float f){return static_cast<std::uint8_t>(f*255+.5f);};
    for(size_t i=0;i<corner.texels.size();++i)if(corner.coverage[i]){Check(images.ao[i*4]==byte(corner.texels[i].ao) && images.height[i*4]==byte(corner.texels[i].curvature) && images.metallic[i*4]==byte(corner.texels[i].cavity),"material channels match independent baked maps");}
    preview.channel=PreviewChannel::Curvature;pixels=RenderMaterialPreview(program,{}, {},preview,32,&corner);
    for(size_t i=0;i<corner.texels.size();++i)if(corner.coverage[i])Check(pixels[i*4]==byte(corner.texels[i].curvature),"raw map preview preserves scalar values");
    GeometryBakeJob job;Check(job.Start(geometry_fixtures::Plane(),settings),"async job starts");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(job.Running() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(job.TakeResult(decoded,error) && error.empty() && decoded.meshHash==oldHash && Near(job.Progress(),1),"async production completion");
    settings.resolution=512;settings.aoSamples=128;Check(job.Start(geometry_fixtures::Corner(),settings),"cancel job starts");job.Cancel();
    while(job.Running() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(job.TakeResult(decoded,error) && error.find("cancelled")!=std::string::npos && decoded.meshHash==oldHash,"async cancellation preserves result");
    if(argc>1){settings.resolution=256;settings.aoSamples=32;const auto start=std::chrono::steady_clock::now();Check(BakeGeometry(geometry_fixtures::Box(),settings,repeat,error),"benchmark box");std::cout<<"box 12 triangles, 256x256, 32 AO samples: "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<" ms; covered="<<repeat.coveredTexels<<"\n";}
    return failures?1:0;
}
