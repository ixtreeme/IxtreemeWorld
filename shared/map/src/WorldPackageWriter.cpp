#include "map/WorldPackageWriter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "map/WorldPackage.h"

namespace mx::map {
namespace fs = std::filesystem;

namespace {

void PutU8(std::vector<std::uint8_t>& out, std::uint8_t value)
{
    out.push_back(value);
}

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

void PutF32(std::vector<std::uint8_t>& out, float value)
{
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    PutU32(out, bits);
}

void PutRect(std::vector<std::uint8_t>& out, const Rect& r)
{
    PutF32(out, r.min_x);
    PutF32(out, r.min_y);
    PutF32(out, r.max_x);
    PutF32(out, r.max_y);
}

bool WriteFile(const fs::path& path, const std::vector<std::uint8_t>& bytes, std::string& error)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "cannot create " + path.parent_path().string() + ": " + ec.message();
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot open " + path.string() + " for writing";
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        error = "short write to " + path.string();
        return false;
    }
    return true;
}

std::string ChunkFile(std::uint32_t x, std::uint32_t y)
{
    return "chunks/chunk_" + std::to_string(x) + "_" + std::to_string(y) + ".mxchunk";
}

} // namespace

std::vector<std::uint8_t> EncodeWorldLogic(const WorldLogic& logic)
{
    std::vector<std::uint8_t> out;
    PutU32(out, kWorldLogicFileMagic);
    PutU32(out, kWorldLogicFileVersion);
    PutU32(out, static_cast<std::uint32_t>(logic.zones.size()));
    PutU32(out, static_cast<std::uint32_t>(logic.spawns.size()));
    PutU32(out, static_cast<std::uint32_t>(logic.warps.size()));
    for (const auto& zone : logic.zones) {
        PutU32(out, zone.id);
        const auto length = static_cast<std::uint8_t>(std::min<std::size_t>(zone.name.size(), 255));
        PutU8(out, length);
        out.insert(out.end(), zone.name.begin(), zone.name.begin() + length);
        PutRect(out, zone.bounds);
    }
    for (const auto& spawn : logic.spawns) {
        PutU32(out, spawn.id);
        PutU32(out, spawn.zone_id);
        PutRect(out, spawn.bounds);
    }
    for (const auto& warp : logic.warps) {
        PutU32(out, warp.id);
        PutRect(out, warp.source);
        PutF32(out, warp.target_x);
        PutF32(out, warp.target_y);
    }
    return out;
}

std::vector<std::uint8_t> EncodeChunk(const PackageWriteSpec& spec, std::uint32_t chunk_x, std::uint32_t chunk_y)
{
    const std::uint32_t n = spec.chunk_size_cells;
    const bool splats = spec.splat_size > 0;
    const std::uint16_t section_count = splats ? 4 : 2;
    const std::size_t height_bytes = static_cast<std::size_t>(n + 1) * (n + 1) * 2;
    const std::size_t attribute_bytes = static_cast<std::size_t>(n) * n * 2;
    const std::size_t splat_bytes = splats ? 4u + static_cast<std::size_t>(spec.splat_size) * spec.splat_size * 4u : 0;
    const auto height_offset =
        static_cast<std::uint32_t>(kChunkHeaderBytes + section_count * kChunkTocEntryBytes);
    const auto attribute_offset = static_cast<std::uint32_t>(height_offset + height_bytes);
    const auto splat_a_offset = static_cast<std::uint32_t>(attribute_offset + attribute_bytes);
    const auto splat_b_offset = static_cast<std::uint32_t>(splat_a_offset + splat_bytes);

    std::vector<std::uint8_t> out;
    out.reserve(splat_b_offset + splat_bytes);
    PutU32(out, kChunkFileMagic);
    PutU16(out, kChunkFileVersion);
    PutU16(out, static_cast<std::uint16_t>(chunk_x));
    PutU16(out, static_cast<std::uint16_t>(chunk_y));
    PutU16(out, static_cast<std::uint16_t>(n));
    PutU16(out, section_count);
    auto toc = [&](std::uint16_t type, std::uint8_t element, std::uint32_t offset, std::size_t length) {
        PutU16(out, type);
        PutU8(out, element);
        PutU8(out, 0);
        PutU32(out, offset);
        PutU32(out, static_cast<std::uint32_t>(length));
    };
    toc(kSectionHeight, kElementInt16, height_offset, height_bytes);
    toc(kSectionAttributes, kElementU16Bitfield, attribute_offset, attribute_bytes);
    if (splats) {
        toc(kSectionSplatA, kElementRgba8Image, splat_a_offset, splat_bytes);
        toc(kSectionSplatB, kElementRgba8Image, splat_b_offset, splat_bytes);
    }
    for (std::uint32_t y = 0; y <= n; ++y) {
        for (std::uint32_t x = 0; x <= n; ++x) {
            const std::int16_t h = spec.height_cm ? spec.height_cm(chunk_x * n + x, chunk_y * n + y) : 0;
            PutU16(out, static_cast<std::uint16_t>(h));
        }
    }
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            PutU16(out, spec.attributes ? spec.attributes(chunk_x * n + x, chunk_y * n + y) : 0);
        }
    }
    if (splats) {
        std::vector<std::uint8_t> a;
        std::vector<std::uint8_t> b;
        if (spec.splat) {
            spec.splat(chunk_x, chunk_y, a, b);
        }
        const std::size_t pixels = static_cast<std::size_t>(spec.splat_size) * spec.splat_size * 4u;
        a.resize(pixels, 0);
        b.resize(pixels, 0);
        for (auto* image : {&a, &b}) {
            PutU16(out, static_cast<std::uint16_t>(spec.splat_size));
            PutU16(out, static_cast<std::uint16_t>(spec.splat_size));
            out.insert(out.end(), image->begin(), image->begin() + static_cast<std::ptrdiff_t>(pixels));
        }
    }
    return out;
}

