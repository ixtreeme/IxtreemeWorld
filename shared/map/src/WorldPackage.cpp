#include "map/WorldPackage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <capnp/serialize.h>
#include <kj/exception.h>

#include "schema/map_manifest.capnp.h"

namespace mx::map {
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxRecordedIssues = 256;
constexpr std::uint32_t kMaxLayerDecls = 32;
constexpr const char* kManifestFile = "map.manifest";
constexpr const char* kLegacyWorldLogicFile = "worldlogic.dat";
constexpr const char* kLegacySpawnFile = "mob_spawns.conf";

// ---- issue collection --------------------------------------------------------

struct IssueSite {
    std::string layer;
    std::string file;
    std::string field;
    std::int64_t offset = -1;
    std::int32_t chunk_x = -1;
    std::int32_t chunk_y = -1;
};

class Issues {
public:
    explicit Issues(PackageReport& report)
        : report_(report)
    {
    }

    void SetPackage(std::string package)
    {
        package_ = std::move(package);
    }

    void Add(IssueSeverity severity,
             PackageErrorCode code,
             const IssueSite& site,
             std::string reason,
             std::string expected = {})
    {
        if (severity == IssueSeverity::Error) {
            ++errors_;
        }
        if (report_.issues.size() >= kMaxRecordedIssues) {
            ++suppressed_;
            return;
        }
        PackageIssue issue;
        issue.severity = severity;
        issue.code = code;
        issue.package = package_;
        issue.layer = site.layer;
        issue.file = site.file;
        issue.field = site.field;
        issue.offset = site.offset;
        issue.chunk_x = site.chunk_x;
        issue.chunk_y = site.chunk_y;
        issue.reason = std::move(reason);
        issue.expected = std::move(expected);
        report_.issues.push_back(std::move(issue));
    }

    void Error(PackageErrorCode code, const IssueSite& site, std::string reason, std::string expected = {})
    {
        Add(IssueSeverity::Error, code, site, std::move(reason), std::move(expected));
    }
    void Warning(PackageErrorCode code, const IssueSite& site, std::string reason, std::string expected = {})
    {
        Add(IssueSeverity::Warning, code, site, std::move(reason), std::move(expected));
    }
    void Info(PackageErrorCode code, const IssueSite& site, std::string reason, std::string expected = {})
    {
        Add(IssueSeverity::Info, code, site, std::move(reason), std::move(expected));
    }

    std::size_t ErrorCount() const noexcept
    {
        return errors_;
    }

