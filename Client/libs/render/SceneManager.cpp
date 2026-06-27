#include "SceneManager.h"

#include "Common.h"
#include "Debug.h"
#include "ProjectManager.h"
#include "map/MapData.h"
#include "schema/map_manifest.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#endif

namespace
{
using ixtreeme::common::EscapeJson;
using ixtreeme::common::GenericPath;
using ixtreeme::common::TimestampUtc;

struct JsonValue
{
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    const JsonValue* Find(const std::string& key) const
    {
        auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }

    std::string StringOr(const std::string& fallback = {}) const
    {
        return type == Type::String ? string : fallback;
    }

    double NumberOr(double fallback = 0.0) const
    {
        return type == Type::Number ? number : fallback;
    }

    bool BoolOr(bool fallback = false) const
    {
        return type == Type::Bool ? boolean : fallback;
    }
};

class JsonParser
{
public:
    explicit JsonParser(std::string text) : m_text(std::move(text)) {}

    bool Parse(JsonValue& out)
    {
        SkipWs();
        if (!ParseValue(out))
            return false;
        SkipWs();
        return m_pos == m_text.size();
    }

private:
    void SkipWs()
    {
        while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
            ++m_pos;
    }

    bool Match(char ch)
    {
        SkipWs();
        if (m_pos >= m_text.size() || m_text[m_pos] != ch)
            return false;
        ++m_pos;
        return true;
    }

    bool ParseValue(JsonValue& out)
    {
        SkipWs();
        if (m_pos >= m_text.size())
            return false;

        const char ch = m_text[m_pos];
        if (ch == '{')
            return ParseObject(out);
        if (ch == '[')
            return ParseArray(out);
        if (ch == '"')
        {
            out.type = JsonValue::Type::String;
            return ParseString(out.string);
        }
        if (ch == 't' && m_text.substr(m_pos, 4) == "true")
        {
            m_pos += 4;
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (ch == 'f' && m_text.substr(m_pos, 5) == "false")
        {
            m_pos += 5;
            out.type = JsonValue::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (ch == 'n' && m_text.substr(m_pos, 4) == "null")
        {
            m_pos += 4;
            out.type = JsonValue::Type::Null;
            return true;
        }
        return ParseNumber(out);
    }

    bool ParseObject(JsonValue& out)
    {
        if (!Match('{'))
            return false;
        out.type = JsonValue::Type::Object;
        SkipWs();
        if (Match('}'))
            return true;
        while (true)
        {
            std::string key;
            if (!ParseString(key) || !Match(':'))
                return false;
            JsonValue value;
            if (!ParseValue(value))
                return false;
            out.object[std::move(key)] = std::move(value);
            if (Match('}'))
                return true;
            if (!Match(','))
                return false;
        }
    }

    bool ParseArray(JsonValue& out)
    {
        if (!Match('['))
            return false;
        out.type = JsonValue::Type::Array;
        SkipWs();
        if (Match(']'))
            return true;
        while (true)
        {
            JsonValue value;
            if (!ParseValue(value))
                return false;
            out.array.push_back(std::move(value));
            if (Match(']'))
                return true;
            if (!Match(','))
                return false;
        }
    }

    bool ParseString(std::string& out)
    {
        SkipWs();
        if (m_pos >= m_text.size() || m_text[m_pos] != '"')
            return false;
        ++m_pos;
        out.clear();
        while (m_pos < m_text.size())
        {
            const char ch = m_text[m_pos++];
            if (ch == '"')
                return true;
            if (ch != '\\')
            {
                out.push_back(ch);
                continue;
            }
            if (m_pos >= m_text.size())
                return false;
            const char escaped = m_text[m_pos++];
            switch (escaped)
            {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(escaped); break;
            }
        }
        return false;
    }

    bool ParseNumber(JsonValue& out)
    {
        SkipWs();
        const size_t start = m_pos;
        if (m_pos < m_text.size() && (m_text[m_pos] == '-' || m_text[m_pos] == '+'))
            ++m_pos;
        while (m_pos < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_pos])))
            ++m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '.')
        {
            ++m_pos;
            while (m_pos < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_pos])))
                ++m_pos;
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E'))
        {
            ++m_pos;
            if (m_pos < m_text.size() && (m_text[m_pos] == '-' || m_text[m_pos] == '+'))
                ++m_pos;
            while (m_pos < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_pos])))
                ++m_pos;
        }
        if (start == m_pos)
            return false;
        out.type = JsonValue::Type::Number;
        out.number = std::strtod(m_text.c_str() + start, nullptr);
        return true;
    }

    std::string m_text;
    size_t m_pos = 0;
};

std::string SceneNameFromPath(const std::string& path)
{
    if (path.empty())
        return "Untitled";
    return std::filesystem::path(path).stem().string();
}

std::filesystem::path SceneSidecarPath(const std::filesystem::path& scenePath, const char* extension)
{
    std::filesystem::path sidecar = scenePath;
    sidecar.replace_extension(extension);
    return sidecar;
}

std::string ResolveProjectScenePath(const std::string& path)
{
    const std::filesystem::path input(path);
    if (!ProjectManager::Instance().HasProject() || input.is_absolute())
        return path;
    return (ProjectManager::Instance().ProjectRoot() / input).string();
}

std::string ProjectSceneRecentPath(const std::string& path)
{
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return path;

    std::error_code ec;
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
    if (ec)
        return path;
    const std::filesystem::path relative = std::filesystem::relative(absolutePath, projects.ProjectRoot(), ec);
    if (!ec && !relative.empty() && relative.string().rfind("..", 0) != 0)
        return relative.generic_string();
    return path;
}

std::string DefaultProjectScenePath(const SceneData& scene)
{
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return {};

    std::string name = scene.name.empty() ? "Untitled" : scene.name;
    for (char& ch : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-')
            ch = '_';
    }
    if (name.empty())
        name = "Untitled";
    return (projects.ScenesPath() / name / (name + ".scene")).string();
}

void WritePlaceholderBinary(const std::filesystem::path& path, const char* magic)
{
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    if (!file)
        return;
    file.write(magic, static_cast<std::streamsize>(std::strlen(magic)));
    const std::uint32_t version = 1;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    if (!bytes.empty())
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return true;
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void PushU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void PushU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
}

void PushI16(std::vector<std::uint8_t>& bytes, std::int16_t value)
{
    PushU16(bytes, static_cast<std::uint16_t>(value));
}

std::vector<std::uint8_t> BuildMxChunkBytes(const TerrainSceneData& terrain,
                                            std::uint32_t chunkX,
                                            std::uint32_t chunkY,
                                            std::uint32_t worldCells,
                                            std::uint32_t chunkSize)
{
    struct Section
    {
        std::uint16_t type = 0;
        std::vector<std::uint8_t> bytes;
    };

    const std::uint32_t chunkVertices = chunkSize + 1u;
    std::vector<Section> sections;
    Section height;
    height.type = 1;
    height.bytes.reserve(static_cast<size_t>(chunkVertices) * chunkVertices * sizeof(std::int16_t));
    for (std::uint32_t y = 0; y < chunkVertices; ++y)
    {
        for (std::uint32_t x = 0; x < chunkVertices; ++x)
        {
            const std::uint32_t gx = chunkX * chunkSize + x;
            const std::uint32_t gy = chunkY * chunkSize + y;
            float h = 0.0f;
            if (gx <= terrain.cellsX && gy <= terrain.cellsZ &&
                !terrain.heightCmGrid.empty())
            {
                const size_t src = static_cast<size_t>(gy) * (terrain.cellsX + 1u) + gx;
                if (src < terrain.heightCmGrid.size())
                    h = terrain.heightCmGrid[src];
            }
            PushI16(height.bytes, static_cast<std::int16_t>(std::lround(std::clamp(h, -32768.0f, 32767.0f))));
        }
    }
    sections.push_back(std::move(height));

    auto buildSplatSection = [&](std::uint16_t type, const std::vector<std::uint8_t>& source) {
        Section splat;
        splat.type = type;
        PushU16(splat.bytes, static_cast<std::uint16_t>(chunkSize));
        PushU16(splat.bytes, static_cast<std::uint16_t>(chunkSize));
        splat.bytes.resize(4u + static_cast<size_t>(chunkSize) * chunkSize * 4u, 0);
        for (std::uint32_t y = 0; y < chunkSize; ++y)
        {
            for (std::uint32_t x = 0; x < chunkSize; ++x)
            {
                const std::uint32_t sx = chunkX * chunkSize + x;
                const std::uint32_t sy = chunkY * chunkSize + y;
                const size_t dst = 4u + (static_cast<size_t>(y) * chunkSize + x) * 4u;
                if (sx < terrain.cellsX && sy < terrain.cellsZ && !source.empty())
                {
                    const size_t src = (static_cast<size_t>(sy) * terrain.cellsX + sx) * 4u;
                    if (src + 4u <= source.size())
                    {
                        std::memcpy(splat.bytes.data() + dst, source.data() + src, 4u);
                        continue;
                    }
                }
                if (type == 2)
                    splat.bytes[dst + 0] = 255;
            }
        }
        sections.push_back(std::move(splat));
    };
    buildSplatSection(2, terrain.splatABytes);

    Section attributes;
    attributes.type = 3;
    attributes.bytes.assign(static_cast<size_t>(chunkSize) * chunkSize * sizeof(std::uint16_t), 0);
    sections.push_back(std::move(attributes));
    buildSplatSection(4, terrain.splatBBytes);

    constexpr std::size_t kHeaderSize = 14;
    constexpr std::size_t kTocEntrySize = 12;
    std::vector<std::uint8_t> bytes;
    PushU32(bytes, 0x3143584d); // MXC1
    PushU16(bytes, 2);
    PushU16(bytes, static_cast<std::uint16_t>(chunkX));
    PushU16(bytes, static_cast<std::uint16_t>(chunkY));
    PushU16(bytes, static_cast<std::uint16_t>(chunkSize));
    PushU16(bytes, static_cast<std::uint16_t>(sections.size()));

    std::uint32_t offset = static_cast<std::uint32_t>(kHeaderSize + sections.size() * kTocEntrySize);
    for (const Section& section : sections)
    {
        PushU16(bytes, section.type);
        PushU16(bytes, 0);
        PushU32(bytes, offset);
        PushU32(bytes, static_cast<std::uint32_t>(section.bytes.size()));
        offset += static_cast<std::uint32_t>(section.bytes.size());
    }
    for (const Section& section : sections)
        bytes.insert(bytes.end(), section.bytes.begin(), section.bytes.end());
    (void)worldCells;
    return bytes;
}

std::vector<std::uint8_t> BuildTerrainManifestBytes(const TerrainSceneData& terrain,
                                                    const std::array<MapEditorPaletteSlot, 8>& paletteSlots,
                                                    std::uint32_t worldCells,
                                                    std::uint32_t chunkSize,
                                                    std::uint32_t chunksX,
                                                    std::uint32_t chunksY)
{
    capnp::MallocMessageBuilder builder;
    auto manifest = builder.initRoot<mx::map::schema::MapManifest>();
    manifest.setFormatVersion(2);
    manifest.setWorldId("scene-terrain");
    manifest.setWorldName(terrain.name.empty() ? "Terrain" : terrain.name);
    manifest.setWorldSizeCells(worldCells);
    manifest.setCellSizeMeters(terrain.cellSizeMeters);
    manifest.setHeightUnit(mx::map::schema::HeightUnit::CENTIMETERS);
    manifest.setChunkSizeCells(chunkSize);
    manifest.setTriplanarSlopeThreshold(std::clamp(terrain.triplanarSlopeThreshold, 0.0f, 1.0f));
    manifest.setTriplanarSlopeTransition(std::clamp(terrain.triplanarSlopeTransition, 0.001f, 1.0f));
    auto grid = manifest.initZoneGridDims();
    grid.setX(chunksX);
    grid.setY(chunksY);
    manifest.setZoneSizeCells(chunkSize);
    auto palette = manifest.initTexturePalette(8);
    for (std::uint16_t i = 0; i < 8; ++i)
    {
        palette[i].setId(i);
        const MapEditorPaletteSlot& slot = paletteSlots[i];
        palette[i].setPath(slot.texturePath);
        palette[i].setTilingX(std::clamp(slot.tilingScaleX, 0.01f, 64.0f));
        palette[i].setTilingY(std::clamp(slot.tilingScaleY, 0.01f, 64.0f));
        palette[i].setNormalStrength(std::clamp(slot.normalStrength, 0.0f, 4.0f));
        palette[i].setRoughnessStrength(std::clamp(slot.roughnessStrength, 0.0f, 4.0f));
        palette[i].setTintR(std::clamp(slot.colorTint[0], 0.0f, 8.0f));
        palette[i].setTintG(std::clamp(slot.colorTint[1], 0.0f, 8.0f));
        palette[i].setTintB(std::clamp(slot.colorTint[2], 0.0f, 8.0f));
        palette[i].setMetallicStrength(std::clamp(slot.metallicStrength, 0.0f, 1.0f));
        palette[i].setAoStrength(std::clamp(slot.aoStrength, 0.0f, 1.0f));
        palette[i].setUvOffsetX(slot.uvOffset[0]);
        palette[i].setUvOffsetY(slot.uvOffset[1]);
        palette[i].setUvRotationDegrees(slot.uvRotationDegrees);
    }
    kj::Array<capnp::word> words = capnp::messageToFlatArray(builder);
    const auto bytes = words.asBytes();
    return {bytes.begin(), bytes.end()};
}

