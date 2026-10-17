#pragma once

#include <ixtreemetree/tree_mesh.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace tree_tool
{
// Backend-independent preview: identical cutouts in the editor and in headless QA.
struct PreviewImage
{
    int width=0, height=0;
    std::vector<unsigned char> pixels;
    std::vector<float> depth;
    std::array<float,4> sample(float u, float v) const
    {
        if (pixels.empty()) return {1,1,1,1};
        u-=std::floor(u); v-=std::floor(v);
        const float x=u*width-0.5f, y=v*height-0.5f;
        const int x0=static_cast<int>(std::floor(x)), y0=static_cast<int>(std::floor(y));
        const float tx=x-x0, ty=y-y0;
        std::array<float,4> result{};
        for (int j=0;j<2;++j) for (int i=0;i<2;++i)
        {
            const int ix=(x0+i+width)%width, iy=(y0+j+height)%height;
            const float weight=(i?tx:1-tx)*(j?ty:1-ty)/255;
            for (int c=0;c<4;++c) result[c]+=pixels[(iy*width+ix)*4+c]*weight;
        }
        return result;
    }
};
struct PreviewLighting
{
    float azimuth=-138.0f, elevation=58.0f; // world-space sun angles, degrees
    float ambient=0.24f, intensity=0.95f, exposure=0.0f, transmission=0.45f;
    bool operator==(const PreviewLighting&) const = default;
};
struct PreviewView
{
    int width=768, height=768;
    ixtreemetree::Vec3 center{};
    float radius=6, yaw=-0.65f, pitch=0.12f, zoom=1;
    bool shadows=true;
    PreviewLighting lighting{};
};
struct PreviewSurface
{
    const PreviewImage* image=nullptr;
    std::array<float,4> color{1,1,1,1};
    float cutoff=0;
    bool leaves=false;
};
inline PreviewImage RasterTreePreview(const ixtreemetree::TreeMesh& mesh, const PreviewView& view,
    PreviewSurface bark, PreviewSurface leaves)
{
    PreviewImage out{view.width,view.height};
    out.pixels.resize(std::size_t(view.width)*view.height*4);
    std::vector<float> depth(std::size_t(view.width)*view.height,std::numeric_limits<float>::max());
    for (int y=0;y<view.height;++y) for (int x=0;x<view.width;++x)
    {
        const float gradient=float(y)/view.height;
        auto* p=&out.pixels[(y*view.width+x)*4];
        p[0]=static_cast<unsigned char>(214-54*gradient);
        p[1]=static_cast<unsigned char>(219-53*gradient);
        p[2]=static_cast<unsigned char>(225-52*gradient); p[3]=255;
    }
    struct Point { float x,y,z; };
    auto project=[&](ixtreemetree::Vec3 p) {
        p.x-=view.center.x; p.y-=view.center.y; p.z-=view.center.z;
        const float x=p.x*std::cos(view.yaw)-p.z*std::sin(view.yaw);
        const float z=p.x*std::sin(view.yaw)+p.z*std::cos(view.yaw);
        const float y=p.y*std::cos(view.pitch)-z*std::sin(view.pitch);
        const float s=std::min(view.width,view.height)*0.46f*view.zoom/std::max(0.1f,view.radius);
        return Point{view.width*0.5f+x*s,view.height*0.52f-y*s,p.y*std::sin(view.pitch)+z*std::cos(view.pitch)};
    };
    PreviewImage shadow;
    PreviewView lightView=view;
    constexpr float radians=3.14159265359f/180.0f;
    const float azimuth=view.lighting.azimuth*radians;
    const float elevation=std::clamp(view.lighting.elevation,5.0f,85.0f)*radians;
    const float lx=std::cos(elevation)*std::sin(azimuth),ly=std::sin(elevation),lz=std::cos(elevation)*std::cos(azimuth);
    lightView.yaw=azimuth+3.14159265359f; lightView.pitch=-elevation;
    if (view.shadows)
    {
        const auto a=mesh.bboxMin,b=mesh.bboxMax;
        lightView.width=lightView.height=768; lightView.zoom=1;
        lightView.radius=std::max(view.radius,std::sqrt((b.x-a.x)*(b.x-a.x)+(b.y-a.y)*(b.y-a.y)+(b.z-a.z)*(b.z-a.z))*0.55f);
        lightView.shadows=false;
        // The same alpha test is used in the light view. Empty texels cast no shadow.
        shadow=RasterTreePreview(mesh,lightView,bark,leaves);
    }
    const float lightScale=768*0.46f/std::max(0.1f,lightView.radius);
    const float lightCosYaw=std::cos(lightView.yaw),lightSinYaw=std::sin(lightView.yaw);
    const float lightCosPitch=std::cos(lightView.pitch),lightSinPitch=std::sin(lightView.pitch);
    auto visibility=[&](ixtreemetree::Vec3 p) {
        if (shadow.depth.empty()) return 1.0f;
        p.x-=view.center.x; p.y-=view.center.y; p.z-=view.center.z;
        const float x=p.x*lightCosYaw-p.z*lightSinYaw;
        const float z=p.x*lightSinYaw+p.z*lightCosYaw;
        const float y=p.y*lightCosPitch-z*lightSinPitch;
        const float d=p.y*lightSinPitch+z*lightCosPitch;
        const int sx=int(384+x*lightScale), sy=int(768*0.52f-y*lightScale);
        float lit=0;
        for (int j=-1;j<=1;++j) for (int i=-1;i<=1;++i)
        {
            const int ix=sx+i,iy=sy+j;
            lit+=(ix<0 || ix>=768 || iy<0 || iy>=768 || d-0.035f<=shadow.depth[iy*768+ix]) ? 1.0f : 0.0f;
        }
        return lit/9;
    };
    auto surface=[&](const auto& source, PreviewSurface material) {
        std::vector<Point> points;
        points.reserve(source.vertices.size());
        for (const auto& v:source.vertices) points.push_back(project(v.position));
        for (std::size_t k=0;k+2<source.indices.size();k+=3)
        {
            const auto ia=source.indices[k], ib=source.indices[k+1], ic=source.indices[k+2];
            const auto a=points[ia], b=points[ib], c=points[ic];
            const float area=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
            if (std::abs(area)<1e-6f) continue;
            const int x0=std::max(0,int(std::floor(std::min({a.x,b.x,c.x}))));
            const int x1=std::min(view.width-1,int(std::ceil(std::max({a.x,b.x,c.x}))));
            const int y0=std::max(0,int(std::floor(std::min({a.y,b.y,c.y}))));
            const int y1=std::min(view.height-1,int(std::ceil(std::max({a.y,b.y,c.y}))));
            const auto& va=source.vertices[ia]; const auto& vb=source.vertices[ib]; const auto& vc=source.vertices[ic];
            for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x)
            {
                const float px=x+0.5f, py=y+0.5f;
                const float w1=((px-a.x)*(c.y-a.y)-(py-a.y)*(c.x-a.x))/area;
                const float w2=((b.x-a.x)*(py-a.y)-(b.y-a.y)*(px-a.x))/area;
                const float w0=1-w1-w2;
                if (w0<0 || w1<0 || w2<0) continue;
                const float z=w0*a.z+w1*b.z+w2*c.z;
                const int index=y*view.width+x;
                if (z>=depth[index]) continue;
                const float u=w0*va.uv.x+w1*vb.uv.x+w2*vc.uv.x;
                const float v=w0*va.uv.y+w1*vb.uv.y+w2*vc.uv.y;
                const auto tex=material.image ? material.image->sample(u,v) : std::array<float,4>{1,1,1,1};
                const float alpha=tex[3]*material.color[3];
                if (alpha<=0 || alpha<material.cutoff) continue; // rejected texels never write depth
                float nx=w0*va.normal.x+w1*vb.normal.x+w2*vc.normal.x;
                float ny=w0*va.normal.y+w1*vb.normal.y+w2*vc.normal.y;
                float nz=w0*va.normal.z+w1*vb.normal.z+w2*vc.normal.z;
                const float len=std::max(0.001f,std::sqrt(nx*nx+ny*ny+nz*nz));
                const float dot=(lx*nx+ly*ny+lz*nz)/len;
                const ixtreemetree::Vec3 world{
                    w0*va.position.x+w1*vb.position.x+w2*vc.position.x,
                    w0*va.position.y+w1*vb.position.y+w2*vc.position.y,
                    w0*va.position.z+w1*vb.position.z+w2*vc.position.z};
                const float direct=visibility(world);
                const float diffuse=std::max(0.0f,dot)+(material.leaves ?
                    std::clamp(view.lighting.transmission,0.0f,1.0f)*std::max(0.0f,-dot) : 0.0f);
                const float light=(std::clamp(view.lighting.ambient,0.0f,1.0f)+
                    std::clamp(view.lighting.intensity,0.0f,3.0f)*diffuse*direct)*
                    std::exp2(std::clamp(view.lighting.exposure,-3.0f,3.0f));
                // Texture bytes are sRGB. Apply illumination in linear light,
                // then encode for the editor instead of darkening sRGB bytes.
                const float displayLight=std::pow(light,1.0f/2.2f);
                auto* pixel=&out.pixels[index*4];
                for (int channel=0;channel<3;++channel)
                {
                    const float value=std::clamp(tex[channel]*material.color[channel]*displayLight,0.0f,1.0f)*255;
                    pixel[channel]=static_cast<unsigned char>(value); // alpha mask, matching export
                }
                depth[index]=z;
            }
        }
    };
    surface(mesh.bark,bark); surface(mesh.leaves,leaves);
    out.depth=std::move(depth);
    return out;
}
}