    void Finish()
    {
        if (suppressed_ > 0) {
            PackageIssue issue;
            issue.severity = IssueSeverity::Info;
            issue.code = PackageErrorCode::Internal;
            issue.package = package_;
            issue.reason = std::to_string(suppressed_) + " further issue(s) not recorded (limit " +
                           std::to_string(kMaxRecordedIssues) + ")";
            report_.issues.push_back(std::move(issue));
        }
    }

private:
    PackageReport& report_;
    std::string package_;
    std::size_t errors_ = 0;
    std::size_t suppressed_ = 0;
};

std::string Str(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << value;
    return out.str();
}

// ---- bounded little-endian reading ------------------------------------------

std::uint16_t LoadU16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t LoadU32(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

float LoadF32(const std::uint8_t* p) noexcept
{
    const std::uint32_t bits = LoadU32(p);
    float value = 0.0f;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

class Cursor {
public:
    explicit Cursor(const std::vector<std::uint8_t>& bytes)
        : bytes_(bytes)
    {
    }
    std::size_t Offset() const noexcept
    {
        return offset_;
    }
    std::size_t Remaining() const noexcept
    {
        return bytes_.size() - offset_;
    }
    bool U8(std::uint8_t& out) noexcept
    {
        if (Remaining() < 1) {
            return false;
        }
        out = bytes_[offset_++];
        return true;
    }
    bool U32(std::uint32_t& out) noexcept
    {
        if (Remaining() < 4) {
            return false;
        }
        out = LoadU32(bytes_.data() + offset_);
        offset_ += 4;
        return true;
    }
    bool F32(float& out) noexcept
    {
        if (Remaining() < 4) {
            return false;
        }
        out = LoadF32(bytes_.data() + offset_);
        offset_ += 4;
        return true;
    }
    bool Text(std::size_t length, std::string& out)
    {
        if (Remaining() < length) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
        offset_ += length;
        return true;
    }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_ = 0;
};

// ---- package file access -----------------------------------------------------

fs::path PathFromUtf8(std::string_view text)
{
    std::u8string utf8;
    utf8.reserve(text.size());
    for (const char c : text) {
        utf8.push_back(static_cast<char8_t>(c));
    }
    return fs::path(utf8);
}

struct FileAccess {
    fs::path root;
    PackageReport& report;
    Issues& issues;

    std::optional<std::vector<std::uint8_t>> Read(std::string_view reference,
                                                  std::uint64_t max_bytes,
                                                  IssueSite site)
    {
        site.file = std::string(reference);
        std::string why;
        const auto path = ResolvePackageReference(root, reference, why);
        if (!path) {
            issues.Error(why.find("outside") != std::string::npos
                             ? PackageErrorCode::PathOutsidePackage
                             : PackageErrorCode::PathInvalid,
                         site,
                         "reference '" + std::string(reference) + "' rejected: " + why,
                         "a relative '/'-separated path inside the package");
            return std::nullopt;
        }
        std::error_code ec;
        const auto status = fs::status(*path, ec);
        if (ec || !fs::exists(status)) {
            issues.Error(PackageErrorCode::FileMissing, site, "file does not exist");
            return std::nullopt;
        }
        if (!fs::is_regular_file(status)) {
            issues.Error(PackageErrorCode::FileUnreadable, site, "not a regular file");
            return std::nullopt;
        }
        const auto size = fs::file_size(*path, ec);
        if (ec) {
            issues.Error(PackageErrorCode::FileUnreadable, site, "cannot stat: " + ec.message());
            return std::nullopt;
        }
        if (size > max_bytes) {
            issues.Error(PackageErrorCode::FileTooLarge,
                         site,
                         "file is " + std::to_string(size) + " bytes",
                         "<= " + std::to_string(max_bytes) + " bytes");
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        std::ifstream file(*path, std::ios::binary);
        if (!file) {
            issues.Error(PackageErrorCode::FileUnreadable, site, "cannot open for reading");
            return std::nullopt;
        }
        if (!bytes.empty()) {
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file || file.gcount() != static_cast<std::streamsize>(bytes.size())) {
                issues.Error(PackageErrorCode::FileUnreadable, site, "short read");
                return std::nullopt;
            }
        }
        ++report.files_read;
        report.bytes_read += bytes.size();
        return bytes;
    }

    // Size of a package file without reading it (streaming startup check).
    // Same reference/containment rules and issues as Read.
    std::optional<std::uint64_t> Stat(std::string_view reference, IssueSite site)
    {
        site.file = std::string(reference);
        std::string why;
        const auto path = ResolvePackageReference(root, reference, why);
        if (!path) {
            issues.Error(why.find("outside") != std::string::npos
                             ? PackageErrorCode::PathOutsidePackage
                             : PackageErrorCode::PathInvalid,
                         site,
                         "reference '" + std::string(reference) + "' rejected: " + why,
                         "a relative '/'-separated path inside the package");
            return std::nullopt;
        }
        std::error_code ec;
        const auto status = fs::status(*path, ec);
        if (ec || !fs::exists(status)) {
            issues.Error(PackageErrorCode::FileMissing, site, "file does not exist");
            return std::nullopt;
        }
        if (!fs::is_regular_file(status)) {
            issues.Error(PackageErrorCode::FileUnreadable, site, "not a regular file");
            return std::nullopt;
        }
        const auto size = fs::file_size(*path, ec);
        if (ec) {
            issues.Error(PackageErrorCode::FileUnreadable, site, "cannot stat: " + ec.message());
            return std::nullopt;
        }
        return size;
    }

    // Existence probe for optional/legacy files (no issue when absent).
    bool Exists(std::string_view reference)
    {
        std::string why;
        const auto path = ResolvePackageReference(root, reference, why);
        std::error_code ec;
        return path && fs::is_regular_file(*path, ec);
    }
};

// ---- manifest ----------------------------------------------------------------

bool IsChunkSectionKind(LayerKind kind) noexcept
{
    return kind == LayerKind::Height || kind == LayerKind::Attributes ||
           kind == LayerKind::SplatA || kind == LayerKind::SplatB;
}

bool IsValidWorldId(const std::string& id) noexcept
{
    if (id.empty() || id.size() > 64) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.';
    });
}

std::string ChunkFileName(std::uint32_t x, std::uint32_t y)
{
    return "chunks/chunk_" + std::to_string(x) + "_" + std::to_string(y) + ".mxchunk";
}

LayerInfo MakeLayer(LayerKind kind, bool required, LayerAudience audience, std::string file)
{
    LayerInfo layer;
    layer.kind = kind;
    layer.raw_kind = static_cast<std::uint16_t>(kind);
    layer.required = required;
    layer.audience = audience;
    layer.version = kLayerEncodingVersion;
    layer.file = std::move(file);
    layer.implied = true;
    return layer;
}

// `eager`: the whole terrain will be resident -> the resident-sample cap
// applies (checked here, before the chunk index, as the size limit it is).
bool ParseManifest(const std::vector<std::uint8_t>& bytes, PackageManifest& out, Issues& issues, bool eager)
{
    const IssueSite site{"manifest", kManifestFile};
    if (bytes.empty() || bytes.size() % sizeof(capnp::word) != 0) {
        issues.Error(PackageErrorCode::ManifestCorrupt,
                     site,
                     "size " + std::to_string(bytes.size()) + " is not a whole number of 8-byte words",
                     "a Cap'n Proto message (unpacked, with segment table)");
        return false;
    }
    // Copy into word-aligned storage; bounded traversal (<= 4x the message).
    std::vector<capnp::word> words(bytes.size() / sizeof(capnp::word));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    capnp::ReaderOptions options;
    options.traversalLimitInWords = std::max<std::uint64_t>(64, words.size() * 4);
    options.nestingLimit = 16;
    const std::size_t errors_before = issues.ErrorCount();
    try {
        capnp::FlatArrayMessageReader reader(kj::ArrayPtr<const capnp::word>(words.data(), words.size()),
                                             options);
        if (reader.getEnd() != words.data() + words.size()) {
            issues.Error(PackageErrorCode::ManifestCorrupt,
                         site,
                         "trailing bytes after the message",
                         "exactly one message");
            return false;
        }
        const auto root = reader.getRoot<schema::MapManifest>();
        out.format_version = root.getFormatVersion();
        if (out.format_version != kManifestVersionLegacy &&
            out.format_version != kManifestVersionCurrent) {
            issues.Error(PackageErrorCode::ManifestVersionUnsupported,
                         IssueSite{"manifest", kManifestFile, "formatVersion"},
                         "formatVersion " + std::to_string(out.format_version),
                         "2 (legacy, read-only) or 3");
            return false;
        }
        const bool v3 = out.format_version == kManifestVersionCurrent;
        out.world_id = root.getWorldId().cStr();
        out.world_name = root.getWorldName().cStr();
        issues.SetPackage(out.world_id.empty() ? std::string("?") : out.world_id);
        if (!IsValidWorldId(out.world_id)) {
            issues.Error(PackageErrorCode::ManifestFieldInvalid,
                         IssueSite{"manifest", kManifestFile, "worldId"},
                         "worldId '" + out.world_id + "'",
                         "1..64 chars of [A-Za-z0-9_.-]");
        }
        if (out.world_name.size() > 256) {
            issues.Error(PackageErrorCode::ManifestFieldInvalid,
                         IssueSite{"manifest", kManifestFile, "worldName"},
                         "worldName is " + std::to_string(out.world_name.size()) + " bytes",
                         "<= 256 bytes");
        }
        if (static_cast<std::uint16_t>(root.getHeightUnit()) !=
            static_cast<std::uint16_t>(schema::HeightUnit::CENTIMETERS)) {
            issues.Error(PackageErrorCode::ManifestFieldInvalid,
                         IssueSite{"manifest", kManifestFile, "heightUnit"},
                         "height unit " + std::to_string(static_cast<unsigned>(root.getHeightUnit())),
                         "centimeters (0)");
        }
        out.cell_size_m = root.getCellSizeMeters();
        if (!std::isfinite(out.cell_size_m) || out.cell_size_m < 0.05f || out.cell_size_m > 1000.0f) {
            issues.Error(PackageErrorCode::ManifestFieldInvalid,
                         IssueSite{"manifest", kManifestFile, "cellSizeMeters"},
                         "cellSizeMeters " + Str(out.cell_size_m),
                         "finite, 0.05 .. 1000");
        }
        out.size_cells_x = root.getWorldSizeCells();
        out.chunk_size_cells = root.getChunkSizeCells();
        if (out.size_cells_x == 0 || out.size_cells_x > kMaxWorldSizeCells) {
            issues.Error(PackageErrorCode::ManifestFieldInvalid,
                         IssueSite{"manifest", kManifestFile, "worldSizeCells"},
                         "worldSizeCells " + std::to_string(out.size_cells_x),
                         "1 .. " + std::to_string(kMaxWorldSizeCells));
        }
        if (out.chunk_size_cells == 0 || out.chunk_size_cells > kMaxChunkSizeCells) {
            issues.Error(PackageErrorCode::ManifestFieldInvalid,
                         IssueSite{"manifest", kManifestFile, "chunkSizeCells"},
                         "chunkSizeCells " + std::to_string(out.chunk_size_cells),
                         "1 .. " + std::to_string(kMaxChunkSizeCells));
        }
        out.texture_palette_entries = root.getTexturePalette().size();

        if (!v3) {
            // ---- v2 legacy transition rule ----
            if (root.getWorldSizeCellsY() != 0 || root.hasOrigin() || root.hasChunkGrid() ||
                root.hasLayers() || root.hasChunks() || root.hasHeightEncoding() || root.hasWater()) {
                issues.Error(PackageErrorCode::ManifestFieldForbidden,
                             IssueSite{"manifest", kManifestFile, "@12..@18"},
                             "a formatVersion 2 manifest carries v3 fields (ambiguous)",
                             "formatVersion 3, or no v3 fields");
            }
            out.size_cells_y = out.size_cells_x;
            const auto zx = root.getZoneGridDims().getX();
            const auto zy = root.getZoneGridDims().getY();
            if (out.chunk_size_cells != 0) {
                const std::uint32_t grid =
                    (out.size_cells_x + out.chunk_size_cells - 1) / out.chunk_size_cells;
                if (zx != grid || zy != grid) {
                    issues.Warning(PackageErrorCode::LegacyFieldIgnored,
                                   IssueSite{"manifest", kManifestFile, "zoneGridDims"},
                                   "zoneGridDims " + std::to_string(zx) + "x" + std::to_string(zy) +
                                       " differs from the chunk grid " + std::to_string(grid) + "x" +
                                       std::to_string(grid) +
                                       "; the legacy loader would enumerate chunks by it",
                                   "ignored: chunks come from ceil(worldSizeCells / chunkSizeCells)");
                } else {
                    issues.Info(PackageErrorCode::LegacyFieldIgnored,
                                IssueSite{"manifest", kManifestFile, "zoneGridDims"},
                                "zoneGridDims " + std::to_string(zx) + "x" + std::to_string(zy) +
                                    " / zoneSizeCells " + std::to_string(root.getZoneSizeCells()) +
                                    " ignored (not chunk or server-zone information)");
                }
            }
            const std::string environment = root.getEnvironmentFile().cStr();
            if (!environment.empty()) {
                issues.Info(PackageErrorCode::LegacyFieldIgnored,
                            IssueSite{"manifest", kManifestFile, "environmentFile"},
                            "environmentFile '" + environment + "' ignored (no consumer)");
            }
            std::string logic_file = root.getWorldLogicFile().cStr();
            if (logic_file.empty()) {
                logic_file = kLegacyWorldLogicFile;
            }
            out.layers.push_back(MakeLayer(LayerKind::Height, true, LayerAudience::Shared, {}));
            out.layers.push_back(MakeLayer(LayerKind::Attributes, true, LayerAudience::Shared, {}));
            out.layers.push_back(MakeLayer(LayerKind::SplatA, false, LayerAudience::Client, {}));
            out.layers.push_back(MakeLayer(LayerKind::SplatB, false, LayerAudience::Client, {}));
            out.layers.push_back(
                MakeLayer(LayerKind::WorldLogic, true, LayerAudience::Shared, std::move(logic_file)));
            out.layers.push_back(
                MakeLayer(LayerKind::MobSpawns, false, LayerAudience::Server, kLegacySpawnFile));
        } else {
            // ---- v3 ----
            if (root.getZoneGridDims().getX() != 0 || root.getZoneGridDims().getY() != 0 ||
                root.getZoneSizeCells() != 0) {
                issues.Error(PackageErrorCode::ManifestFieldForbidden,
                             IssueSite{"manifest", kManifestFile, "zoneGridDims/zoneSizeCells"},
                             "server-zone fields are set",
                             "0 (the map format has no server-zone concept)");
            }
            if (root.getWorldLogicFile().size() != 0 || root.getEnvironmentFile().size() != 0) {
                issues.Error(PackageErrorCode::ManifestFieldForbidden,
                             IssueSite{"manifest", kManifestFile, "worldLogicFile/environmentFile"},
                             "legacy file references are set",
                             "empty (v3 references come from `layers`)");
            }
            // MAP-2: independent X/Y sizes, any finite origin (negative
            // included) inside the f32-precise coordinate range.
            out.size_cells_y = root.getWorldSizeCellsY();
            if (out.size_cells_y == 0 || out.size_cells_y > kMaxWorldSizeCells) {
                issues.Error(PackageErrorCode::ManifestFieldInvalid,
                             IssueSite{"manifest", kManifestFile, "worldSizeCellsY"},
                             "worldSizeCellsY " + std::to_string(out.size_cells_y),
                             "1 .. " + std::to_string(kMaxWorldSizeCells));
            }
            out.origin_x = root.getOrigin().getX();
            out.origin_y = root.getOrigin().getY();
            if (!std::isfinite(out.origin_x) || !std::isfinite(out.origin_y)) {
                issues.Error(PackageErrorCode::ManifestFieldInvalid,
                             IssueSite{"manifest", kManifestFile, "origin"},
                             "non-finite origin",
                             "finite coordinates");
            }
            out.chunk_grid_x = root.getChunkGrid().getX();
            out.chunk_grid_y = root.getChunkGrid().getY();

            const auto layers = root.getLayers();
            if (layers.size() > kMaxLayerDecls) {
                issues.Error(PackageErrorCode::LayerDeclInvalid,
                             IssueSite{"manifest", kManifestFile, "layers"},
                             std::to_string(layers.size()) + " layer declarations",
                             "<= " + std::to_string(kMaxLayerDecls));
            } else {
                std::set<std::uint16_t> seen;
                for (std::uint32_t i = 0; i < layers.size(); ++i) {
                    const auto decl = layers[i];
                    const IssueSite lsite{"manifest", kManifestFile, "layers[" + std::to_string(i) + "]"};
                    LayerInfo layer;
                    layer.raw_kind = static_cast<std::uint16_t>(decl.getKind());
                    layer.kind = static_cast<LayerKind>(layer.raw_kind);
                    layer.required = decl.getRequired();
                    const auto raw_audience = static_cast<std::uint16_t>(decl.getAudience());
                    layer.audience = static_cast<LayerAudience>(std::min<std::uint16_t>(raw_audience, 2));
                    layer.version = decl.getVersion();
                    layer.file = decl.getFile().cStr();
                    if (!seen.insert(layer.raw_kind).second) {
                        issues.Error(PackageErrorCode::LayerDuplicate,
                                     lsite,
                                     "layer kind " + std::to_string(layer.raw_kind) + " declared twice",
                                     "each kind at most once");
                        continue;
                    }
                    if (layer.raw_kind > static_cast<std::uint16_t>(LayerKind::WaterBodies)) {
                        if (layer.required) {
                            issues.Error(PackageErrorCode::LayerUnsupported,
                                         lsite,
                                         "unknown required layer kind " + std::to_string(layer.raw_kind),
                                         "a kind this reader supports (0..6)");
                        } else {
                            issues.Info(PackageErrorCode::LayerSkipped,
                                        lsite,
                                        "unknown optional layer kind " +
                                            std::to_string(layer.raw_kind) + " skipped");
                        }
                        continue;
                    }
                    if (raw_audience > 2) {
                        issues.Error(PackageErrorCode::LayerDeclInvalid,
                                     lsite,
                                     "audience " + std::to_string(raw_audience),
                                     "server (0), client (1) or shared (2)");
                        continue;
                    }
                    const bool version_ok =
                        layer.kind == LayerKind::Height
                            ? layer.version >= 1 && layer.version <= kHeightLayerVersionMax
                            : layer.kind == LayerKind::MobSpawns
                                ? layer.version >= 1 && layer.version <= 2
                                : layer.version == kLayerEncodingVersion;
                    if (!version_ok) {
                        if (layer.required) {
                            issues.Error(PackageErrorCode::LayerUnsupported,
                                         lsite,
                                         std::string(ToString(layer.kind)) + " version " +
                                             std::to_string(layer.version),
                                         layer.kind == LayerKind::Height
                                             ? "version 1..." + std::to_string(kHeightLayerVersionMax)
                                             : "version " + std::to_string(kLayerEncodingVersion));
                        } else {
                            issues.Warning(PackageErrorCode::LayerSkipped,
                                           lsite,
                                           std::string("optional ") + ToString(layer.kind) +
                                               " version " + std::to_string(layer.version) +
                                               " unsupported; skipped");
                        }
                        continue;
                    }
                    const bool chunk_kind = IsChunkSectionKind(layer.kind);
                    if (chunk_kind != layer.file.empty()) {
                        issues.Error(PackageErrorCode::LayerDeclInvalid,
                                     lsite,
                                     std::string(ToString(layer.kind)) +
                                         (chunk_kind ? " is a chunk section but names a file"
                                                     : " is a file layer without a file"),
                                     chunk_kind ? "empty file" : "a package-relative file");
                        continue;
                    }
                    const bool server_data = layer.kind == LayerKind::Height ||
                                             layer.kind == LayerKind::Attributes ||
                                             layer.kind == LayerKind::WorldLogic ||
                                             layer.kind == LayerKind::MobSpawns ||
                                             layer.kind == LayerKind::WaterBodies;
                    if (server_data && layer.audience == LayerAudience::Client) {
                        issues.Error(PackageErrorCode::LayerDeclInvalid,
                                     lsite,
                                     std::string(ToString(layer.kind)) + " declared client-only",
                                     "server or shared audience");
                        continue;
                    }
                    out.layers.push_back(std::move(layer));
                }
            }
            const auto chunks = root.getChunks();
            if (chunks.size() > static_cast<std::uint64_t>(kMaxWorldSizeCells) * 4) {
                issues.Error(PackageErrorCode::ChunkIndexInvalid,
                             IssueSite{"chunkIndex", kManifestFile, "chunks"},
                             std::to_string(chunks.size()) + " chunk entries",
                             "one per chunk grid cell");
            } else {
                out.chunks.reserve(chunks.size());
                for (const auto chunk : chunks) {
                    ChunkEntry entry;
                    entry.x = chunk.getX();
                    entry.y = chunk.getY();
                    entry.file = chunk.getFile().cStr();
                    entry.byte_size = chunk.getByteSize();
                    entry.crc32 = chunk.getCrc32();
                    out.chunks.push_back(std::move(entry));
                }
            }
            // ---- height encoding (height layer version 2 = range extension) ----
            const LayerInfo* height_layer = out.FindLayer(LayerKind::Height);
            const IssueSite esite{"manifest", kManifestFile, "heightEncoding"};
            if (height_layer != nullptr && height_layer->version == 2) {
                if (!root.hasHeightEncoding()) {
                    issues.Error(PackageErrorCode::ManifestFieldInvalid, esite,
                                 "height layer version 2 without heightEncoding",
                                 "sampleType, metersPerUnit, offsetMeters");
                } else {
                    const auto enc = root.getHeightEncoding();
                    const auto sample_type = static_cast<std::uint16_t>(enc.getSampleType());
                    out.height_encoding.layer_version = 2;
                    out.height_encoding.int32_samples =
                        sample_type == static_cast<std::uint16_t>(schema::HeightSampleType::INT32);
                    out.height_encoding.meters_per_unit = enc.getMetersPerUnit();
                    out.height_encoding.offset_m = enc.getOffsetMeters();
                    if (sample_type > static_cast<std::uint16_t>(schema::HeightSampleType::INT32)) {
                        issues.Error(PackageErrorCode::ManifestFieldInvalid, esite,
                                     "sampleType " + std::to_string(sample_type), "int16 (0) or int32 (1)");
                    }
                    const double unit = out.height_encoding.meters_per_unit;
                    const double offset = out.height_encoding.offset_m;
                    if (!std::isfinite(unit) || unit < 1e-4 || unit > 10.0) {
                        issues.Error(PackageErrorCode::ManifestFieldInvalid, esite,
                                     "metersPerUnit " + Str(unit), "finite, 0.0001 .. 10");
                    }
                    if (!std::isfinite(offset) || std::abs(offset) > 1.0e5) {
                        issues.Error(PackageErrorCode::ManifestFieldInvalid, esite,
                                     "offsetMeters " + Str(offset), "finite, |offset| <= 100000");
                    }
                }
            } else if (root.hasHeightEncoding()) {
                issues.Error(PackageErrorCode::ManifestFieldForbidden, esite,
                             "heightEncoding set for height layer version 1",
                             "absent (version 1 is int16 centimeters)");
            }
            // ---- water capability (MAP-3) ----
            if (root.hasWater()) {
                const auto water = root.getWater();
                const auto raw_model = static_cast<std::uint16_t>(water.getModel());
                const IssueSite wsite{"manifest", kManifestFile, "water"};
                if (raw_model > static_cast<std::uint16_t>(schema::WaterModel::BODIES)) {
                    issues.Error(PackageErrorCode::ManifestFieldInvalid, wsite, "water model " + std::to_string(raw_model),
                                 "undeclared (0), none (1), seaLevel (2) or bodies (3)");
                } else {
                    out.water_model = static_cast<WaterModel>(raw_model);
                    out.sea_level_m = water.getSeaLevelMeters();
                    if (out.water_model == WaterModel::SeaLevel &&
                        (!std::isfinite(out.sea_level_m) || std::abs(out.sea_level_m) > 1.0e5)) {
                        issues.Error(PackageErrorCode::ManifestFieldInvalid, wsite,
                                     "seaLevelMeters " + Str(out.sea_level_m), "finite, |sea level| <= 100000");
                    }
                }
            }
        }
    } catch (const kj::Exception& error) {
        issues.Error(PackageErrorCode::ManifestCorrupt,
                     site,
                     std::string("Cap'n Proto decode failed: ") + error.getDescription().cStr(),
                     "a valid MapManifest message");
        return false;
    } catch (const std::exception& error) {
        issues.Error(PackageErrorCode::ManifestCorrupt, site, std::string("decode failed: ") + error.what());
        return false;
    }
    if (issues.ErrorCount() != errors_before) {
        return false;
    }

    // ---- geometry shared by both versions ----
    const std::uint32_t grid_x = (out.size_cells_x + out.chunk_size_cells - 1) / out.chunk_size_cells;
    const std::uint32_t grid_y = (out.size_cells_y + out.chunk_size_cells - 1) / out.chunk_size_cells;
    const bool partial = out.size_cells_x % out.chunk_size_cells != 0 || out.size_cells_y % out.chunk_size_cells != 0;
    if (partial && out.format_version == kManifestVersionLegacy) {
        // v2 edge chunks were always written full-size; their out-of-world
        // samples have no defined meaning, so v2 stays whole-chunk only.
        issues.Error(PackageErrorCode::UnsupportedFeature,
                     IssueSite{"manifest", kManifestFile, "chunkSizeCells"},
                     "formatVersion 2 world " + std::to_string(out.size_cells_x) +
                         " cells is not a whole number of " + std::to_string(out.chunk_size_cells) +
                         "-cell chunks",
                     "v3 for partial edge chunks (they store only their in-world cells)");
        return false;
    }
    {
        // Every world coordinate within the f32-precise range.
        const double coords[4] = {out.origin_x, out.origin_y, out.origin_x + out.ExtentX(),
                                  out.origin_y + out.ExtentY()};
        for (const double c : coords) {
            if (std::abs(c) > kMaxWorldCoordinate) {
                issues.Error(PackageErrorCode::UnsupportedFeature,
                             IssueSite{"manifest", kManifestFile, "origin/worldSize"},
                             "world spans (" + Str(out.origin_x) + "," + Str(out.origin_y) + ")-(" +
                                 Str(coords[2]) + "," + Str(coords[3]) + ") m",
                             "every coordinate within +-" + Str(kMaxWorldCoordinate) +
                                 " m (f32 runtime positions, step <= 1/64 m)");
                return false;
            }
        }
    }
    if (out.format_version == kManifestVersionCurrent) {
        if (out.chunk_grid_x != grid_x || out.chunk_grid_y != grid_y) {
            issues.Error(PackageErrorCode::ManifestFieldInvalid,
                         IssueSite{"manifest", kManifestFile, "chunkGrid"},
                         "chunkGrid " + std::to_string(out.chunk_grid_x) + "x" +
                             std::to_string(out.chunk_grid_y),
                         std::to_string(grid_x) + "x" + std::to_string(grid_y) +
                             " = ceil(worldSizeCells / chunkSizeCells)");
            return false;
        }
    }
    out.chunk_grid_x = grid_x;
    out.chunk_grid_y = grid_y;
    // The chunk index is the per-package metadata the server always keeps
    // (streaming too): bounded. The resident-sample cap applies to EAGER
    // residency only (LoadServerWorld); streaming bounds residency by its
    // memory budget instead.
    const std::uint64_t samples =
        (static_cast<std::uint64_t>(out.size_cells_x) + 1) * (static_cast<std::uint64_t>(out.size_cells_y) + 1);
    if (eager && samples > kMaxResidentCells) {
        issues.Error(PackageErrorCode::ManifestSizeOverflow, IssueSite{"manifest", kManifestFile, "worldSizeCells"},
                     std::to_string(samples) + " height samples",
                     "<= " + std::to_string(kMaxResidentCells) +
                         " for eager (whole-world) residency; larger worlds need terrain_residency=streaming");
        return false;
    }
    const std::uint64_t chunk_count = static_cast<std::uint64_t>(grid_x) * grid_y;
    if (chunk_count > kMaxChunkCount) {
        issues.Error(PackageErrorCode::ManifestSizeOverflow,
                     IssueSite{"manifest", kManifestFile, "chunkGrid"},
                     std::to_string(chunk_count) + " chunks",
                     "<= " + std::to_string(kMaxChunkCount) + " (chunk index size)");
        return false;
    }
    const std::uint64_t chunk_bytes =
        (static_cast<std::uint64_t>(out.chunk_size_cells) + 1) * (out.chunk_size_cells + 1) *
            (out.height_encoding.int32_samples ? 4u : 2u) +
        static_cast<std::uint64_t>(out.chunk_size_cells) * out.chunk_size_cells * 2;
    if (chunk_bytes > kMaxChunkFileBytes) {
        issues.Error(PackageErrorCode::ManifestSizeOverflow,
                     IssueSite{"manifest", kManifestFile, "chunkSizeCells"},
                     "a chunk would need " + std::to_string(chunk_bytes) + " bytes of server data",
                     "<= " + std::to_string(kMaxChunkFileBytes));
        return false;
    }

    if (out.format_version == kManifestVersionLegacy) {
        out.chunks.clear();
        for (std::uint32_t y = 0; y < grid_y; ++y) {
            for (std::uint32_t x = 0; x < grid_x; ++x) {
                out.chunks.push_back(
                    ChunkEntry{x, y, out.chunk_size_cells, out.chunk_size_cells, ChunkFileName(x, y), 0, std::nullopt});
            }
        }
        issues.Info(PackageErrorCode::ChunkIntegrityUnavailable,
                    IssueSite{"chunkIndex", kManifestFile},
                    "formatVersion 2 has no chunk index: sizes/checksums cannot be verified");
    } else {
        // Exactly one entry per grid cell; row-major order after sorting.
        const std::uint64_t expected = static_cast<std::uint64_t>(grid_x) * grid_y;
        std::set<std::pair<std::uint32_t, std::uint32_t>> cells;
        std::set<std::string> files;
        bool index_ok = out.chunks.size() == expected;
        if (!index_ok) {
            issues.Error(PackageErrorCode::ChunkIndexInvalid,
                         IssueSite{"chunkIndex", kManifestFile, "chunks"},
                         std::to_string(out.chunks.size()) + " chunk entries",
                         std::to_string(expected) + " (one per grid cell)");
        }
        for (std::size_t i = 0; i < out.chunks.size(); ++i) {
            const auto& c = out.chunks[i];
            IssueSite csite{"chunkIndex", kManifestFile, "chunks[" + std::to_string(i) + "]"};
            csite.chunk_x = static_cast<std::int32_t>(c.x);
            csite.chunk_y = static_cast<std::int32_t>(c.y);
            if (c.x >= grid_x || c.y >= grid_y) {
                issues.Error(PackageErrorCode::ChunkIndexInvalid, csite, "chunk outside the grid",
                             "x < " + std::to_string(grid_x) + ", y < " + std::to_string(grid_y));
                index_ok = false;
            } else if (!cells.insert({c.x, c.y}).second) {
                issues.Error(PackageErrorCode::ChunkIndexInvalid, csite, "duplicate chunk cell",
                             "each grid cell exactly once");
                index_ok = false;
            }
            if (c.file.empty() || !files.insert(c.file).second) {
                issues.Error(PackageErrorCode::ChunkIndexInvalid, csite,
                             c.file.empty() ? "empty chunk file" : "chunk file '" + c.file + "' listed twice",
                             "a distinct file per chunk");
                index_ok = false;
            }
            if (c.byte_size == 0 || c.byte_size > kMaxChunkFileBytes) {
                issues.Error(PackageErrorCode::ChunkIndexInvalid, csite,
                             "byteSize " + std::to_string(c.byte_size),
                             "1 .. " + std::to_string(kMaxChunkFileBytes));
                index_ok = false;
            }
        }
        if (!index_ok) {
            return false;
        }
        std::sort(out.chunks.begin(), out.chunks.end(), [](const ChunkEntry& a, const ChunkEntry& b) {
            return a.y != b.y ? a.y < b.y : a.x < b.x;
        });
        const GridGeometry geometry = out.Geometry();
        for (auto& c : out.chunks) {
            c.cells_x = geometry.ChunkCellsX(c.x);
            c.cells_y = geometry.ChunkCellsY(c.y);
        }
    }
    return true;
}

// ---- chunks ------------------------------------------------------------------

struct ChunkPlan {
    bool splat_declared = false; // v3: declared => present in every chunk
    bool strict_v3 = false;
    bool decode_client = false;  // Full depth
    HeightEncoding encoding;     // height element format + storage
};

struct DecodedChunk {
    std::uint32_t splat_w = 0;
    std::uint32_t splat_h = 0;
    bool has_splat_a = false;
    bool has_splat_b = false;
};

std::uint8_t ExpectedElementFormat(std::uint16_t type, const HeightEncoding& encoding) noexcept
{
    switch (type) {
    case kSectionHeight:
        return encoding.int32_samples ? kElementInt32 : kElementInt16;
    case kSectionAttributes:
        return kElementU16Bitfield;
    case kSectionSplatA:
    case kSectionSplatB:
        return kElementRgba8Image;
    default:
        return 0;
    }
}

const char* SectionLayerName(std::uint16_t type) noexcept
{
    switch (type) {
    case kSectionHeight:
        return "height";
    case kSectionAttributes:
        return "attributes";
    case kSectionSplatA:
        return "splatA";
    case kSectionSplatB:
        return "splatB";
    default:
        return "chunk";
    }
}

// Decodes one chunk file into `out` (its own samples; partial edge chunks
// store only their in-world cells). Returns false on any Error (reported).
bool DecodeChunk(const std::vector<std::uint8_t>& bytes,
                 const ChunkEntry& entry,
                 const PackageManifest& manifest,
                 const ChunkPlan& plan,
                 TerrainChunk& out,
                 DecodedChunk& decoded,
                 Issues& issues)
{
    const std::uint32_t n = manifest.chunk_size_cells;
    const std::uint32_t cells_x = entry.cells_x;
    const std::uint32_t cells_y = entry.cells_y;
    IssueSite site{"chunk", entry.file};
    site.chunk_x = static_cast<std::int32_t>(entry.x);
    site.chunk_y = static_cast<std::int32_t>(entry.y);
    auto at = [&](const char* layer, const std::string& field, std::int64_t offset) {
        IssueSite s = site;
        s.layer = layer;
        s.field = field;
        s.offset = offset;
        return s;
    };
    const std::size_t errors_before = issues.ErrorCount();
    if (bytes.size() < kChunkHeaderBytes) {
        issues.Error(PackageErrorCode::ChunkHeaderInvalid, at("chunk", "header", 0),
                     "file is " + std::to_string(bytes.size()) + " bytes",
                     ">= " + std::to_string(kChunkHeaderBytes) + " (header)");
        return false;
    }
    const auto magic = LoadU32(bytes.data());
    const auto version = LoadU16(bytes.data() + 4);
    const auto hx = LoadU16(bytes.data() + 6);
    const auto hy = LoadU16(bytes.data() + 8);
    const auto cells = LoadU16(bytes.data() + 10);
    const auto sections = LoadU16(bytes.data() + 12);
    if (magic != kChunkFileMagic) {
        issues.Error(PackageErrorCode::ChunkHeaderInvalid, at("chunk", "magic", 0), "bad magic",
                     "0x3143584d (\"MXC1\")");
        return false;
    }
    if (version != kChunkFileVersion) {
        issues.Error(PackageErrorCode::ChunkHeaderInvalid, at("chunk", "version", 4),
                     "container version " + std::to_string(version), std::to_string(kChunkFileVersion));
        return false;
    }
    if (hx != entry.x || hy != entry.y) {
        issues.Error(PackageErrorCode::ChunkHeaderInvalid, at("chunk", "x/y", 6),
                     "header says chunk (" + std::to_string(hx) + "," + std::to_string(hy) + ")",
                     "(" + std::to_string(entry.x) + "," + std::to_string(entry.y) + ")");
    }
    // cellsPerSide is the nominal chunk pitch, also for a partial edge chunk
    // (its real extent follows from the manifest geometry).
    if (cells != n) {
        issues.Error(PackageErrorCode::ChunkHeaderInvalid, at("chunk", "cellsPerSide", 10),
                     "cellsPerSide " + std::to_string(cells), std::to_string(n) + " (chunkSizeCells)");
    }
    if (sections == 0 || sections > kMaxChunkSections) {
        issues.Error(PackageErrorCode::ChunkTocInvalid, at("chunk", "sectionCount", 12),
                     "sectionCount " + std::to_string(sections),
                     "1 .. " + std::to_string(kMaxChunkSections));
        return false;
    }
    const std::size_t toc_end = kChunkHeaderBytes + static_cast<std::size_t>(sections) * kChunkTocEntryBytes;
    if (bytes.size() < toc_end) {
        issues.Error(PackageErrorCode::ChunkTocInvalid, at("chunk", "toc", kChunkHeaderBytes),
                     "file ends inside the section table", ">= " + std::to_string(toc_end) + " bytes");
        return false;
    }
    if (issues.ErrorCount() != errors_before) {
        return false;
    }

    struct Section {
        std::uint16_t type = 0;
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        std::size_t toc_offset = 0;
    };
    std::vector<Section> found;
    std::set<std::uint16_t> seen_types;
    for (std::uint16_t i = 0; i < sections; ++i) {
        const std::size_t toc = kChunkHeaderBytes + static_cast<std::size_t>(i) * kChunkTocEntryBytes;
        Section s;
        s.type = LoadU16(bytes.data() + toc);
        const std::uint8_t element = bytes[toc + 2];
        const std::uint8_t reserved = bytes[toc + 3];
        s.offset = LoadU32(bytes.data() + toc + 4);
        s.length = LoadU32(bytes.data() + toc + 8);
        s.toc_offset = toc;
        const std::string field = "toc[" + std::to_string(i) + "]";
        const std::uint8_t expected_element = ExpectedElementFormat(s.type, plan.encoding);
        if (expected_element == 0) {
            if (plan.strict_v3) {
                issues.Error(PackageErrorCode::ChunkTocInvalid, at("chunk", field, static_cast<std::int64_t>(toc)),
                             "unknown section type " + std::to_string(s.type), "1..4 (declared layers)");
            } else {
                issues.Warning(PackageErrorCode::ChunkTocInvalid, at("chunk", field, static_cast<std::int64_t>(toc)),
                               "unknown section type " + std::to_string(s.type) + " ignored (v2)");
            }
        } else {
            if (!seen_types.insert(s.type).second) {
                issues.Error(PackageErrorCode::ChunkTocInvalid,
                             at(SectionLayerName(s.type), field, static_cast<std::int64_t>(toc)),
                             "section type " + std::to_string(s.type) + " appears twice", "at most once");
            }
            if (element != expected_element) {
                issues.Error(PackageErrorCode::ChunkTocInvalid,
                             at(SectionLayerName(s.type), field + ".elementFormat", static_cast<std::int64_t>(toc + 2)),
                             "elementFormat " + std::to_string(element), std::to_string(expected_element));
            }
            if (plan.strict_v3 && reserved != 0) {
                issues.Error(PackageErrorCode::ChunkTocInvalid,
                             at(SectionLayerName(s.type), field + ".reserved", static_cast<std::int64_t>(toc + 3)),
                             "reserved byte " + std::to_string(reserved), "0");
            }
            if (plan.strict_v3 && (s.type == kSectionSplatA || s.type == kSectionSplatB) &&
                !plan.splat_declared) {
                issues.Error(PackageErrorCode::ChunkTocInvalid,
                             at(SectionLayerName(s.type), field, static_cast<std::int64_t>(toc)),
                             "splat section present but no splat layer is declared",
                             "declared layers only");
            }
        }
        if (s.offset < toc_end || s.offset + s.length > bytes.size()) {
            issues.Error(PackageErrorCode::ChunkSectionRange,
                         at(SectionLayerName(s.type), field, static_cast<std::int64_t>(toc + 4)),
                         "section bytes [" + std::to_string(s.offset) + ", " +
                             std::to_string(s.offset + s.length) + ")",
                         "inside [" + std::to_string(toc_end) + ", " + std::to_string(bytes.size()) + ")");
            continue;
        }
        found.push_back(s);
    }
    if (issues.ErrorCount() != errors_before) {
        return false;
    }
    // Every byte after the table belongs to exactly one section.
    std::vector<Section> ordered = found;
    std::sort(ordered.begin(), ordered.end(), [](const Section& a, const Section& b) {
        return a.offset < b.offset;
    });
    std::uint64_t cursor = toc_end;
    for (const auto& s : ordered) {
        if (s.offset != cursor) {
            issues.Error(PackageErrorCode::ChunkSectionRange,
                         at(SectionLayerName(s.type), "layout", static_cast<std::int64_t>(std::min(s.offset, cursor))),
                         s.offset < cursor ? "sections overlap" : "unaccounted bytes between sections",
                         "contiguous, non-overlapping sections");
            return false;
        }
        cursor = s.offset + s.length;
    }
    if (cursor != bytes.size()) {
        issues.Error(PackageErrorCode::ChunkSectionRange, at("chunk", "layout", static_cast<std::int64_t>(cursor)),
                     std::to_string(bytes.size() - cursor) + " trailing bytes after the last section",
                     "no trailing data");
        return false;
    }

    const Section* height = nullptr;
    const Section* attr = nullptr;
    const Section* splat_a = nullptr;
    const Section* splat_b = nullptr;
    for (const auto& s : found) {
        if (s.type == kSectionHeight) {
            height = &s;
        } else if (s.type == kSectionAttributes) {
            attr = &s;
        } else if (s.type == kSectionSplatA) {
            splat_a = &s;
        } else if (s.type == kSectionSplatB) {
            splat_b = &s;
        }
    }
    if (height == nullptr) {
        issues.Error(PackageErrorCode::ChunkSectionMissing, at("height", "toc", kChunkHeaderBytes),
                     "no height section", "section type 1");
    }
    if (attr == nullptr) {
        issues.Error(PackageErrorCode::ChunkSectionMissing, at("attributes", "toc", kChunkHeaderBytes),
                     "no attribute section", "section type 3");
    }
    if (plan.strict_v3 && plan.splat_declared && (splat_a == nullptr || splat_b == nullptr)) {
        issues.Error(PackageErrorCode::ChunkSectionMissing, at("splatA", "toc", kChunkHeaderBytes),
                     "a declared splat layer is missing from this chunk", "splat A and B in every chunk");
    }
    if (issues.ErrorCount() != errors_before) {
        return false;
    }
    const std::uint64_t sample_bytes = plan.encoding.int32_samples ? 4u : 2u;
    const std::uint64_t vertices = static_cast<std::uint64_t>(cells_x + 1) * (cells_y + 1);
    if (height->length != vertices * sample_bytes) {
        issues.Error(PackageErrorCode::ChunkSectionSize,
                     at("height", "length", static_cast<std::int64_t>(height->toc_offset + 8)),
                     "height section is " + std::to_string(height->length) + " bytes",
                     std::to_string(vertices * sample_bytes) + " = (" + std::to_string(cells_x) + "+1)x(" +
                         std::to_string(cells_y) + "+1) samples x " + std::to_string(sample_bytes) + " B");
    }
    const std::uint64_t cell_count = static_cast<std::uint64_t>(cells_x) * cells_y;
    if (attr->length != cell_count * 2) {
        issues.Error(PackageErrorCode::ChunkSectionSize,
                     at("attributes", "length", static_cast<std::int64_t>(attr->toc_offset + 8)),
                     "attribute section is " + std::to_string(attr->length) + " bytes",
                     std::to_string(cell_count * 2) + " = " + std::to_string(cells_x) + "x" +
                         std::to_string(cells_y) + " cells x uint16");
    }
    if (issues.ErrorCount() != errors_before) {
        return false;
    }

    out = TerrainChunk{};
    out.x = entry.x;
    out.y = entry.y;
    out.cells_x = cells_x;
    out.cells_y = cells_y;
    const std::uint8_t* hp = bytes.data() + height->offset;
    if (plan.encoding.int32_samples) {
        out.heights32.resize(static_cast<std::size_t>(vertices));
        for (std::size_t i = 0; i < out.heights32.size(); ++i) {
            out.heights32[i] = static_cast<std::int32_t>(LoadU32(hp + i * 4));
        }
    } else {
        out.heights16.resize(static_cast<std::size_t>(vertices));
        for (std::size_t i = 0; i < out.heights16.size(); ++i) {
            out.heights16[i] = static_cast<std::int16_t>(LoadU16(hp + i * 2));
        }
    }
    out.attributes.resize(static_cast<std::size_t>(cell_count));
    std::uint64_t reserved_cells = 0;
    std::int64_t first_reserved = -1;
    const std::uint8_t* ap = bytes.data() + attr->offset;
    for (std::size_t i = 0; i < out.attributes.size(); ++i) {
        const std::uint16_t value = LoadU16(ap + i * 2);
        if ((value & kAttributeReservedMask) != 0) {
            if (first_reserved < 0) {
                first_reserved = static_cast<std::int64_t>(attr->offset + i * 2);
            }
            ++reserved_cells;
        }
        out.attributes[i] = value;
    }
    out.resident = true;
    if (reserved_cells > 0) {
        issues.Add(plan.strict_v3 ? IssueSeverity::Error : IssueSeverity::Warning,
                   PackageErrorCode::ChunkAttributeReservedBits, at("attributes", "bits 1..15", first_reserved),
                   std::to_string(reserved_cells) + " cell(s) use reserved attribute bits",
                   "only bit 0 (blocked)");
    }

    decoded.has_splat_a = splat_a != nullptr;
    decoded.has_splat_b = splat_b != nullptr;
    if (plan.decode_client) {
        auto splat_dims = [&](const Section* s, const char* layer, std::uint32_t& w, std::uint32_t& h) {
            if (s == nullptr) {
                return;
            }
            if (s->length < 4) {
                issues.Error(PackageErrorCode::ChunkSplatGeometry,
                             at(layer, "length", static_cast<std::int64_t>(s->toc_offset + 8)),
                             "splat section shorter than its 4-byte header", ">= 4 bytes");
                return;
            }
            w = LoadU16(bytes.data() + s->offset);
            h = LoadU16(bytes.data() + s->offset + 2);
            if (w == 0 || h == 0 || s->length != 4ull + static_cast<std::uint64_t>(w) * h * 4) {
                issues.Error(PackageErrorCode::ChunkSplatGeometry,
                             at(layer, "width/height", static_cast<std::int64_t>(s->offset)),
                             "splat " + std::to_string(w) + "x" + std::to_string(h) + " in " +
                                 std::to_string(s->length) + " bytes",
                             "w,h > 0 and length == 4 + w*h*4");
            }
        };
        std::uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
        splat_dims(splat_a, "splatA", aw, ah);
        splat_dims(splat_b, "splatB", bw, bh);
        if (splat_a != nullptr && splat_b != nullptr && (aw != bw || ah != bh)) {
            issues.Error(PackageErrorCode::ChunkSplatGeometry, at("splatB", "width/height", -1),
                         "splat A is " + std::to_string(aw) + "x" + std::to_string(ah) + ", B is " +
                             std::to_string(bw) + "x" + std::to_string(bh),
                         "identical A/B geometry");
        }
        decoded.splat_w = splat_a != nullptr ? aw : bw;
        decoded.splat_h = splat_a != nullptr ? ah : bh;
    }
    return issues.ErrorCount() == errors_before;
}

// Shared border samples: chunk (x, y)'s west column must equal chunk
// (x-1, y)'s east column and its south row chunk (x, y-1)'s north row -- no
// silent last-writer-wins at a seam.
void CheckChunkSeams(const GridGeometry& geometry,
                     const HeightEncoding& encoding,
                     const std::vector<TerrainChunk>& chunks,
                     const std::vector<ChunkEntry>& entries,
                     Issues& issues)
{
    auto raw = [&](const TerrainChunk& c, std::uint32_t lx, std::uint32_t ly) -> std::int64_t {
        const std::size_t i = static_cast<std::size_t>(ly) * (c.cells_x + 1) + lx;
        return encoding.int32_samples ? c.heights32[i] : c.heights16[i];
    };
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const TerrainChunk& c = chunks[index];
        if (!c.resident) {
            continue;
        }
        std::uint64_t mismatches = 0;
        const char* side = "";
        if (c.x > 0) {
            const TerrainChunk& w = chunks[index - 1];
            if (w.resident) {
                for (std::uint32_t ly = 0; ly <= c.cells_y; ++ly) {
                    mismatches += raw(c, 0, ly) != raw(w, w.cells_x, ly) ? 1u : 0u;
                }
                side = mismatches > 0 ? "west" : side;
            }
        }
        if (c.y > 0) {
            const TerrainChunk& s = chunks[index - geometry.chunks_x];
            if (s.resident) {
                std::uint64_t south = 0;
                for (std::uint32_t lx = 0; lx <= c.cells_x; ++lx) {
                    south += raw(c, lx, 0) != raw(s, lx, s.cells_y) ? 1u : 0u;
                }
                if (south > 0) {
                    side = mismatches > 0 ? "west+south" : "south";
                }
                mismatches += south;
            }
        }
        if (mismatches > 0) {
            IssueSite site{"height", entries[index].file, std::string(side) + " border"};
            site.chunk_x = static_cast<std::int32_t>(c.x);
            site.chunk_y = static_cast<std::int32_t>(c.y);
            issues.Error(PackageErrorCode::ChunkEdgeMismatch, site,
                         std::to_string(mismatches) + " shared border sample(s) differ from the neighbour chunk",
                         "identical samples on shared chunk borders");
        }
    }
}