bool WriteTerrainChunkSet(const std::filesystem::path& scenePath,
                          TerrainSceneData& terrain,
                          const std::array<MapEditorPaletteSlot, 8>& paletteSlots)
{
    terrain.chunkSizeCells = std::clamp(terrain.chunkSizeCells == 0 ? 64u : terrain.chunkSizeCells, 32u, 256u);
    const std::uint32_t worldCells = std::max(terrain.cellsX, terrain.cellsZ);
    const std::uint32_t chunkSize = terrain.chunkSizeCells;
    const std::uint32_t chunksX = (worldCells + chunkSize - 1u) / chunkSize;
    const std::uint32_t chunksY = chunksX;
    const std::filesystem::path mapDir = scenePath.stem().string() + "_terrain_map";
    const std::filesystem::path absoluteMapDir = scenePath.parent_path() / mapDir;
    std::filesystem::create_directories(absoluteMapDir / "chunks");

    const std::vector<std::uint8_t> manifest = BuildTerrainManifestBytes(terrain, paletteSlots, worldCells, chunkSize, chunksX, chunksY);
    if (!WriteBytes(absoluteMapDir / "map.manifest", manifest))
        return false;

    std::uint32_t written = 0;
    for (std::uint32_t cy = 0; cy < chunksY; ++cy)
    {
        for (std::uint32_t cx = 0; cx < chunksX; ++cx)
        {
            const std::filesystem::path chunkPath = absoluteMapDir / "chunks" /
                ("chunk_" + std::to_string(cx) + "_" + std::to_string(cy) + ".mxchunk");
            if (!WriteBytes(chunkPath, BuildMxChunkBytes(terrain, cx, cy, worldCells, chunkSize)))
                return false;
            ++written;
        }
    }

    terrain.chunkManifestRef = GenericPath(mapDir / "map.manifest");
    terrain.heightmapRef.clear();
    terrain.splatRef.clear();
    terrain.maskRef.clear();
    Tracenf("[TCHUNK] save chunks=%u manifest=%s",
        written,
        terrain.chunkManifestRef.c_str());
    Tracen("[TMAT] saved per-layer params: layers=8");
    return true;
}

bool ReadTerrainChunkSet(const std::filesystem::path& sceneDir,
                         TerrainSceneData& terrain,
                         std::array<MapEditorPaletteSlot, 8>* outPaletteSlots)
{
    if (terrain.chunkManifestRef.empty())
        return false;
    const std::uint32_t sceneCellsX = terrain.cellsX;
    const std::uint32_t sceneCellsZ = terrain.cellsZ;
    const std::filesystem::path manifestPath = sceneDir / terrain.chunkManifestRef;
    const std::filesystem::path mapRoot = manifestPath.parent_path();
    const auto read = [&](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
        std::filesystem::path p(path);
        if (!p.is_absolute())
            p = sceneDir / p;
        std::vector<std::uint8_t> bytes = ReadBytes(p);
        if (bytes.empty())
            return std::nullopt;
        return bytes;
    };
    const std::filesystem::path relativeRoot = std::filesystem::relative(mapRoot, sceneDir);
    const std::string mapRootString = relativeRoot.generic_string();
    auto field = mx::map::LoadHeightField(read, mapRootString);
    if (!field)
        return false;
    terrain.exists = true;
    terrain.cellsX = sceneCellsX == 0 ? field->manifest.world_size_cells : std::min(sceneCellsX, field->manifest.world_size_cells);
    terrain.cellsZ = sceneCellsZ == 0 ? field->manifest.world_size_cells : std::min(sceneCellsZ, field->manifest.world_size_cells);
    terrain.cellSizeMeters = field->manifest.cell_size_meters;
    terrain.widthMeters = static_cast<float>(terrain.cellsX) * terrain.cellSizeMeters;
    terrain.depthMeters = static_cast<float>(terrain.cellsZ) * terrain.cellSizeMeters;
    terrain.chunkSizeCells = field->manifest.chunk_size_cells;
    terrain.triplanarSlopeThreshold = std::clamp(field->manifest.triplanar_slope_threshold, 0.0f, 1.0f);
    terrain.triplanarSlopeTransition = std::clamp(field->manifest.triplanar_slope_transition, 0.001f, 1.0f);
    terrain.heightCmGrid.clear();
    terrain.heightCmGrid.reserve(static_cast<size_t>(terrain.cellsX + 1u) * (terrain.cellsZ + 1u));
    for (std::uint32_t y = 0; y <= terrain.cellsZ; ++y)
    {
        for (std::uint32_t x = 0; x <= terrain.cellsX; ++x)
        {
            const size_t src = static_cast<size_t>(y) * field->width_vertices + x;
            terrain.heightCmGrid.push_back(src < field->heights_cm.size() ? static_cast<float>(field->heights_cm[src]) : 0.0f);
        }
    }
    const size_t splatBytes = static_cast<size_t>(terrain.cellsX) * terrain.cellsZ * 4u;
    terrain.splatABytes.assign(splatBytes, 0);
    terrain.splatBBytes.assign(splatBytes, 0);
    for (std::uint32_t y = 0; y < terrain.cellsZ; ++y)
    {
        for (std::uint32_t x = 0; x < terrain.cellsX; ++x)
        {
            const size_t dst = (static_cast<size_t>(y) * terrain.cellsX + x) * 4u;
            const size_t src = (static_cast<size_t>(y) * field->splat_width + x) * 4u;
            if (src + 4u <= field->splat_a_rgba8.size())
                std::memcpy(terrain.splatABytes.data() + dst, field->splat_a_rgba8.data() + src, 4u);
            else
                terrain.splatABytes[dst] = 255;
            if (src + 4u <= field->splat_b_rgba8.size())
                std::memcpy(terrain.splatBBytes.data() + dst, field->splat_b_rgba8.data() + src, 4u);
        }
    }
    Tracenf("[TCHUNK] load chunks=%u from manifest=%s",
        field->manifest.zone_grid_x * field->manifest.zone_grid_y,
        terrain.chunkManifestRef.c_str());
    if (outPaletteSlots)
    {
        for (std::uint32_t i = 0; i < outPaletteSlots->size(); ++i)
        {
            MapEditorPaletteSlot& slot = (*outPaletteSlots)[i];
            slot.slot = i;
            if (i < field->manifest.texture_palette_paths.size())
                slot.texturePath = field->manifest.texture_palette_paths[i];
            if (i < field->manifest.texture_palette_tiling_x.size())
                slot.tilingScaleX = field->manifest.texture_palette_tiling_x[i];
            if (i < field->manifest.texture_palette_tiling_y.size())
                slot.tilingScaleY = field->manifest.texture_palette_tiling_y[i];
            if (i < field->manifest.texture_palette_normal_strength.size())
                slot.normalStrength = field->manifest.texture_palette_normal_strength[i];
            if (i < field->manifest.texture_palette_roughness_strength.size())
                slot.roughnessStrength = field->manifest.texture_palette_roughness_strength[i];
            if (i < field->manifest.texture_palette_tint_r.size())
                slot.colorTint[0] = field->manifest.texture_palette_tint_r[i];
            if (i < field->manifest.texture_palette_tint_g.size())
                slot.colorTint[1] = field->manifest.texture_palette_tint_g[i];
            if (i < field->manifest.texture_palette_tint_b.size())
                slot.colorTint[2] = field->manifest.texture_palette_tint_b[i];
            if (i < field->manifest.texture_palette_metallic_strength.size())
                slot.metallicStrength = field->manifest.texture_palette_metallic_strength[i];
            if (i < field->manifest.texture_palette_ao_strength.size())
                slot.aoStrength = field->manifest.texture_palette_ao_strength[i];
            if (i < field->manifest.texture_palette_uv_offset_x.size())
                slot.uvOffset[0] = field->manifest.texture_palette_uv_offset_x[i];
            if (i < field->manifest.texture_palette_uv_offset_y.size())
                slot.uvOffset[1] = field->manifest.texture_palette_uv_offset_y[i];
            if (i < field->manifest.texture_palette_uv_rotation_degrees.size())
                slot.uvRotationDegrees = field->manifest.texture_palette_uv_rotation_degrees[i];
        }
        Tracen("[TMAT] loaded per-layer params");
    }
    return true;
}

bool WriteTerrainHeightmap(const std::filesystem::path& path, const TerrainSceneData& terrain)
{
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    const char magic[12] = {'I','W','T','E','R','R','H','G','R','I','D','1'};
    const std::uint32_t version = 1;
    file.write(magic, sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&terrain.cellsX), sizeof(terrain.cellsX));
    file.write(reinterpret_cast<const char*>(&terrain.cellsZ), sizeof(terrain.cellsZ));
    file.write(reinterpret_cast<const char*>(&terrain.cellSizeMeters), sizeof(terrain.cellSizeMeters));
    const std::uint64_t count = static_cast<std::uint64_t>(terrain.heightCmGrid.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    if (!terrain.heightCmGrid.empty())
        file.write(reinterpret_cast<const char*>(terrain.heightCmGrid.data()),
            static_cast<std::streamsize>(terrain.heightCmGrid.size() * sizeof(float)));
    return true;
}

bool ReadTerrainHeightmap(const std::filesystem::path& path, TerrainSceneData& terrain)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    char magic[12]{};
    std::uint32_t version = 0;
    file.read(magic, sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!file || std::memcmp(magic, "IWTERRHGRID1", sizeof(magic)) != 0 || version != 1)
        return false;
    std::uint32_t cellsX = 0;
    std::uint32_t cellsZ = 0;
    float cellSize = 1.0f;
    std::uint64_t count = 0;
    file.read(reinterpret_cast<char*>(&cellsX), sizeof(cellsX));
    file.read(reinterpret_cast<char*>(&cellsZ), sizeof(cellsZ));
    file.read(reinterpret_cast<char*>(&cellSize), sizeof(cellSize));
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!file || count > 100000000ull)
        return false;
    std::vector<float> heights(static_cast<size_t>(count));
    if (!heights.empty())
        file.read(reinterpret_cast<char*>(heights.data()), static_cast<std::streamsize>(heights.size() * sizeof(float)));
    if (!file)
        return false;
    terrain.cellsX = cellsX;
    terrain.cellsZ = cellsZ;
    terrain.cellSizeMeters = cellSize;
    terrain.widthMeters = static_cast<float>(cellsX) * cellSize;
    terrain.depthMeters = static_cast<float>(cellsZ) * cellSize;
    terrain.heightCmGrid = std::move(heights);
    return true;
}

bool WriteTerrainSplat(const std::filesystem::path& path, const TerrainSceneData& terrain)
{
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    const char magic[12] = {'I','W','T','E','R','R','S','P','L','A','T','1'};
    const std::uint32_t version = 1;
    const std::uint64_t aSize = static_cast<std::uint64_t>(terrain.splatABytes.size());
    const std::uint64_t bSize = static_cast<std::uint64_t>(terrain.splatBBytes.size());
    file.write(magic, sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&terrain.cellsX), sizeof(terrain.cellsX));
    file.write(reinterpret_cast<const char*>(&terrain.cellsZ), sizeof(terrain.cellsZ));
    file.write(reinterpret_cast<const char*>(&aSize), sizeof(aSize));
    if (!terrain.splatABytes.empty())
        file.write(reinterpret_cast<const char*>(terrain.splatABytes.data()), static_cast<std::streamsize>(terrain.splatABytes.size()));
    file.write(reinterpret_cast<const char*>(&bSize), sizeof(bSize));
    if (!terrain.splatBBytes.empty())
        file.write(reinterpret_cast<const char*>(terrain.splatBBytes.data()), static_cast<std::streamsize>(terrain.splatBBytes.size()));
    return true;
}

bool ReadTerrainSplat(const std::filesystem::path& path, TerrainSceneData& terrain)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    char magic[12]{};
    std::uint32_t version = 0;
    std::uint32_t cellsX = 0;
    std::uint32_t cellsZ = 0;
    std::uint64_t aSize = 0;
    std::uint64_t bSize = 0;
    file.read(magic, sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!file || std::memcmp(magic, "IWTERRSPLAT1", sizeof(magic)) != 0 || version != 1)
        return false;
    file.read(reinterpret_cast<char*>(&cellsX), sizeof(cellsX));
    file.read(reinterpret_cast<char*>(&cellsZ), sizeof(cellsZ));
    file.read(reinterpret_cast<char*>(&aSize), sizeof(aSize));
    if (!file || aSize > 512000000ull)
        return false;
    terrain.splatABytes.resize(static_cast<size_t>(aSize));
    if (!terrain.splatABytes.empty())
        file.read(reinterpret_cast<char*>(terrain.splatABytes.data()), static_cast<std::streamsize>(terrain.splatABytes.size()));
    file.read(reinterpret_cast<char*>(&bSize), sizeof(bSize));
    if (!file || bSize > 512000000ull)
        return false;
    terrain.splatBBytes.resize(static_cast<size_t>(bSize));
    if (!terrain.splatBBytes.empty())
        file.read(reinterpret_cast<char*>(terrain.splatBBytes.data()), static_cast<std::streamsize>(terrain.splatBBytes.size()));
    terrain.cellsX = terrain.cellsX == 0 ? cellsX : terrain.cellsX;
    terrain.cellsZ = terrain.cellsZ == 0 ? cellsZ : terrain.cellsZ;
    return static_cast<bool>(file);
}

