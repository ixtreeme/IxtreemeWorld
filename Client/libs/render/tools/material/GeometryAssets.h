#pragma once
#include "GeometryBaker.h"
#include <filesystem>
#include <vector>

namespace material_tool {
// Native GUID bridge. Assimp/import/database work stays on the editor thread.
class GraphGeometryCache {
public:
    bool Select(const std::filesystem::path& path,unsigned meshIndex,std::string& error);
    // Every submesh drawn with one material slot, as a single surface: a slot is the unit a material is assigned to.
    bool SelectMaterialSlot(const std::filesystem::path& path,unsigned slot,std::string& error);
    bool Bind(const std::string& bakeGuid,std::string& error);
    bool Accept(GeometryBake bake,std::string& error);
    bool Save(const std::filesystem::path& path,std::string& error);
    bool Refresh(bool force = false); // True when a dependency changed, including an invalidation.
    void Clear();
    const GeometryBake* Bake() const {return bakeValid_?&bake_:nullptr;}
    const GeometryMesh* Mesh() const {return meshValid_?&mesh_:nullptr;}
    const std::string& MeshGuid() const {return meshGuid_;}
    const std::string& BakeGuid() const {return bakeGuid_;}
    const std::string& Name() const {return name_;}
    const std::string& Error() const {return error_;}
    unsigned MeshIndex() const {return meshIndex_;}
    // Source submeshes that make up the selection, in model order.
    const std::vector<unsigned>& MeshIndices() const {return meshIndices_;}
    unsigned MeshCount() const {return meshCount_;}
    std::filesystem::path MeshPath() const {return meshStamp_.path;}
private:
    struct Stamp {
        std::filesystem::path path;
        std::filesystem::file_time_type time{};
        std::filesystem::file_time_type metaTime{};
        std::uintmax_t bytes=0;
        std::uintmax_t metaBytes=0;
        bool exists=false;
        bool operator==(const Stamp&) const = default;
    };
    static Stamp Current(const std::string& guid);
    GeometryMesh mesh_;
    GeometryBake bake_;
    std::string meshGuid_,bakeGuid_,name_,error_;
    Stamp meshStamp_,bakeStamp_;
    std::vector<unsigned> meshIndices_;
    unsigned meshIndex_=0,meshCount_=0,slot_=0;
    bool bySlot_=false;
    bool meshValid_=false,bakeValid_=false;
};
}