// One chunk: read, v3 size + CRC-32 against the index, decode + validate into
// `out`. The single path for eager loading, the streaming startup set and
// runtime (streamed) loads. Errors go to `issues`; true = `out` is usable.
bool LoadChunkFile(FileAccess& files,
                   const ChunkEntry& entry,
                   const PackageManifest& manifest,
                   const ChunkPlan& plan,
                   TerrainChunk& out,
                   DecodedChunk& decoded,
                   Issues& issues)
{
    IssueSite csite{"chunk", entry.file};
    csite.chunk_x = static_cast<std::int32_t>(entry.x);
    csite.chunk_y = static_cast<std::int32_t>(entry.y);
    const auto bytes = files.Read(entry.file, kMaxChunkFileBytes, csite);
    if (!bytes) {
        return false;
    }
    if (files.report.residency == ResidencyMode::Eager) {
        ++files.report.chunks_checked; // streaming counted it in the startup size check
    }
    if (manifest.format_version == kManifestVersionCurrent) {
        if (bytes->size() != entry.byte_size) {
            issues.Error(PackageErrorCode::ChunkSizeMismatch, csite, "file is " + std::to_string(bytes->size()) + " bytes",
                         std::to_string(entry.byte_size) + " (chunk index)");
            return false;
        }
        const std::uint32_t crc = Crc32(bytes->data(), bytes->size());
        if (!entry.crc32 || crc != *entry.crc32) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "crc32 %08x", crc);
            char want[64];
            std::snprintf(want, sizeof(want), "%08x (chunk index)", entry.crc32.value_or(0));
            issues.Error(PackageErrorCode::ChunkChecksumMismatch, csite, buf, want);
            return false;
        }
    }
    if (!DecodeChunk(*bytes, entry, manifest, plan, out, decoded, issues)) {
        out.resident = false;
        return false;
    }
    ++files.report.chunks_decoded;
    return true;
}