const JsonValue* Find(const JsonValue& object, const char* key)
{
    return object.type == JsonValue::Type::Object ? object.Find(key) : nullptr;
}

float ReadFloat(const JsonValue& object, const char* key, float fallback)
{
    if (const JsonValue* value = Find(object, key))
        return static_cast<float>(value->NumberOr(fallback));
    return fallback;
}

std::uint32_t ReadU32(const JsonValue& object, const char* key, std::uint32_t fallback)
{
    if (const JsonValue* value = Find(object, key))
        return static_cast<std::uint32_t>(std::max(0.0, value->NumberOr(fallback)));
    return fallback;
}

bool ReadBool(const JsonValue& object, const char* key, bool fallback)
{
    if (const JsonValue* value = Find(object, key))
        return value->BoolOr(fallback);
    return fallback;
}

std::string ReadString(const JsonValue& object, const char* key, const std::string& fallback = {})
{
    if (const JsonValue* value = Find(object, key))
        return value->StringOr(fallback);
    return fallback;
}

void ReadFloatArray(const JsonValue& object, const char* key, float* values, size_t count)
{
    const JsonValue* array = Find(object, key);
    if (!array || array->type != JsonValue::Type::Array)
        return;
    for (size_t i = 0; i < count && i < array->array.size(); ++i)
        values[i] = static_cast<float>(array->array[i].NumberOr(values[i]));
}

void ReadBoolArray(const JsonValue& object, const char* key, bool* values, size_t count)
{
    const JsonValue* array = Find(object, key);
    if (!array || array->type != JsonValue::Type::Array)
        return;
    for (size_t i = 0; i < count && i < array->array.size(); ++i)
        values[i] = array->array[i].BoolOr(values[i]);
}

std::string FloatArray(const float* values, size_t count)
{
    std::ostringstream out;
    out << '[';
    for (size_t i = 0; i < count; ++i)
    {
        if (i > 0)
            out << ", ";
        out << values[i];
    }
    out << ']';
    return out.str();
}

std::string BoolArray(const bool* values, size_t count)
{
    std::ostringstream out;
    out << '[';
    for (size_t i = 0; i < count; ++i)
    {
        if (i > 0)
            out << ", ";
        out << (values[i] ? "true" : "false");
    }
    out << ']';
    return out.str();
}

void WriteWaterConfig(std::ostream& out, const WaterConfig& config, int indent)
{
    const std::string pad(static_cast<size_t>(indent), ' ');
    out << pad << "\"water_config\": {\n";
    out << pad << "  \"enabled\": " << (config.enabled ? "true" : "false") << ",\n";
    out << pad << "  \"water_level_y\": " << config.waterLevelY << ",\n";
    out << pad << "  \"base_color\": " << FloatArray(config.baseColor, 4) << ",\n";
    out << pad << "  \"deep_color\": " << FloatArray(config.deepColor, 3) << ",\n";
    out << pad << "  \"shallow_color\": " << FloatArray(config.shallowColor, 3) << ",\n";
    out << pad << "  \"foam_intensity\": " << config.foamIntensity << ",\n";
    out << pad << "  \"edge_fade_distance\": " << config.edgeFadeDistance << ",\n";
    out << pad << "  \"edge_fade_curve\": " << static_cast<int>(config.edgeFadeCurve) << "\n";
    out << pad << '}';
}

void ReadWaterConfig(const JsonValue& object, WaterConfig& config)
{
    const JsonValue* water = Find(object, "water_config");
    if (!water || water->type != JsonValue::Type::Object)
        return;
    config.enabled = ReadBool(*water, "enabled", config.enabled);
    config.waterLevelY = ReadFloat(*water, "water_level_y", config.waterLevelY);
    ReadFloatArray(*water, "base_color", config.baseColor, 4);
    ReadFloatArray(*water, "deep_color", config.deepColor, 3);
    ReadFloatArray(*water, "shallow_color", config.shallowColor, 3);
    config.foamIntensity = ReadFloat(*water, "foam_intensity", config.foamIntensity);
    config.edgeFadeDistance = ReadFloat(*water, "edge_fade_distance", config.edgeFadeDistance);
    config.edgeFadeCurve = static_cast<WaterConfig::EdgeFadeCurve>(
        std::clamp<int>(static_cast<int>(ReadFloat(*water, "edge_fade_curve", static_cast<float>(config.edgeFadeCurve))), 0, 2));
}

void WritePaletteSlot(std::ostream& out, const MapEditorPaletteSlot& slot, bool comma)
{
    out << "    {\n";
    out << "      \"slot\": " << slot.slot << ",\n";
    out << "      \"asset_id\": \"" << EscapeJson(slot.assetId) << "\",\n";
    out << "      \"display_name\": \"" << EscapeJson(slot.displayName) << "\",\n";
    out << "      \"texture_path\": \"" << EscapeJson(slot.texturePath) << "\",\n";
    out << "      \"normal_texture_path\": \"" << EscapeJson(slot.normalTexturePath) << "\",\n";
    out << "      \"ao_texture_path\": \"" << EscapeJson(slot.aoTexturePath) << "\",\n";
    out << "      \"roughness_texture_path\": \"" << EscapeJson(slot.roughnessTexturePath) << "\",\n";
    out << "      \"metallic_texture_path\": \"" << EscapeJson(slot.metallicTexturePath) << "\",\n";
    out << "      \"height_texture_path\": \"" << EscapeJson(slot.heightTexturePath) << "\",\n";
    out << "      \"tiling\": [" << slot.tilingScaleX << ", " << slot.tilingScaleY << "],\n";
    out << "      \"tint\": [" << slot.colorTint[0] << ", " << slot.colorTint[1] << ", " << slot.colorTint[2] << "],\n";
    out << "      \"normal_strength\": " << slot.normalStrength << ",\n";
    out << "      \"roughness_strength\": " << slot.roughnessStrength << ",\n";
    out << "      \"ao_strength\": " << slot.aoStrength << ",\n";
    out << "      \"metallic_strength\": " << slot.metallicStrength << ",\n";
    out << "      \"uv_offset\": [" << slot.uvOffset[0] << ", " << slot.uvOffset[1] << "],\n";
    out << "      \"uv_rotation_degrees\": " << slot.uvRotationDegrees << "\n";
    out << "    }" << (comma ? "," : "") << "\n";
}

MapEditorPaletteSlot ReadPaletteSlot(const JsonValue& object)
{
    MapEditorPaletteSlot slot;
    slot.slot = ReadU32(object, "slot", slot.slot);
    slot.assetId = ReadString(object, "asset_id");
    slot.displayName = ReadString(object, "display_name");
    slot.texturePath = ReadString(object, "texture_path");
    slot.normalTexturePath = ReadString(object, "normal_texture_path");
    slot.aoTexturePath = ReadString(object, "ao_texture_path");
    slot.roughnessTexturePath = ReadString(object, "roughness_texture_path");
    slot.metallicTexturePath = ReadString(object, "metallic_texture_path");
    slot.heightTexturePath = ReadString(object, "height_texture_path");
    ReadFloatArray(object, "tiling", &slot.tilingScaleX, 2);
    ReadFloatArray(object, "tint", slot.colorTint, 3);
    slot.normalStrength = ReadFloat(object, "normal_strength", slot.normalStrength);
    slot.roughnessStrength = ReadFloat(object, "roughness_strength", slot.roughnessStrength);
    slot.aoStrength = ReadFloat(object, "ao_strength", slot.aoStrength);
    slot.metallicStrength = ReadFloat(object, "metallic_strength", 0.0f);
    ReadFloatArray(object, "uv_offset", slot.uvOffset, 2);
    slot.uvRotationDegrees = ReadFloat(object, "uv_rotation_degrees", slot.uvRotationDegrees);
    return slot;
}

void WriteSceneEntity(std::ostream& out,
                      const WaterBody& body,
                      const std::string& maskRef,
                      bool comma)
{
    out << "    {\n";
    out << "      \"type\": \"water_body\",\n";
    out << "      \"id\": " << body.id << ",\n";
    out << "      \"name\": \"" << EscapeJson(body.name) << "\",\n";
    out << "      \"bbox_min\": " << FloatArray(body.bboxMin, 2) << ",\n";
    out << "      \"bbox_max\": " << FloatArray(body.bboxMax, 2) << ",\n";
    out << "      \"water_level_y\": " << body.waterLevelY << ",\n";
    out << "      \"material_id\": \"" << EscapeJson(body.materialId) << "\",\n";
    out << "      \"mask_width\": " << body.maskWidth << ",\n";
    out << "      \"mask_height\": " << body.maskHeight << ",\n";
    out << "      \"shape_mask_ref\": \"" << EscapeJson(maskRef) << "\",\n";
    WriteWaterConfig(out, body.config, 6);
    out << "\n    }" << (comma ? "," : "") << "\n";
}

std::string PrefabAssetIdForSave(const std::string& legacyAssetId, const PrefabInstanceState& prefab)
{
    return !prefab.assetId.empty() ? prefab.assetId : legacyAssetId;
}

void WritePrefabInstance(std::ostream& out,
                         const std::string& legacyAssetId,
                         const PrefabInstanceState& prefab,
                         const std::string& indent)
{
    const std::string assetId = PrefabAssetIdForSave(legacyAssetId, prefab);
    if (assetId.empty())
        return;

    out << indent << "\"prefab_asset_id\": \"" << EscapeJson(assetId) << "\",\n";
    out << indent << "\"prefab_instance\": {\n";
    out << indent << "  \"linked\": " << (prefab.linked || !assetId.empty() ? "true" : "false") << ",\n";
    out << indent << "  \"asset_id\": \"" << EscapeJson(assetId) << "\",\n";
    out << indent << "  \"local_id\": " << (prefab.localId == 0 ? 1u : prefab.localId) << ",\n";
    out << indent << "  \"preserve_transform\": " << (prefab.preserveTransform ? "true" : "false") << ",\n";
    out << indent << "  \"name_override\": " << (prefab.nameOverride ? "true" : "false") << "\n";
    out << indent << "},\n";
}

void WriteParentRef(std::ostream& out, const SceneParentRef& parent, const std::string& indent)
{
    if (!parent.IsValid())
        return;
    out << indent << "\"parent\": {\n";
    out << indent << "  \"type\": \"" << EscapeJson(parent.type) << "\",\n";
    out << indent << "  \"id\": " << parent.id << "\n";
    out << indent << "},\n";
}

void WriteSceneEntity(std::ostream& out, const PointLight& light, bool comma)
{
    out << "    {\n";
    out << "      \"type\": \"dynamic_light\",\n";
    out << "      \"id\": " << light.id << ",\n";
    out << "      \"name\": \"" << EscapeJson(light.name) << "\",\n";
    WritePrefabInstance(out, light.prefabAssetId, light.prefabInstance, "      ");
    WriteParentRef(out, light.parent, "      ");
    out << "      \"light_type\": \"point\",\n";
    out << "      \"position\": " << FloatArray(light.position, 3) << ",\n";
    const float color[3] = {light.r, light.g, light.b};
    out << "      \"color\": " << FloatArray(color, 3) << ",\n";
    out << "      \"intensity\": " << light.intensity << ",\n";
    out << "      \"radius\": " << light.radius << ",\n";
    out << "      \"enabled\": " << (light.enabled ? "true" : "false") << "\n";
    out << "    }" << (comma ? "," : "") << "\n";
}

void WriteSceneEntity(std::ostream& out, const SpotLight& light, bool comma)
{
    out << "    {\n";
    out << "      \"type\": \"dynamic_light\",\n";
    out << "      \"id\": " << light.id << ",\n";
    out << "      \"name\": \"" << EscapeJson(light.name) << "\",\n";
    WritePrefabInstance(out, light.prefabAssetId, light.prefabInstance, "      ");
    WriteParentRef(out, light.parent, "      ");
    out << "      \"light_type\": \"spot\",\n";
    out << "      \"position\": " << FloatArray(light.position, 3) << ",\n";
    out << "      \"rotation\": " << FloatArray(light.rotation, 3) << ",\n";
    const float color[3] = {light.r, light.g, light.b};
    out << "      \"color\": " << FloatArray(color, 3) << ",\n";
    out << "      \"intensity\": " << light.intensity << ",\n";
    out << "      \"radius\": " << light.radius << ",\n";
    out << "      \"inner_cone_deg\": " << light.innerConeDegrees << ",\n";
    out << "      \"outer_cone_deg\": " << light.outerConeDegrees << ",\n";
    out << "      \"enabled\": " << (light.enabled ? "true" : "false") << "\n";
    out << "    }" << (comma ? "," : "") << "\n";
}

