#include "MaterialGraphAssets.h"
#include "GeometryAssets.h"
#include "MaterialInstance.h"
#include "MaterialGraphIO.h"
#include "asset/AssetDatabase.h"
#include "asset/ExrImage.h"
#include "asset/MaterialAssetManager.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace material_tool {
bool LoadSmartGeneratorAsset(const std::filesystem::path& path,SmartGenerator& generator,std::string& error) {
    if(detectAssetType(path)!=AssetType::SmartGenerator){error="Choose an .ixsmartgenerator asset";return false;}
    std::string text;return LoadMaterialText(path,text,error) && generator.FromJson(text,error);
}
GraphSubgraphCache::Stamp GraphSubgraphCache::Current(const std::string& guid)
{
    Stamp result;
    if (const auto parsed = Guid::fromString(guid)) if (const auto path = AssetDatabase::Instance().resolveGuid(*parsed)) {
        result.path = *path; std::error_code error;
        const auto type=detectAssetType(*path);
        if ((type != AssetType::MaterialSubgraph && type != AssetType::SmartGenerator) || !std::filesystem::is_regular_file(*path, error)) return result;
        result.modified = std::filesystem::last_write_time(*path, error); if (error) return result;
        result.bytes = std::filesystem::file_size(*path, error); if (error) return result;
        result.exists = true;
    }
    return result;
}
void GraphSubgraphCache::FinishCompilation()
{
    std::erase_if(entries_, [&](const auto& item) { return std::find(used_.begin(), used_.end(), item.first) == used_.end(); });
}
bool GraphSubgraphCache::Changed() const
{
    for (const auto& [guid, entry] : entries_) if (Current(guid) != entry.stamp) return true;
    return false;
}
bool GraphSubgraphCache::Resolve(const std::string& guid, MaterialSubgraph& asset, std::string& error)
{
    error.clear();
    if (!IsMaterialAssetGuid(guid)) { error = "Invalid subgraph GUID"; return false; }
    if (std::find(used_.begin(), used_.end(), guid) == used_.end()) used_.push_back(guid);
    const auto current = Current(guid);
    auto found = entries_.find(guid);
    if (found == entries_.end() || found->second.stamp != current) {
        // A new definition may replace a full previous dependency set. Only entries
        // used in this compilation count toward the live limit.
        if (found == entries_.end() && entries_.size() >= 256)
            std::erase_if(entries_, [&](const auto& item) { return std::find(used_.begin(),used_.end(),item.first)==used_.end(); });
        if (found == entries_.end() && entries_.size() >= 256) { error = "Subgraph cache exceeds 256 assets"; return false; }
        Entry entry; entry.stamp = current; std::string text;
        if (!current.exists) entry.error = "Missing MaterialSubgraph asset GUID: " + guid;
        else if(detectAssetType(current.path)==AssetType::SmartGenerator) {
            SmartGenerator generator;if(LoadSmartGeneratorAsset(current.path,generator,entry.error))entry.asset=std::move(generator.definition);
        }
        else if (!LoadMaterialText(current.path, text, entry.error) || !entry.asset.FromJson(text, entry.error))
            entry.error = "Subgraph " + guid + ": " + entry.error;
        if(entry.error.empty())for (auto& node:entry.asset.graph.nodes) if (node.assetGuid.empty() && node.assetId.empty() && !node.resource.empty() &&
            (node.kind==NodeKind::TextureParameter || node.kind==NodeKind::TextureSample || node.kind==NodeKind::Triplanar)) {
            auto path=std::filesystem::path(std::u8string(node.resource.begin(),node.resource.end()));
            if (path.is_relative()) { const auto utf8=(current.path.parent_path()/path).generic_u8string(); node.resource.assign(utf8.begin(),utf8.end()); }
        }
        found = entries_.insert_or_assign(guid, std::move(entry)).first;
    }
    if (!found->second.error.empty()) { error = found->second.error; return false; }
    asset = found->second.asset; return true;
}
void GraphTextureCache::Configure(AssetLibrary* library,const std::filesystem::path& graphFolder) {
    if(library!=library_ || graphFolder!=graphFolder_){Clear();library_=library;graphFolder_=graphFolder;}
}
void GraphTextureCache::Clear(){images_.clear();bindings_.clear();error_.clear();bytes_=0;}
std::filesystem::path GraphTextureCache::Resolve(const GraphNode& n,const std::string& overridePath) const {
    // An explicit GUID is authoritative. Never let a stale path pick another texture.
    const auto guid = overridePath.starts_with("guid:") ? overridePath.substr(5) : (overridePath.empty() ? n.assetGuid : std::string{});
    if (!guid.empty()) {
        if (const auto parsed=Guid::fromString(guid)) if (const auto path=AssetDatabase::Instance().resolveGuid(*parsed))
            return detectAssetType(*path)==AssetType::Texture ? *path : std::filesystem::path{};
        if (library_) for (const auto& entry : library_->Entries()) if (entry.category == AssetLibrary::Category::Texture && entry.guid == guid) return library_->AbsolutePath(entry);
        return {};
    }
    if(library_)for(const auto& entry:library_->Entries())if(entry.category==AssetLibrary::Category::Texture &&
        ((overridePath.empty() && ((!n.assetGuid.empty() && entry.guid==n.assetGuid) || (!n.assetId.empty() && entry.id==n.assetId))) ||
         overridePath=="asset:"+entry.id || library_->AssetRelativePath(entry)==(overridePath.empty()?n.resource:overridePath)))return library_->AbsolutePath(entry);
    const auto path=std::filesystem::path(std::u8string((overridePath.empty()?n.resource:overridePath).begin(),(overridePath.empty()?n.resource:overridePath).end()));
    if(path.is_absolute())return path;
    if(library_) {const auto candidate=library_->LibraryRoot()/path;std::error_code ec;if(std::filesystem::exists(candidate,ec))return candidate;}
    return graphFolder_/path;
}
SampledTexture GraphTextureCache::Sample(const GraphNode& n,const std::string& overridePath,float u,float v) {
    if(overridePath.empty() && n.resource.empty() && n.assetGuid.empty() && n.assetId.empty())return n.value;
    auto [bindingIt,newBinding]=bindings_.try_emplace(n.id);auto& binding=bindingIt->second;
    if(newBinding || binding.resource!=n.resource || binding.guid!=n.assetGuid || binding.assetId!=n.assetId || binding.overridePath!=overridePath) {
        binding.resource=n.resource;binding.guid=n.assetGuid;binding.assetId=n.assetId;binding.overridePath=overridePath;
        const auto path=Resolve(n,overridePath);const auto utf8=path.generic_u8string();binding.key.assign(utf8.begin(),utf8.end());binding.exr=client::asset::IsExrPath(path);
    }
    if (binding.key.empty()) { error_="Node "+std::to_string(n.id)+": Missing texture GUID "+(overridePath.empty()?n.assetGuid:overridePath); return Float4{1,0,1,1}; }
    const auto& key=binding.key;auto [it,inserted]=images_.try_emplace(key);auto& image=it->second;
    if(inserted) {
        const auto path=std::filesystem::path(std::u8string(key.begin(),key.end()));
        const auto fail=[&](const std::string& why){image.error="Cannot read texture: "+why+" ("+key+")";};
        int width=0,height=0,channels=0;
        const bool exr=client::asset::IsExrPath(path);std::string error;
        FILE* file=nullptr;
        if(exr) {
            if(!client::asset::ReadExrResolution(path,width,height,error))fail(error);
        } else {
            _wfopen_s(&file,path.c_str(),L"rb");
            if(!file || !stbi_info_from_file(file,&width,&height,&channels))fail("Cannot decode texture");
            if(file)std::rewind(file);
        }
        const size_t pixels=width>0 && height>0?static_cast<size_t>(width)*height:0;
        if(!pixels || pixels>16U*1024U*1024U || bytes_+pixels*16>256U*1024U*1024U) {
            if(pixels)fail("Texture cache exceeds its 256 MB limit");
        } else if(exr) {
            if(auto decoded=client::asset::LoadExr(path,error)){image.width=width;image.height=height;image.pixels=std::move(decoded->rgba);}
            else fail(error);
        } else if(file) {
            if(auto* raw=stbi_load_from_file(file,&width,&height,&channels,4)) {
                image.width=width;image.height=height;image.pixels.resize(pixels*4);
                for(size_t k=0;k<image.pixels.size();++k)image.pixels[k]=raw[k]/255.0f;stbi_image_free(raw);
            } else fail("Cannot decode texture pixels");
        }
        if(file)std::fclose(file);bytes_+=image.pixels.size()*sizeof(float);
    }
    if(image.pixels.empty()){error_="Node "+std::to_string(n.id)+": "+image.error;return Float4{1,0,1,1};}
    // Repeat before integer conversion, including negative coordinates and very large UVs.
    u-=std::floor(u);v-=std::floor(v);const float px=u*image.width-.5f,py=v*image.height-.5f;
    const int x=static_cast<int>(std::floor(px)),y=static_cast<int>(std::floor(py));const float tx=px-x,ty=py-y;
    const auto fetch=[&](int ix,int iy,int channel){ix=(ix%image.width+image.width)%image.width;iy=(iy%image.height+image.height)%image.height;return image.pixels[(static_cast<size_t>(iy)*image.width+ix)*4+channel];};
    Float4 result{};for(int k=0;k<4;++k) {
        const float a=fetch(x,y,k)+(fetch(x+1,y,k)-fetch(x,y,k))*tx,b=fetch(x,y+1,k)+(fetch(x+1,y+1,k)-fetch(x,y+1,k))*tx;result[k]=a+(b-a)*ty;
    }
    return {result,binding.exr};
}
bool PublishBakedMaterial(AssetLibrary& library,const MaterialGraphDocument& document,const ParameterOverrides& parameters,unsigned resolution,const std::string& name,AssetLibrary::Entry& result,std::string& error,const std::filesystem::path& graphFolder,const GeometryBake* geometry,const std::string& geometryGuid) {
    GraphGeometryCache verifiedGeometry;std::string geometryBytes;
    if(geometry) {
        if(resolution!=geometry->settings.resolution){error="Material resolution must match the Geometry Bake atlas";return false;}
        if(!verifiedGeometry.Bind(geometryGuid,error))return false;
        std::string savedBytes;
        if(!EncodeGeometryBake(*geometry,geometryBytes,error) || !EncodeGeometryBake(*verifiedGeometry.Bake(),savedBytes,error))return false;
        if(savedBytes!=geometryBytes){error="Geometry Bake changed since preview; reload or save the cache before publishing";return false;}
    } else if(!geometryGuid.empty()){error="Geometry Bake GUID requires its validated map data";return false;}
    auto source = document; GraphSubgraphCache subgraphs; MaterialSubgraphExpansion expansion;
    const bool hasSubgraphs = std::any_of(document.nodes.begin(), document.nodes.end(), [](const auto& n) { return n.kind == NodeKind::Subgraph; });
    if (hasSubgraphs && !ExpandMaterialSubgraphs(document, subgraphs.Resolver(), source, expansion, error)) return false;
    MaterialGraphProgram program;if(!program.Compile(source,error))return false;
    if (!program.ValidateOverrides(parameters, error)) return false;
    GraphTextureCache cache;cache.Configure(&library,graphFolder);BakedMaterialImages images;
    const TextureSampler sampler=[&](const auto& n,const auto& p,float u,float v){return cache.Sample(n,p,u,v);};
    if(geometry ? !BakeMaterialOnGeometry(program,parameters,sampler,*geometry,images,error) : !BakeMaterialUV(program,parameters,sampler,resolution,images,error))return false;
    if(!cache.Error().empty()){error=cache.Error();return false;}
    std::string stem;for(const unsigned char c:name)stem+=std::isalnum(c)?static_cast<char>(c):'_';if(stem.empty())stem="GraphMaterial";
    const auto folder=library.LibraryRoot()/"Materials"/"GraphBakes"/(stem+"_"+generateGuidV4().toString());std::error_code ec;std::filesystem::create_directories(folder,ec);
    if(ec){error="Cannot create material bake folder: "+ec.message();return false;}
    const char* filenames[]={"albedo.png","normal.png","ao.png","roughness.png","metallic.png","height.png","emissive.png"};
    const std::vector<std::uint8_t>* data[]={&images.color,&images.normal,&images.ao,&images.roughness,&images.metallic,&images.height,&images.emissive};
    for(int k=0;k<7;++k) {
        FILE* file=nullptr;_wfopen_s(&file,(folder/filenames[k]).c_str(),L"wb");if(!file){error="Cannot write baked texture";return false;}
        const auto write=[](void* context,void* bytes,int count){std::fwrite(bytes,1,count,static_cast<FILE*>(context));};
        const bool ok=stbi_write_png_to_func(write,file,static_cast<int>(resolution),static_cast<int>(resolution),4,data[k]->data(),static_cast<int>(resolution)*4)!=0;
        const bool failed=std::ferror(file)!=0;const int closed=std::fclose(file);if(!ok || failed || closed){error="Cannot finish baked texture";return false;}
    }
    // Generated images must be in the database before the material resolves their texture GUIDs.
    for(const auto* filename:filenames)AssetDatabase::Instance().getOrCreateGuid(folder/filename);
    for(auto& n:source.nodes) {
        if(parameters.values.contains(n.parameter))n.value=parameters.values.at(n.parameter);
        if(parameters.textures.contains(n.parameter)){const auto& overrideValue=parameters.textures.at(n.parameter);n.resource=overrideValue;n.assetGuid.clear();n.assetId.clear();
            if(overrideValue.starts_with("asset:"))if(const auto entry=library.FindById(overrideValue.substr(6))){n.assetId=entry->id;n.assetGuid=entry->guid;n.resource=library.AssetRelativePath(*entry);}
            if(overrideValue.starts_with("guid:"))for(const auto& entry:library.Entries())if(entry.guid==overrideValue.substr(5)){n.assetGuid=entry.guid;n.assetId=entry.id;n.resource=library.AssetRelativePath(entry);break;}}
    }
    std::vector<Guid> sourceDependencies;
    for(const auto& node:source.nodes)if(const auto guid=Guid::fromString(node.assetGuid))if(std::find(sourceDependencies.begin(),sourceDependencies.end(),*guid)==sourceDependencies.end())sourceDependencies.push_back(*guid);
    if(geometry) {
        const auto geometryPath=folder/"geometry.ixgeometrybake";
        if(!SaveMaterialTextAtomic(geometryPath,geometryBytes,error))return false;
        auto& database=AssetDatabase::Instance();const auto geometrySnapshotGuid=database.getOrCreateGuid(geometryPath);
        if(!database.writeDependencies(geometryPath,{*Guid::fromString(verifiedGeometry.MeshGuid())})){error="Cannot write geometry snapshot dependency";return false;}
        sourceDependencies.push_back(geometrySnapshotGuid);
    }
    const auto sourcePath=folder/"source.materialgraph";
    if (!SaveMaterialTextAtomic(sourcePath,source.ToJson(),error)) return false;
    AssetDatabase::Instance().getOrCreateGuid(sourcePath);
    if(!AssetDatabase::Instance().writeDependencies(sourcePath,sourceDependencies)){error="Cannot write material bake source dependencies";return false;}
    if (hasSubgraphs) {
        // Keep both the authored definition and an effective, self-contained source for
        // this bake. Changes to an external subgraph cannot alter the baked snapshot.
        const auto authored = folder/"definition.materialgraph";
        if (!SaveMaterialTextAtomic(authored,document.ToJson(),error)) return false;
        AssetDatabase::Instance().getOrCreateGuid(authored);
        std::vector<Guid> dependencies;
        for (const auto& guid : expansion.dependencyGuids) if (const auto parsed=Guid::fromString(guid)) dependencies.push_back(*parsed);
        if (!AssetDatabase::Instance().writeDependencies(authored,dependencies)) { error="Cannot write baked definition dependencies"; return false; }
    }
    GltfMaterialSource material;material.name=name;material.baseColorTexturePath=folder/filenames[0];material.normalTexturePath=folder/filenames[1];material.aoTexturePath=folder/filenames[2];material.roughnessTexturePath=folder/filenames[3];material.metallicTexturePath=folder/filenames[4];material.heightTexturePath=folder/filenames[5];material.emissiveTexturePath=folder/filenames[6];material.emissive={1,1,1,1};material.roughness=1;material.metallic=1;
    for(size_t at=3;at<images.color.size();at+=4)if((!geometry || geometry->coverage[at/4]) && images.color[at]<255){material.alphaMode="blend";break;}
    auto& manager=MaterialAssetManager::Instance();const auto guid=manager.createFromGltfMaterial(material,folder,stem);
    if(!manager.getOrLoad(guid)){error="Cannot create material asset";return false;}
    if(!library.Refresh(error))return false;
    for(const auto& entry:library.Entries())if(entry.category==AssetLibrary::Category::Material && entry.guid==guid.toString()){result=entry;return true;}
    error="Baked material was saved but the Asset Browser could not register it";return false;
}
}