// Runtime chunk loader of one validated package (streaming). Each Load owns
// its report/issues: no shared mutable state, safe on any thread.
class PackageChunkSource final : public ChunkSource {
public:
    PackageChunkSource(fs::path root, PackageManifest manifest, ChunkPlan plan)
        : root_(std::move(root))
        , manifest_(std::move(manifest))
        , plan_(plan)
    {
    }

    ChunkLoadResult Load(std::uint32_t chunk_index) const override
    {
        ChunkLoadResult result;
        if (chunk_index >= manifest_.chunks.size()) {
            result.code = PackageErrorCode::ChunkIndexInvalid;
            result.error = "chunk index " + std::to_string(chunk_index) + " out of range";
            return result;
        }
        PackageReport report;
        report.root = root_;
        Issues issues(report);
        issues.SetPackage(manifest_.world_id);
        FileAccess files{root_, report, issues};
        auto chunk = std::make_shared<TerrainChunk>();
        DecodedChunk decoded;
        try {
            result.ok = LoadChunkFile(files, manifest_.chunks[chunk_index], manifest_, plan_, *chunk, decoded, issues);
        } catch (const std::exception& error) {
            issues.Error(PackageErrorCode::Internal, IssueSite{"chunk", manifest_.chunks[chunk_index].file},
                         std::string("chunk load failed: ") + error.what());
            result.ok = false;
        }
        result.bytes_read = report.bytes_read;
        if (result.ok) {
            result.chunk = std::move(chunk);
        } else if (const auto* first = report.FirstError()) {
            result.code = first->code;
            result.error = first->Format();
        }
        return result;
    }

