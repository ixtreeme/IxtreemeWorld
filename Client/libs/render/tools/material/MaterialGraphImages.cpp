#include "MaterialGraphImages.h"
#include <algorithm>
#include <cmath>
namespace material_tool
{
namespace {
std::uint8_t Byte(float f) { return static_cast<std::uint8_t>(std::clamp(f,0.0f,1.0f)*255+0.5f); }
Float4 Color(const Surface& s, PreviewChannel channel) {
    switch(channel) {
    case PreviewChannel::Roughness: return {s.roughness,s.roughness,s.roughness,1};
    case PreviewChannel::Metallic: return {s.metallic,s.metallic,s.metallic,1};
    case PreviewChannel::AO: return {s.ao,s.ao,s.ao,1};
    case PreviewChannel::Height: return {s.height,s.height,s.height,1};
    case PreviewChannel::Normal: return {s.normal[0]*.5f+.5f,s.normal[1]*.5f+.5f,s.normal[2]*.5f+.5f,1};
    case PreviewChannel::Emissive: return s.emissive;
    default: return s.color;
    }
}
EvaluationContext Context(const GeometryTexel& t) {
    EvaluationContext c;c.uv={t.uv[0],t.uv[1],0,0};
    c.worldPosition={t.worldPosition[0],t.worldPosition[1],t.worldPosition[2],0};
    c.worldNormal={t.worldNormal[0],t.worldNormal[1],t.worldNormal[2],0};
    c.ao=t.ao;c.curvature=t.curvature;c.cavity=t.cavity;return c;
}
}
std::vector<std::uint8_t> RenderMaterialPreview(const MaterialGraphProgram& program,const ParameterOverrides& parameters,
    const TextureSampler& sample,const PreviewSettings& settings,unsigned size,const GeometryBake* geometry)
{
    size=std::clamp(size,16U,512U); std::vector<std::uint8_t> result(size*size*4);
    std::vector<std::array<GraphValue,14>> workspace;
    const auto light=Normalize3({-.4f,.65f,.65f,0});
    GeometryVec3 positionMin{INFINITY,INFINITY,INFINITY},positionMax{-INFINITY,-INFINITY,-INFINITY};
    if(geometry && (settings.channel==PreviewChannel::ObjectPosition || settings.channel==PreviewChannel::WorldPosition)) {
        for(size_t i=0;i<geometry->texels.size();++i)if(geometry->coverage[i]==1) {
            const auto& p=settings.channel==PreviewChannel::ObjectPosition?geometry->texels[i].objectPosition:geometry->texels[i].worldPosition;
            for(int k=0;k<3;++k){positionMin[k]=std::min(positionMin[k],p[k]);positionMax[k]=std::max(positionMax[k],p[k]);}
        }
    }
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
        const float u=(x+.5f)/size,v=(y+.5f)/size,px=2*u-1,py=1-2*v,r2=px*px+py*py;
        const size_t at=(static_cast<size_t>(y)*size+x)*4;
        Float4 rgb{.04f,.05f,.065f,1};
        GeometryTexel texel;
        if(geometry ? geometry->Sample(u,v,texel) : (!settings.sphere || r2<.94f)) {
            auto c=settings.surfaceData;c.uv={u,v,0,0}; c.worldPosition={(u-.5f)*settings.worldSize,settings.heightOffset,(v-.5f)*settings.worldSize,0}; c.worldNormal={0,1,0,0};
            if(geometry)c=Context(texel);
            else if(settings.sphere) {const float z=std::sqrt(1-r2); c.worldNormal={px,py,z,0};c.worldPosition={px*settings.worldSize*.5f,py*settings.worldSize*.5f+settings.heightOffset,z*settings.worldSize*.5f,0}; c.uv={.5f+std::atan2(px,z)/6.2831853f,.5f-std::asin(py)/3.14159265f,0,0};}
            auto s=program.Evaluate(c,parameters,sample,workspace);rgb=Color(s,settings.channel);
            bool srgb=settings.channel==PreviewChannel::Shaded || settings.channel==PreviewChannel::BaseColor || settings.channel==PreviewChannel::Emissive;
            if(geometry && settings.channel>=PreviewChannel::GeometryAO) {
                float scalar=0;
                switch(settings.channel) {
                case PreviewChannel::GeometryAO:scalar=texel.ao;break;
                case PreviewChannel::Curvature:scalar=texel.curvature;break;
                case PreviewChannel::Cavity:scalar=texel.cavity;break;
                case PreviewChannel::Thickness:scalar=texel.thickness/geometry->settings.rayDistance;break;
                default:break;
                }
                rgb={scalar,scalar,scalar,1};
                if(settings.channel==PreviewChannel::ObjectNormal || settings.channel==PreviewChannel::WorldNormal) {
                    const auto& n=settings.channel==PreviewChannel::ObjectNormal?texel.objectNormal:texel.worldNormal;
                    for(int k=0;k<3;++k)rgb[k]=n[k]*.5f+.5f;
                }
                if(settings.channel==PreviewChannel::ObjectPosition || settings.channel==PreviewChannel::WorldPosition) {
                    const auto& p=settings.channel==PreviewChannel::ObjectPosition?texel.objectPosition:texel.worldPosition;
                    for(int k=0;k<3;++k)rgb[k]=positionMax[k]>positionMin[k]?(p[k]-positionMin[k])/(positionMax[k]-positionMin[k]):.5f;
                }
            }
            if(settings.channel==PreviewChannel::SelectedOutput) {
                const auto selected=program.PreviewValue(workspace); rgb=selected.number;
                if(settings.selectedType==PinType::Surface)rgb=selected.surface.color;
                if(settings.selectedType==PinType::Scalar)rgb={rgb[0],rgb[0],rgb[0],1};
                if(settings.selectedType==PinType::Vector3)for(int k=0;k<3;++k)rgb[k]=rgb[k]*.5f+.5f;
                srgb=settings.selectedType==PinType::Color || settings.selectedType==PinType::Surface;
            }
            if(settings.channel==PreviewChannel::Shaded) {
                auto n=c.worldNormal;
                // Transform the tangent-space material normal into the preview's surface frame.
                const auto tangent=geometry?Float4{texel.worldTangent[0],texel.worldTangent[1],texel.worldTangent[2],0}:Normalize3({n[2],0,-n[0],0},{1,0,0,0});
                const Float4 bitangent=geometry?Float4{texel.worldBitangent[0],texel.worldBitangent[1],texel.worldBitangent[2],0}:Float4{n[1]*tangent[2]-n[2]*tangent[1],n[2]*tangent[0]-n[0]*tangent[2],n[0]*tangent[1]-n[1]*tangent[0],0};
                for(int k=0;k<3;++k)n[k]=tangent[k]*s.normal[0]+bitangent[k]*s.normal[1]+c.worldNormal[k]*s.normal[2]; n=Normalize3(n);
                const float ndotl=std::max(0.0f,n[0]*light[0]+n[1]*light[1]+n[2]*light[2]),ndotv=std::max(.001f,n[2]);
                const auto half=Normalize3({light[0],light[1],light[2]+1,0}); const float ndoth=std::max(0.0f,n[0]*half[0]+n[1]*half[1]+n[2]*half[2]),vdoth=std::max(0.0f,half[2]);
                const float alpha=std::max(.045f,s.roughness*s.roughness),a2=alpha*alpha,denom=ndoth*ndoth*(a2-1)+1;
                const float distribution=a2/(3.14159265f*denom*denom),k=(s.roughness+1)*(s.roughness+1)/8;
                const float geometryTerm=(ndotl/(ndotl*(1-k)+k))*(ndotv/(ndotv*(1-k)+k));
                for(int ch=0;ch<3;++ch) {const float f0=.04f+(s.color[ch]-.04f)*s.metallic,fresnel=f0+(1-f0)*std::pow(1-vdoth,5.0f); const float spec=distribution*geometryTerm*fresnel/std::max(4*ndotl*ndotv,.001f);
                    rgb[ch]=s.color[ch]*.15f*s.ao + ((1-fresnel)*(1-s.metallic)*s.color[ch]/3.14159265f+spec)*ndotl*2.5f+s.emissive[ch]; rgb[ch]=rgb[ch]/(1+rgb[ch]);}
            }
            if(srgb)for(int k=0;k<3;++k)rgb[k]=LinearToSrgb(rgb[k]);
        }
        for(int k=0;k<3;++k)result[at+k]=Byte(rgb[k]);result[at+3]=255;
    }return result;
}
namespace {
bool BakeImages(const MaterialGraphProgram& program,const ParameterOverrides& parameters,const TextureSampler& sample,unsigned size,BakedMaterialImages& images,std::string& error,const GeometryBake* geometry)
{
    error.clear();if(!program.HasOutput()){error="Add a PBR Output before baking";return false;}if(!geometry && !program.CanBakeUV()){error="World/geometry-dependent graphs need per-object or terrain baking; use a UV graph for material export.";return false;}
    if(geometry && !ValidateGeometryBake(*geometry,error))return false;
    if(!program.ValidateOverrides(parameters,error))return false;
    if(size<16 || size>2048){error="Bake size must be between 16 and 2048";return false;}
    BakedMaterialImages draft;draft.size=size;
    for(auto* image:{&draft.color,&draft.normal,&draft.ao,&draft.roughness,&draft.metallic,&draft.height,&draft.emissive})image->resize(static_cast<size_t>(size)*size*4);
    std::vector<std::array<GraphValue,14>> workspace;
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
        EvaluationContext c;c.uv={(x+.5f)/size,(y+.5f)/size,0,0};
        if(geometry){GeometryTexel t;if(!geometry->Sample(c.uv[0],c.uv[1],t))continue;c=Context(t);}
        const auto s=program.Evaluate(c,parameters,sample,workspace);const size_t at=(static_cast<size_t>(y)*size+x)*4;
        for(int k=0;k<3;++k){draft.color[at+k]=Byte(LinearToSrgb(s.color[k]));draft.normal[at+k]=Byte(s.normal[k]*.5f+.5f);draft.emissive[at+k]=Byte(LinearToSrgb(s.emissive[k]));}
        draft.color[at+3]=Byte(s.opacity);draft.normal[at+3]=draft.emissive[at+3]=255;
        for(const auto& [image,value]:{std::pair{&draft.ao,s.ao},std::pair{&draft.roughness,s.roughness},std::pair{&draft.metallic,s.metallic},std::pair{&draft.height,s.height}}) {
            (*image)[at]=(*image)[at+1]=(*image)[at+2]=Byte(value);(*image)[at+3]=255;
        }
    }images=std::move(draft);return true;
}
}
bool BakeMaterialUV(const MaterialGraphProgram& program,const ParameterOverrides& parameters,const TextureSampler& sample,unsigned size,BakedMaterialImages& images,std::string& error) {
    return BakeImages(program,parameters,sample,size,images,error,nullptr);
}
bool BakeMaterialOnGeometry(const MaterialGraphProgram& program,const ParameterOverrides& parameters,const TextureSampler& sample,const GeometryBake& geometry,BakedMaterialImages& images,std::string& error) {
    return BakeImages(program,parameters,sample,geometry.settings.resolution,images,error,&geometry);
}
}
