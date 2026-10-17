#include "tools/tree/TreePreviewRaster.h"
#include "tools/tree/TreePreviewRenderer.h"
#include "tools/tree/TreeTexturePalette.h"
#include "import_export/tree/TreeGlbExporter.h"
#include <ixtreemetree/tree.h>
#include <ixtreemetree/preset.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>

namespace
{
void Check(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
class PreviewProvider final : public ixeditor::graphics::IEditorTextureProvider
{
public:
    using Handle=ixeditor::graphics::EditorTextureHandle;
    int uploads=0,releases=0;
    Handle active{};
    std::vector<unsigned char> uploadedPixels;
    bool IsReady() const override { return true; }
    void BeginBackendFrame() override {}
    bool IsBackendFrameActive() const override { return true; }
    void SetSceneViewTexture(std::shared_ptr<ixrhi::IXRHITexture>,std::shared_ptr<ixrhi::IXRHISampler>,std::uint32_t,std::uint32_t) override {}
    void SetGameViewTexture(std::shared_ptr<ixrhi::IXRHITexture>,std::shared_ptr<ixrhi::IXRHISampler>,std::uint32_t,std::uint32_t) override {}
    void* GetSceneViewTexture() override { return nullptr; }
    void GetSceneViewSize(std::uint32_t&,std::uint32_t&) override {}
    void* GetGameViewTexture() override { return nullptr; }
    void GetGameViewSize(std::uint32_t&,std::uint32_t&) override {}
    Handle UploadPreviewTexture(const void* pixels,std::uint32_t width,std::uint32_t height,const char*) override
    {
        Check(pixels && width>0 && height>0,"editor uploads rendered pixels");
        const auto* bytes=static_cast<const unsigned char*>(pixels);
        uploadedPixels.assign(bytes,bytes+std::size_t(width)*height*4);
        active={std::uint64_t(++uploads)}; return active;
    }
    void ReleasePreviewTexture(Handle handle) override
    { Check(handle==active,"editor releases its own live texture"); ++releases; active={}; }
    void ReleaseAllPreviewTextures() override { active={}; }
    void* GetPreviewTexture(Handle handle) override
    { return handle==active ? reinterpret_cast<void*>(std::uintptr_t(1)) : nullptr; }
};
void TestEditorPreview()
{
    auto* context=ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize={640,480}; io.DeltaTime=1.0f/60;
    unsigned char* pixels=nullptr; int width=0,height=0;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    PreviewProvider provider;
    {
        tree_tool::TreePreviewRenderer renderer;
        renderer.SetTextureProvider(&provider);
        ixtreemetree::TreeMesh mesh;
        mesh.bark.vertices={{{-1,0,0}},{{1,0,0}},{{0,2,0}}};
        mesh.bark.indices={0,1,2}; mesh.bboxMin={-1,0,0}; mesh.bboxMax={1,2,0};
        auto frame=[&](const tree_tool::TreePreviewStyle& style=tree_tool::TreePreviewStyle{}) {
            ImGui::NewFrame(); ImGui::SetNextWindowSize({400,400},ImGuiCond_Always);
            ImGui::Begin("Preview test"); renderer.Render(mesh,256,256,style); ImGui::End(); ImGui::Render();
        };
        auto goldenPixelCount=[&] {
            int count=0;
            for (std::size_t i=0;i<provider.uploadedPixels.size();i+=4)
                count+=provider.uploadedPixels[i]>provider.uploadedPixels[i+2]+60 &&
                    provider.uploadedPixels[i+1]>provider.uploadedPixels[i+2]+40;
            return count;
        };
        frame(); frame();
        Check(provider.uploads==1,"unchanged editor previews reuse their texture");
        renderer.InvalidateMesh(); frame();
        Check(provider.uploads==2 && provider.releases==1,"regeneration replaces and releases the preview texture");
        struct TextureWorkspace
        {
            std::filesystem::path previous=std::filesystem::current_path();
            std::filesystem::path root=std::filesystem::temp_directory_path()/
                ("ix_tree_preview_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            TextureWorkspace() { std::filesystem::create_directories(root/u8"árvíztűrő projekt"); }
            ~TextureWorkspace()
            {
                std::filesystem::current_path(previous);
                std::error_code ec; std::filesystem::remove_all(root,ec);
            }
        } workspace;
        const auto source=workspace.root/"source.png";
        const unsigned char gold[]={235,180,35,255,235,180,35,255,235,180,35,255,235,180,35,255};
        Check(stbi_write_png(source.string().c_str(),2,2,4,gold,8)!=0,"test texture writes");
        const auto unicodeTexture=workspace.root/u8"árvíztűrő projekt"/u8"日本語"/"bark.png";
        std::filesystem::create_directories(unicodeTexture.parent_path());
        std::filesystem::copy_file(source,unicodeTexture);
        tree_tool::TreePreviewStyle textured;
        textured.barkTexture=unicodeTexture;
        frame(textured);
        Check(goldenPixelCount()>100,"preview textures load from Hungarian project paths");
        std::filesystem::current_path(workspace.root);
        auto& palette=tree_tool::TreeTexturePalette::Instance();
        palette.EnsureLoaded("internal");
        Check(palette.Leaf(ixtreemetree::LeafType::Larch).path.is_absolute(),
            "built-in texture paths remain absolute across project working-directory changes");
        textured.barkTexture=palette.Leaf(ixtreemetree::LeafType::Larch).path;
        std::filesystem::copy_file(source,textured.barkTexture,std::filesystem::copy_options::overwrite_existing);
        std::filesystem::current_path(workspace.root/u8"árvíztűrő projekt");
        frame(textured);
        Check(goldenPixelCount()>100 && renderer.TextureWarning().empty(),
            "built-in preview textures still render after switching to a new project directory");
        textured.barkTexture=workspace.root/"missing.png";
        textured.leafTexture=workspace.root/"missing-leaf.png";
        frame(textured);
        Check(renderer.TextureWarning().find("Bark")!=std::string::npos &&
            renderer.TextureWarning().find("Leaf")!=std::string::npos,
            "missing textures report the affected surfaces instead of silently turning gray");
        std::filesystem::copy_file(source,textured.barkTexture);
        std::filesystem::copy_file(source,textured.leafTexture);
        renderer.InvalidateMesh(); frame(textured);
        Check(renderer.TextureWarning().empty() && goldenPixelCount()>100,
            "regeneration reloads repaired textures and clears the warning");
        const auto previousPixels=provider.uploadedPixels;
        const auto previousUploads=provider.uploads;
        textured.lighting.exposure=1;
        frame(textured);
        Check(provider.uploads==previousUploads+1 && provider.uploadedPixels!=previousPixels,
            "changing preview lighting refreshes a cached preview without regenerating geometry");
        renderer.SetTextureProvider(nullptr);
        Check(provider.releases==provider.uploads,"editor teardown detaches the texture provider safely");
    }
    ImGui::DestroyContext(context);
}
tree_tool::PreviewImage Load(const std::filesystem::path& path)
{
    tree_tool::PreviewImage image;
    int channels=0;
    auto* p=stbi_load(path.string().c_str(),&image.width,&image.height,&channels,4);
    Check(p!=nullptr,"texture must decode");
    image.pixels.assign(p,p+image.width*image.height*4); stbi_image_free(p);
    return image;
}
void TestCutoutDepth()
{
    using namespace tree_tool;
    using namespace ixtreemetree;
    TreeMesh mesh;
    auto quad=[](auto& surface,float z) {
        surface.vertices={{{-0.8f,-0.8f,z},{0,0,-1},{0,1}},{{0.8f,-0.8f,z},{0,0,-1},{1,1}},
            {{0.8f,0.8f,z},{0,0,-1},{1,0}},{{-0.8f,0.8f,z},{0,0,-1},{0,0}}};
        surface.indices={0,1,2,0,2,3};
    };
    quad(mesh.bark,0); quad(mesh.leaves,-0.2f);
    PreviewImage texture{2,1,{255,0,0,255,255,0,0,0}};
    PreviewView view; view.width=view.height=128; view.center={0,0,0}; view.radius=1; view.yaw=view.pitch=0;
    view.shadows=false;
    const auto image=RasterTreePreview(mesh,view,{nullptr,{0,0,1,1},0,false},{&texture,{1,1,1,1},0.5f,true});
    const auto* opaque=&image.pixels[(64*128+40)*4];
    const auto* hole=&image.pixels[(64*128+85)*4];
    Check(opaque[0]>100 && opaque[2]==0,"visible leaf texels cover geometry behind them");
    Check(hole[2]>100 && hole[0]==0,"transparent leaf texels preserve the bark behind them");
    const auto cut=RasterTreePreview(mesh,view,{nullptr,{0,0,1,1},0,false},{&texture,{1,1,1,1},1.01f,true});
    Check(cut.pixels[(64*128+40)*4+2]>100,"alpha cutoff is applied per texel");
    view.shadows=true;
    const auto shadowed=RasterTreePreview(mesh,view,{nullptr,{0,0,1,1},0,false},{&texture,{1,1,1,1},0.5f,true});
    int shadowPixels=0;
    for (std::size_t i=0;i<image.pixels.size();i+=4)
        shadowPixels += image.pixels[i]==0 && image.pixels[i+2]>100 && shadowed.pixels[i+2]+15<image.pixels[i+2];
    Check(shadowPixels>5,"alpha-tested leaves cast shadows onto visible surfaces behind them");
}
void TestPreviewLighting()
{
    using namespace tree_tool;
    ixtreemetree::TreeMesh mesh;
    mesh.bark.vertices={{{-0.8f,-0.8f,0},{0,0,-1},{0,1}},{{0.8f,-0.8f,0},{0,0,-1},{1,1}},
        {{0.8f,0.8f,0},{0,0,-1},{1,0}},{{-0.8f,0.8f,0},{0,0,-1},{0,0}}};
    mesh.bark.indices={0,1,2,0,2,3}; mesh.bboxMin={-0.8f,-0.8f,0}; mesh.bboxMax={0.8f,0.8f,0};
    PreviewView view; view.width=view.height=128; view.center={0,0,0}; view.radius=1;
    view.yaw=view.pitch=0; view.shadows=false;
    view.lighting.ambient=0; view.lighting.intensity=1; view.lighting.azimuth=0; view.lighting.elevation=45;
    const auto away=RasterTreePreview(mesh,view,{},{});
    view.lighting.azimuth=180;
    const auto toward=RasterTreePreview(mesh,view,{},{});
    const auto center=(64*128+64)*4;
    Check(away.pixels[center]==0 && toward.pixels[center]>200 && away.depth==toward.depth,
        "sun direction changes surface lighting while preserving silhouette and depth");
    view.lighting.azimuth=0; view.lighting.ambient=0.2f;
    const auto filled=RasterTreePreview(mesh,view,{},{});
    Check(filled.pixels[center]>100,"ambient fill lights surfaces facing away from the sun");
    mesh.leaves.vertices=mesh.bark.vertices; mesh.leaves.indices=mesh.bark.indices;
    mesh.bark.vertices.clear(); mesh.bark.indices.clear();
    view.lighting.ambient=0; view.lighting.transmission=0;
    const auto opaque=RasterTreePreview(mesh,view,{}, {nullptr,{1,1,1,1},0,true});
    view.lighting.transmission=1; view.lighting.intensity=0.25f;
    const auto transmitted=RasterTreePreview(mesh,view,{}, {nullptr,{1,1,1,1},0,true});
    view.lighting.exposure=1;
    const auto exposed=RasterTreePreview(mesh,view,{}, {nullptr,{1,1,1,1},0,true});
    Check(opaque.pixels[center]==0 && transmitted.pixels[center]>100 &&
        exposed.pixels[center]>transmitted.pixels[center]+30,
        "leaf transmission and exposure control backlit foliage independently");
}

void WritePresetGallery(const std::filesystem::path& destination)
{
    namespace fs=std::filesystem;
    using namespace tree_tool;
    auto& palette=TreeTexturePalette::Instance(); palette.EnsureLoaded(IXTREEME_TREE_INTERNAL_DIR);
    fs::create_directories(destination);
    constexpr int tileWidth=600,tileHeight=720;
    PreviewImage sheet{tileWidth*3,tileHeight*3};
    sheet.pixels.resize(std::size_t(sheet.width)*sheet.height*4);
    const char* names[]={"palm_tall","palm_curved","fir_tree","snowy_fir","blossom_tree",
        "blossom_shrub","round_shrub","weeping_willow","dead_tree"};
    std::ofstream index(destination/"index.html");
    index << "<!doctype html><meta charset='utf-8'><title>Tree Generator preset gallery</title>"
        "<style>body{background:#20252b;color:#eee;font:16px sans-serif;margin:24px}"
        "main{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:20px}"
        "img{width:100%;border-radius:8px}figure{margin:0}figcaption{padding:8px 0}</style>"
        "<h1>Tree Generator presets</h1><main>";
    for (int i=0;i<9;++i)
    {
        const auto preset=ixtreemetree::loadPresetFile(fs::path(IXTREEME_TREE_INTERNAL_DIR)/"tree_presets"/(std::string(names[i])+".json"));
        Check(bool(preset),"gallery preset loads");
        ixtreemetree::Tree tree; tree.options=preset->options;
        const auto mesh=tree.generate();
        const auto bark=Load(palette.Bark(tree.options.bark.type).path);
        const auto leaves=Load(palette.Leaf(tree.options.leaves.type).path);
        PreviewView view; view.width=tileWidth; view.height=tileHeight;
        const auto a=mesh.bboxMin,b=mesh.bboxMax;
        view.center={(a.x+b.x)*0.5f,(a.y+b.y)*0.5f,(a.z+b.z)*0.5f};
        view.radius=std::max({0.6f,(b.y-a.y)*0.55f,std::hypot(b.x-a.x,b.z-a.z)*0.5f});
        auto tint=[](std::uint32_t color) {
            return std::array<float,4>{float(color>>24&255)/255,float(color>>16&255)/255,float(color>>8&255)/255,float(color&255)/255};
        };
        const auto image=RasterTreePreview(mesh,view,{&bark,tint(tree.options.bark.tint),0,false},
            {&leaves,tint(tree.options.leaves.tint),tree.options.leaves.alphaTest,true});
        const auto name=std::string(names[i])+".png";
        Check(stbi_write_png((destination/name).string().c_str(),tileWidth,tileHeight,4,image.pixels.data(),tileWidth*4)!=0,"gallery image writes");
        for (int y=0;y<tileHeight;++y)
        {
            const auto offset=(std::size_t(i/3*tileHeight+y)*sheet.width+i%3*tileWidth)*4;
            std::copy_n(image.pixels.data()+y*tileWidth*4,tileWidth*4,sheet.pixels.data()+offset);
        }
        index << "<figure><a href='" << name << "'><img src='" << name << "'></a><figcaption>"
              << preset->name << "</figcaption></figure>";
        TreeMaterialBinding binding;
        binding.barkBaseColorTexturePath=palette.Bark(tree.options.bark.type).path;
        binding.leafBaseColorTexturePath=palette.Leaf(tree.options.leaves.type).path;
        binding.barkTint=tint(tree.options.bark.tint); binding.leafTint=tint(tree.options.leaves.tint);
        binding.leafAlphaCutoff=tree.options.leaves.alphaTest;
        binding.impostor.enabled=true; binding.impostor.resolution=128;
        const auto result=TreeGlbExporter::SaveAsAsset(mesh,destination/"project",names[i],binding);
        Check(result.ok,result.error.c_str());
        Check(result.impostorBaked,"gallery preset bakes an impostor");
        Check(ixtreemetree::savePresetFile(result.modelPath.parent_path()/(std::string(names[i])+".tree.json"),*preset),
            "gallery includes editable growth recipes");
    }
    index << "</main>";
    Check(index.good(),"gallery index writes");
    Check(stbi_write_png((destination/"contact-sheet.png").string().c_str(),sheet.width,sheet.height,4,sheet.pixels.data(),sheet.width*4)!=0,"gallery contact sheet writes");
}

void Larch(const std::filesystem::path& artifactDir)
{
    using namespace tree_tool;
    namespace fs=std::filesystem;
    const fs::path root=IXTREEME_TREE_INTERNAL_DIR;
    auto& palette=TreeTexturePalette::Instance(); palette.EnsureLoaded(root);
    const auto preset=ixtreemetree::loadPresetFile(root/"tree_presets/golden_larch.json");
    Check(bool(preset),"larch preset loads");
    ixtreemetree::Tree tree; tree.options=preset->options;
    const auto mesh=tree.generate();
    Check(mesh.stats.barkTriangles>10000 && mesh.stats.barkTriangles<200000,"larch has detailed branches within its triangle budget");
    Check(mesh.leaves.vertices.size()>4000 && mesh.leaves.vertices.size()<100000,"larch has distributed foliage within its budget");
    const auto bark=Load(palette.Bark(tree.options.bark.type).path);
    const auto leaves=Load(palette.Leaf(tree.options.leaves.type).path);
    int visible=0,holes=0;
    for (std::size_t i=3;i<leaves.pixels.size();i+=4)
        (leaves.pixels[i]>128?visible:holes)++;
    Check(visible>5000 && holes>visible,"larch atlas contains fine needles with transparent gaps");
    PreviewView view; view.width=800; view.height=1000;
    view.center={(mesh.bboxMin.x+mesh.bboxMax.x)*0.5f,(mesh.bboxMin.y+mesh.bboxMax.y)*0.5f,(mesh.bboxMin.z+mesh.bboxMax.z)*0.5f};
    view.radius=std::max(6.5f,(mesh.bboxMax.y-mesh.bboxMin.y)*0.55f); view.zoom=1.2f;
    for (int angle=0;angle<3;++angle)
    {
        view.yaw=-0.65f+angle*2.094395f;
        const auto image=RasterTreePreview(mesh,view,{&bark,{1,1,1,1},0,false},{&leaves,{1,1,1,1},tree.options.leaves.alphaTest,true});
        if (!artifactDir.empty())
        {
            fs::create_directories(artifactDir);
            const auto path=artifactDir/("golden_larch_"+std::to_string(angle)+".png");
            Check(stbi_write_png(path.string().c_str(),image.width,image.height,4,image.pixels.data(),image.width*4)!=0,"preview writes");
        }
    }
    if (!artifactDir.empty())
    {
        TreeMaterialBinding binding;
        binding.barkBaseColorTexturePath=palette.Bark(tree.options.bark.type).path;
        binding.leafBaseColorTexturePath=palette.Leaf(tree.options.leaves.type).path;
        binding.leafAlphaCutoff=tree.options.leaves.alphaTest;
        binding.impostor.enabled=true; binding.impostor.resolution=256;
        const auto result=TreeGlbExporter::SaveAsAsset(mesh,artifactDir/"project","GoldenLarch_Organic",binding);
        Check(result.ok,result.error.c_str());
        Check(result.impostorBaked,"organic tree bakes an impostor");
        Check(ixtreemetree::savePresetFile(artifactDir/"golden_larch.json",*preset),"editable growth preset writes");
    }
    std::cout<<"Golden Larch: "<<mesh.stats.barkTriangles<<" bark triangles, "<<mesh.stats.leafTriangles
        <<" leaf triangles, generated in "<<mesh.stats.generationMs<<" ms\n";
}
}
int main(int argc,char** argv)
{
    try
    {
        TestCutoutDepth();
        TestPreviewLighting();
        TestEditorPreview();
        Larch(argc==3 && std::string(argv[1])=="--write-preview" ? std::filesystem::path(argv[2]) : std::filesystem::path{});
        if (argc==3 && std::string(argv[1])=="--write-gallery") WritePresetGallery(argv[2]);
        std::cout<<"TreeAuthoringTest: all checks passed\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