    std::uint64_t FileBytes(std::uint32_t chunk_index) const override
    {
        return chunk_index < manifest_.chunks.size() ? manifest_.chunks[chunk_index].byte_size : 0;
    }

private:
    fs::path root_;
    PackageManifest manifest_;
    ChunkPlan plan_;
};

// ---- worldlogic (MXL1 v1) --------------------------------------------------------

bool FiniteRect(const Rect& r) noexcept
{
    return std::isfinite(r.min_x) && std::isfinite(r.min_y) && std::isfinite(r.max_x) &&
           std::isfinite(r.max_y);
}

// Half-open [min, max): open intersection test.
bool Overlaps(const Rect& a, const Rect& b) noexcept
{
    return a.min_x < b.max_x && b.min_x < a.max_x && a.min_y < b.max_y && b.min_y < a.max_y;
}

bool InsideRect(const Rect& inner, const Rect& outer) noexcept
{
    return inner.min_x >= outer.min_x && inner.min_y >= outer.min_y && inner.max_x <= outer.max_x &&
           inner.max_y <= outer.max_y;
}

std::string RectText(const Rect& r)
{
    return "(" + Str(r.min_x) + "," + Str(r.min_y) + ")-(" + Str(r.max_x) + "," + Str(r.max_y) + ")";
}

bool ParseWorldLogic(const std::vector<std::uint8_t>& bytes,
                     const std::string& file,
                     WorldLogic& logic,
                     Issues& issues)
{
    auto at = [&](std::string field, std::size_t offset) {
        return IssueSite{"worldLogic", file, std::move(field), static_cast<std::int64_t>(offset)};
    };
    Cursor cur(bytes);
    std::uint32_t magic = 0, version = 0, zone_count = 0, spawn_count = 0, warp_count = 0;
    if (!cur.U32(magic) || !cur.U32(version) || !cur.U32(zone_count) || !cur.U32(spawn_count) ||
        !cur.U32(warp_count)) {
        issues.Error(PackageErrorCode::WorldLogicTruncated, at("header", cur.Offset()),
                     "file is " + std::to_string(bytes.size()) + " bytes", ">= 20 (header)");
        return false;
    }
    if (magic != kWorldLogicFileMagic) {
        issues.Error(PackageErrorCode::WorldLogicHeaderInvalid, at("magic", 0), "bad magic",
                     "0x314c584d (\"MXL1\")");
        return false;
    }
    if (version != kWorldLogicFileVersion) {
        issues.Error(PackageErrorCode::WorldLogicHeaderInvalid, at("version", 4),
                     "version " + std::to_string(version), std::to_string(kWorldLogicFileVersion));
        return false;
    }
    const std::uint32_t counts[3] = {zone_count, spawn_count, warp_count};
    const char* names[3] = {"zoneCount", "spawnCount", "warpCount"};
    for (int i = 0; i < 3; ++i) {
        if (counts[i] > kMaxWorldLogicRecords) {
            issues.Error(PackageErrorCode::WorldLogicHeaderInvalid, at(names[i], 8 + 4 * static_cast<std::size_t>(i)),
                         std::to_string(counts[i]) + " records",
                         "<= " + std::to_string(kMaxWorldLogicRecords));
            return false;
        }
    }
    auto read_rect = [&](Rect& r) {
        return cur.F32(r.min_x) && cur.F32(r.min_y) && cur.F32(r.max_x) && cur.F32(r.max_y);
    };
    logic.zones.reserve(zone_count);
    for (std::uint32_t i = 0; i < zone_count; ++i) {
        const std::size_t start = cur.Offset();
        Zone zone;
        std::uint8_t name_length = 0;
        if (!cur.U32(zone.id) || !cur.U8(name_length) || !cur.Text(name_length, zone.name) ||
            !read_rect(zone.bounds)) {
            issues.Error(PackageErrorCode::WorldLogicTruncated, at("zones[" + std::to_string(i) + "]", start),
                         "file ends inside zone record " + std::to_string(i), "complete records");
            return false;
        }
        logic.zones.push_back(std::move(zone));
    }
    logic.spawns.reserve(spawn_count);
    for (std::uint32_t i = 0; i < spawn_count; ++i) {
        const std::size_t start = cur.Offset();
        SpawnRegion spawn;
        if (!cur.U32(spawn.id) || !cur.U32(spawn.zone_id) || !read_rect(spawn.bounds)) {
            issues.Error(PackageErrorCode::WorldLogicTruncated, at("spawns[" + std::to_string(i) + "]", start),
                         "file ends inside spawn record " + std::to_string(i), "complete records");
            return false;
        }
        logic.spawns.push_back(spawn);
    }
    logic.warps.reserve(warp_count);
    for (std::uint32_t i = 0; i < warp_count; ++i) {
        const std::size_t start = cur.Offset();
        WarpRegion warp;
        if (!cur.U32(warp.id) || !read_rect(warp.source) || !cur.F32(warp.target_x) ||
            !cur.F32(warp.target_y)) {
            issues.Error(PackageErrorCode::WorldLogicTruncated, at("warps[" + std::to_string(i) + "]", start),
                         "file ends inside warp record " + std::to_string(i), "complete records");
            return false;
        }
        logic.warps.push_back(warp);
    }
    if (cur.Remaining() != 0) {
        issues.Error(PackageErrorCode::WorldLogicTrailingData, at("end", cur.Offset()),
                     std::to_string(cur.Remaining()) + " trailing bytes", "no data after the last record");
        return false;
    }
    return true;
}

// Record-level rules against the world rectangle (half-open, origin-aware).
// MAP-2: MXL1 "zone" records are AREAS -- design metadata with their own
// AreaId namespace. They no longer bootstrap server zones, so they need not
// cover the world; v1 has no overlay area kind, so they stay disjoint.
void ValidateWorldLogic(const WorldLogic& logic,
                        const std::string& file,
                        const GridGeometry& geometry,
                        Issues& issues, WarpPolicy policy)
{
    const Rect world{static_cast<float>(geometry.MinX()), static_cast<float>(geometry.MinY()),
                     static_cast<float>(geometry.MaxX()), static_cast<float>(geometry.MaxY())};
    auto at = [&](std::string field) { return IssueSite{"worldLogic", file, std::move(field)}; };
    std::unordered_map<std::uint32_t, std::size_t> area_index;
    std::vector<std::size_t> valid_areas;
    for (std::size_t i = 0; i < logic.zones.size(); ++i) {
        const auto& a = logic.zones[i];
        const std::string field = "areas[" + std::to_string(i) + "]";
        if (a.id == 0) {
            issues.Error(PackageErrorCode::WorldLogicIdInvalid, at(field + ".id"), "area id 0",
                         ">= 1 (0 = no area)");
        } else if (!area_index.emplace(a.id, i).second) {
            issues.Error(PackageErrorCode::WorldLogicIdDuplicate, at(field + ".id"),
                         "area id " + std::to_string(a.id) + " already used", "unique area ids");
        }
        if (!FiniteRect(a.bounds) || !(a.bounds.min_x < a.bounds.max_x) || !(a.bounds.min_y < a.bounds.max_y)) {
            issues.Error(PackageErrorCode::WorldLogicRectInvalid, at(field + ".bounds"),
                         "bounds " + RectText(a.bounds), "finite, min < max");
            continue;
        }
        if (!InsideRect(a.bounds, world)) {
            issues.Error(PackageErrorCode::WorldLogicOutOfBounds, at(field + ".bounds"),
                         "bounds " + RectText(a.bounds), "inside the world " + RectText(world));
            continue;
        }
        valid_areas.push_back(i);
    }
    for (std::size_t a = 0; a < valid_areas.size(); ++a) {
        for (std::size_t b = a + 1; b < valid_areas.size(); ++b) {
            const auto& za = logic.zones[valid_areas[a]];
            const auto& zb = logic.zones[valid_areas[b]];
            if (Overlaps(za.bounds, zb.bounds)) {
                issues.Error(PackageErrorCode::WorldLogicZoneOverlap,
                             at("areas[" + std::to_string(valid_areas[a]) + "]"),
                             "area " + std::to_string(za.id) + " " + RectText(za.bounds) + " overlaps area " +
                                 std::to_string(zb.id) + " " + RectText(zb.bounds),
                             "disjoint half-open [min, max) areas (MXL1 v1 has no overlay kind)");
            }
        }
    }

    // Player spawn regions: at least one (the server's player spawn rule
    // needs a defined place); areaId 0 = world-level, else inside its area.
    if (logic.spawns.empty()) {
        issues.Error(PackageErrorCode::WorldLogicNoPlayerSpawn, at("spawns"), "no player spawn region",
                     ">= 1 spawn region (where players enter the world)");
    }
    std::unordered_set<std::uint32_t> spawn_ids;
    for (std::size_t i = 0; i < logic.spawns.size(); ++i) {
        const auto& s = logic.spawns[i];
        const std::string field = "spawns[" + std::to_string(i) + "]";
        if (s.id == 0) {
            issues.Error(PackageErrorCode::WorldLogicIdInvalid, at(field + ".id"), "spawn id 0", ">= 1");
        } else if (!spawn_ids.insert(s.id).second) {
            issues.Error(PackageErrorCode::WorldLogicIdDuplicate, at(field + ".id"),
                         "spawn id " + std::to_string(s.id) + " already used", "unique spawn ids");
        }
        if (!FiniteRect(s.bounds) || !(s.bounds.min_x < s.bounds.max_x) || !(s.bounds.min_y < s.bounds.max_y)) {
            issues.Error(PackageErrorCode::WorldLogicRectInvalid, at(field + ".bounds"),
                         "bounds " + RectText(s.bounds), "finite, min < max");
            continue;
        }
        if (s.zone_id == 0) {
            // areaId 0 = no area: a world-level spawn region (areas are
            // optional metadata, the player spawn region is not). It only has
            // to lie inside the world.
            if (!InsideRect(s.bounds, world)) {
                issues.Error(PackageErrorCode::WorldLogicOutOfBounds, at(field + ".bounds"),
                             "spawn " + RectText(s.bounds), "inside the world " + RectText(world));
            }
            continue;
        }
        const auto area = area_index.find(s.zone_id);
        if (area == area_index.end()) {
            issues.Error(PackageErrorCode::WorldLogicReferenceInvalid, at(field + ".areaId"),
                         "spawn references area " + std::to_string(s.zone_id),
                         "an existing area id, or 0 (no area)");
            continue;
        }
        if (!InsideRect(s.bounds, logic.zones[area->second].bounds)) {
            issues.Error(PackageErrorCode::WorldLogicSpawnOutsideZone, at(field + ".bounds"),
                         "spawn " + RectText(s.bounds) + " is not inside area " + std::to_string(s.zone_id) +
                             " " + RectText(logic.zones[area->second].bounds),
                         "inside its area");
        }
    }

    std::unordered_set<std::uint32_t> warp_ids;
    std::vector<bool> warp_ok(logic.warps.size(), false);
    for (std::size_t i = 0; i < logic.warps.size(); ++i) {
        const auto& w = logic.warps[i];
        const std::string field = "warps[" + std::to_string(i) + "]";
        if (w.id == 0) {
            issues.Error(PackageErrorCode::WorldLogicIdInvalid, at(field + ".id"), "warp id 0", ">= 1");
        } else if (!warp_ids.insert(w.id).second) {
            issues.Error(PackageErrorCode::WorldLogicIdDuplicate, at(field + ".id"),
                         "warp id " + std::to_string(w.id) + " already used", "unique warp ids");
        }
        if (!FiniteRect(w.source) || !(w.source.min_x < w.source.max_x) || !(w.source.min_y < w.source.max_y)) {
            issues.Error(PackageErrorCode::WorldLogicRectInvalid, at(field + ".source"),
                         "source " + RectText(w.source), "finite, min < max");
            continue;
        }
        if (!InsideRect(w.source, world)) {
            issues.Error(PackageErrorCode::WorldLogicOutOfBounds, at(field + ".source"),
                         "source " + RectText(w.source), "inside the world " + RectText(world));
            continue;
        }
        if (!geometry.Contains(w.target_x, w.target_y)) {
            issues.Error(PackageErrorCode::WorldLogicWarpTargetInvalid, at(field + ".target"),
                         "target (" + Str(w.target_x) + "," + Str(w.target_y) + ")",
                         "finite, inside the world " + RectText(world) + " (half-open)");
            continue;
        }
        warp_ok[i] = true;
    }
    // Trigger graph with the server's half-open containment: warp i -> warp
    // j when i's target lies in j's source. A cycle (incl. a warp into its own
    // source) makes an entity bounce forever: rejected. A chain is reported
    // (R6: targets should not land in any trigger).
    const std::size_t n = logic.warps.size();
    std::vector<std::vector<std::size_t>> edges(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!warp_ok[i]) {
            continue;
        }
        for (std::size_t j = 0; j < n; ++j) {
            if (warp_ok[j] &&
                logic.warps[j].source.ContainsHalfOpen(logic.warps[i].target_x, logic.warps[i].target_y)) {
                edges[i].push_back(j);
            }
        }
    }
    std::vector<int> state(n, 0); // 0 new, 1 on stack, 2 done
    std::vector<bool> in_cycle(n, false);
    std::function<void(std::size_t, std::vector<std::size_t>&)> dfs = [&](std::size_t v, std::vector<std::size_t>& stack) {
        state[v] = 1;
        stack.push_back(v);
        for (const std::size_t next : edges[v]) {
            if (state[next] == 1) {
                for (auto it = std::find(stack.begin(), stack.end(), next); it != stack.end(); ++it) {
                    in_cycle[*it] = true;
                }
            } else if (state[next] == 0) {
                dfs(next, stack);
            }
        }
        stack.pop_back();
        state[v] = 2;
    };
    for (std::size_t i = 0; i < n; ++i) {
        if (state[i] == 0) {
            std::vector<std::size_t> stack;
            dfs(i, stack);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (in_cycle[i]) {
            issues.Error(PackageErrorCode::WorldLogicWarpCycle, at("warps[" + std::to_string(i) + "].target"),
                         "warp " + std::to_string(logic.warps[i].id) +
                             " is part of a trigger cycle (its target re-enters a warp source)",
                         "no warp target inside a source that leads back");
        } else if (!edges[i].empty()) {
            // Stable code 413; only severity depends on the explicit operator policy.
            auto target_issue = [&](auto code, auto site, auto reason, auto expected) {
                if(policy==WarpPolicy::Strict) issues.Error(code,site,reason,expected);
                else issues.Warning(code,site,reason,expected);
            };
            target_issue(PackageErrorCode::WorldLogicWarpTargetInTrigger,
                           at("warps[" + std::to_string(i) + "].target"),
                           "warp " + std::to_string(logic.warps[i].id) + " target lands in warp " +
                               std::to_string(logic.warps[edges[i].front()].id) + "'s source (chain)",
                           "strict: targets outside every trigger; explicit legacy: acyclic chain only");
        }
    }
}

