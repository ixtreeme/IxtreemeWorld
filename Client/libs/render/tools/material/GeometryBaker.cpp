#include "GeometryBaker.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace material_tool {
namespace {
using V = GeometryVec3;
constexpr float Pi = 3.14159265358979323846f;
V Add(V a,V b) { for(int k=0;k<3;++k)a[k]+=b[k];return a; }
V Sub(V a,V b) { for(int k=0;k<3;++k)a[k]-=b[k];return a; }
V Mul(V a,float s) { for(auto& v:a)v*=s;return a; }
float Dot(V a,V b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
V Cross(V a,V b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
V Unit(V a) { const auto length=std::sqrt(Dot(a,a));return length>0?Mul(a,1/length):V{0,1,0}; }
bool Finite(V a) { return std::all_of(a.begin(),a.end(),[](float v){return std::isfinite(v);}); }
V Interpolate(V a,V b,V c,float u,float v,float w) { return Add(Add(Mul(a,u),Mul(b,v)),Mul(c,w)); }
float Area(std::array<float,2> a,std::array<float,2> b,std::array<float,2> c) { return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]); }
bool SettingsValid(const GeometryBakeSettings& s,std::string& error) {
    if(s.resolution<16 || s.resolution>512 || s.padding>32 || s.aoSamples<1 || s.aoSamples>128 ||
       !std::isfinite(s.rayDistance) || !std::isfinite(s.rayBias) || s.rayBias<=0 || s.rayDistance<=s.rayBias) {
        error="Geometry bake settings: resolution 16..512, padding 0..32, AO samples 1..128, positive ray bias below distance required";return false;
    }
    const auto& m=s.objectToWorld;
    if(!std::all_of(m.begin(),m.end(),[](float v){return std::isfinite(v);}) || m[12]!=0 || m[13]!=0 || m[14]!=0 || m[15]!=1) {
        error="Geometry transform must be a finite affine matrix";return false;
    }
    const float det=Dot(V{m[0],m[4],m[8]},Cross(V{m[1],m[5],m[9]},V{m[2],m[6],m[10]}));
    if(!std::isfinite(det) || std::abs(det)<1e-12f) {error="Geometry transform is singular";return false;}return true;
}
V Point(const std::array<float,16>& m,V p) { return {m[0]*p[0]+m[1]*p[1]+m[2]*p[2]+m[3],m[4]*p[0]+m[5]*p[1]+m[6]*p[2]+m[7],m[8]*p[0]+m[9]*p[1]+m[10]*p[2]+m[11]}; }
struct Triangle { V a,b,c,normal,objectNormal,center; };
struct Bounds {
    V lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
    void Include(V p) {for(int k=0;k<3;++k){lo[k]=std::min(lo[k],p[k]);hi[k]=std::max(hi[k],p[k]);}}
    bool Hit(V p,V d,float far) const {
        float near=0;
        for(int k=0;k<3;++k) {
            if(std::abs(d[k])<1e-20f){if(p[k]<lo[k] || p[k]>hi[k])return false;continue;}
            auto a=(lo[k]-p[k])/d[k],b=(hi[k]-p[k])/d[k];if(a>b)std::swap(a,b);
            near=std::max(near,a);far=std::min(far,b);if(near>far)return false;
        }return true;
    }
};
class Bvh {
    struct Node {Bounds box;unsigned begin=0,count=0,left=0,right=0;};
    const std::vector<Triangle>& triangles_;
    std::vector<Node> nodes_;
    std::vector<unsigned> order_;
    unsigned Build(unsigned begin,unsigned count) {
        const auto id=static_cast<unsigned>(nodes_.size());nodes_.push_back({});
        Bounds centers,box;
        for(unsigned i=begin;i<begin+count;++i){const auto& t=triangles_[order_[i]];box.Include(t.a);box.Include(t.b);box.Include(t.c);centers.Include(t.center);}
        nodes_[id].box=box;
        if(count<=8){nodes_[id].begin=begin;nodes_[id].count=count;return id;}
        int axis=0;for(int k=1;k<3;++k)if(centers.hi[k]-centers.lo[k]>centers.hi[axis]-centers.lo[axis])axis=k;
        const auto middle=begin+count/2;
        std::nth_element(order_.begin()+begin,order_.begin()+middle,order_.begin()+begin+count,[&](unsigned a,unsigned b){return triangles_[a].center[axis]<triangles_[b].center[axis] || (triangles_[a].center[axis]==triangles_[b].center[axis] && a<b);});
        const auto left=Build(begin,count/2),right=Build(middle,count-count/2);nodes_[id].left=left;nodes_[id].right=right;return id;
    }
public:
    explicit Bvh(const std::vector<Triangle>& triangles):triangles_(triangles),order_(triangles.size()) {std::iota(order_.begin(),order_.end(),0);nodes_.reserve(triangles.size()*2);Build(0,static_cast<unsigned>(order_.size()));}
    bool Trace(V p,V direction,unsigned skip,float& distance,bool exitOnly=false) const {
        unsigned stack[64]{},count=1;bool hit=false;
        while(count) {
            const auto& node=nodes_[stack[--count]];if(!node.box.Hit(p,direction,distance))continue;
            if(!node.count){stack[count++]=node.right;stack[count++]=node.left;continue;}
            for(unsigned i=node.begin;i<node.begin+node.count;++i) {
                const auto index=order_[i];if(index==skip)continue;const auto& t=triangles_[index];
                if(exitOnly && Dot(t.normal,direction)<=0)continue;
                const auto e1=Sub(t.b,t.a),e2=Sub(t.c,t.a),h=Cross(direction,e2);const auto det=Dot(e1,h);
                if(std::abs(det)<1e-12f)continue;
                const auto s=Sub(p,t.a);const float u=Dot(s,h)/det;if(u<0 || u>1)continue;
                const auto q=Cross(s,e1);const float v=Dot(direction,q)/det;if(v<0 || u+v>1)continue;
                const float d=Dot(e2,q)/det;if(d>0 && d<distance){distance=d;hit=true;}
            }
        }return hit;
    }
};
std::uint32_t Mix(std::uint32_t a) {a^=a>>16;a*=0x7feb352dU;a^=a>>15;a*=0x846ca68bU;return a^(a>>16);}
std::uint64_t Hash(const char* data,size_t size) {std::uint64_t h=14695981039346656037ULL;for(size_t i=0;i<size;++i){h^=static_cast<unsigned char>(data[i]);h*=1099511628211ULL;}return h;}
void U32(std::string& s,std::uint32_t v) {for(unsigned k=0;k<4;++k)s+=static_cast<char>(v>>(k*8));}
void U64(std::string& s,std::uint64_t v) {U32(s,static_cast<std::uint32_t>(v));U32(s,static_cast<std::uint32_t>(v>>32));}
void F32(std::string& s,float v) {U32(s,std::bit_cast<std::uint32_t>(v));}
struct Reader {
    const std::string& data;size_t offset=0;
    std::uint32_t U32() {std::uint32_t v=0;for(unsigned k=0;k<4;++k)v|=static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset++]))<<(8*k);return v;}
    std::uint64_t U64() {const auto lo=U32();return lo|(static_cast<std::uint64_t>(U32())<<32);}
    float F32() {return std::bit_cast<float>(U32());}
};
void WriteSettings(std::string& s,const GeometryBakeSettings& v) {U32(s,v.resolution);U32(s,v.padding);U32(s,v.aoSamples);F32(s,v.rayDistance);F32(s,v.rayBias);U32(s,v.seed);for(float f:v.objectToWorld)F32(s,f);}
GeometryBakeSettings ReadSettings(Reader& r) {GeometryBakeSettings s;s.resolution=r.U32();s.padding=r.U32();s.aoSamples=r.U32();s.rayDistance=r.F32();s.rayBias=r.F32();s.seed=r.U32();for(auto& f:s.objectToWorld)f=r.F32();return s;}
}

