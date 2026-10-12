#pragma once

#include "MaterialGraphDocument.h"
#include <functional>

namespace material_tool
{
struct MaterialSubgraph
{
    std::uint32_t interfaceVersion = 1;
    MaterialGraphDocument graph;
    static MaterialSubgraph Example();
    bool GetInterface(std::vector<MaterialGraphPort>& ports, std::string& error) const;
    std::string ToJson() const;
    bool FromJson(const std::string& text, std::string& error);
    bool operator==(const MaterialSubgraph&) const = default;
};
using MaterialSubgraphResolver = std::function<bool(const std::string& guid, MaterialSubgraph& asset, std::string& error)>;
struct MaterialNodeOrigin
{
    std::uint32_t expandedNode = 0, sourceNode = 0;
    std::string assetGuid, instancePath;
};
struct MaterialSubgraphExpansion
{
    std::vector<std::string> dependencyGuids;
    std::vector<MaterialNodeOrigin> origins;
    std::uint32_t previewPin = 0;
};
// Explicit bind/refresh preserves links by stable interface ID. Connected removed or
// type-changed ports reject the transaction; there is no automatic lossy migration.
bool BindMaterialSubgraph(MaterialGraphDocument& document, std::uint32_t node,
                         const std::string& guid, const MaterialSubgraph& asset, std::string& error);
// Expansion produces only existing operational nodes. The saved definition is never
// flattened or modified, and both outputs are retained on failure.
bool ExpandMaterialSubgraphs(const MaterialGraphDocument& document, const MaterialSubgraphResolver& resolver,
                             MaterialGraphDocument& expanded, MaterialSubgraphExpansion& info,
                             std::string& error, std::uint32_t previewPin = 0);
}