// Cross-layer rules against the decoded terrain: warp targets and the
// centre of every player spawn region must be walkable cells.
void ValidateAgainstTerrain(const WorldLogic& logic,
                            const std::string& file,
                            const ServerTerrain& terrain,
                            Issues& issues)
{
    for (std::size_t i = 0; i < logic.warps.size(); ++i) {
        const auto& w = logic.warps[i];
        const auto cell = terrain.Cell(w.target_x, w.target_y);
        if (cell.status == TerrainStatus::OutsideWorld) {
            continue; // already reported
        }
        if (!cell.Walkable()) {
            issues.Error(PackageErrorCode::WorldLogicWarpTargetInvalid,
                         IssueSite{"worldLogic", file, "warps[" + std::to_string(i) + "].target"},
                         "target (" + Str(w.target_x) + "," + Str(w.target_y) + ") is " +
                             (cell.Ok() ? "a blocked cell" : ToString(cell.status)),
                         "a walkable cell (attribute bit 0 clear)");
        }
    }
    for (std::size_t i = 0; i < logic.spawns.size(); ++i) {
        const auto& s = logic.spawns[i];
        if (!FiniteRect(s.bounds)) {
            continue; // already reported
        }
        const auto cell = terrain.Cell(s.bounds.CenterX(), s.bounds.CenterY());
        if (cell.status != TerrainStatus::OutsideWorld && !cell.Walkable()) {
            issues.Error(PackageErrorCode::WorldLogicSpawnBlocked,
                         IssueSite{"worldLogic", file, "spawns[" + std::to_string(i) + "].bounds"},
                         "spawn region centre (" + Str(s.bounds.CenterX()) + "," + Str(s.bounds.CenterY()) +
                             ") is not walkable",
                         "a walkable centre cell (players are placed there)");
        }
    }
}

// ---- server water bodies (MXWS v1, MAP-3) ------------------------------------------
//   u32 magic "MXWS"  u32 version = 1  u32 count (<= 4096)
//   count x { u32 id, f32 minX, minY, maxX, maxY, f32 surfaceMeters }
// Exactly 12 + 24*count bytes. ids >= 1 unique; finite; min < max; inside the
// world; |surface| <= 100000; bodies disjoint (half-open) so a point has one
// surface.
bool ParseWaterBodies(const std::vector<std::uint8_t>& bytes,
                      const std::string& file,
                      const GridGeometry& geometry,
                      std::vector<WaterBodyRect>& out,
                      Issues& issues)
{
    auto at = [&](std::string field, std::size_t offset) {
        return IssueSite{"waterBodies", file, std::move(field), static_cast<std::int64_t>(offset)};
    };
    Cursor cur(bytes);
    std::uint32_t magic = 0, version = 0, count = 0;
    if (!cur.U32(magic) || !cur.U32(version) || !cur.U32(count)) {
        issues.Error(PackageErrorCode::WaterBodiesTruncated, at("header", cur.Offset()),
                     "file is " + std::to_string(bytes.size()) + " bytes", ">= 12 (header)");
        return false;
    }
    if (magic != kWaterBodiesFileMagic || version != kWaterBodiesFileVersion || count > kMaxWaterBodies) {
        issues.Error(PackageErrorCode::WaterBodiesHeader, at("header", 0),
                     "magic/version/count " + std::to_string(magic) + "/" + std::to_string(version) + "/" +
                         std::to_string(count),
                     "\"MXWS\", version 1, count <= " + std::to_string(kMaxWaterBodies));
        return false;
    }
    if (bytes.size() != 12u + 24u * static_cast<std::size_t>(count)) {
        issues.Error(PackageErrorCode::WaterBodiesTruncated, at("records", 12),
                     "file is " + std::to_string(bytes.size()) + " bytes",
                     std::to_string(12u + 24u * static_cast<std::size_t>(count)) + " (12 + 24 * count)");
        return false;
    }
    const Rect world{static_cast<float>(geometry.MinX()), static_cast<float>(geometry.MinY()),
                     static_cast<float>(geometry.MaxX()), static_cast<float>(geometry.MaxY())};
    std::unordered_set<std::uint32_t> ids;
    const std::size_t errors_before = issues.ErrorCount();
    for (std::uint32_t i = 0; i < count; ++i) {
        WaterBodyRect body;
        const std::size_t offset = cur.Offset();
        (void)(cur.U32(body.id) && cur.F32(body.bounds.min_x) && cur.F32(body.bounds.min_y) &&
               cur.F32(body.bounds.max_x) && cur.F32(body.bounds.max_y) && cur.F32(body.surface_m));
        const std::string field = "bodies[" + std::to_string(i) + "]";
        if (body.id == 0 || !ids.insert(body.id).second) {
            issues.Error(PackageErrorCode::WaterBodyInvalid, at(field + ".id", offset),
                         "water body id " + std::to_string(body.id), ">= 1, unique");
        }
        if (!FiniteRect(body.bounds) || !(body.bounds.min_x < body.bounds.max_x) ||
            !(body.bounds.min_y < body.bounds.max_y) || !InsideRect(body.bounds, world) ||
            !std::isfinite(body.surface_m) || std::abs(body.surface_m) > 1.0e5f) {
            issues.Error(PackageErrorCode::WaterBodyInvalid, at(field, offset),
                         "bounds " + RectText(body.bounds) + " surface " + Str(body.surface_m),
                         "finite, min < max, inside the world " + RectText(world) + ", |surface| <= 100000");
        }
        out.push_back(body);
    }
    for (std::size_t a = 0; a < out.size(); ++a) {
        for (std::size_t b = a + 1; b < out.size(); ++b) {
            if (Overlaps(out[a].bounds, out[b].bounds)) {
                issues.Error(PackageErrorCode::WaterBodyOverlap, at("bodies[" + std::to_string(a) + "]", 12 + 24 * a),
                             "water body " + std::to_string(out[a].id) + " overlaps body " +
                                 std::to_string(out[b].id),
                             "disjoint half-open bodies (one surface per point)");
            }
        }
    }
    return issues.ErrorCount() == errors_before;
}

// ---- spawn table (mob_spawns format v1) -----------------------------------------------

bool ParseUnsigned(const std::string& text, std::uint32_t& out)
{
    if (text.empty() || text.size() > 10 ||
        !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    const unsigned long long value = std::stoull(text);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool ParseFloat(const std::string& text, float& out)
{
    if (text.empty() || text.size() > 32) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) {
            return false;
        }
    }
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    double value = 0.0;
    in >> value;
    if (in.fail() || !in.eof() || !std::isfinite(value) ||
        std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

void ParseSpawns(const std::vector<std::uint8_t>& bytes,
                 const std::string& file,
                 const GridGeometry& geometry,
                 std::uint16_t version,
                 const WorldLogic& logic,
                 std::vector<SpawnRecord>& out,
                 Issues& issues)
{
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t pos = 0;
    std::uint32_t line_number = 0;
    std::uint32_t records = 0;
    std::unordered_set<std::uint32_t> ids;
    while (pos <= text.size()) {
        const std::size_t end = std::min(text.find('\n', pos), text.size());
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            if (end == text.size()) {
                break;
            }
            continue;
        }
        IssueSite site{"mobSpawns", file, {}, static_cast<std::int64_t>(line_number)};
        if (++records > kMaxSpawnLines) {
            issues.Error(PackageErrorCode::SpawnsTooMany, site, "more than " + std::to_string(kMaxSpawnLines) +
                         " spawn lines", "<= " + std::to_string(kMaxSpawnLines));
            return;
        }
        std::istringstream tokens(line);
        std::string token;
        SpawnRecord record;
        record.line = line_number;
        record.spawn_id = records; // legacy v1: immutable package record order
        std::set<std::string> keys;
        bool line_ok = true;
        while (tokens >> token) {
            const auto equals = token.find('=');
            if (equals == std::string::npos || equals == 0) {
                issues.Error(PackageErrorCode::SpawnsSyntax, site, "token '" + token + "'", "key=value");
                line_ok = false;
                continue;
            }
            const std::string key = token.substr(0, equals);
            const std::string value = token.substr(equals + 1);
            site.field = key;
            if (!keys.insert(key).second) {
                issues.Error(PackageErrorCode::SpawnsSyntax, site, "key '" + key + "' repeated", "each key once");
                line_ok = false;
                continue;
            }
            bool ok = true;
            if (key == "spawn_id" && version == 2) {
                ok = ParseUnsigned(value, record.spawn_id) && record.spawn_id != 0;
            } else if (key == "area_id" && version == 2) {
                ok = ParseUnsigned(value, record.area_id);
            } else if (key == "mob_type_id") {
                ok = ParseUnsigned(value, record.mob_type_id) && record.mob_type_id != 0;
            } else if (key == "count") {
                ok = ParseUnsigned(value, record.count) && record.count >= 1 && record.count <= kMaxSpawnCount;
            } else if (key == "x") {
                ok = ParseFloat(value, record.x);
            } else if (key == "y") {
                ok = ParseFloat(value, record.y);
            } else if (key == "radius") {
                ok = ParseFloat(value, record.radius) && record.radius >= 0.0f &&
                     record.radius <= static_cast<float>(std::max(geometry.MaxX() - geometry.MinX(),
                                                                  geometry.MaxY() - geometry.MinY()));
            } else {
                issues.Error(PackageErrorCode::SpawnsSyntax, site, "unknown key '" + key + "'",
                             "mob_type_id, x, y, count, radius");
                line_ok = false;
                continue;
            }
            if (!ok) {
                issues.Error(PackageErrorCode::SpawnsFieldInvalid, site, key + "='" + value + "'",
                             key == "mob_type_id" || key == "spawn_id" ? "integer 1 .. 4294967295"
                             : key == "area_id"   ? "integer 0 .. 4294967295"
                             : key == "count"     ? "integer 1 .. " + std::to_string(kMaxSpawnCount)
                             : key == "radius"    ? "finite, 0 .. world extent"
                                                  : "finite decimal number");
                line_ok = false;
            }
        }
        site.field.clear();
        for (const char* required : {"mob_type_id", "x", "y", "count", "radius"}) {
            if (!keys.contains(required)) {
                issues.Error(PackageErrorCode::SpawnsSyntax, site, std::string("missing key '") + required + "'",
                             "mob_type_id, x, y, count, radius");
                line_ok = false;
            }
        }
        if (!line_ok) {
            continue;
        }
        if (version == 2 && (!keys.contains("spawn_id") || !keys.contains("area_id"))) {
            issues.Error(PackageErrorCode::SpawnsSyntax, site, "v2 missing identity/reference",
                         "spawn_id and area_id required");
            continue;
        }
        if (!ids.insert(record.spawn_id).second) {
            issues.Error(PackageErrorCode::SpawnsFieldInvalid, site, "duplicate spawn_id",
                         "unique nonzero spawn_id within world package");
            continue;
        }
        if (record.area_id != 0 && std::none_of(logic.zones.begin(), logic.zones.end(),
                [&](const auto& area) { return area.id == record.area_id &&
                    area.bounds.ContainsHalfOpen(record.x, record.y); })) {
            issues.Error(PackageErrorCode::SpawnsFieldInvalid, site, "invalid area_id or centre outside area",
                         "0 (world) or existing containing area");
            continue;
        }
        if (!geometry.Contains(record.x, record.y)) {
            issues.Error(PackageErrorCode::SpawnsOutOfBounds, site,
                         "spawn centre (" + Str(record.x) + "," + Str(record.y) + ")",
                         "inside the world [min, max) on both axes");
            continue;
        }
        out.push_back(record);
        if (end == text.size()) {
            break;
        }
    }
}

} // namespace

// ---- public ------------------------------------------------------------------