void WriteSceneEntity(std::ostream& out, const CameraEntity& camera, bool comma)
{
    out << "    {\n";
    out << "      \"type\": \"camera\",\n";
    out << "      \"id\": " << camera.id << ",\n";
    out << "      \"name\": \"" << EscapeJson(camera.name) << "\",\n";
    WritePrefabInstance(out, camera.prefabAssetId, camera.prefabInstance, "      ");
    WriteParentRef(out, camera.parent, "      ");
    out << "      \"position\": " << FloatArray(camera.position, 3) << ",\n";
    out << "      \"rotation\": " << FloatArray(camera.rotation, 3) << ",\n";
    out << "      \"fov\": " << camera.fovDegrees << ",\n";
    out << "      \"near\": " << camera.nearPlane << ",\n";
    out << "      \"far\": " << camera.farPlane << ",\n";
    out << "      \"editor_hidden\": " << (camera.editorHidden ? "true" : "false") << "\n";
    out << "    }" << (comma ? "," : "") << "\n";
}

void WriteMaterialOverride(std::ostream& out, const MeshSceneEntity::MaterialOverride& material, bool comma)
{
    out << "        {\n";
    out << "          \"slot\": " << material.slot << ",\n";
    out << "          \"enabled\": " << (material.enabled ? "true" : "false") << ",\n";
    out << "          \"base_color\": " << FloatArray(material.baseColor, 4) << ",\n";
    out << "          \"metallic\": " << material.metallic << ",\n";
    out << "          \"roughness\": " << material.roughness << ",\n";
    out << "          \"normal_strength\": " << material.normalStrength << ",\n";
    out << "          \"ao_strength\": " << material.aoStrength << ",\n";
    out << "          \"emissive\": " << FloatArray(material.emissive, 3) << ",\n";
    out << "          \"emissive_intensity\": " << material.emissiveIntensity << ",\n";
    out << "          \"uv_tiling\": " << FloatArray(material.uvTiling, 2) << ",\n";
    out << "          \"uv_offset\": " << FloatArray(material.uvOffset, 2) << "\n";
    out << "        }" << (comma ? "," : "") << "\n";
}

void WriteEditorComponent(std::ostream& out, const EditorAttachedComponent& component, bool comma)
{
    out << "        {\n";
    out << "          \"type\": \"" << EscapeJson(component.type) << "\",\n";
    out << "          \"display_name\": \"" << EscapeJson(component.displayName) << "\",\n";
    out << "          \"category\": \"" << EscapeJson(component.category) << "\",\n";
    out << "          \"note\": \"" << EscapeJson(component.note) << "\"\n";
    out << "        }" << (comma ? "," : "") << "\n";
}

void WriteEditorComponents(std::ostream& out, const std::vector<EditorAttachedComponent>& components, bool comma)
{
    if (components.empty())
        return;
    out << "      \"editor_components\": [\n";
    for (size_t i = 0; i < components.size(); ++i)
        WriteEditorComponent(out, components[i], i + 1 < components.size());
    out << "      ]" << (comma ? "," : "") << "\n";
}

void WriteLodConfig(std::ostream& out, const LodConfig& config, const std::string& indent)
{
    const std::uint32_t levelCount = std::clamp(config.levelCount, 1u, LodConfig::MaxLevels);
    out << indent << "\"level_count\": " << levelCount << ",\n";
    out << indent << "\"hysteresis_m\": " << std::max(0.0f, config.hysteresisMeters);
    for (std::uint32_t i = 0; i < LodConfig::MaxLevels; ++i)
    {
        out << ",\n" << indent << "\"target_ratio_" << i << "\": "
            << (i == 0 ? 1.0f : std::clamp(config.targetRatios[i], 0.001f, 1.0f));
        out << ",\n" << indent << "\"distance_m_" << i << "\": "
            << (i == 0 ? 0.0f : std::max(0.0f, config.distances[i]));
    }
}

void WriteLodComponent(std::ostream& out, const LodComponent& lod, bool comma)
{
    out << "      \"lod_component\": {\n";
    out << "        \"enabled\": " << (lod.enabled ? "true" : "false") << ",\n";
    out << "        \"override_asset_default\": " << (lod.overrideAssetDefault ? "true" : "false") << ",\n";
    out << "        \"config\": {\n";
    WriteLodConfig(out, lod.config, "          ");
    out << "\n";
    out << "        }\n";
    out << "      }" << (comma ? "," : "") << "\n";
}

void WriteRigidbodyComponent(std::ostream& out, const ixtreeme::physics::RigidbodyComponent& rigidbody)
{
    out << "      \"rigidbody\": {\n";
    out << "        \"enabled\": " << (rigidbody.enabled ? "true" : "false") << ",\n";
    out << "        \"body_type\": \"" << ixtreeme::physics::ToString(rigidbody.bodyType) << "\",\n";
    out << "        \"mass\": " << rigidbody.mass << ",\n";
    out << "        \"linear_damping\": " << rigidbody.linearDamping << ",\n";
    out << "        \"angular_damping\": " << rigidbody.angularDamping << ",\n";
    out << "        \"use_gravity\": " << (rigidbody.useGravity ? "true" : "false") << ",\n";
    out << "        \"allow_sleeping\": " << (rigidbody.allowSleeping ? "true" : "false") << ",\n";
    out << "        \"continuous_collision\": " << (rigidbody.continuousCollision ? "true" : "false") << ",\n";
    out << "        \"freeze_position\": " << BoolArray(rigidbody.freezePosition, 3) << ",\n";
    out << "        \"freeze_rotation\": " << BoolArray(rigidbody.freezeRotation, 3) << "\n";
    out << "      }";
}

void WriteColliderComponent(std::ostream& out, const ixtreeme::physics::ColliderComponent& collider)
{
    out << "      \"collider\": {\n";
    out << "        \"enabled\": " << (collider.enabled ? "true" : "false") << ",\n";
    out << "        \"trigger\": " << (collider.trigger ? "true" : "false") << ",\n";
    out << "        \"shape\": \"" << ixtreeme::physics::ToString(collider.shape) << "\",\n";
    out << "        \"center\": " << FloatArray(collider.center, 3) << ",\n";
    out << "        \"size\": " << FloatArray(collider.size, 3) << ",\n";
    out << "        \"radius\": " << collider.radius << ",\n";
    out << "        \"height\": " << collider.height << ",\n";
    out << "        \"friction\": " << collider.friction << ",\n";
    out << "        \"restitution\": " << collider.restitution << ",\n";
    out << "        \"layer\": \"" << ixtreeme::physics::ToString(collider.layer) << "\",\n";
    out << "        \"material_asset_id\": \"" << EscapeJson(collider.materialAssetId) << "\"\n";
    out << "      }";
}

void WriteFixedJointComponent(std::ostream& out, const ixtreeme::physics::FixedJointComponent& joint)
{
    out << "      \"fixed_joint\": {\n";
    out << "        \"enabled\": " << (joint.enabled ? "true" : "false") << ",\n";
    out << "        \"connected_entity_id\": " << joint.connectedEntityId << "\n";
    out << "      }";
}

void WriteHingeJointComponent(std::ostream& out, const ixtreeme::physics::HingeJointComponent& joint)
{
    out << "      \"hinge_joint\": {\n";
    out << "        \"enabled\": " << (joint.enabled ? "true" : "false") << ",\n";
    out << "        \"connected_entity_id\": " << joint.connectedEntityId << ",\n";
    out << "        \"anchor\": " << FloatArray(joint.anchor, 3) << ",\n";
    out << "        \"axis\": " << FloatArray(joint.axis, 3) << ",\n";
    out << "        \"limits_enabled\": " << (joint.limitsEnabled ? "true" : "false") << ",\n";
    out << "        \"min_angle_deg\": " << joint.minAngleDegrees << ",\n";
    out << "        \"max_angle_deg\": " << joint.maxAngleDegrees << ",\n";
    out << "        \"friction_torque\": " << joint.frictionTorque << "\n";
    out << "      }";
}

void WriteCharacterControllerComponent(std::ostream& out, const ixtreeme::physics::CharacterControllerComponent& cc)
{
    out << "      \"character_controller\": {\n";
    out << "        \"enabled\": " << (cc.enabled ? "true" : "false") << ",\n";
    out << "        \"walk_speed\": " << cc.walkSpeed << ",\n";
    out << "        \"run_speed\": " << cc.runSpeed << ",\n";
    out << "        \"jump_height\": " << cc.jumpHeight << ",\n";
    out << "        \"gravity_scale\": " << cc.gravityScale << ",\n";
    out << "        \"slope_limit_deg\": " << cc.slopeLimitDegrees << ",\n";
    out << "        \"step_height\": " << cc.stepHeight << ",\n";
    out << "        \"capsule_radius\": " << cc.capsuleRadius << ",\n";
    out << "        \"capsule_height\": " << cc.capsuleHeight << ",\n";
    out << "        \"camera_mode\": \"" << ixtreeme::physics::ToString(cc.cameraMode) << "\",\n";
    out << "        \"eye_height\": " << cc.eyeHeight << ",\n";
    out << "        \"third_person_distance\": " << cc.thirdPersonDistance << ",\n";
    out << "        \"third_person_height\": " << cc.thirdPersonHeight << ",\n";
    out << "        \"third_person_pitch_deg\": " << cc.thirdPersonPitchDegrees << ",\n";
    out << "        \"top_down_height\": " << cc.topDownHeight << ",\n";
    out << "        \"top_down_pitch_deg\": " << cc.topDownPitchDegrees << ",\n";
    out << "        \"mouse_sensitivity\": " << cc.mouseSensitivity << "\n";
    out << "      }";
}

void WriteAudioSourceComponent(std::ostream& out, const ixaudio::AudioSourceComponent& a)
{
    out << "      \"audio_source\": {\n";
    out << "        \"clip_asset_id\": \"" << EscapeJson(a.clipAssetId) << "\",\n";
    out << "        \"enabled\": " << (a.enabled ? "true" : "false") << ",\n";
    out << "        \"loop\": " << (a.loop ? "true" : "false") << ",\n";
    out << "        \"is_3d\": " << (a.is3d ? "true" : "false") << ",\n";
    out << "        \"play_on_start\": " << (a.playOnStart ? "true" : "false") << ",\n";
    out << "        \"volume\": " << a.volume << ",\n";
    out << "        \"pitch\": " << a.pitch << ",\n";
    out << "        \"min_distance\": " << a.minDistance << ",\n";
    out << "        \"max_distance\": " << a.maxDistance << ",\n";
    out << "        \"bus\": \"" << ixaudio::BusName(a.bus) << "\"\n";
    out << "      }";
}

void WriteScriptComponent(std::ostream& out, const ixscript::ScriptComponent& s)
{
    out << "      \"script\": {\n";
    out << "        \"backend\": \"" << ixscript::BackendName(s.backend) << "\",\n";
    out << "        \"script_asset_id\": \"" << EscapeJson(s.scriptAssetId) << "\",\n";
    out << "        \"native_class\": \"" << EscapeJson(s.nativeClassName) << "\",\n";
    out << "        \"enabled\": " << (s.enabled ? "true" : "false") << ",\n";
    out << "        \"parameters\": {";
    bool first = true;
    for (const auto& [key, value] : s.parameters)
    {
        out << (first ? "\n" : ",\n");
        out << "          \"" << EscapeJson(key) << "\": \"" << EscapeJson(value) << "\"";
        first = false;
    }
    out << (first ? "}\n" : "\n        }\n");
    out << "      }";
}

