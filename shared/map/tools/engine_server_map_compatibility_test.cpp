#include "map/MapData.h"
#include "schema/map_manifest.capnp.h"
#include "schema/world_package_manifest.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void Check(const char* name, bool value)
{
    std::cout << "MAP INTEGRATION " << name << ": " << (value ? "PASS" : "FAIL") << '\n';
    failures += value ? 0 : 1;
}

bool Near(float a, float b)
{
    return std::abs(a - b) < 0.00001f;
}

mx::map::AssetReadFn ReadManifest(capnp::MessageBuilder& message)
{
    const auto flat = capnp::messageToFlatArray(message);
    const auto bytes = flat.asBytes();
    return [data = std::vector<std::uint8_t>(bytes.begin(), bytes.end())](std::string_view path)
        -> std::optional<std::vector<std::uint8_t>> {
        if (path == "fixture/map.manifest") {
            return data;
        }
        return std::nullopt;
    };
}
} // namespace

int main()
{
    // Both generated types must coexist in one translation unit and library.
    capnp::MallocMessageBuilder engine_message;
    auto engine = engine_message.initRoot<mx::map::schema::MapManifest>();
    engine.setFormatVersion(2);
    engine.setWorldId("engine-v2");
    engine.setWorldSizeCells(32);
    engine.setCellSizeMeters(2.0f);
    engine.setChunkSizeCells(16);
    engine.setTriplanarSlopeThreshold(0.37f);
    engine.setTriplanarSlopeTransition(0.41f);
    auto material = engine.initTexturePalette(1)[0];
    material.setPath("textures/bridge.dds");
    material.setTilingX(2.0f);
    material.setTilingY(3.0f);
    material.setNormalStrength(0.6f);
    material.setRoughnessStrength(0.7f);
    material.setTintR(0.2f);
    material.setTintG(0.3f);
    material.setTintB(0.4f);
    material.setMetallicStrength(0.8f);
    material.setAoStrength(0.9f);
    material.setUvOffsetX(0.1f);
    material.setUvOffsetY(0.15f);
    material.setUvRotationDegrees(45.0f);
    const auto loaded = mx::map::LoadManifest(ReadManifest(engine_message), "fixture");
    Check("engine-v2-load", loaded && loaded->world_id == "engine-v2");
    Check("engine-triplanar-preserved", loaded && Near(loaded->triplanar_slope_threshold, 0.37f) &&
          Near(loaded->triplanar_slope_transition, 0.41f));
    Check("engine-material-preserved", loaded && loaded->texture_palette_paths.size() == 1 &&
          loaded->texture_palette_paths[0] == "textures/bridge.dds" &&
          Near(loaded->texture_palette_tiling_x[0], 2.0f) &&
          Near(loaded->texture_palette_tiling_y[0], 3.0f) &&
          Near(loaded->texture_palette_normal_strength[0], 0.6f) &&
          Near(loaded->texture_palette_roughness_strength[0], 0.7f) &&
          Near(loaded->texture_palette_tint_r[0], 0.2f) &&
          Near(loaded->texture_palette_tint_g[0], 0.3f) &&
          Near(loaded->texture_palette_tint_b[0], 0.4f) &&
          Near(loaded->texture_palette_metallic_strength[0], 0.8f) &&
          Near(loaded->texture_palette_ao_strength[0], 0.9f) &&
          Near(loaded->texture_palette_uv_offset_x[0], 0.1f) &&
          Near(loaded->texture_palette_uv_offset_y[0], 0.15f) &&
          Near(loaded->texture_palette_uv_rotation_degrees[0], 45.0f));

    capnp::MallocMessageBuilder package_message;
    auto package = package_message.initRoot<mx::map::package_schema::MapManifest>();
    package.setFormatVersion(2);
    package.setWorldId("server-legacy-v2");
    package.initTexturePalette(1)[0].setPath("textures/ground.dds");
    const auto legacy = mx::map::LoadManifest(ReadManifest(package_message), "fixture");
    Check("server-v2-common-fields-readable", legacy && legacy->world_id == "server-legacy-v2" &&
          legacy->texture_palette_paths.size() == 1);
    Check("server-v2-engine-material-defaults", legacy && legacy->texture_palette_paths.size() == 1 &&
          Near(legacy->triplanar_slope_threshold, 0.18f) &&
          Near(legacy->triplanar_slope_transition, 0.20f) &&
          Near(legacy->texture_palette_tiling_x[0], 1.0f) &&
          Near(legacy->texture_palette_tint_r[0], 1.0f) &&
          Near(legacy->texture_palette_uv_rotation_degrees[0], 0.0f));

    package.setFormatVersion(3);
    package.setWorldSizeCellsY(48);
    package.initOrigin().setX(-100.0);
    package.getOrigin().setY(250.0);
    const auto flat = capnp::messageToFlatArray(package_message);
    capnp::FlatArrayMessageReader reader(flat.asPtr());
    const auto decoded = reader.getRoot<mx::map::package_schema::MapManifest>();
    Check("server-v3-extensions-preserved", decoded.getWorldSizeCellsY() == 48 &&
          decoded.getOrigin().getX() == -100.0 && decoded.getOrigin().getY() == 250.0);
    Check("engine-v3-refused", !mx::map::LoadManifest(ReadManifest(package_message), "fixture"));

    const mx::map::Rect rect{0, 0, 32, 32};
    Check("engine-closed-boundary-preserved", rect.Contains(32, 32));
    Check("server-half-open-boundary-preserved", rect.ContainsHalfOpen(0, 0) &&
          rect.ContainsHalfOpen(31, 31) && !rect.ContainsHalfOpen(32, 31) &&
          !rect.ContainsHalfOpen(31, 32));
    std::cout << "MAP INTEGRATION summary: failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