const char* ToString(PackageErrorCode code) noexcept
{
    switch (code) {
    case PackageErrorCode::PackageRootMissing: return "PACKAGE_ROOT_MISSING";
    case PackageErrorCode::PackageRootNotDirectory: return "PACKAGE_ROOT_NOT_DIRECTORY";
    case PackageErrorCode::FileMissing: return "FILE_MISSING";
    case PackageErrorCode::FileUnreadable: return "FILE_UNREADABLE";
    case PackageErrorCode::FileTooLarge: return "FILE_TOO_LARGE";
    case PackageErrorCode::PathInvalid: return "PATH_INVALID";
    case PackageErrorCode::PathOutsidePackage: return "PATH_OUTSIDE_PACKAGE";
    case PackageErrorCode::ManifestCorrupt: return "MANIFEST_CORRUPT";
    case PackageErrorCode::ManifestVersionUnsupported: return "MANIFEST_VERSION_UNSUPPORTED";
    case PackageErrorCode::ManifestFieldInvalid: return "MANIFEST_FIELD_INVALID";
    case PackageErrorCode::ManifestFieldForbidden: return "MANIFEST_FIELD_FORBIDDEN";
    case PackageErrorCode::ManifestSizeOverflow: return "MANIFEST_SIZE_OVERFLOW";
    case PackageErrorCode::UnsupportedFeature: return "UNSUPPORTED_FEATURE";
    case PackageErrorCode::LegacyFieldIgnored: return "LEGACY_FIELD_IGNORED";
    case PackageErrorCode::LayerDuplicate: return "LAYER_DUPLICATE";
    case PackageErrorCode::LayerRequiredMissing: return "LAYER_REQUIRED_MISSING";
    case PackageErrorCode::LayerUnsupported: return "LAYER_UNSUPPORTED";
    case PackageErrorCode::LayerDeclInvalid: return "LAYER_DECL_INVALID";
    case PackageErrorCode::LayerSkipped: return "LAYER_SKIPPED";
    case PackageErrorCode::LayerNotValidated: return "LAYER_NOT_VALIDATED";
    case PackageErrorCode::ChunkIndexInvalid: return "CHUNK_INDEX_INVALID";
    case PackageErrorCode::ChunkSizeMismatch: return "CHUNK_SIZE_MISMATCH";
    case PackageErrorCode::ChunkChecksumMismatch: return "CHUNK_CHECKSUM_MISMATCH";
    case PackageErrorCode::ChunkIntegrityUnavailable: return "CHUNK_INTEGRITY_UNAVAILABLE";
    case PackageErrorCode::ChunkHeaderInvalid: return "CHUNK_HEADER_INVALID";
    case PackageErrorCode::ChunkTocInvalid: return "CHUNK_TOC_INVALID";
    case PackageErrorCode::ChunkSectionRange: return "CHUNK_SECTION_RANGE";
    case PackageErrorCode::ChunkSectionSize: return "CHUNK_SECTION_SIZE";
    case PackageErrorCode::ChunkSectionMissing: return "CHUNK_SECTION_MISSING";
    case PackageErrorCode::ChunkAttributeReservedBits: return "CHUNK_ATTRIBUTE_RESERVED_BITS";
    case PackageErrorCode::ChunkSplatGeometry: return "CHUNK_SPLAT_GEOMETRY";
    case PackageErrorCode::ChunkEdgeMismatch: return "CHUNK_EDGE_MISMATCH";
    case PackageErrorCode::WorldLogicHeaderInvalid: return "WORLDLOGIC_HEADER_INVALID";
    case PackageErrorCode::WorldLogicTruncated: return "WORLDLOGIC_TRUNCATED";
    case PackageErrorCode::WorldLogicTrailingData: return "WORLDLOGIC_TRAILING_DATA";
    case PackageErrorCode::WorldLogicIdInvalid: return "WORLDLOGIC_ID_INVALID";
    case PackageErrorCode::WorldLogicIdDuplicate: return "WORLDLOGIC_ID_DUPLICATE";
    case PackageErrorCode::WorldLogicRectInvalid: return "WORLDLOGIC_RECT_INVALID";
    case PackageErrorCode::WorldLogicOutOfBounds: return "WORLDLOGIC_OUT_OF_BOUNDS";
    case PackageErrorCode::WorldLogicZoneOverlap: return "WORLDLOGIC_ZONE_OVERLAP";
    case PackageErrorCode::WorldLogicCoverageGap: return "WORLDLOGIC_COVERAGE_GAP";
    case PackageErrorCode::WorldLogicReferenceInvalid: return "WORLDLOGIC_REFERENCE_INVALID";
    case PackageErrorCode::WorldLogicSpawnOutsideZone: return "WORLDLOGIC_SPAWN_OUTSIDE_ZONE";
    case PackageErrorCode::WorldLogicWarpTargetInvalid: return "WORLDLOGIC_WARP_TARGET_INVALID";
    case PackageErrorCode::WorldLogicWarpCycle: return "WORLDLOGIC_WARP_CYCLE";
    case PackageErrorCode::WorldLogicWarpTargetInTrigger: return "WORLDLOGIC_WARP_TARGET_IN_TRIGGER";
    case PackageErrorCode::WorldLogicNoZones: return "WORLDLOGIC_NO_ZONES";
    case PackageErrorCode::WorldLogicNoPlayerSpawn: return "WORLDLOGIC_NO_PLAYER_SPAWN";
    case PackageErrorCode::WorldLogicSpawnBlocked: return "WORLDLOGIC_SPAWN_BLOCKED";
    case PackageErrorCode::SpawnsSyntax: return "SPAWNS_SYNTAX";
    case PackageErrorCode::SpawnsFieldInvalid: return "SPAWNS_FIELD_INVALID";
    case PackageErrorCode::SpawnsOutOfBounds: return "SPAWNS_OUT_OF_BOUNDS";
    case PackageErrorCode::SpawnsTooMany: return "SPAWNS_TOO_MANY";
    case PackageErrorCode::SpawnsMobTypeUnknown: return "SPAWNS_MOB_TYPE_UNKNOWN";
    case PackageErrorCode::WaterDeclInvalid: return "WATER_DECL_INVALID";
    case PackageErrorCode::WaterBodiesHeader: return "WATER_BODIES_HEADER";
    case PackageErrorCode::WaterBodiesTruncated: return "WATER_BODIES_TRUNCATED";
    case PackageErrorCode::WaterBodyInvalid: return "WATER_BODY_INVALID";
    case PackageErrorCode::WaterBodyOverlap: return "WATER_BODY_OVERLAP";
    case PackageErrorCode::LayeredWorldHeader: return "LAYERED_WORLD_HEADER";
    case PackageErrorCode::LayeredWorldTruncated: return "LAYERED_WORLD_TRUNCATED";
    case PackageErrorCode::LayeredWorldInvalid: return "LAYERED_WORLD_INVALID";
    case PackageErrorCode::LayeredWorldTrailingData: return "LAYERED_WORLD_TRAILING_DATA";
    case PackageErrorCode::LayeredWorldLimit: return "LAYERED_WORLD_LIMIT";
    case PackageErrorCode::StartupDataInvalid: return "STARTUP_DATA_INVALID";
    case PackageErrorCode::Internal: return "INTERNAL";
    }
    return "UNKNOWN";
}

const char* ToString(IssueSeverity severity) noexcept
{
    switch (severity) {
    case IssueSeverity::Error: return "ERROR";
    case IssueSeverity::Warning: return "WARNING";
    case IssueSeverity::Info: return "INFO";
    }
    return "?";
}

const char* ToString(ResidencyMode mode) noexcept
{
    return mode == ResidencyMode::Streaming ? "streaming" : "eager";
}

const char* ToString(LayerKind kind) noexcept
{
    switch (kind) {
    case LayerKind::Height: return "height";
    case LayerKind::Attributes: return "attributes";
    case LayerKind::SplatA: return "splatA";
    case LayerKind::SplatB: return "splatB";
    case LayerKind::WorldLogic: return "worldLogic";
    case LayerKind::MobSpawns: return "mobSpawns";
    case LayerKind::Water: return "water";
    case LayerKind::WaterBodies: return "waterBodies";
    }
    return "unknown";
}

const char* ToString(LayerAudience audience) noexcept
{
    switch (audience) {
    case LayerAudience::Server: return "server";
    case LayerAudience::Client: return "client";
    case LayerAudience::Shared: return "shared";
    }
    return "?";
}

const char* ToString(LayerStatus status) noexcept
{
    switch (status) {
    case LayerStatus::Loaded: return "loaded";
    case LayerStatus::Validated: return "validated";
    case LayerStatus::RangeChecked: return "range-checked";
    case LayerStatus::Skipped: return "skipped";
    case LayerStatus::NotValidated: return "not-validated";
    case LayerStatus::Absent: return "absent";
    }
    return "?";
}

std::string PackageIssue::Format() const
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << ToString(severity) << ' ' << ToString(code) << '(' << static_cast<unsigned>(code) << ')';
    if (!package.empty()) {
        out << " package=" << package;
    }
    if (!layer.empty()) {
        out << " layer=" << layer;
    }
    if (!file.empty()) {
        out << " file=" << file;
    }
    if (chunk_x >= 0) {
        out << " chunk=(" << chunk_x << ',' << chunk_y << ')';
    }
    if (!field.empty()) {
        out << " field=" << field;
    }
    if (offset >= 0) {
        out << (layer == "mobSpawns" ? " line=" : " offset=") << offset;
    }
    out << " reason=\"" << reason << '"';
    if (!expected.empty()) {
        out << " expected=\"" << expected << '"';
    }
    return out.str();
}

GridGeometry PackageManifest::Geometry() const noexcept
{
    GridGeometry g;
    g.origin_x = origin_x;
    g.origin_y = origin_y;
    g.cell_size_m = cell_size_m;
    g.cells_x = size_cells_x;
    g.cells_y = size_cells_y;
    g.chunk_cells = chunk_size_cells;
    g.chunks_x = chunk_grid_x;
    g.chunks_y = chunk_grid_y;
    return g;
}

namespace {
std::atomic<std::uint64_t> g_package_loads{0};
} // namespace

std::uint64_t PackageLoadCount() noexcept
{
    return g_package_loads.load(std::memory_order_relaxed);
}

const LayerInfo* PackageManifest::FindLayer(LayerKind kind) const noexcept
{
    for (const auto& layer : layers) {
        if (layer.kind == kind) {
            return &layer;
        }
    }
    return nullptr;
}

bool PackageReport::Ok() const noexcept
{
    return FirstError() == nullptr;
}

std::size_t PackageReport::Count(IssueSeverity severity) const noexcept
{
    return static_cast<std::size_t>(std::count_if(issues.begin(), issues.end(), [&](const PackageIssue& i) {
        return i.severity == severity;
    }));
}