void WriteSceneEntity(std::ostream& out, const MeshSceneEntity& mesh, bool comma)
{
    out << "    {\n";
    out << "      \"type\": \"mesh_entity\",\n";
    out << "      \"id\": " << mesh.id << ",\n";
    out << "      \"name\": \"" << EscapeJson(mesh.name) << "\",\n";
    WritePrefabInstance(out, mesh.prefabAssetId, mesh.prefabInstance, "      ");
    WriteParentRef(out, mesh.parent, "      ");
    out << "      \"position\": " << FloatArray(mesh.position, 3) << ",\n";
    out << "      \"rotation\": " << FloatArray(mesh.rotation, 3) << ",\n";
    out << "      \"scale\": " << FloatArray(mesh.scale, 3) << ",\n";
    out << "      \"mesh_asset_id\": \"" << EscapeJson(mesh.meshAssetId) << "\",\n";
    out << "      \"mesh_asset_path\": \"" << EscapeJson(mesh.meshAssetPath) << "\",\n";
    out << "      \"skinned\": " << (mesh.skinned ? "true" : "false");
    if (!mesh.materialSlots.empty())
    {
        out << ",\n";
        out << "      \"materials\": [";
        for (size_t i = 0; i < mesh.materialSlots.size(); ++i)
        {
            if (i)
                out << ", ";
            out << "\"" << EscapeJson(mesh.materialSlots[i]) << "\"";
        }
        out << "]";
        Tracenf("[MATERIAL-SLOTS] saved entity=%u slots=%zu", mesh.id, mesh.materialSlots.size());
    }
    if (!mesh.materialOverrides.empty())
    {
        out << ",\n";
        out << "      \"material_overrides\": [\n";
        for (size_t i = 0; i < mesh.materialOverrides.size(); ++i)
            WriteMaterialOverride(out, mesh.materialOverrides[i], i + 1 < mesh.materialOverrides.size());
        out << "      ]";
        Tracenf("[MMAT] saved override entity=%u slots=%zu", mesh.id, mesh.materialOverrides.size());
    }
    if (!mesh.editorComponents.empty())
    {
        out << ",\n";
        WriteEditorComponents(out, mesh.editorComponents, mesh.lod.enabled);
        Tracenf("[INSPECTOR-COMP] saved entity=%u components=%zu", mesh.id, mesh.editorComponents.size());
    }
    if (mesh.lod.enabled)
    {
        out << (!mesh.editorComponents.empty() ? "" : ",\n");
        WriteLodComponent(out, mesh.lod, false);
        Tracenf("[LOD] saved component entity=%u levels=%u override=%d",
            mesh.id,
            mesh.lod.config.levelCount,
            mesh.lod.overrideAssetDefault ? 1 : 0);
    }
    if (mesh.hasRigidbody)
    {
        out << ",\n";
        WriteRigidbodyComponent(out, mesh.rigidbody);
        out << "\n";
        Tracenf("[PHYSICS] saved rigidbody entity=%u", mesh.id);
    }
    if (mesh.hasCollider)
    {
        out << ",\n";
        WriteColliderComponent(out, mesh.collider);
        Tracenf("[PHYSICS] saved collider entity=%u shape=%s", mesh.id, ixtreeme::physics::ToString(mesh.collider.shape));
    }
    if (mesh.hasFixedJoint)
    {
        out << ",\n";
        WriteFixedJointComponent(out, mesh.fixedJoint);
        Tracenf("[PHYSICS-JOINT] saved fixed entity=%u connected=%u",
            mesh.id,
            mesh.fixedJoint.connectedEntityId);
    }
    if (mesh.hasHingeJoint)
    {
        out << ",\n";
        WriteHingeJointComponent(out, mesh.hingeJoint);
        Tracenf("[PHYSICS-JOINT] saved hinge entity=%u connected=%u",
            mesh.id,
            mesh.hingeJoint.connectedEntityId);
    }
    if (mesh.hasCharacterController)
    {
        out << ",\n";
        WriteCharacterControllerComponent(out, mesh.characterController);
        Tracenf("[CHARACTER] saved controller entity=%u camera=%s",
            mesh.id,
            ixtreeme::physics::ToString(mesh.characterController.cameraMode));
    }
    if (mesh.hasAudioSource)
    {
        out << ",\n";
        WriteAudioSourceComponent(out, mesh.audioSource);
    }
    if (mesh.hasAudioListener)
    {
        out << ",\n";
        out << "      \"audio_listener\": {\n";
        out << "        \"enabled\": " << (mesh.audioListener.enabled ? "true" : "false") << "\n";
        out << "      }";
    }
    if (mesh.hasScript)
    {
        out << ",\n";
        WriteScriptComponent(out, mesh.script);
    }
    out << "\n";
    out << "    }" << (comma ? "," : "") << "\n";
}

PrefabInstanceState ReadPrefabInstance(const JsonValue& entity, const std::string& legacyAssetId)
{
    PrefabInstanceState prefab = MakePrefabInstanceState(legacyAssetId);
    if (const JsonValue* object = Find(entity, "prefab_instance"); object && object->type == JsonValue::Type::Object)
    {
        prefab.assetId = ReadString(*object, "asset_id", legacyAssetId);
        prefab.linked = ReadBool(*object, "linked", !prefab.assetId.empty());
        prefab.localId = ReadU32(*object, "local_id", prefab.localId);
        prefab.preserveTransform = ReadBool(*object, "preserve_transform", prefab.preserveTransform);
        prefab.nameOverride = ReadBool(*object, "name_override", prefab.nameOverride);
    }
    if (prefab.assetId.empty())
        prefab.linked = false;
    return prefab;
}

SceneParentRef ReadParentRef(const JsonValue& entity)
{
    SceneParentRef parent;
    if (const JsonValue* object = Find(entity, "parent"); object && object->type == JsonValue::Type::Object)
    {
        parent.type = ReadString(*object, "type");
        parent.id = ReadU32(*object, "id", 0);
    }
    else
    {
        parent.type = ReadString(entity, "parent_type");
        parent.id = ReadU32(entity, "parent_id", 0);
    }
    if (!parent.IsValid())
        return {};
    return parent;
}

PointLight ReadPointLight(const JsonValue& entity)
{
    PointLight light;
    light.id = ReadU32(entity, "id", light.id);
    light.name = ReadString(entity, "name", light.name);
    light.prefabAssetId = ReadString(entity, "prefab_asset_id");
    light.prefabInstance = ReadPrefabInstance(entity, light.prefabAssetId);
    light.prefabAssetId = light.prefabInstance.assetId;
    light.parent = ReadParentRef(entity);
    ReadFloatArray(entity, "position", light.position, 3);
    float color[3] = {light.r, light.g, light.b};
    ReadFloatArray(entity, "color", color, 3);
    light.r = color[0];
    light.g = color[1];
    light.b = color[2];
    light.intensity = ReadFloat(entity, "intensity", light.intensity);
    light.radius = ReadFloat(entity, "radius", light.radius);
    light.enabled = ReadBool(entity, "enabled", light.enabled);
    return light;
}

SpotLight ReadSpotLight(const JsonValue& entity)
{
    SpotLight light;
    light.id = ReadU32(entity, "id", light.id);
    light.name = ReadString(entity, "name", light.name);
    light.prefabAssetId = ReadString(entity, "prefab_asset_id");
    light.prefabInstance = ReadPrefabInstance(entity, light.prefabAssetId);
    light.prefabAssetId = light.prefabInstance.assetId;
    light.parent = ReadParentRef(entity);
    ReadFloatArray(entity, "position", light.position, 3);
    ReadFloatArray(entity, "rotation", light.rotation, 3);
    float color[3] = {light.r, light.g, light.b};
    ReadFloatArray(entity, "color", color, 3);
    light.r = color[0];
    light.g = color[1];
    light.b = color[2];
    light.intensity = ReadFloat(entity, "intensity", light.intensity);
    light.radius = ReadFloat(entity, "radius", light.radius);
    light.innerConeDegrees = ReadFloat(entity, "inner_cone_deg", light.innerConeDegrees);
    light.outerConeDegrees = ReadFloat(entity, "outer_cone_deg", light.outerConeDegrees);
    light.enabled = ReadBool(entity, "enabled", light.enabled);
    return light;
}

CameraEntity ReadCameraEntity(const JsonValue& entity)
{
    CameraEntity camera;
    camera.id = ReadU32(entity, "id", camera.id);
    camera.name = ReadString(entity, "name", camera.name);
    camera.prefabAssetId = ReadString(entity, "prefab_asset_id");
    camera.prefabInstance = ReadPrefabInstance(entity, camera.prefabAssetId);
    camera.prefabAssetId = camera.prefabInstance.assetId;
    camera.parent = ReadParentRef(entity);
    ReadFloatArray(entity, "position", camera.position, 3);
    ReadFloatArray(entity, "rotation", camera.rotation, 3);
    camera.fovDegrees = ReadFloat(entity, "fov", camera.fovDegrees);
    camera.nearPlane = ReadFloat(entity, "near", camera.nearPlane);
    camera.farPlane = ReadFloat(entity, "far", camera.farPlane);
    camera.editorHidden = ReadBool(entity, "editor_hidden", camera.editorHidden);
    return camera;
}

MeshSceneEntity::MaterialOverride ReadMaterialOverride(const JsonValue& object)
{
    MeshSceneEntity::MaterialOverride material;
    material.slot = ReadU32(object, "slot", material.slot);
    material.enabled = ReadBool(object, "enabled", material.enabled);
    ReadFloatArray(object, "base_color", material.baseColor, 4);
    material.metallic = ReadFloat(object, "metallic", material.metallic);
    material.roughness = ReadFloat(object, "roughness", material.roughness);
    material.normalStrength = ReadFloat(object, "normal_strength", material.normalStrength);
    material.aoStrength = ReadFloat(object, "ao_strength", material.aoStrength);
    ReadFloatArray(object, "emissive", material.emissive, 3);
    material.emissiveIntensity = ReadFloat(object, "emissive_intensity", material.emissiveIntensity);
    ReadFloatArray(object, "uv_tiling", material.uvTiling, 2);
    ReadFloatArray(object, "uv_offset", material.uvOffset, 2);
    return material;
}

EditorAttachedComponent ReadEditorComponent(const JsonValue& object)
{
    EditorAttachedComponent component;
    component.type = ReadString(object, "type");
    component.displayName = ReadString(object, "display_name", component.type);
    component.category = ReadString(object, "category", "Editor");
    component.note = ReadString(object, "note");
    return component;
}

std::vector<EditorAttachedComponent> ReadEditorComponents(const JsonValue& entity)
{
    std::vector<EditorAttachedComponent> components;
    if (const JsonValue* values = Find(entity, "editor_components"); values && values->type == JsonValue::Type::Array)
    {
        for (const JsonValue& value : values->array)
        {
            if (value.type != JsonValue::Type::Object)
                continue;
            EditorAttachedComponent component = ReadEditorComponent(value);
            if (!component.type.empty())
                components.push_back(std::move(component));
        }
    }
    return components;
}

LodConfig ReadLodConfig(const JsonValue& object)
{
    LodConfig config;
    config.levelCount = std::clamp(ReadU32(object, "level_count", config.levelCount), 1u, LodConfig::MaxLevels);
    config.hysteresisMeters = std::max(0.0f, ReadFloat(object, "hysteresis_m", config.hysteresisMeters));
    for (std::uint32_t i = 0; i < LodConfig::MaxLevels; ++i)
    {
        const std::string ratioKey = "target_ratio_" + std::to_string(i);
        const std::string distanceKey = "distance_m_" + std::to_string(i);
        config.targetRatios[i] = std::clamp(ReadFloat(object, ratioKey.c_str(), config.targetRatios[i]), 0.001f, 1.0f);
        config.distances[i] = std::max(0.0f, ReadFloat(object, distanceKey.c_str(), config.distances[i]));
    }
    config.targetRatios[0] = 1.0f;
    config.distances[0] = 0.0f;
    return config;
}

LodComponent ReadLodComponent(const JsonValue& entity)
{
    LodComponent lod;
    if (const JsonValue* object = Find(entity, "lod_component"); object && object->type == JsonValue::Type::Object)
    {
        lod.enabled = ReadBool(*object, "enabled", true);
        lod.overrideAssetDefault = ReadBool(*object, "override_asset_default", lod.overrideAssetDefault);
        if (const JsonValue* config = Find(*object, "config"); config && config->type == JsonValue::Type::Object)
            lod.config = ReadLodConfig(*config);
    }
    return lod;
}

ixtreeme::physics::RigidbodyComponent ReadRigidbodyComponent(const JsonValue& entity)
{
    ixtreeme::physics::RigidbodyComponent rigidbody;
    if (const JsonValue* object = Find(entity, "rigidbody"); object && object->type == JsonValue::Type::Object)
    {
        rigidbody.enabled = ReadBool(*object, "enabled", rigidbody.enabled);
        rigidbody.bodyType = ixtreeme::physics::BodyTypeFromString(ReadString(*object, "body_type"), rigidbody.bodyType);
        rigidbody.mass = ReadFloat(*object, "mass", rigidbody.mass);
        rigidbody.linearDamping = ReadFloat(*object, "linear_damping", rigidbody.linearDamping);
        rigidbody.angularDamping = ReadFloat(*object, "angular_damping", rigidbody.angularDamping);
        rigidbody.useGravity = ReadBool(*object, "use_gravity", rigidbody.useGravity);
        rigidbody.allowSleeping = ReadBool(*object, "allow_sleeping", rigidbody.allowSleeping);
        rigidbody.continuousCollision = ReadBool(*object, "continuous_collision", rigidbody.continuousCollision);
        ReadBoolArray(*object, "freeze_position", rigidbody.freezePosition, 3);
        ReadBoolArray(*object, "freeze_rotation", rigidbody.freezeRotation, 3);
        ixtreeme::physics::Sanitize(rigidbody);
    }
    return rigidbody;
}

ixtreeme::physics::ColliderComponent ReadColliderComponent(const JsonValue& entity)
{
    ixtreeme::physics::ColliderComponent collider;
    if (const JsonValue* object = Find(entity, "collider"); object && object->type == JsonValue::Type::Object)
    {
        collider.enabled = ReadBool(*object, "enabled", collider.enabled);
        collider.trigger = ReadBool(*object, "trigger", collider.trigger);
        collider.shape = ixtreeme::physics::ColliderShapeFromString(ReadString(*object, "shape"), collider.shape);
        ReadFloatArray(*object, "center", collider.center, 3);
        ReadFloatArray(*object, "size", collider.size, 3);
        collider.radius = ReadFloat(*object, "radius", collider.radius);
        collider.height = ReadFloat(*object, "height", collider.height);
        collider.friction = ReadFloat(*object, "friction", collider.friction);
        collider.restitution = ReadFloat(*object, "restitution", collider.restitution);
        collider.layer = ixtreeme::physics::PhysicsLayerFromString(ReadString(*object, "layer"), collider.layer);
        collider.materialAssetId = ReadString(*object, "material_asset_id");
        ixtreeme::physics::Sanitize(collider);
    }
    return collider;
}