PackageWriteResult WritePackage(const fs::path& out_dir, const PackageWriteSpec& spec)
{
    PackageWriteResult result;
    if (spec.format_version != kManifestVersionLegacy && spec.format_version != kManifestVersionCurrent) {
        result.error = "format_version must be 2 or 3";
        return result;
    }
    if (spec.size_cells == 0 || spec.chunk_size_cells == 0 || spec.size_cells % spec.chunk_size_cells != 0) {
        result.error = "size_cells must be a positive multiple of chunk_size_cells";
        return result;
    }
    if (spec.format_version == kManifestVersionLegacy && spec.splat_size == 0) {
        result.error = "the legacy (v2) layout always carries splat sections";
        return result;
    }
    std::error_code ec;
    if (!spec.overwrite && fs::exists(out_dir / "map.manifest", ec)) {
        result.error = "refusing to overwrite the existing package at " + out_dir.string();
        return result;
    }
    const std::uint32_t grid = spec.size_cells / spec.chunk_size_cells;
    capnp::MallocMessageBuilder message;
    auto manifest = message.initRoot<schema::MapManifest>();
    manifest.setFormatVersion(spec.format_version);
    manifest.setWorldId(spec.world_id);
    manifest.setWorldName(spec.world_name);
    manifest.setWorldSizeCells(spec.size_cells);
    manifest.setCellSizeMeters(spec.cell_size_m);
    manifest.setHeightUnit(schema::HeightUnit::CENTIMETERS);
    manifest.setChunkSizeCells(spec.chunk_size_cells);
    auto palette = manifest.initTexturePalette(static_cast<unsigned>(spec.texture_palette.size()));
    for (std::size_t i = 0; i < spec.texture_palette.size(); ++i) {
        palette[static_cast<unsigned>(i)].setId(static_cast<std::uint16_t>(i));
        palette[static_cast<unsigned>(i)].setPath(spec.texture_palette[i]);
    }

    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;
    for (std::uint32_t y = 0; y < grid; ++y) {
        for (std::uint32_t x = 0; x < grid; ++x) {
            files.emplace_back(ChunkFile(x, y), EncodeChunk(spec, x, y));
        }
    }
    files.emplace_back("worldlogic.dat", EncodeWorldLogic(spec.logic));
    if (spec.mob_spawns) {
        files.emplace_back("mob_spawns.conf",
                           std::vector<std::uint8_t>(spec.mob_spawns->begin(), spec.mob_spawns->end()));
    }

    if (spec.format_version == kManifestVersionLegacy) {
        // v2: the historic generator wrote the chunk count into zoneGridDims.
        auto dims = manifest.initZoneGridDims();
        dims.setX(grid);
        dims.setY(grid);
        manifest.setZoneSizeCells(spec.size_cells);
        manifest.setWorldLogicFile("");
        manifest.setEnvironmentFile("");
    } else {
        manifest.setWorldSizeCellsY(spec.size_cells);
        auto origin = manifest.initOrigin();
        origin.setX(0.0);
        origin.setY(0.0);
        auto chunk_grid = manifest.initChunkGrid();
        chunk_grid.setX(grid);
        chunk_grid.setY(grid);
        struct Decl {
            schema::LayerKind kind;
            bool required;
            schema::LayerAudience audience;
            const char* file;
        };
        std::vector<Decl> decls = {
            {schema::LayerKind::HEIGHT, true, schema::LayerAudience::SHARED, ""},
            {schema::LayerKind::ATTRIBUTES, true, schema::LayerAudience::SHARED, ""},
        };
        if (spec.splat_size > 0) {
            decls.push_back({schema::LayerKind::SPLAT_A, false, schema::LayerAudience::CLIENT, ""});
            decls.push_back({schema::LayerKind::SPLAT_B, false, schema::LayerAudience::CLIENT, ""});
        }
        decls.push_back({schema::LayerKind::WORLD_LOGIC, true, schema::LayerAudience::SHARED, "worldlogic.dat"});
        if (spec.mob_spawns) {
            decls.push_back(
                {schema::LayerKind::MOB_SPAWNS, spec.mob_spawns_required, schema::LayerAudience::SERVER, "mob_spawns.conf"});
        }
        auto layers = manifest.initLayers(static_cast<unsigned>(decls.size()));
        for (std::size_t i = 0; i < decls.size(); ++i) {
            auto layer = layers[static_cast<unsigned>(i)];
            layer.setKind(decls[i].kind);
            layer.setRequired(decls[i].required);
            layer.setAudience(decls[i].audience);
            layer.setVersion(kLayerEncodingVersion);
            layer.setFile(decls[i].file);
        }
        auto chunks = manifest.initChunks(grid * grid);
        for (std::uint32_t i = 0; i < grid * grid; ++i) {
            const auto& [name, bytes] = files[i];
            auto chunk = chunks[i];
            chunk.setX(i % grid);
            chunk.setY(i / grid);
            chunk.setFile(name);
            chunk.setByteSize(bytes.size());
            chunk.setCrc32(Crc32(bytes.data(), bytes.size()));
        }
    }
    if (spec.patch_manifest) {
        spec.patch_manifest(manifest);
    }
    const auto words = capnp::messageToFlatArray(message);
    const auto manifest_bytes = words.asBytes();
    files.emplace_back("map.manifest", std::vector<std::uint8_t>(manifest_bytes.begin(), manifest_bytes.end()));

    for (const auto& [name, bytes] : files) {
        if (!WriteFile(out_dir / name, bytes, result.error)) {
            return result;
        }
        result.files.push_back(name);
    }
    result.ok = true;
    return result;
}

} // namespace mx::map
