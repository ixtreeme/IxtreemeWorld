#pragma once
#include "MaterialSubgraph.h"

namespace material_tool {
enum class SmartGeneratorKind { EdgeWear, DirtAccumulation, RustSpread, MossGrowth, DustAccumulation, WaterStreaks, Custom };
struct SmartGeneratorPreset {
    SmartGeneratorKind kind;
    const char* id;
    const char* name;
    const char* description;
};
std::span<const SmartGeneratorPreset> SmartGeneratorPresets();
struct SmartGenerator {
    SmartGeneratorKind kind = SmartGeneratorKind::Custom;
    std::string name = "Custom Generator", description;
    MaterialSubgraph definition;
    static bool Create(SmartGeneratorKind kind, SmartGenerator& result, std::string& error);
    bool Validate(std::string& error) const;
    std::string ToJson() const;
    bool FromJson(const std::string& text, std::string& error);
    bool operator==(const SmartGenerator&) const = default;
};
// A generator call is an ordinary typed subgraph. Optional assignment uses the
// canonical layer transaction; every failure retains the authored graph.
std::uint32_t AddSmartGenerator(MaterialGraphDocument& graph, const std::string& guid,
    const SmartGenerator& generator, std::uint32_t layer, std::string& error, std::array<float,2> position = {});
}