ixtreeme::physics::FixedJointComponent ReadFixedJointComponent(const JsonValue& entity)
{
    ixtreeme::physics::FixedJointComponent joint;
    if (const JsonValue* object = Find(entity, "fixed_joint"); object && object->type == JsonValue::Type::Object)
    {
        joint.enabled = ReadBool(*object, "enabled", joint.enabled);
        joint.connectedEntityId = ReadU32(*object, "connected_entity_id", joint.connectedEntityId);
    }
    return joint;
}

ixtreeme::physics::HingeJointComponent ReadHingeJointComponent(const JsonValue& entity)
{
    ixtreeme::physics::HingeJointComponent joint;
    if (const JsonValue* object = Find(entity, "hinge_joint"); object && object->type == JsonValue::Type::Object)
    {
        joint.enabled = ReadBool(*object, "enabled", joint.enabled);
        joint.connectedEntityId = ReadU32(*object, "connected_entity_id", joint.connectedEntityId);
        ReadFloatArray(*object, "anchor", joint.anchor, 3);
        ReadFloatArray(*object, "axis", joint.axis, 3);
        joint.limitsEnabled = ReadBool(*object, "limits_enabled", joint.limitsEnabled);
        joint.minAngleDegrees = ReadFloat(*object, "min_angle_deg", joint.minAngleDegrees);
        joint.maxAngleDegrees = ReadFloat(*object, "max_angle_deg", joint.maxAngleDegrees);
        joint.frictionTorque = std::max(0.0f, ReadFloat(*object, "friction_torque", joint.frictionTorque));
    }
    return joint;
}

ixtreeme::physics::CharacterControllerComponent ReadCharacterControllerComponent(const JsonValue& entity)
{
    ixtreeme::physics::CharacterControllerComponent cc;
    if (const JsonValue* object = Find(entity, "character_controller"); object && object->type == JsonValue::Type::Object)
    {
        cc.enabled = ReadBool(*object, "enabled", cc.enabled);
        cc.walkSpeed = ReadFloat(*object, "walk_speed", cc.walkSpeed);
        cc.runSpeed = ReadFloat(*object, "run_speed", cc.runSpeed);
        cc.jumpHeight = ReadFloat(*object, "jump_height", cc.jumpHeight);
        cc.gravityScale = ReadFloat(*object, "gravity_scale", cc.gravityScale);
        cc.slopeLimitDegrees = ReadFloat(*object, "slope_limit_deg", cc.slopeLimitDegrees);
        cc.stepHeight = ReadFloat(*object, "step_height", cc.stepHeight);
        cc.capsuleRadius = ReadFloat(*object, "capsule_radius", cc.capsuleRadius);
        cc.capsuleHeight = ReadFloat(*object, "capsule_height", cc.capsuleHeight);
        cc.cameraMode = ixtreeme::physics::CameraModeFromString(ReadString(*object, "camera_mode"), cc.cameraMode);
        cc.eyeHeight = ReadFloat(*object, "eye_height", cc.eyeHeight);
        cc.thirdPersonDistance = ReadFloat(*object, "third_person_distance", cc.thirdPersonDistance);
        cc.thirdPersonHeight = ReadFloat(*object, "third_person_height", cc.thirdPersonHeight);
        cc.thirdPersonPitchDegrees = ReadFloat(*object, "third_person_pitch_deg", cc.thirdPersonPitchDegrees);
        cc.topDownHeight = ReadFloat(*object, "top_down_height", cc.topDownHeight);
        cc.topDownPitchDegrees = ReadFloat(*object, "top_down_pitch_deg", cc.topDownPitchDegrees);
        cc.mouseSensitivity = ReadFloat(*object, "mouse_sensitivity", cc.mouseSensitivity);
        ixtreeme::physics::Sanitize(cc);
    }
    return cc;
}

ixaudio::AudioSourceComponent ReadAudioSourceComponent(const JsonValue& entity)
{
    ixaudio::AudioSourceComponent a;
    if (const JsonValue* object = Find(entity, "audio_source"); object && object->type == JsonValue::Type::Object)
    {
        a.clipAssetId = ReadString(*object, "clip_asset_id");
        a.enabled = ReadBool(*object, "enabled", a.enabled);
        a.loop = ReadBool(*object, "loop", a.loop);
        a.is3d = ReadBool(*object, "is_3d", a.is3d);
        a.playOnStart = ReadBool(*object, "play_on_start", a.playOnStart);
        a.volume = ReadFloat(*object, "volume", a.volume);
        a.pitch = ReadFloat(*object, "pitch", a.pitch);
        a.minDistance = ReadFloat(*object, "min_distance", a.minDistance);
        a.maxDistance = ReadFloat(*object, "max_distance", a.maxDistance);
        a.bus = ixaudio::ParseBus(ReadString(*object, "bus"));
        ixaudio::Sanitize(a);
    }
    return a;
}

ixscript::ScriptComponent ReadScriptComponent(const JsonValue& entity)
{
    ixscript::ScriptComponent s;
    if (const JsonValue* object = Find(entity, "script"); object && object->type == JsonValue::Type::Object)
    {
        s.backend = ixscript::ParseBackend(ReadString(*object, "backend"));
        s.scriptAssetId = ReadString(*object, "script_asset_id");
        s.nativeClassName = ReadString(*object, "native_class");
        s.enabled = ReadBool(*object, "enabled", s.enabled);
        if (const JsonValue* params = object->Find("parameters"); params && params->type == JsonValue::Type::Object)
        {
            for (const auto& [key, value] : params->object)
            {
                if (value.type == JsonValue::Type::String)
                    s.parameters[key] = value.string;
            }
        }
    }
    return s;
}

MeshSceneEntity ReadMeshSceneEntity(const JsonValue& entity)
{
    MeshSceneEntity mesh;
    mesh.id = ReadU32(entity, "id", mesh.id);
    mesh.name = ReadString(entity, "name", mesh.name);
    mesh.prefabAssetId = ReadString(entity, "prefab_asset_id");
    mesh.prefabInstance = ReadPrefabInstance(entity, mesh.prefabAssetId);
    mesh.prefabAssetId = mesh.prefabInstance.assetId;
    mesh.parent = ReadParentRef(entity);
    ReadFloatArray(entity, "position", mesh.position, 3);
    ReadFloatArray(entity, "rotation", mesh.rotation, 3);
    ReadFloatArray(entity, "scale", mesh.scale, 3);
    mesh.meshAssetId = ReadString(entity, "mesh_asset_id");
    mesh.meshAssetPath = ReadString(entity, "mesh_asset_path");
    mesh.skinned = ReadBool(entity, "skinned", mesh.skinned);
    if (const JsonValue* materials = Find(entity, "materials"); materials && materials->type == JsonValue::Type::Array)
    {
        for (const JsonValue& value : materials->array)
        {
            if (value.type == JsonValue::Type::String)
                mesh.materialSlots.push_back(value.string);
        }
        Tracenf("[MATERIAL-SLOTS] loaded entity=%u slots=%zu", mesh.id, mesh.materialSlots.size());
    }
    if (const JsonValue* materials = Find(entity, "material_overrides"); materials && materials->type == JsonValue::Type::Array)
    {
        for (const JsonValue& value : materials->array)
        {
            if (value.type == JsonValue::Type::Object)
                mesh.materialOverrides.push_back(ReadMaterialOverride(value));
        }
        Tracenf("[MMAT] loaded override entity=%u slots=%zu", mesh.id, mesh.materialOverrides.size());
    }
    mesh.editorComponents = ReadEditorComponents(entity);
    mesh.lod = ReadLodComponent(entity);
    if (Find(entity, "rigidbody"))
    {
        mesh.hasRigidbody = true;
        mesh.rigidbody = ReadRigidbodyComponent(entity);
    }
    if (Find(entity, "collider"))
    {
        mesh.hasCollider = true;
        mesh.collider = ReadColliderComponent(entity);
    }
    if (Find(entity, "fixed_joint"))
    {
        mesh.hasFixedJoint = true;
        mesh.fixedJoint = ReadFixedJointComponent(entity);
    }
    if (Find(entity, "hinge_joint"))
    {
        mesh.hasHingeJoint = true;
        mesh.hingeJoint = ReadHingeJointComponent(entity);
    }
    if (Find(entity, "character_controller"))
    {
        mesh.hasCharacterController = true;
        mesh.characterController = ReadCharacterControllerComponent(entity);
    }
    if (Find(entity, "audio_source"))
    {
        mesh.hasAudioSource = true;
        mesh.audioSource = ReadAudioSourceComponent(entity);
    }
    if (const JsonValue* listenerObj = Find(entity, "audio_listener"); listenerObj && listenerObj->type == JsonValue::Type::Object)
    {
        mesh.hasAudioListener = true;
        mesh.audioListener.enabled = ReadBool(*listenerObj, "enabled", true);
    }
    if (const JsonValue* scriptObj = Find(entity, "script"); scriptObj && scriptObj->type == JsonValue::Type::Object)
    {
        mesh.hasScript = true;
        mesh.script = ReadScriptComponent(entity);
    }
    if (mesh.lod.enabled &&
        std::none_of(mesh.editorComponents.begin(), mesh.editorComponents.end(), [](const EditorAttachedComponent& component) {
            return component.type == "rendering.lod";
        }))
    {
        mesh.editorComponents.push_back({"rendering.lod", "LOD Group", "Rendering", {}});
    }
    if (!mesh.editorComponents.empty())
    {
        std::string restored;
        for (size_t i = 0; i < mesh.editorComponents.size(); ++i)
        {
            if (i > 0)
                restored += ",";
            restored += mesh.editorComponents[i].displayName.empty() ? mesh.editorComponents[i].type : mesh.editorComponents[i].displayName;
        }
        Tracenf("[INSPECTOR-COMP] restored entity=%u components=[%s]", mesh.id, restored.c_str());
    }
    if (mesh.lod.enabled)
        Tracenf("[LOD] restored component entity=%u levels=%u override=%d",
            mesh.id,
            mesh.lod.config.levelCount,
            mesh.lod.overrideAssetDefault ? 1 : 0);
    if (mesh.hasRigidbody || mesh.hasCollider || mesh.hasFixedJoint || mesh.hasHingeJoint)
        Tracenf("[PHYSICS] restored entity=%u rigidbody=%d collider=%d fixed=%d hinge=%d shape=%s layer=%s",
            mesh.id,
            mesh.hasRigidbody ? 1 : 0,
            mesh.hasCollider ? 1 : 0,
            mesh.hasFixedJoint ? 1 : 0,
            mesh.hasHingeJoint ? 1 : 0,
            mesh.hasCollider ? ixtreeme::physics::ToString(mesh.collider.shape) : "none",
            mesh.hasCollider ? ixtreeme::physics::ToString(mesh.collider.layer) : "none");
    return mesh;
}
}

SceneManager& SceneManager::Instance()
{
    static SceneManager manager;
    return manager;
}

void SceneManager::SetWindowTitleCallback(std::function<void(const std::string&)> callback)
{
    m_windowTitleCallback = std::move(callback);
    UpdateWindowTitle();
}

void SceneManager::SetWindowTitleSuffix(std::string suffix)
{
    if (m_windowTitleSuffix == suffix)
        return;
    m_windowTitleSuffix = std::move(suffix);
    UpdateWindowTitle();
}

void SceneManager::SetCurrentSceneSnapshot(const SceneData& scene)
{
    SceneData snapshot = scene;
    snapshot.name = m_currentScene.name.empty() ? scene.name : m_currentScene.name;
    m_currentScene = std::move(snapshot);
}

void SceneManager::RestoreSceneSnapshot(const SceneData& scene, const std::string& path, bool dirty)
{
    m_currentScene = scene;
    m_pendingScene = scene;
    m_hasPendingScene = true;
    m_currentScenePath = path;
    m_sceneOpen = true;
    m_isDirty = dirty;
    UpdateWindowTitle();
    Tracenf("[SCENE] Restored snapshot: name=%s path=%s dirty=%d",
        m_currentScene.name.c_str(),
        m_currentScenePath.c_str(),
        m_isDirty ? 1 : 0);
}

bool SceneManager::ConsumePendingScene(SceneData& outScene)
{
    if (!m_hasPendingScene)
        return false;
    outScene = m_pendingScene;
    m_hasPendingScene = false;
    return true;
}

static void EnsureSceneMainCamera(SceneData& scene)
{
    if (scene.cameras.empty())
    {
        CameraEntity camera;
        camera.id = 1;
        scene.cameras.push_back(camera);
    }
    const bool mainValid = std::any_of(scene.cameras.begin(), scene.cameras.end(),
        [&](const CameraEntity& cam) { return cam.id == scene.mainCameraId; });
    if (!mainValid)
        scene.mainCameraId = scene.cameras.front().id;
}

void SceneManager::NewScene()
{
    if (m_isDirty && !PromptSaveBeforeAction("New Scene"))
        return;

    m_currentScene = SceneData{};
    EnsureSceneMainCamera(m_currentScene);
    m_currentScenePath.clear();
    m_sceneOpen = true;
    m_isDirty = false;
    m_pendingScene = m_currentScene;
    m_hasPendingScene = true;
    UpdateWindowTitle();
    Tracenf("[SCENE] New empty scene created (Main Camera id=%u)", m_currentScene.mainCameraId);
}