std::uint64_t GeometryMeshHash(const GeometryMesh& mesh) {
    std::string bytes;bytes.reserve(mesh.vertices.size()*32+mesh.indices.size()*4+8);
    U32(bytes,static_cast<std::uint32_t>(mesh.vertices.size()));U32(bytes,static_cast<std::uint32_t>(mesh.indices.size()));
    for(const auto& v:mesh.vertices){for(float f:v.position)F32(bytes,f);for(float f:v.normal)F32(bytes,f);for(float f:v.uv)F32(bytes,f);}
    for(auto i:mesh.indices)U32(bytes,i);return Hash(bytes.data(),bytes.size());
}
bool BakeGeometry(const GeometryMesh& mesh,const GeometryBakeSettings& settings,GeometryBake& result,std::string& error,const GeometryBakeControl& control) {
    error.clear();const auto stop=[&] {if(control.cancelled && control.cancelled()){error="Geometry bake cancelled";return true;}return false;};
    const auto progress=[&](float v){if(control.progress)control.progress(v);};
    if(stop() || !SettingsValid(settings,error))return false;
    if(mesh.vertices.empty() || mesh.vertices.size()>1000000 || mesh.indices.empty() || mesh.indices.size()%3 || mesh.indices.size()>3000000) {error="Geometry bake requires an indexed triangle mesh (maximum 1M vertices/triangles)";return false;}
    std::map<V,unsigned> weld;std::vector<unsigned> welded(mesh.vertices.size());std::vector<V> positions,world,worldNormals;
    world.reserve(mesh.vertices.size());worldNormals.reserve(mesh.vertices.size());
    const auto& m=settings.objectToWorld;
    const V col0{m[0],m[4],m[8]},col1{m[1],m[5],m[9]},col2{m[2],m[6],m[10]};
    const float determinant=Dot(col0,Cross(col1,col2));
    for(size_t i=0;i<mesh.vertices.size();++i) {
        if(i%1024==0 && stop())return false;const auto& v=mesh.vertices[i];
        if(!Finite(v.position) || !Finite(v.normal) || Dot(v.normal,v.normal)<1e-12f || !std::isfinite(v.uv[0]) || !std::isfinite(v.uv[1]) || v.uv[0]<0 || v.uv[0]>1 || v.uv[1]<0 || v.uv[1]>1) {error="Geometry vertex has invalid position/normal or UV outside the 0..1 atlas";return false;}
        const auto [it,inserted]=weld.emplace(v.position,static_cast<unsigned>(positions.size()));if(inserted)positions.push_back(v.position);welded[i]=it->second;
        world.push_back(Point(m,v.position));
        // Columns of the cofactor matrix divided by determinant = inverse transpose.
        worldNormals.push_back(Unit(Mul(Add(Add(Mul(Cross(col1,col2),v.normal[0]),Mul(Cross(col2,col0),v.normal[1])),Mul(Cross(col0,col1),v.normal[2])),1/determinant)));
        if(!Finite(world.back()) || !Finite(worldNormals.back())){error="Geometry transform overflow";return false;}
    }
    struct EdgeFace {unsigned face,a,b,opposite;};
    std::map<std::pair<unsigned,unsigned>,std::vector<EdgeFace>> edges;
    std::vector<Triangle> triangles;triangles.reserve(mesh.indices.size()/3);
    for(unsigned f=0;f<mesh.indices.size()/3;++f) {
        if(f%1024==0 && stop())return false;
        const auto a=mesh.indices[f*3],b=mesh.indices[f*3+1],c=mesh.indices[f*3+2];
        if(a>=mesh.vertices.size() || b>=mesh.vertices.size() || c>=mesh.vertices.size()){error="Geometry triangle index is out of range";return false;}
        const auto objectCross=Cross(Sub(mesh.vertices[b].position,mesh.vertices[a].position),Sub(mesh.vertices[c].position,mesh.vertices[a].position));
        const auto worldCross=Cross(Sub(world[b],world[a]),Sub(world[c],world[a]));
        if(!Finite(objectCross) || !Finite(worldCross) || Dot(objectCross,objectCross)<1e-20f || Dot(worldCross,worldCross)<1e-20f || std::abs(Area(mesh.vertices[a].uv,mesh.vertices[b].uv,mesh.vertices[c].uv))<1e-12f){error="Geometry triangle has degenerate geometry or UVs";return false;}
        const auto objectNormal=Unit(objectCross);
        if(Dot(objectNormal,Add(Add(mesh.vertices[a].normal,mesh.vertices[b].normal),mesh.vertices[c].normal))<=0){error="Geometry triangle winding disagrees with its normals";return false;}
        triangles.push_back({world[a],world[b],world[c],Mul(Unit(worldCross),determinant<0?-1.0f:1.0f),objectNormal,Add(world[a],Mul(Add(Sub(world[b],world[a]),Sub(world[c],world[a])),1.0f/3))});
        const unsigned vs[]={welded[a],welded[b],welded[c]};
        for(unsigned k=0;k<3;++k){const auto x=vs[k],y=vs[(k+1)%3];edges[std::minmax(x,y)].push_back({f,x,y,vs[(k+2)%3]});}
    }
    GeometryBake draft;draft.settings=settings;draft.meshHash=GeometryMeshHash(mesh);
    std::vector<float> bends(positions.size()),concavity(positions.size()),weights(positions.size());
    for(const auto& [edge,faces]:edges) {
        if(faces.size()==1){++draft.boundaryEdges;continue;}
        if(faces.size()!=2 || faces[0].a!=faces[1].b || faces[0].b!=faces[1].a){error="Geometry has non-manifold edges or inconsistent winding";return false;}
        const auto n0=triangles[faces[0].face].objectNormal,n1=triangles[faces[1].face].objectNormal;
        const float angle=std::acos(std::clamp(Dot(n0,n1),-1.0f,1.0f));
        const bool convex=Dot(n1,Sub(positions[faces[0].opposite],positions[edge.first]))<=0;
        const float signedAngle=convex?angle:-angle,length=std::sqrt(Dot(Sub(positions[edge.second],positions[edge.first]),Sub(positions[edge.second],positions[edge.first])));
        for(auto v:{edge.first,edge.second}){bends[v]+=signedAngle*length;concavity[v]+=std::max(0.0f,-signedAngle)*length;weights[v]+=length;}
    }
    std::vector<float> curvature(positions.size(),.5f),cavity(positions.size());
    for(size_t i=0;i<positions.size();++i)if(weights[i]>0){curvature[i]=std::clamp(.5f+.5f*bends[i]/(Pi*weights[i]),0.0f,1.0f);cavity[i]=std::clamp(concavity[i]/(Pi*weights[i]),0.0f,1.0f);}
    const auto size=settings.resolution;const size_t count=static_cast<size_t>(size)*size;
    draft.texels.resize(count);draft.coverage.resize(count);
    std::vector<unsigned> owners(count,std::numeric_limits<unsigned>::max());
    std::vector<bool> interior(count);
    for(unsigned f=0;f<triangles.size();++f) {
        if(f%64==0){if(stop())return false;progress(.15f+.15f*f/triangles.size());}
        const auto a=mesh.indices[f*3],b=mesh.indices[f*3+1],c=mesh.indices[f*3+2];
        const auto& va=mesh.vertices[a];const auto& vb=mesh.vertices[b];const auto& vc=mesh.vertices[c];const float area=Area(va.uv,vb.uv,vc.uv);
        const int x0=std::max(0,static_cast<int>(std::ceil(std::min({va.uv[0],vb.uv[0],vc.uv[0]})*size-.5f))),x1=std::min(static_cast<int>(size)-1,static_cast<int>(std::floor(std::max({va.uv[0],vb.uv[0],vc.uv[0]})*size-.5f)));
        const int y0=std::max(0,static_cast<int>(std::ceil(std::min({va.uv[1],vb.uv[1],vc.uv[1]})*size-.5f))),y1=std::min(static_cast<int>(size)-1,static_cast<int>(std::floor(std::max({va.uv[1],vb.uv[1],vc.uv[1]})*size-.5f)));
        const float du1=vb.uv[0]-va.uv[0],dv1=vb.uv[1]-va.uv[1],du2=vc.uv[0]-va.uv[0],dv2=vc.uv[1]-va.uv[1];
        const auto e1=Sub(world[b],world[a]),e2=Sub(world[c],world[a]);
        const auto rawTangent=Mul(Sub(Mul(e1,dv2),Mul(e2,dv1)),1/area),rawBitangent=Mul(Sub(Mul(e2,du1),Mul(e1,du2)),1/area);
        for(int y=y0;y<=y1;++y){if(stop())return false;for(int x=x0;x<=x1;++x) {
            const std::array<float,2> uv{(x+.5f)/size,(y+.5f)/size};
            const float u=Area(vb.uv,vc.uv,uv)/area,v=Area(vc.uv,va.uv,uv)/area,w=1-u-v;
            if(std::min({u,v,w})<-1e-6f)continue;const bool inside=std::min({u,v,w})>1e-6f;const size_t at=static_cast<size_t>(y)*size+x;
            if(draft.coverage[at]){if(interior[at] || inside){error="Geometry UV atlas overlaps at texel "+std::to_string(x)+","+std::to_string(y);return false;}continue;}
            auto& t=draft.texels[at];t.uv=uv;t.objectPosition=Interpolate(va.position,vb.position,vc.position,u,v,w);t.worldPosition=Interpolate(world[a],world[b],world[c],u,v,w);
            t.objectNormal=Unit(Interpolate(va.normal,vb.normal,vc.normal,u,v,w));t.worldNormal=Unit(Interpolate(worldNormals[a],worldNormals[b],worldNormals[c],u,v,w));
            t.worldTangent=Unit(Sub(rawTangent,Mul(t.worldNormal,Dot(rawTangent,t.worldNormal))));
            t.worldBitangent=Mul(Cross(t.worldNormal,t.worldTangent),Dot(Cross(t.worldNormal,t.worldTangent),rawBitangent)<0?-1.0f:1.0f);
            t.curvature=curvature[welded[a]]*u+curvature[welded[b]]*v+curvature[welded[c]]*w;t.cavity=cavity[welded[a]]*u+cavity[welded[b]]*v+cavity[welded[c]]*w;
            draft.coverage[at]=1;owners[at]=f;interior[at]=inside;++draft.coveredTexels;
        }}
    }
    if(!draft.coveredTexels){error="Geometry UV atlas has no covered texels at this resolution";return false;}
    if(stop())return false;progress(.3f);const Bvh bvh(triangles);
    unsigned finished=0;
    for(size_t at=0;at<count;++at)if(draft.coverage[at]) {
        if(finished%64==0){if(stop())return false;progress(.3f+.6f*finished/draft.coveredTexels);}++finished;
        auto& t=draft.texels[at];const auto n=t.worldNormal;
        const auto tangent=Unit(Cross(std::abs(n[1])<.9f?V{0,1,0}:V{1,0,0},n)),bitangent=Cross(n,tangent);
        const auto origin=Add(t.worldPosition,Mul(n,settings.rayBias));unsigned occluded=0;
        const float rotation=(Mix(static_cast<std::uint32_t>(at)^settings.seed)&0xffffffU)*(2*Pi/16777216.0f);
        for(unsigned sample=0;sample<settings.aoSamples;++sample) {
            const float r=std::sqrt((sample+.5f)/settings.aoSamples),phi=rotation+sample*2.39996323f;
            const auto direction=Add(Add(Mul(tangent,r*std::cos(phi)),Mul(bitangent,r*std::sin(phi))),Mul(n,std::sqrt(1-r*r)));
            float distance=settings.rayDistance;if(bvh.Trace(origin,direction,owners[at],distance))++occluded;
        }
        t.ao=1-static_cast<float>(occluded)/settings.aoSamples;
        float distance=settings.rayDistance;
        if(bvh.Trace(Sub(t.worldPosition,Mul(n,settings.rayBias)),Mul(n,-1),owners[at],distance,true))t.thickness=distance+settings.rayBias;
    }
    // Deterministic 8-neighbour dilation, bounded Chebyshev distance. No wrapping across the atlas.
    std::vector<unsigned> queue;queue.reserve(count);std::vector<unsigned> depth(count);
    for(unsigned i=0;i<count;++i)if(draft.coverage[i]==1)queue.push_back(i);
    for(size_t head=0;head<queue.size();++head) {
        if(head%1024==0 && stop())return false;const auto at=queue[head];if(depth[at]>=settings.padding)continue;
        const int x=at%size,y=at/size;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const int nx=x+dx,ny=y+dy;if(nx<0 || ny<0 || nx>=static_cast<int>(size) || ny>=static_cast<int>(size))continue;
            const auto next=static_cast<unsigned>(ny)*size+nx;if(draft.coverage[next])continue;draft.coverage[next]=2;draft.texels[next]=draft.texels[at];depth[next]=depth[at]+1;queue.push_back(next);}
    }
    if(stop() || !ValidateGeometryBake(draft,error))return false;progress(1);result=std::move(draft);return true;
}
bool GeometryBake::Sample(float u,float v,GeometryTexel& texel) const {
    const auto size=settings.resolution;
    if(size<16 || size>512 || !std::isfinite(u) || !std::isfinite(v) || u<0 || u>1 || v<0 || v>1 || texels.size()!=static_cast<size_t>(size)*size || coverage.size()!=texels.size())return false;
    const auto x=std::min(static_cast<unsigned>(u*size),size-1),y=std::min(static_cast<unsigned>(v*size),size-1);const size_t at=static_cast<size_t>(y)*size+x;
    if(!coverage[at])return false;texel=texels[at];return true;
}
bool ValidateGeometryBake(const GeometryBake& bake,std::string& error) {
    error.clear();if(!SettingsValid(bake.settings,error))return false;
    const size_t count=static_cast<size_t>(bake.settings.resolution)*bake.settings.resolution;
    if(bake.texels.size()!=count || bake.coverage.size()!=count || !bake.meshHash){error="Geometry cache has invalid dimensions or mesh identity";return false;}
    unsigned covered=0;
    for(size_t i=0;i<count;++i){const auto& t=bake.texels[i];if(bake.coverage[i]>2){error="Geometry cache has invalid coverage";return false;}if(bake.coverage[i]==1)++covered;
        if(!std::isfinite(t.uv[0]) || !std::isfinite(t.uv[1]) || t.uv[0]<0 || t.uv[0]>1 || t.uv[1]<0 || t.uv[1]>1 || !Finite(t.objectPosition) || !Finite(t.worldPosition) || !Finite(t.objectNormal) || !Finite(t.worldNormal) || !Finite(t.worldTangent) || !Finite(t.worldBitangent) || std::abs(Dot(t.objectNormal,t.objectNormal)-1)>.01f || std::abs(Dot(t.worldNormal,t.worldNormal)-1)>.01f || std::abs(Dot(t.worldTangent,t.worldTangent)-1)>.01f || std::abs(Dot(t.worldBitangent,t.worldBitangent)-1)>.01f || std::abs(Dot(t.worldNormal,t.worldTangent))>.01f || std::abs(Dot(t.worldNormal,t.worldBitangent))>.01f || std::abs(Dot(t.worldTangent,t.worldBitangent))>.01f || !std::isfinite(t.ao) || t.ao<0 || t.ao>1 || !std::isfinite(t.curvature) || t.curvature<0 || t.curvature>1 || !std::isfinite(t.cavity) || t.cavity<0 || t.cavity>1 || !std::isfinite(t.thickness) || t.thickness<0 || t.thickness>bake.settings.rayDistance+bake.settings.rayBias){error="Geometry cache contains invalid map values";return false;}}
    if(!covered || covered!=bake.coveredTexels){error="Geometry cache has invalid covered-texel count";return false;}return true;
}
bool EncodeGeometryBake(const GeometryBake& bake,std::string& bytes,std::string& error) {
    if(!ValidateGeometryBake(bake,error))return false;std::string draft="IXGEOM01";WriteSettings(draft,bake.settings);U64(draft,bake.meshHash);U32(draft,bake.coveredTexels);U32(draft,bake.boundaryEdges);
    for(size_t i=0;i<bake.texels.size();++i){draft+=static_cast<char>(bake.coverage[i]);const auto& t=bake.texels[i];for(float f:t.uv)F32(draft,f);for(const auto& v:{t.objectPosition,t.worldPosition,t.objectNormal,t.worldNormal,t.worldTangent,t.worldBitangent})for(float f:v)F32(draft,f);for(float f:{t.ao,t.curvature,t.cavity,t.thickness})F32(draft,f);}
    U64(draft,Hash(draft.data(),draft.size()));bytes=std::move(draft);return true;
}
bool DecodeGeometryBake(const std::string& bytes,GeometryBake& bake,std::string& error) {
    error.clear();constexpr size_t header=8+88+16,trailer=8;
    if(bytes.size()<header+trailer || bytes.size()>header+512ULL*512*97+trailer || bytes.compare(0,8,"IXGEOM01")){error="Invalid geometry cache header/version or size";return false;}
    Reader check{bytes,bytes.size()-8};if(check.U64()!=Hash(bytes.data(),bytes.size()-8)){error="Geometry cache checksum mismatch";return false;}
    Reader r{bytes,8};GeometryBake draft;draft.settings=ReadSettings(r);if(!SettingsValid(draft.settings,error))return false;
    const size_t count=static_cast<size_t>(draft.settings.resolution)*draft.settings.resolution;if(bytes.size()!=header+count*97+trailer){error="Geometry cache payload size mismatch";return false;}
    draft.meshHash=r.U64();draft.coveredTexels=r.U32();draft.boundaryEdges=r.U32();draft.texels.resize(count);draft.coverage.resize(count);
    for(size_t i=0;i<count;++i){draft.coverage[i]=static_cast<std::uint8_t>(bytes[r.offset++]);auto& t=draft.texels[i];for(auto& f:t.uv)f=r.F32();for(auto* v:{&t.objectPosition,&t.worldPosition,&t.objectNormal,&t.worldNormal,&t.worldTangent,&t.worldBitangent})for(float& f:*v)f=r.F32();t.ao=r.F32();t.curvature=r.F32();t.cavity=r.F32();t.thickness=r.F32();}
    if(!ValidateGeometryBake(draft,error))return false;bake=std::move(draft);return true;
}
GeometryBakeJob::~GeometryBakeJob() {Cancel();if(worker_.joinable())worker_.join();}
bool GeometryBakeJob::Start(GeometryMesh mesh,GeometryBakeSettings settings) {
    if(running_)return false;if(worker_.joinable())worker_.join();ready_=false;progress_=0;error_.clear();result_={};running_=true;
    worker_=std::jthread([this,mesh=std::move(mesh),settings](std::stop_token token){
        try {success_=BakeGeometry(mesh,settings,result_,error_,{[&]{return token.stop_requested();},[&](float v){progress_=v;}});}
        catch(const std::exception& e){success_=false;error_=std::string("Geometry bake failed: ")+e.what();}
        ready_=true;running_=false;
    });return true;
}
void GeometryBakeJob::Cancel() {if(worker_.joinable())worker_.request_stop();}
bool GeometryBakeJob::TakeResult(GeometryBake& result,std::string& error) {
    if(!ready_.load() || running_.load())return false;if(worker_.joinable())worker_.join();ready_=false;error=error_;if(success_)result=std::move(result_);return true;
}
}
