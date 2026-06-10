#include "SceneManager.h"

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

std::string EscapeJson(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value)
    {
        switch (ch)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string GenericPath(const std::filesystem::path& path)
{
    return path.generic_string();
}

std::string TimestampUtc()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

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

void WriteSceneEntity(std::ostream& out, const PointLight& light, bool comma)
{
    out << "    {\n";
    out << "      \"type\": \"dynamic_light\",\n";
    out << "      \"id\": " << light.id << ",\n";
    out << "      \"name\": \"" << EscapeJson(light.name) << "\",\n";
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

void WriteSceneEntity(std::ostream& out, const MeshSceneEntity& mesh, bool comma)
{
    out << "    {\n";
    out << "      \"type\": \"mesh_entity\",\n";
    out << "      \"id\": " << mesh.id << ",\n";
    out << "      \"name\": \"" << EscapeJson(mesh.name) << "\",\n";
    out << "      \"position\": " << FloatArray(mesh.position, 3) << ",\n";
    out << "      \"rotation\": " << FloatArray(mesh.rotation, 3) << ",\n";
    out << "      \"scale\": " << FloatArray(mesh.scale, 3) << ",\n";
    out << "      \"mesh_asset_id\": \"" << EscapeJson(mesh.meshAssetId) << "\",\n";
    out << "      \"mesh_asset_path\": \"" << EscapeJson(mesh.meshAssetPath) << "\",\n";
    out << "      \"skinned\": " << (mesh.skinned ? "true" : "false") << "\n";
    out << "    }" << (comma ? "," : "") << "\n";
}

PointLight ReadPointLight(const JsonValue& entity)
{
    PointLight light;
    light.id = ReadU32(entity, "id", light.id);
    light.name = ReadString(entity, "name", light.name);
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

MeshSceneEntity ReadMeshSceneEntity(const JsonValue& entity)
{
    MeshSceneEntity mesh;
    mesh.id = ReadU32(entity, "id", mesh.id);
    mesh.name = ReadString(entity, "name", mesh.name);
    ReadFloatArray(entity, "position", mesh.position, 3);
    ReadFloatArray(entity, "rotation", mesh.rotation, 3);
    ReadFloatArray(entity, "scale", mesh.scale, 3);
    mesh.meshAssetId = ReadString(entity, "mesh_asset_id");
    mesh.meshAssetPath = ReadString(entity, "mesh_asset_path");
    mesh.skinned = ReadBool(entity, "skinned", mesh.skinned);
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

void SceneManager::NewScene()
{
    if (m_isDirty && !PromptSaveBeforeAction("New Scene"))
        return;

    m_currentScene = SceneData{};
    m_currentScenePath.clear();
    m_sceneOpen = true;
    m_isDirty = false;
    m_pendingScene = m_currentScene;
    m_hasPendingScene = true;
    UpdateWindowTitle();
    Tracen("[SCENE] New empty scene created");
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
    if (const JsonValue* camera = Find(root, "camera"))
    {
        ReadFloatArray(*camera, "position", scene.cameraPosition, 3);
        ReadFloatArray(*camera, "rotation", scene.cameraRotation, 4);
        scene.cameraFov = ReadFloat(*camera, "fov", scene.cameraFov);
        scene.cameraNear = ReadFloat(*camera, "near", scene.cameraNear);
        scene.cameraFar = ReadFloat(*camera, "far", scene.cameraFar);
    }
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
    out << "  \"camera\": {\n";
    out << "    \"position\": " << FloatArray(scene.cameraPosition, 3) << ",\n";
    out << "    \"rotation\": " << FloatArray(scene.cameraRotation, 4) << ",\n";
    out << "    \"fov\": " << scene.cameraFov << ",\n";
    out << "    \"near\": " << scene.cameraNear << ",\n";
    out << "    \"far\": " << scene.cameraFar << "\n";
    out << "  },\n";
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
        scene.waterBodies.size() + scene.pointLights.size() + scene.spotLights.size() + scene.meshEntities.size();
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
    std::string title = "IxtreemeWorld Engine - Editor";
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

    if (m_windowTitleCallback)
        m_windowTitleCallback(title);
}