bool SceneManager::LoadScene(const std::string& path)
{
    if (m_isDirty && !PromptSaveBeforeAction("Open Scene"))
        return false;
    return LoadSceneInternal(ResolveProjectScenePath(path));
}

bool SceneManager::SaveScene()
{
    if (m_currentScenePath.empty())
        return SaveSceneAs({});
    return SaveSceneInternal(m_currentScenePath);
}

bool SceneManager::SaveSceneAs(const std::string& path)
{
    std::string target = path.empty() ? DefaultProjectScenePath(m_currentScene) : path;
    if (target.empty())
        target = SaveSceneDialog();
    if (target.empty())
        return false;
    target = ResolveProjectScenePath(target);
    if (std::filesystem::path(target).extension().empty())
        target += ".scene";
    return SaveSceneInternal(target);
}

void SceneManager::CloseScene()
{
    m_currentScene = SceneData{};
    m_currentScenePath.clear();
    m_sceneOpen = false;
    m_isDirty = false;
    m_pendingScene = m_currentScene;
    m_hasPendingScene = true;
    UpdateWindowTitle();
}

void SceneManager::SetSceneName(const std::string& name)
{
    m_currentScene.name = name.empty() ? "Untitled" : name;
    MarkDirty();
}

void SceneManager::SetPhysicsSettings(const PhysicsSceneSettings& settings)
{
    PhysicsSceneSettings sanitized = settings;
    for (float& value : sanitized.gravity)
        value = std::clamp(value, -1000.0f, 1000.0f);
    sanitized.fixedDeltaSeconds = std::clamp(sanitized.fixedDeltaSeconds, 0.001f, 0.1f);
    sanitized.maxSubsteps = std::clamp(sanitized.maxSubsteps, 1u, 16u);
    if (std::abs(m_currentScene.physics.gravity[0] - sanitized.gravity[0]) <= 0.0001f &&
        std::abs(m_currentScene.physics.gravity[1] - sanitized.gravity[1]) <= 0.0001f &&
        std::abs(m_currentScene.physics.gravity[2] - sanitized.gravity[2]) <= 0.0001f &&
        std::abs(m_currentScene.physics.fixedDeltaSeconds - sanitized.fixedDeltaSeconds) <= 0.000001f &&
        m_currentScene.physics.maxSubsteps == sanitized.maxSubsteps)
    {
        return;
    }
    m_currentScene.physics = sanitized;
    MarkDirty();
}

void SceneManager::MarkDirty()
{
    if (!m_isDirty)
        Tracen("[SCENE] Dirty mark");
    m_isDirty = true;
    UpdateWindowTitle();
}

bool SceneManager::LoadSceneInternal(const std::string& path)
{
    Tracenf("[SCENE] Loading: %s", path.c_str());
    Tracenf("[SCENE] load attempt: %s", path.c_str());
    std::ifstream file(path);
    if (!file)
    {
        TraceError("[SCENE] Failed to open: %s", path.c_str());
        TraceError("[SCENE] load FAILED: file not found/unreadable path=%s", path.c_str());
        return false;
    }

    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    JsonValue root;
    if (!JsonParser(std::move(text)).Parse(root) || root.type != JsonValue::Type::Object)
    {
        TraceError("[SCENE] JSON parse error: %s", path.c_str());
        TraceError("[SCENE] load FAILED: parse error path=%s", path.c_str());
        return false;
    }

    if (static_cast<int>(ReadFloat(root, "version", 1.0f)) != 1)
    {
        TraceError("[SCENE] Unsupported scene version");
        TraceError("[SCENE] load FAILED: unsupported version path=%s", path.c_str());
        return false;
    }

    SceneData scene;
    const JsonValue* metadata = Find(root, "metadata");
    if (metadata)
    {
        scene.name = ReadString(*metadata, "name", SceneNameFromPath(path));
    }
    if (const JsonValue* editorCamera = Find(root, "editor_camera"))
    {
        ReadFloatArray(*editorCamera, "eye", scene.editorCamera.eye, 3);
        scene.editorCamera.yaw = ReadFloat(*editorCamera, "yaw", scene.editorCamera.yaw);
        scene.editorCamera.pitch = ReadFloat(*editorCamera, "pitch", scene.editorCamera.pitch);
    }
    scene.mainCameraId = ReadU32(root, "main_camera_id", scene.mainCameraId);
    if (const JsonValue* env = Find(root, "environment"))
    {
        scene.lighting.directional.elevationDegrees = ReadFloat(*env, "directional_light_angle_x", scene.lighting.directional.elevationDegrees);
        scene.lighting.directional.azimuthDegrees = ReadFloat(*env, "directional_light_angle_y", scene.lighting.directional.azimuthDegrees);
        scene.lighting.directional.intensity = ReadFloat(*env, "directional_light_intensity", scene.lighting.directional.intensity);
        float dirColor[3] = {scene.lighting.directional.r, scene.lighting.directional.g, scene.lighting.directional.b};
        ReadFloatArray(*env, "directional_light_color", dirColor, 3);
        scene.lighting.directional.r = dirColor[0];
        scene.lighting.directional.g = dirColor[1];
        scene.lighting.directional.b = dirColor[2];
        scene.lighting.ambient.intensity = ReadFloat(*env, "ambient_intensity", scene.lighting.ambient.intensity);
        float ambientColor[3] = {scene.lighting.ambient.r, scene.lighting.ambient.g, scene.lighting.ambient.b};
        ReadFloatArray(*env, "ambient_color", ambientColor, 3);
        scene.lighting.ambient.r = ambientColor[0];
        scene.lighting.ambient.g = ambientColor[1];
        scene.lighting.ambient.b = ambientColor[2];
    }
    if (const JsonValue* physics = Find(root, "physics"); physics && physics->type == JsonValue::Type::Object)
    {
        ReadFloatArray(*physics, "gravity", scene.physics.gravity, 3);
        for (float& value : scene.physics.gravity)
            value = std::clamp(value, -1000.0f, 1000.0f);
        scene.physics.fixedDeltaSeconds = std::clamp(
            ReadFloat(*physics, "fixed_delta_seconds", scene.physics.fixedDeltaSeconds),
            0.001f,
            0.1f);
        scene.physics.maxSubsteps = std::clamp(
            ReadU32(*physics, "max_substeps", scene.physics.maxSubsteps),
            1u,
            16u);
        Tracenf("[PHYSICS] scene settings loaded gravity=(%.2f,%.2f,%.2f) fixedDt=%.4f maxSubsteps=%u",
            scene.physics.gravity[0],
            scene.physics.gravity[1],
            scene.physics.gravity[2],
            scene.physics.fixedDeltaSeconds,
            scene.physics.maxSubsteps);
    }
    const std::filesystem::path sceneDir = std::filesystem::path(path).parent_path();
    if (const JsonValue* terrain = Find(root, "terrain"); terrain && terrain->type == JsonValue::Type::Object)
    {
        scene.terrain.exists = ReadBool(*terrain, "exists", true);
        scene.terrain.name = ReadString(*terrain, "name", scene.terrain.name);
        scene.terrain.widthMeters = ReadFloat(*terrain, "width_m", scene.terrain.widthMeters);
        scene.terrain.depthMeters = ReadFloat(*terrain, "depth_m", scene.terrain.depthMeters);
        scene.terrain.cellSizeMeters = ReadFloat(*terrain, "cell_size_m", scene.terrain.cellSizeMeters);
        scene.terrain.cellsX = ReadU32(*terrain, "cells_x", scene.terrain.cellsX);
        scene.terrain.cellsZ = ReadU32(*terrain, "cells_z", scene.terrain.cellsZ);
        scene.terrain.chunkSizeCells = ReadU32(*terrain, "chunk_size_cells", scene.terrain.chunkSizeCells);
        scene.terrain.chunkManifestRef = ReadString(*terrain, "chunk_manifest_ref");
        scene.terrain.heightmapRef = ReadString(*terrain, "heightmap_ref");
        scene.terrain.splatRef = ReadString(*terrain, "splat_ref");
        scene.terrain.maskRef = ReadString(*terrain, "mask_ref");
        scene.terrain.triplanarEnabled = ReadBool(*terrain, "triplanar_enabled", scene.terrain.triplanarEnabled);
        scene.terrain.triplanarSharpness = ReadFloat(*terrain, "triplanar_sharpness", scene.terrain.triplanarSharpness);
        scene.terrain.triplanarSlopeThreshold = ReadFloat(*terrain, "triplanar_slope_threshold", scene.terrain.triplanarSlopeThreshold);
        scene.terrain.triplanarSlopeTransition = ReadFloat(*terrain, "triplanar_slope_transition", scene.terrain.triplanarSlopeTransition);
    }
    else
    {
        const std::string legacyTerrainRef = ReadString(root, "terrain_ref");
        const std::string legacySplatRef = ReadString(root, "splat_ref");
        if (!legacyTerrainRef.empty() || !legacySplatRef.empty())
        {
            scene.terrain.exists = true;
            scene.terrain.heightmapRef = legacyTerrainRef;
            scene.terrain.splatRef = legacySplatRef;
            scene.terrain.name = "Terrain";
        }
    }
    if (scene.terrain.exists)
    {
        const bool loadedChunkSet = ReadTerrainChunkSet(sceneDir, scene.terrain, &scene.paletteSlots);
        if (!loadedChunkSet && !scene.terrain.heightmapRef.empty())
            ReadTerrainHeightmap(sceneDir / scene.terrain.heightmapRef, scene.terrain);
        if (!loadedChunkSet && !scene.terrain.splatRef.empty())
            ReadTerrainSplat(sceneDir / scene.terrain.splatRef, scene.terrain);
        if (!loadedChunkSet && (!scene.terrain.heightmapRef.empty() || !scene.terrain.splatRef.empty()))
        {
            const std::uint32_t chunkSize = std::clamp(scene.terrain.chunkSizeCells == 0 ? 64u : scene.terrain.chunkSizeCells, 32u, 256u);
            const std::uint32_t worldCells = std::max(scene.terrain.cellsX, scene.terrain.cellsZ);
            const std::uint32_t chunks = ((worldCells + chunkSize - 1u) / chunkSize);
            Tracenf("[TCHUNK] legacy import -> converted chunks=%u", chunks * chunks);
        }
        scene.terrain.cellSizeMeters = std::max(0.01f, scene.terrain.cellSizeMeters);
        scene.terrain.cellsX = std::max(1u, scene.terrain.cellsX);
        scene.terrain.cellsZ = std::max(1u, scene.terrain.cellsZ);
        scene.terrain.chunkSizeCells = std::clamp(scene.terrain.chunkSizeCells == 0 ? 64u : scene.terrain.chunkSizeCells, 32u, 256u);
        scene.terrain.triplanarSharpness = std::clamp(scene.terrain.triplanarSharpness, 1.0f, 16.0f);
        scene.terrain.triplanarSlopeThreshold = std::clamp(scene.terrain.triplanarSlopeThreshold, 0.0f, 1.0f);
        scene.terrain.triplanarSlopeTransition = std::clamp(scene.terrain.triplanarSlopeTransition, 0.001f, 1.0f);
        if (scene.terrain.widthMeters <= 0.0f)
            scene.terrain.widthMeters = static_cast<float>(scene.terrain.cellsX) * scene.terrain.cellSizeMeters;
        if (scene.terrain.depthMeters <= 0.0f)
            scene.terrain.depthMeters = static_cast<float>(scene.terrain.cellsZ) * scene.terrain.cellSizeMeters;
        Tracenf("[SCENE] terrain loaded: dims=%.2fx%.2f m cellSize=%.2f cells=%ux%u chunkSize=%u manifest=%s files=%s/%s/%s triplanar=%s sharpness=%.2f slopeThreshold=%.3f transition=%.3f",
            scene.terrain.widthMeters,
            scene.terrain.depthMeters,
            scene.terrain.cellSizeMeters,
            scene.terrain.cellsX,
            scene.terrain.cellsZ,
            scene.terrain.chunkSizeCells,
            scene.terrain.chunkManifestRef.c_str(),
            scene.terrain.heightmapRef.c_str(),
            scene.terrain.splatRef.c_str(),
            scene.terrain.maskRef.c_str(),
            scene.terrain.triplanarEnabled ? "yes" : "no",
            scene.terrain.triplanarSharpness,
            scene.terrain.triplanarSlopeThreshold,
            scene.terrain.triplanarSlopeTransition);
    }
    else
    {
        Tracen("[SCENE] no terrain in scene");
    }

    if (const JsonValue* entities = Find(root, "entities"); entities && entities->type == JsonValue::Type::Array)
    {
        for (const JsonValue& entity : entities->array)
        {
            const std::string type = ReadString(entity, "type");
            if (type == "water_body")
            {
                WaterBody body;
                body.id = ReadU32(entity, "id", body.id);
                body.name = ReadString(entity, "name");
                ReadFloatArray(entity, "bbox_min", body.bboxMin, 2);
                ReadFloatArray(entity, "bbox_max", body.bboxMax, 2);
                body.waterLevelY = ReadFloat(entity, "water_level_y", body.waterLevelY);
                body.materialId = ReadString(entity, "material_id");
                body.maskWidth = ReadU32(entity, "mask_width", body.maskWidth);
                body.maskHeight = ReadU32(entity, "mask_height", body.maskHeight);
                ReadWaterConfig(entity, body.config);
                body.config.waterLevelY = body.waterLevelY;
                const std::string maskRef = ReadString(entity, "shape_mask_ref");
                if (!maskRef.empty())
                    body.shapeMask = ReadBytes(sceneDir / maskRef);
                scene.waterBodies.push_back(std::move(body));
            }
            else if (type == "dynamic_light")
            {
                const std::string lightType = ReadString(entity, "light_type");
                if (lightType == "point")
                    scene.pointLights.push_back(ReadPointLight(entity));
                else if (lightType == "spot")
                    scene.spotLights.push_back(ReadSpotLight(entity));
                else
                    Tracenf("[SCENE] Unknown dynamic light type: %s", lightType.c_str());
            }
            else if (type == "mesh_entity")
            {
                scene.meshEntities.push_back(ReadMeshSceneEntity(entity));
            }
            else if (type == "camera")
            {
                scene.cameras.push_back(ReadCameraEntity(entity));
            }
            else
            {
                Tracenf("[SCENE] Unknown entity type: %s", type.c_str());
            }
        }
    }
    if (const JsonValue* palette = Find(root, "terrain_palette"); palette && palette->type == JsonValue::Type::Array)
    {
        for (const JsonValue& slotJson : palette->array)
        {
            MapEditorPaletteSlot slot = ReadPaletteSlot(slotJson);
            if (slot.slot < scene.paletteSlots.size())
                scene.paletteSlots[slot.slot] = slot;
        }
    }
    if (const JsonValue* preload = Find(root, "preload_assets"); preload && preload->type == JsonValue::Type::Array)
    {
        for (const JsonValue& value : preload->array)
            scene.preloadAssets.push_back(value.StringOr());
    }

    scene.lighting.numPointLights = static_cast<std::uint32_t>(std::min<std::size_t>(scene.pointLights.size(), kMaxDynamicPointLights));
    for (std::uint32_t i = 0; i < scene.lighting.numPointLights; ++i)
        scene.lighting.pointLights[i] = scene.pointLights[i];
    scene.lighting.numSpotLights = static_cast<std::uint32_t>(std::min<std::size_t>(scene.spotLights.size(), kMaxDynamicSpotLights));
    for (std::uint32_t i = 0; i < scene.lighting.numSpotLights; ++i)
        scene.lighting.spotLights[i] = scene.spotLights[i];

    EnsureSceneMainCamera(scene);

    m_currentScene = scene;
    m_pendingScene = scene;
    m_hasPendingScene = true;
    m_currentScenePath = path;
    m_sceneOpen = true;
    m_isDirty = false;
    UpdateRecentList(path);
    UpdateWindowTitle();
    Tracenf("[SCENE] load OK: name=%s water=%zu point_lights=%zu spot_lights=%zu mesh=%zu",
        m_currentScene.name.c_str(),
        m_currentScene.waterBodies.size(),
        m_currentScene.pointLights.size(),
        m_currentScene.spotLights.size(),
        m_currentScene.meshEntities.size());
    Tracenf("[SCENE] Loaded successfully: %s (%zu entities)",
        path.c_str(),
        scene.waterBodies.size() + scene.pointLights.size() + scene.spotLights.size() + scene.meshEntities.size());
    return true;
}

