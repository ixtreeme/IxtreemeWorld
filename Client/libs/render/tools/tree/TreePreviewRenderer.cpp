#include "TreePreviewRenderer.h"
#include "asset/ExrImage.h"
#include <imgui.h>
#include <stb_image.h>
#include <fstream>
#include <iterator>
#include <limits>
namespace tree_tool
{
namespace
{
PreviewImage LoadImage(const std::filesystem::path& path)
{
    PreviewImage image;
    if (path.empty()) return image;
    if (client::asset::IsExrPath(path))
    {
        std::string error;
        if (auto exr=client::asset::LoadExr(path,error))
        { image.width=exr->width; image.height=exr->height; image.pixels=client::asset::ExrRgba8(*exr,client::asset::ExrByteMode::Preview); }
    }
    else
    {
        // filesystem::path keeps Windows Unicode names intact when opening the file.
        std::ifstream file(path,std::ios::binary);
        const std::vector<unsigned char> bytes(std::istreambuf_iterator<char>{file},std::istreambuf_iterator<char>{});
        int channels=0;
        if (!bytes.empty() && bytes.size()<=std::size_t(std::numeric_limits<int>::max()))
        {
            if (auto* p=stbi_load_from_memory(bytes.data(),static_cast<int>(bytes.size()),&image.width,&image.height,&channels,4))
            { image.pixels.assign(p,p+std::size_t(image.width)*image.height*4); stbi_image_free(p); }
        }
    }
    return image;
}
}
TreePreviewRenderer::~TreePreviewRenderer() { ReleaseTexture(); }
void TreePreviewRenderer::ReleaseTexture()
{
    if (provider_ && texture_.IsValid()) provider_->ReleasePreviewTexture(texture_);
    texture_={};
}
void TreePreviewRenderer::SetTextureProvider(ixeditor::graphics::IEditorTextureProvider* provider)
{
    if (provider_==provider) return;
    ReleaseTexture(); provider_=provider; dirty_=true;
}
void TreePreviewRenderer::ResetView() { view_.yaw=-0.65f; view_.pitch=0.12f; view_.zoom=1; dirty_=true; }
void TreePreviewRenderer::ResetView(const ixtreemetree::TreeMesh& mesh) { ResetView(); FitToMesh(mesh); }
void TreePreviewRenderer::FitToMesh(const ixtreemetree::TreeMesh& mesh)
{
    const auto a=mesh.bboxMin,b=mesh.bboxMax;
    view_.center={(a.x+b.x)*0.5f,(a.y+b.y)*0.5f,(a.z+b.z)*0.5f};
    view_.radius=std::max({0.6f,(b.y-a.y)*0.55f,std::hypot(b.x-a.x,b.z-a.z)*0.5f});
    hasFit_=true; dirty_=true;
}
void TreePreviewRenderer::Render(const ixtreemetree::TreeMesh& mesh,float width,float height,const TreePreviewStyle& style)
{
    const ImVec2 size(std::max(128.0f,width),std::max(160.0f,height));
    if (!hasFit_) FitToMesh(mesh);
    ImGui::InvisibleButton("##TreePreviewCanvas",size,ImGuiButtonFlags_MouseButtonLeft);
    const auto origin=ImGui::GetItemRectMin();
    if (ImGui::IsItemHovered())
    {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ResetView(mesh);
        if (ImGui::GetIO().MouseWheel!=0)
        { view_.zoom=std::clamp(view_.zoom+ImGui::GetIO().MouseWheel*0.1f,0.4f,4.0f); dirty_=true; }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        view_.yaw+=ImGui::GetIO().MouseDelta.x*0.01f;
        view_.pitch=std::clamp(view_.pitch+ImGui::GetIO().MouseDelta.y*0.008f,-1.2f,1.2f); dirty_=true;
    }
    const float scale=std::min(1.5f,1200.0f/std::max(size.x,size.y));
    const int w=std::max(1,int(size.x*scale)),h=std::max(1,int(size.y*scale));
    if (w!=view_.width || h!=view_.height) { view_.width=w; view_.height=h; dirty_=true; }
    if (!(style==style_))
    {
        if (style.barkTexture!=style_.barkTexture || style.leafTexture!=style_.leafTexture)
            texturesDirty_=true;
        style_=style; dirty_=true;
        view_.lighting=style.lighting; view_.shadows=style.shadows;
    }
    if (dirty_ && provider_ && provider_->IsReady())
    {
        if (texturesDirty_)
        {
            bark_=LoadImage(style.barkTexture); leaves_=LoadImage(style.leafTexture); texturesDirty_=false;
        }
        textureWarning_.clear();
        if (style.barkTextured && !style.barkTexture.empty() && bark_.pixels.empty())
            textureWarning_="Bark texture unavailable";
        if (style.leafTextured && !style.leafTexture.empty() && leaves_.pixels.empty())
        {
            if (!textureWarning_.empty()) textureWarning_+='\n';
            textureWarning_+="Leaf texture unavailable";
        }
        const auto image=RasterTreePreview(mesh,view_,
            {style.barkTextured?&bark_:nullptr,style.barkColor,0,false},
            {style.leafTextured?&leaves_:nullptr,style.leafColor,style.leafAlphaCutoff,true});
        ReleaseTexture();
        texture_=provider_->UploadPreviewTexture(image.pixels.data(),image.width,image.height,"Tree Generator");
        dirty_=!texture_.IsValid();
    }
    auto* draw=ImGui::GetWindowDrawList();
    if (provider_ && texture_.IsValid())
    {
        if (auto* id=provider_->GetPreviewTexture(texture_))
            draw->AddImage(reinterpret_cast<ImTextureID>(id),origin,{origin.x+size.x,origin.y+size.y});
        else dirty_=true;
    }
    else draw->AddText({origin.x+12,origin.y+12},IM_COL32_WHITE,"Preview renderer unavailable");
    if (!textureWarning_.empty())
    {
        const ImVec2 warningOrigin{origin.x+8,origin.y+size.y-48};
        draw->AddRectFilled(warningOrigin,{origin.x+size.x-8,origin.y+size.y-8},IM_COL32(35,25,15,235));
        draw->AddText({warningOrigin.x+4,warningOrigin.y+3},IM_COL32(255,195,80,255),textureWarning_.c_str());
    }
}
}
