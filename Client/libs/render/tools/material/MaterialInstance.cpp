#include "MaterialInstance.h"
#include <crude_json.h>
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace material_tool
{
bool IsMaterialAssetGuid(const std::string& guid)
{
    if (guid.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t at = 0; at < guid.size(); ++at)
    {
        const char c = guid[at];
        if (at == 8 || at == 13 || at == 18 || at == 23) { if (c != '-') return false; }
        else
        {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            nonzero |= c != '0';
        }
    }
    return nonzero;
}
std::string MaterialInstance::ToJson() const
{
    using Json = crude_json::value;
    Json root(crude_json::type_t::object);
    root["format"] = "ixtreeme.materialinstance"; root["version"] = 1.0;
    root["definition_guid"] = definitionGuid; root["geometry_bake_guid"] = geometryBakeGuid;
    root["values"] = Json(crude_json::type_t::array); root["textures"] = Json(crude_json::type_t::array);
    std::vector<std::string> keys;
    for (const auto& [key, value] : overrides.values) { (void)value; keys.push_back(key); }
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys)
    {
        Json item(crude_json::type_t::object), value(crude_json::type_t::array);
        for (float f : overrides.values.at(key)) value.push_back(static_cast<double>(f));
        item["parameter"] = key; item["value"] = std::move(value); root["values"].push_back(std::move(item));
    }
    keys.clear();
    for (const auto& [key, value] : overrides.textures) { (void)value; keys.push_back(key); }
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys)
    {
        Json item(crude_json::type_t::object); item["parameter"] = key;
        item["resource"] = overrides.textures.at(key); root["textures"].push_back(std::move(item));
    }
    return root.dump(2);
}
bool MaterialInstance::FromJson(const std::string& text, std::string& error)
{
    using Json = crude_json::value;
    error.clear();
    const auto fail = [&](const char* reason) { error = reason; return false; };
    if (text.size() > 4 * 1024 * 1024) return fail("Material instance exceeds 4 MB");
    const auto root = Json::parse(text);
    if (!root.is_object() || !root.contains("format") || !root["format"].is_string() ||
        root["format"].get<crude_json::string>() != "ixtreeme.materialinstance") return fail("Not a material instance");
    if (!root.contains("version") || !root["version"].is_number() || root["version"].get<crude_json::number>() != 1)
        return fail("Unsupported material instance version");
    MaterialInstance draft;
    for (const auto* key : {"definition_guid", "geometry_bake_guid"})
        if (!root.contains(key) || !root[key].is_string()) return fail("Missing material instance GUID");
    draft.definitionGuid = root["definition_guid"].get<crude_json::string>();
    draft.geometryBakeGuid = root["geometry_bake_guid"].get<crude_json::string>();
    if (!IsMaterialAssetGuid(draft.definitionGuid) || (!draft.geometryBakeGuid.empty() && !IsMaterialAssetGuid(draft.geometryBakeGuid)))
        return fail("Invalid material instance GUID");
    std::unordered_set<std::string> names;
    for (const auto* key : {"values", "textures"})
    {
        if (!root.contains(key) || !root[key].is_array()) return fail("Missing material instance overrides");
        const auto& list = root[key].get<crude_json::array>();
        if (list.size() > 2048) return fail("Too many material overrides");
        for (const auto& item : list)
        {
            if (!item.is_object() || !item.contains("parameter") || !item["parameter"].is_string()) return fail("Invalid override parameter");
            const auto name = item["parameter"].get<crude_json::string>();
            if (name.empty() || name.size() > 255 || !names.insert(name).second) return fail("Empty or duplicate override parameter");
            if (std::string_view(key) == "values")
            {
                if (!item.contains("value") || !item["value"].is_array() || item["value"].get<crude_json::array>().size() != 4)
                    return fail("Invalid numeric override");
                Float4 value{};
                for (std::size_t k = 0; k < 4; ++k)
                {
                    const auto& number = item["value"][k];
                    if (!number.is_number()) return fail("Invalid numeric override");
                    const double f = number.get<crude_json::number>();
                    if (!std::isfinite(f) || std::abs(f) > 1000000) return fail("Nonfinite or out-of-range override");
                    value[k] = static_cast<float>(f);
                }
                draft.overrides.values.emplace(name, value);
            }
            else
            {
                if (!item.contains("resource") || !item["resource"].is_string()) return fail("Invalid texture override");
                const auto resource = item["resource"].get<crude_json::string>();
                if (resource.empty() || resource.size() > 4096 ||
                    (resource.starts_with("guid:") && !IsMaterialAssetGuid(resource.substr(5)))) return fail("Invalid texture override resource");
                draft.overrides.textures.emplace(name, resource);
            }
        }
    }
    *this = std::move(draft); return true;
}
bool MaterialInstance::Validate(const MaterialGraphProgram& program, std::string& error) const
{
    error.clear();
    if (!IsMaterialAssetGuid(definitionGuid) || (!geometryBakeGuid.empty() && !IsMaterialAssetGuid(geometryBakeGuid)))
    { error = "Invalid material instance GUID"; return false; }
    if (!program.HasOutput()) { error = "Material instance requires a compiled PBR definition"; return false; }
    for (const auto& [name, resource] : overrides.textures) if (resource.starts_with("guid:") && !IsMaterialAssetGuid(resource.substr(5)))
    { error = "Invalid texture override GUID: " + name; return false; }
    return program.ValidateOverrides(overrides, error);
}
bool MaterialInstanceBinding::Bind(std::string resolvedGuid, std::shared_ptr<const MaterialGraphProgram> program,
                                   const MaterialInstance& instance, std::string& error)
{
    error.clear();
    if (!program || resolvedGuid != instance.definitionGuid) { error = "Missing or mismatched material definition"; return false; }
    std::vector<Float4> buffer;
    if (!instance.Validate(*program, error) || !program->BuildParameterBuffer(instance.overrides, buffer, error)) return false;
    instance_ = instance; program_ = std::move(program); buffer_ = std::move(buffer); return true;
}
bool MaterialInstanceBinding::Update(const ParameterOverrides& overrides, std::string& error)
{
    error.clear();
    if (!program_) { error = "Material instance has no definition"; return false; }
    auto candidate = instance_; candidate.overrides = overrides;
    if (!candidate.Validate(*program_, error)) return false;
    std::vector<Float4> buffer;
    if (!program_->BuildParameterBuffer(overrides, buffer, error)) return false;
    instance_.overrides = overrides; buffer_ = std::move(buffer); return true;
}
}