bool SceneManager::SaveSceneInternal(const std::string& path)
{
    Tracenf("[SCENE] Saving: %s", path.c_str());
    const std::filesystem::path scenePath(path);
    if (!scenePath.parent_path().empty())
        std::filesystem::create_directories(scenePath.parent_path());

    SceneData scene = m_currentScene;
    if (scene.name.empty())
        scene.name = SceneNameFromPath(path);
    if (scene.terrain.exists)
    {
        if (!WriteTerrainChunkSet(scenePath, scene.terrain, scene.paletteSlots))
        {
            TraceError("[SCENE] terrain chunk save failed: %s", path.c_str());
            return false;
        }
        Tracenf("[SCENE] terrain saved: dims=%.2fx%.2f m cellSize=%.2f cells=%ux%u chunkSize=%u manifest=%s triplanar=%s sharpness=%.2f slopeThreshold=%.3f transition=%.3f",
            scene.terrain.widthMeters,
            scene.terrain.depthMeters,
            scene.terrain.cellSizeMeters,
            scene.terrain.cellsX,
            scene.terrain.cellsZ,
            scene.terrain.chunkSizeCells,
            scene.terrain.chunkManifestRef.c_str(),
            scene.terrain.triplanarEnabled ? "yes" : "no",
            scene.terrain.triplanarSharpness,
            scene.terrain.triplanarSlopeThreshold,
            scene.terrain.triplanarSlopeTransition);
    }
    else
    {
        Tracen("[SCENE] no terrain in scene");
    }

    std::ofstream out(path);
    if (!out)
    {
        TraceError("[SCENE] Failed to write: %s", path.c_str());
        return false;
    }

    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"metadata\": {\n";
    out << "    \"name\": \"" << EscapeJson(scene.name) << "\",\n";
    out << "    \"author\": \"editor\",\n";
    out << "    \"modified_at\": \"" << TimestampUtc() << "\"\n";
    out << "  },\n";
    out << "  \"editor_camera\": {\n";
    out << "    \"eye\": " << FloatArray(scene.editorCamera.eye, 3) << ",\n";
    out << "    \"yaw\": " << scene.editorCamera.yaw << ",\n";
    out << "    \"pitch\": " << scene.editorCamera.pitch << "\n";
    out << "  },\n";
    out << "  \"main_camera_id\": " << scene.mainCameraId << ",\n";
    out << "  \"environment\": {\n";
    out << "    \"time_of_day\": 12.0,\n";
    const float dirColor[3] = {scene.lighting.directional.r, scene.lighting.directional.g, scene.lighting.directional.b};
    out << "    \"directional_light_color\": " << FloatArray(dirColor, 3) << ",\n";
    out << "    \"directional_light_intensity\": " << scene.lighting.directional.intensity << ",\n";
    out << "    \"directional_light_angle_x\": " << scene.lighting.directional.elevationDegrees << ",\n";
    out << "    \"directional_light_angle_y\": " << scene.lighting.directional.azimuthDegrees << ",\n";
    const float ambientColor[3] = {scene.lighting.ambient.r, scene.lighting.ambient.g, scene.lighting.ambient.b};
    out << "    \"ambient_color\": " << FloatArray(ambientColor, 3) << ",\n";
    out << "    \"ambient_intensity\": " << scene.lighting.ambient.intensity << "\n";
    out << "  },\n";
    out << "  \"physics\": {\n";
    out << "    \"gravity\": " << FloatArray(scene.physics.gravity, 3) << ",\n";
    out << "    \"fixed_delta_seconds\": " << scene.physics.fixedDeltaSeconds << ",\n";
    out << "    \"max_substeps\": " << scene.physics.maxSubsteps << "\n";
    out << "  },\n";
    if (scene.terrain.exists)
    {
        out << "  \"terrain\": {\n";
        out << "    \"exists\": true,\n";
        out << "    \"name\": \"" << EscapeJson(scene.terrain.name) << "\",\n";
        out << "    \"width_m\": " << scene.terrain.widthMeters << ",\n";
        out << "    \"depth_m\": " << scene.terrain.depthMeters << ",\n";
        out << "    \"cell_size_m\": " << scene.terrain.cellSizeMeters << ",\n";
        out << "    \"cells_x\": " << scene.terrain.cellsX << ",\n";
        out << "    \"cells_z\": " << scene.terrain.cellsZ << ",\n";
        out << "    \"chunk_size_cells\": " << scene.terrain.chunkSizeCells << ",\n";
        out << "    \"chunk_manifest_ref\": \"" << EscapeJson(scene.terrain.chunkManifestRef) << "\",\n";
        out << "    \"heightmap_ref\": \"" << EscapeJson(scene.terrain.heightmapRef) << "\",\n";
        out << "    \"splat_ref\": \"" << EscapeJson(scene.terrain.splatRef) << "\",\n";
        out << "    \"mask_ref\": \"" << EscapeJson(scene.terrain.maskRef) << "\",\n";
        out << "    \"triplanar_enabled\": " << (scene.terrain.triplanarEnabled ? "true" : "false") << ",\n";
        out << "    \"triplanar_sharpness\": " << std::clamp(scene.terrain.triplanarSharpness, 1.0f, 16.0f) << ",\n";
        out << "    \"triplanar_slope_threshold\": " << std::clamp(scene.terrain.triplanarSlopeThreshold, 0.0f, 1.0f) << ",\n";
        out << "    \"triplanar_slope_transition\": " << std::clamp(scene.terrain.triplanarSlopeTransition, 0.001f, 1.0f) << "\n";
        out << "  },\n";
    }
    else
    {
        out << "  \"terrain\": null,\n";
    }
    out << "  \"entities\": [\n";

    const size_t entityCount =
        scene.waterBodies.size() + scene.pointLights.size() + scene.spotLights.size() +
        scene.meshEntities.size() + scene.cameras.size();
    size_t entityIndex = 0;
    for (const WaterBody& body : scene.waterBodies)
    {
        const std::filesystem::path maskPath = scenePath.parent_path() /
            (scenePath.stem().string() + "_water_" + std::to_string(body.id) + ".mask");
        WriteBytes(maskPath, body.shapeMask);
        WriteSceneEntity(out, body, GenericPath(maskPath.filename()), ++entityIndex < entityCount);
    }
    for (const PointLight& light : scene.pointLights)
        WriteSceneEntity(out, light, ++entityIndex < entityCount);
    for (const SpotLight& light : scene.spotLights)
        WriteSceneEntity(out, light, ++entityIndex < entityCount);
    for (const MeshSceneEntity& mesh : scene.meshEntities)
        WriteSceneEntity(out, mesh, ++entityIndex < entityCount);
    for (const CameraEntity& camera : scene.cameras)
        WriteSceneEntity(out, camera, ++entityIndex < entityCount);

    out << "  ],\n";
    out << "  \"terrain_palette\": [\n";
    for (size_t i = 0; i < scene.paletteSlots.size(); ++i)
        WritePaletteSlot(out, scene.paletteSlots[i], i + 1 < scene.paletteSlots.size());
    out << "  ],\n";
    out << "  \"preload_assets\": [";
    for (size_t i = 0; i < scene.preloadAssets.size(); ++i)
    {
        if (i > 0)
            out << ", ";
        out << "\"" << EscapeJson(scene.preloadAssets[i]) << "\"";
    }
    out << "]\n";
    out << "}\n";

    m_currentScene = scene;
    m_currentScenePath = path;
    m_sceneOpen = true;
    m_isDirty = false;
    UpdateRecentList(path);
    UpdateWindowTitle();
    Tracenf("[SCENE] Saved successfully: %s (%zu entities)", path.c_str(), entityCount);
    return true;
}

bool SceneManager::PromptSaveBeforeAction(const std::string& actionName)
{
#if defined(_WIN32)
    const std::string text = "The current scene has unsaved changes. Save before '" + actionName + "'?";
    const int result = MessageBoxA(nullptr, text.c_str(), "Unsaved Changes", MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON1);
    if (result == IDYES)
        return SaveScene();
    if (result == IDNO)
        return true;
    return false;
#else
    (void)actionName;
    return true;
#endif
}

void SceneManager::UpdateRecentList(const std::string& path)
{
    if (path.empty())
        return;
    const std::string recentPath = ProjectSceneRecentPath(path);
    m_recentScenes.erase(std::remove(m_recentScenes.begin(), m_recentScenes.end(), recentPath), m_recentScenes.end());
    m_recentScenes.insert(m_recentScenes.begin(), recentPath);
    if (m_recentScenes.size() > 8)
        m_recentScenes.resize(8);
    ProjectManager::Instance().SetRecentScenes(m_recentScenes);
    Tracenf("[SCENE] Recent: %s", recentPath.c_str());
}

std::string SceneManager::OpenSceneDialog() const
{
#if defined(_WIN32)
    char file[MAX_PATH]{};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = "Open Scene";
    ofn.lpstrFilter = "Scene Files (*.scene)\0*.scene\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn))
        return file;
#endif
    return {};
}

std::string SceneManager::SaveSceneDialog() const
{
#if defined(_WIN32)
    char file[MAX_PATH] = "untitled.scene";
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = "Save Scene As";
    ofn.lpstrFilter = "Scene Files (*.scene)\0*.scene\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "scene";
    if (GetSaveFileNameA(&ofn))
        return file;
#endif
    return {};
}

void SceneManager::UpdateWindowTitle()
{
    std::string title = "IxtreemeEngine - Editor";
    if (HasOpenScene())
    {
        std::string sceneLabel = std::filesystem::path(m_currentScenePath).filename().string();
        if (sceneLabel.empty())
            sceneLabel = m_currentScene.name.empty() ? "Untitled.scene" : m_currentScene.name;
        title += " [" + sceneLabel + "]";
        if (m_isDirty)
            title += "*";
    }
    else
    {
        title += " [No Scene]";
    }
    if (!m_windowTitleSuffix.empty())
        title += " | " + m_windowTitleSuffix;

    if (m_windowTitleCallback)
        m_windowTitleCallback(title);
}
