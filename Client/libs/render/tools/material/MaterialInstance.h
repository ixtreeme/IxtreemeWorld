#pragma once

#include "MaterialGraphCompiler.h"
#include <memory>

namespace material_tool
{
// Separate asset data: no graph copy and no GPU handles. AssetDatabase owns GUID resolution.
struct MaterialInstance
{
    std::string definitionGuid;
    std::string geometryBakeGuid;
    ParameterOverrides overrides;
    std::string ToJson() const;
    bool FromJson(const std::string& text, std::string& error);
    bool Validate(const MaterialGraphProgram& program, std::string& error) const;
    bool operator==(const MaterialInstance&) const = default;
};

// Instances may retain the same immutable program across many objects. Changing overrides
// requires validation/packing, never program compilation. Replace the shared program on reload.
class MaterialInstanceBinding
{
public:
    bool Bind(std::string resolvedGuid, std::shared_ptr<const MaterialGraphProgram> program,
              const MaterialInstance& instance, std::string& error);
    bool Update(const ParameterOverrides& overrides, std::string& error);
    const MaterialInstance& Instance() const { return instance_; }
    const std::shared_ptr<const MaterialGraphProgram>& Program() const { return program_; }
    const std::vector<Float4>& ParameterBuffer() const { return buffer_; }
private:
    MaterialInstance instance_;
    std::shared_ptr<const MaterialGraphProgram> program_;
    std::vector<Float4> buffer_;
};
bool IsMaterialAssetGuid(const std::string& guid);
}