const PackageIssue* PackageReport::FirstError() const noexcept
{
    for (const auto& issue : issues) {
        if (issue.severity == IssueSeverity::Error) {
            return &issue;
        }
    }
    return nullptr;
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) noexcept
{
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

std::optional<fs::path> ResolvePackageReference(const fs::path& canonical_root,
                                                std::string_view reference,
                                                std::string& why_not)
{
    if (reference.empty()) {
        why_not = "empty reference";
        return std::nullopt;
    }
    if (reference.size() > 512) {
        why_not = "longer than 512 bytes";
        return std::nullopt;
    }
    for (const char c : reference) {
        if (c == '\\') {
            why_not = "backslash separator (package references use '/')";
            return std::nullopt;
        }
        if (c == ':') {
            why_not = "':' (drive letter / stream syntax)";
            return std::nullopt;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            why_not = "control character";
            return std::nullopt;
        }
    }
    if (reference.front() == '/') {
        why_not = "absolute path";
        return std::nullopt;
    }
    fs::path joined = canonical_root;
    std::size_t start = 0;
    while (start <= reference.size()) {
        const std::size_t slash = std::min(reference.find('/', start), reference.size());
        const std::string_view part = reference.substr(start, slash - start);
        if (part.empty()) {
            why_not = "empty path component";
            return std::nullopt;
        }
        if (part == "." || part == "..") {
            why_not = "'" + std::string(part) + "' component (path traversal)";
            return std::nullopt;
        }
        joined /= PathFromUtf8(part);
        if (slash == reference.size()) {
            break;
        }
        start = slash + 1;
    }
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(joined, ec);
    if (ec) {
        why_not = "cannot canonicalize: " + ec.message();
        return std::nullopt;
    }
    // Component-wise containment (a symlink/junction leading out is caught
    // because weakly_canonical resolved it; "pkg-evil" never matches "pkg").
    auto r = canonical_root.begin();
    auto c = resolved.begin();
    for (; r != canonical_root.end(); ++r, ++c) {
        if (r->empty()) {
            continue;
        }
        if (c == resolved.end() || *r != *c) {
            why_not = "resolves outside the package root";
            return std::nullopt;
        }
    }
    if (c == resolved.end()) {
        why_not = "resolves to the package root itself";
        return std::nullopt;
    }
    return resolved;
}

std::optional<ServerWorldData> LoadServerWorld(const fs::path& package_root,
                                               ValidationDepth depth,
                                               PackageReport& report)
{
    LoadOptions options;
    options.depth = depth;
    return LoadServerWorld(package_root, options, report);
}

std::optional<ServerWorldData> LoadServerWorld(const fs::path& package_root,
                                               const LoadOptions& options,
                                               PackageReport& report)
{
    g_package_loads.fetch_add(1, std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    const ValidationDepth depth = options.depth;
    report = PackageReport{};
    report.depth = depth;
    Issues issues(report);
    std::optional<ServerWorldData> result;
    try {
        issues.SetPackage(package_root.filename().string());
        std::error_code ec;
        const auto status = fs::status(package_root, ec);
        if (ec || !fs::exists(status)) {
            issues.Error(PackageErrorCode::PackageRootMissing, IssueSite{"package", {}},
                         "package directory '" + package_root.string() + "' does not exist",
                         "an existing world package directory");
        } else if (!fs::is_directory(status)) {
            issues.Error(PackageErrorCode::PackageRootNotDirectory, IssueSite{"package", {}},
                         "'" + package_root.string() + "' is not a directory", "a world package directory");
        }
        if (issues.ErrorCount() == 0) {
            report.root = fs::canonical(package_root, ec);
            if (ec) {
                issues.Error(PackageErrorCode::PackageRootMissing, IssueSite{"package", {}},
                             "cannot canonicalize '" + package_root.string() + "': " + ec.message());
            }
        }
        FileAccess files{report.root, report, issues};
        std::optional<std::vector<std::uint8_t>> manifest_bytes;
        if (issues.ErrorCount() == 0) {
            manifest_bytes = files.Read(kManifestFile, kMaxManifestBytes, IssueSite{"manifest"});
        }
        PackageManifest& manifest = report.manifest;
        const bool eager_residency =
            !(options.residency == ResidencyMode::Streaming && options.depth == ValidationDepth::Startup);
        if (manifest_bytes && ParseManifest(*manifest_bytes, manifest, issues, eager_residency)) {
            const bool v3 = manifest.format_version == kManifestVersionCurrent;
            // ---- layer resolution ----
            for (const LayerKind needed : {LayerKind::Height, LayerKind::Attributes, LayerKind::WorldLogic}) {
                if (manifest.FindLayer(needed) == nullptr) {
                    issues.Error(PackageErrorCode::LayerRequiredMissing, IssueSite{"manifest", kManifestFile, "layers"},
                                 std::string("no ") + ToString(needed) + " layer",
                                 "height, attributes and worldLogic (server-mandatory)");
                }
            }
            const LayerInfo* splat_a = manifest.FindLayer(LayerKind::SplatA);
            const LayerInfo* splat_b = manifest.FindLayer(LayerKind::SplatB);
            if (v3 && ((splat_a == nullptr) != (splat_b == nullptr))) {
                issues.Error(PackageErrorCode::LayerDeclInvalid, IssueSite{"manifest", kManifestFile, "layers"},
                             "only one of splatA / splatB declared", "both or neither");
            }
            const bool streaming =
                options.residency == ResidencyMode::Streaming && options.depth == ValidationDepth::Startup;
            report.residency = streaming ? ResidencyMode::Streaming : ResidencyMode::Eager;
            if (issues.ErrorCount() == 0) {
                ChunkPlan plan;
                plan.strict_v3 = v3;
                plan.splat_declared = splat_a != nullptr && splat_b != nullptr;
                plan.decode_client = depth == ValidationDepth::Full;
                plan.encoding = manifest.height_encoding;
                ServerWorldData data;
                const GridGeometry geometry = manifest.Geometry();
                data.residency = report.residency;
                // Optional 3D-2 sidecar. Its absence is the legacy contract;
                // if present, it is strict and must validate before runtime
                // data can be returned.
                if (files.Exists(kLayeredWorldFile)) {
                    if (const auto bytes = files.Read(kLayeredWorldFile,
                                                      kMaxLayeredWorldFileBytes,
                                                      IssueSite{"layeredWorld", kLayeredWorldFile})) {
                        LayeredWorld layered;
                        std::string layered_error;
                        if (!DecodeLayeredWorld(*bytes, layered, layered_error)) {
                            PackageErrorCode code = PackageErrorCode::LayeredWorldInvalid;
                            if (layered_error.find("header") != std::string::npos ||
                                layered_error.find("magic") != std::string::npos ||
                                layered_error.find("version") != std::string::npos) {
                                code = PackageErrorCode::LayeredWorldHeader;
                            } else if (layered_error.find("truncated") != std::string::npos) {
                                code = PackageErrorCode::LayeredWorldTruncated;
                            } else if (layered_error.find("trailing") != std::string::npos) {
                                code = PackageErrorCode::LayeredWorldTrailingData;
                            } else if (layered_error.find("limit") != std::string::npos) {
                                code = PackageErrorCode::LayeredWorldLimit;
                            }
                            issues.Error(code,
                                         IssueSite{"layeredWorld", kLayeredWorldFile},
                                         layered_error,
                                         "MX3D version 1 with bounded records");
                        } else {
                            const double max_x = manifest.origin_x + manifest.ExtentX();
                            const double max_y = manifest.origin_y + manifest.ExtentY();
                            const Rect world_bounds{static_cast<float>(manifest.origin_x),
                                                    static_cast<float>(manifest.origin_y),
                                                    static_cast<float>(max_x),
                                                    static_cast<float>(max_y)};
                            if (!layered.Validate(world_bounds, layered_error)) {
                                issues.Error(PackageErrorCode::LayeredWorldInvalid,
                                             IssueSite{"layeredWorld", kLayeredWorldFile},
                                             layered_error,
                                             "finite, non-overlapping volumes and in-bounds portals");
                            } else {
                                data.layered_world = std::move(layered);
                            }
                        }
                    }
                }
                std::vector<TerrainChunk> chunks(manifest.chunks.size());
                bool any_splat = false;
                std::uint32_t splat_w = 0, splat_h = 0;
                bool splat_geometry_reported = false;
                auto load_chunk = [&](std::size_t chunk_index) {
                    const auto& entry = manifest.chunks[chunk_index];
                    IssueSite csite{"chunk", entry.file};
                    csite.chunk_x = static_cast<std::int32_t>(entry.x);
                    csite.chunk_y = static_cast<std::int32_t>(entry.y);
                    DecodedChunk decoded;
                    if (!LoadChunkFile(files, entry, manifest, plan, chunks[chunk_index], decoded, issues)) {
                        return;
                    }
                    any_splat = any_splat || decoded.has_splat_a || decoded.has_splat_b;
                    if (plan.decode_client && (decoded.has_splat_a || decoded.has_splat_b)) {
                        if (splat_w == 0) {
                            splat_w = decoded.splat_w;
                            splat_h = decoded.splat_h;
                        } else if ((decoded.splat_w != splat_w || decoded.splat_h != splat_h) &&
                                   !splat_geometry_reported) {
                            splat_geometry_reported = true;
                            issues.Error(PackageErrorCode::ChunkSplatGeometry, csite,
                                         "splat " + std::to_string(decoded.splat_w) + "x" +
                                             std::to_string(decoded.splat_h) + " differs from earlier chunks",
                                         std::to_string(splat_w) + "x" + std::to_string(splat_h) +
                                             " in every chunk");
                        }
                    }
                };
                if (!streaming) {
                    for (std::size_t chunk_index = 0; chunk_index < manifest.chunks.size(); ++chunk_index) {
                        load_chunk(chunk_index);
                    }
                } else {
                    // Every chunk file must exist (and match the index size in
                    // v3) at startup -- a missing chunk is a load error, never
                    // free space; its CONTENT is validated when it is loaded.
                    for (const auto& entry : manifest.chunks) {
                        IssueSite csite{"chunk", entry.file};
                        csite.chunk_x = static_cast<std::int32_t>(entry.x);
                        csite.chunk_y = static_cast<std::int32_t>(entry.y);
                        const auto size = files.Stat(entry.file, csite);
                        if (!size) {
                            continue;
                        }
                        ++report.chunks_checked;
                        if (v3 && *size != entry.byte_size) {
                            issues.Error(PackageErrorCode::ChunkSizeMismatch, csite,
                                         "file is " + std::to_string(*size) + " bytes",
                                         std::to_string(entry.byte_size) + " (chunk index)");
                        }
                    }
                }

                // ---- worldlogic ----
                const LayerInfo* logic_layer = manifest.FindLayer(LayerKind::WorldLogic);
                bool logic_ok = false;
                if (logic_layer != nullptr) {
                    if (const auto bytes = files.Read(logic_layer->file, kMaxWorldLogicBytes,
                                                      IssueSite{"worldLogic"})) {
                        const std::size_t before = issues.ErrorCount();
                        if (ParseWorldLogic(*bytes, logic_layer->file, data.logic, issues)) {
                            ValidateWorldLogic(data.logic, logic_layer->file, geometry, issues, options.warp_policy);
                        }
                        logic_ok = issues.ErrorCount() == before;
                    }
                }
                if (streaming && logic_ok && issues.ErrorCount() == 0) {
                    // Startup set: the chunks the cross-layer rules below and
                    // the first player spawns need.
                    const ServerTerrain probe(geometry, plan.encoding);
                    std::vector<std::uint32_t> startup;
                    auto add = [&](double x, double y) {
                        const std::uint32_t index = probe.ChunkIndexOf(x, y);
                        if (index != ServerTerrain::kNoChunk) {
                            startup.push_back(index);
                        }
                    };
                    for (const auto& spawn : data.logic.spawns) {
                        add(spawn.bounds.CenterX(), spawn.bounds.CenterY());
                    }
                    for (const auto& warp : data.logic.warps) {
                        add(warp.target_x, warp.target_y);
                    }
                    std::sort(startup.begin(), startup.end());
                    startup.erase(std::unique(startup.begin(), startup.end()), startup.end());
                    for (const std::uint32_t index : startup) {
                        load_chunk(index);
                    }
                    data.startup_chunks = std::move(startup);
                }
                CheckChunkSeams(geometry, plan.encoding, chunks, manifest.chunks, issues);
                data.terrain = ServerTerrain(geometry, plan.encoding, std::move(chunks));
                report.resident_terrain_bytes = data.terrain.ResidentBytes();
                if (logic_ok && issues.ErrorCount() == 0) {
                    const std::size_t before = issues.ErrorCount();
                    ValidateAgainstTerrain(data.logic, logic_layer->file, data.terrain, issues);
                    logic_ok = issues.ErrorCount() == before;
                }
                data.chunk_source = std::make_shared<PackageChunkSource>(report.root, manifest, plan);

                // ---- spawn table ----
                // An optional table that is absent is skipped (documented); a
                // required one, or any table that exists, is read strictly.
                const LayerInfo* spawn_layer = manifest.FindLayer(LayerKind::MobSpawns);
                bool spawn_ok = false;
                bool spawn_skipped = false;
                if (spawn_layer != nullptr) {
                    if (!spawn_layer->required && !files.Exists(spawn_layer->file)) {
                        spawn_skipped = true;
                    } else if (const auto bytes = files.Read(spawn_layer->file, kMaxSpawnFileBytes,
                                                             IssueSite{"mobSpawns"})) {
                        const std::size_t before = issues.ErrorCount();
                        ParseSpawns(*bytes, spawn_layer->file, geometry, spawn_layer->version,
                                    data.logic, data.spawns, issues);
                        spawn_ok = issues.ErrorCount() == before;
                        data.spawn_layer_present = true;
                    }
                }

                // ---- water capability (MAP-3) ----
                // model = bodies <=> a waterBodies layer; the client MXWB
                // layer (`water`) never counts as server water.
                bool water_ok = false;
                data.water.model = manifest.water_model;
                data.water.sea_level_m = manifest.sea_level_m;
                const LayerInfo* water_layer = manifest.FindLayer(LayerKind::WaterBodies);
                const IssueSite wsite{"manifest", kManifestFile, "water"};
                if (manifest.water_model == WaterModel::Bodies && water_layer == nullptr) {
                    issues.Error(PackageErrorCode::WaterDeclInvalid, wsite, "water model bodies without a waterBodies layer",
                                 "a declared waterBodies layer");
                } else if (manifest.water_model != WaterModel::Bodies && water_layer != nullptr) {
                    issues.Error(PackageErrorCode::WaterDeclInvalid, wsite,
                                 std::string("waterBodies layer declared with water model ") +
                                     ToString(manifest.water_model),
                                 "water.model = bodies");
                } else if (water_layer != nullptr) {
                    if (const auto bytes = files.Read(water_layer->file, kMaxWaterBodiesBytes, IssueSite{"waterBodies"})) {
                        water_ok = ParseWaterBodies(*bytes, water_layer->file, geometry, data.water.bodies, issues);
                    }
                }

                // ---- layer status + client files (Full) ----
                for (auto& layer : manifest.layers) {
                    switch (layer.kind) {
                    case LayerKind::Height:
                    case LayerKind::Attributes:
                        layer.status = LayerStatus::Loaded;
                        break;
                    case LayerKind::SplatA:
                    case LayerKind::SplatB:
                        layer.status = !any_splat ? LayerStatus::Absent
                                       : depth == ValidationDepth::Full ? LayerStatus::Validated
                                                                        : LayerStatus::RangeChecked;
                        break;
                    case LayerKind::WorldLogic:
                        layer.status = logic_ok ? LayerStatus::Loaded : LayerStatus::Absent;
                        break;
                    case LayerKind::MobSpawns:
                        layer.status = spawn_ok ? LayerStatus::Loaded : LayerStatus::Absent;
                        if (spawn_skipped) {
                            issues.Info(PackageErrorCode::LayerSkipped, IssueSite{"mobSpawns", layer.file},
                                        "optional spawn table not present: no mob spawn points");
                        }
                        break;
                    case LayerKind::WaterBodies:
                        layer.status = water_ok ? LayerStatus::Loaded : LayerStatus::Absent;
                        break;
                    case LayerKind::Water:
                        if (!layer.required && !files.Exists(layer.file)) {
                            layer.status = LayerStatus::Absent;
                            break;
                        }
                        layer.status = LayerStatus::NotValidated;
                        if (depth == ValidationDepth::Full || layer.required) {
                            (void)files.Read(layer.file, kMaxChunkFileBytes, IssueSite{"water"});
                        }
                        issues.Info(PackageErrorCode::LayerNotValidated, IssueSite{"water", layer.file},
                                    "water layer present, not validated (no validator yet; client data, "
                                    "unused by the server)");
                        break;
                    }
                }
                if (manifest.texture_palette_entries > 0) {
                    issues.Info(PackageErrorCode::LayerSkipped, IssueSite{"manifest", kManifestFile, "texturePalette"},
                                std::to_string(manifest.texture_palette_entries) +
                                    " texture palette entries (client render data, not loaded)");
                }
                if (issues.ErrorCount() == 0) {
                    result = std::move(data);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        issues.Error(PackageErrorCode::Internal, IssueSite{"package"}, "out of memory while loading");
    } catch (const std::exception& error) {
        issues.Error(PackageErrorCode::Internal, IssueSite{"package"}, std::string("exception: ") + error.what());
    }
    issues.Finish();
    report.elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (issues.ErrorCount() != 0) {
        result.reset();
    }
    return result;
}

PackageReport ValidatePackage(const fs::path& package_root, WarpPolicy policy)
{
    PackageReport report;
    LoadOptions options; options.depth=ValidationDepth::Full; options.warp_policy=policy;
    (void)LoadServerWorld(package_root, options, report);
    return report;
}

} // namespace mx::map
