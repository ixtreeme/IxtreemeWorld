#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "map/MapData.h"

// Strict world-package reader/validator (MAP-1). Format contract:
// docs/map-data-format.md. The legacy MapData.h loaders stay unchanged for
// their existing consumers (client renderer/editor, v2 only); servers and
// tools load packages through this API, which never falls back silently:
// every rejection is a structured PackageIssue with a stable code.
namespace mx::map {

// ---- format contract constants ---------------------------------------------
inline constexpr std::uint32_t kManifestVersionLegacy = 2;  // read-only transition
inline constexpr std::uint32_t kManifestVersionCurrent = 3; // written by tools
inline constexpr std::uint32_t kChunkFileMagic = 0x3143584d;  // "MXC1" little-endian
inline constexpr std::uint16_t kChunkFileVersion = 2;         // MXC1 container version
inline constexpr std::uint32_t kWorldLogicFileMagic = 0x314c584d; // "MXL1"
inline constexpr std::uint32_t kWorldLogicFileVersion = 1;
inline constexpr std::uint32_t kLayerEncodingVersion = 1; // every layer kind today

// MXC1 section types and their fixed element formats.
inline constexpr std::uint16_t kSectionHeight = 1;     // elementFormat 1: int16 LE (cm)
inline constexpr std::uint16_t kSectionSplatA = 2;     // elementFormat 4: u16 w, u16 h, RGBA8
inline constexpr std::uint16_t kSectionAttributes = 3; // elementFormat 3: uint16 LE bitfield
inline constexpr std::uint16_t kSectionSplatB = 4;     // elementFormat 4
inline constexpr std::uint8_t kElementInt16 = 1;
inline constexpr std::uint8_t kElementU16Bitfield = 3;
inline constexpr std::uint8_t kElementRgba8Image = 4;
inline constexpr std::size_t kChunkHeaderBytes = 14;
inline constexpr std::size_t kChunkTocEntryBytes = 12;
inline constexpr std::uint16_t kMaxChunkSections = 16;
// Attribute bits: bit 0 = blocked (not walkable). v3: bits 1..15 reserved = 0.
inline constexpr std::uint16_t kAttributeReservedMask = 0xfffe;

// Hard limits (reject before allocating).
inline constexpr std::uint32_t kMaxWorldSizeCells = 1u << 16;  // per axis
inline constexpr std::uint32_t kMaxChunkSizeCells = 4096;
inline constexpr std::uint64_t kMaxResidentCells = 1ull << 28; // MAP-1 keeps terrain resident
inline constexpr std::uint32_t kMaxWorldLogicRecords = 1024;   // per record type
inline constexpr std::uint32_t kMaxZoneId = 0x00ffffffu;       // above: runtime-allocated ids
inline constexpr std::uint32_t kMaxSpawnLines = 100000;
inline constexpr std::uint32_t kMaxSpawnCount = 100000;        // per spawn line
inline constexpr std::uint64_t kMaxManifestBytes = 1ull << 20;
inline constexpr std::uint64_t kMaxChunkFileBytes = 256ull << 20;
inline constexpr std::uint64_t kMaxWorldLogicBytes = 16ull << 20;
inline constexpr std::uint64_t kMaxSpawnFileBytes = 16ull << 20;

// ---- structured issues -------------------------------------------------------
// Stable numeric codes: never renumber, only append.
enum class PackageErrorCode : std::uint16_t {
    PackageRootMissing = 100,
    PackageRootNotDirectory = 101,
    FileMissing = 102,
    FileUnreadable = 103,
    FileTooLarge = 104,
    PathInvalid = 105,
    PathOutsidePackage = 106,
    ManifestCorrupt = 200,
    ManifestVersionUnsupported = 201,
    ManifestFieldInvalid = 202,
    ManifestFieldForbidden = 203,
    ManifestSizeOverflow = 204,
    UnsupportedFeature = 205,
    LegacyFieldIgnored = 206,
    LayerDuplicate = 210,
    LayerRequiredMissing = 211,
    LayerUnsupported = 212,
    LayerDeclInvalid = 213,
    LayerSkipped = 214,
    LayerNotValidated = 215,
    ChunkIndexInvalid = 220,
    ChunkSizeMismatch = 221,
    ChunkChecksumMismatch = 222,
    ChunkIntegrityUnavailable = 223,
    ChunkHeaderInvalid = 300,
    ChunkTocInvalid = 301,
    ChunkSectionRange = 302,
    ChunkSectionSize = 303,
    ChunkSectionMissing = 304,
    ChunkAttributeReservedBits = 305,
    ChunkSplatGeometry = 306,
    ChunkEdgeMismatch = 307,
    WorldLogicHeaderInvalid = 400,
    WorldLogicTruncated = 401,
    WorldLogicTrailingData = 402,
    WorldLogicIdInvalid = 403,
    WorldLogicIdDuplicate = 404,
    WorldLogicRectInvalid = 405,
    WorldLogicOutOfBounds = 406,
    WorldLogicZoneOverlap = 407,
    WorldLogicCoverageGap = 408,
    WorldLogicReferenceInvalid = 409,
    WorldLogicSpawnOutsideZone = 410,
    WorldLogicWarpTargetInvalid = 411,
    WorldLogicWarpCycle = 412,
    WorldLogicWarpTargetInTrigger = 413,
    WorldLogicNoZones = 414,
    SpawnsSyntax = 500,
    SpawnsFieldInvalid = 501,
    SpawnsOutOfBounds = 502,
    SpawnsTooMany = 503,
    SpawnsMobTypeUnknown = 504,
    StartupDataInvalid = 800, // server-side startup data (mob types, config)
    Internal = 900,
};
const char* ToString(PackageErrorCode code) noexcept;

enum class IssueSeverity : std::uint8_t {
    Error,   // package rejected
    Warning, // accepted; reported (review may promote it to an error)
    Info,    // documented behaviour (e.g. a skipped client-only layer)
};
const char* ToString(IssueSeverity severity) noexcept;

struct PackageIssue {
    IssueSeverity severity = IssueSeverity::Error;
    PackageErrorCode code = PackageErrorCode::Internal;
    std::string package;  // world id (or root folder name before the manifest is read)
    std::string layer;    // manifest, chunkIndex, height, attributes, splatA, splatB, worldLogic, mobSpawns, water
    std::string file;     // package-relative path ("" = package level)
    std::string field;    // record / field path, e.g. "zones[2].bounds.min_x"
    std::int64_t offset = -1;  // byte offset (binary) or line number (text); -1 = n/a
    std::int32_t chunk_x = -1; // -1 = n/a
    std::int32_t chunk_y = -1;
    std::string reason;
    std::string expected;
    // One line: SEVERITY CODE_NAME(code) package=.. layer=.. file=.. ...
    std::string Format() const;
};

// ---- validation depth ---------------------------------------------------------
// Startup: everything the server simulates from is fully validated before it
//   is used -- manifest, layer declarations, chunk index, every chunk's header,
//   TOC, section ranges and the decoded height + attribute content (+ CRC in
//   v3), worldlogic and the spawn table. MAP-1 keeps the whole terrain
//   resident, so every chunk is a startup chunk. Client-only sections are only
//   range-checked (their bytes are never decoded or kept).
// Full: Startup + client-only data structurally decoded (splat headers,
//   geometry consistency across chunks, client layer files present/readable).
//   This is what the offline validator runs. Layers without a validator
//   (water) are reported as LayerNotValidated, never as "validated".
enum class ValidationDepth : std::uint8_t { Startup, Full };

enum class LayerKind : std::uint16_t {
    Height = 0,
    Attributes = 1,
    SplatA = 2,
    SplatB = 3,
    WorldLogic = 4,
    MobSpawns = 5,
    Water = 6,
};
const char* ToString(LayerKind kind) noexcept;

enum class LayerAudience : std::uint8_t { Server = 0, Client = 1, Shared = 2 };
const char* ToString(LayerAudience audience) noexcept;

enum class LayerStatus : std::uint8_t {
    Loaded,         // decoded and kept by the server
    Validated,      // decoded and checked, not kept (client data under Full)
    RangeChecked,   // only its bytes' location was checked (client data under Startup)
    Skipped,        // optional + not for this consumer, or optional + unsupported
    NotValidated,   // present, no validator exists yet (reported, not claimed)
    Absent,         // optional and not in the package
};
const char* ToString(LayerStatus status) noexcept;

struct LayerInfo {
    LayerKind kind = LayerKind::Height;
    std::uint16_t raw_kind = 0; // as declared (unknown kinds keep their number)
    bool required = false;
    LayerAudience audience = LayerAudience::Server;
    std::uint32_t version = 0;
    std::string file; // package-relative; empty for chunk-section layers
    bool implied = false; // v2: derived by the legacy rule, not declared
    LayerStatus status = LayerStatus::Absent;
};

struct ChunkEntry {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::string file;
    std::uint64_t byte_size = 0;              // v3 declared size (0 = unknown, v2)
    std::optional<std::uint32_t> crc32;       // v3 only
};

struct PackageManifest {
    std::uint32_t format_version = 0;
    std::string world_id;
    std::string world_name;
    double origin_x = 0.0;
    double origin_y = 0.0;
    std::uint32_t size_cells_x = 0;
    std::uint32_t size_cells_y = 0;
    float cell_size_m = 0.0f;
    std::uint32_t chunk_size_cells = 0;
    std::uint32_t chunk_grid_x = 0;
    std::uint32_t chunk_grid_y = 0;
    std::vector<LayerInfo> layers;
    std::vector<ChunkEntry> chunks; // row-major (y, then x)
    std::size_t texture_palette_entries = 0; // client render data, never loaded

    const LayerInfo* FindLayer(LayerKind kind) const noexcept;
    double ExtentX() const noexcept
    {
        return static_cast<double>(size_cells_x) * cell_size_m;
    }
    double ExtentY() const noexcept
    {
        return static_cast<double>(size_cells_y) * cell_size_m;
    }
};

// One mob spawn line (mob_spawns format v1). The mob type id is checked
// against the server's type registry by the server, not here.
struct SpawnRecord {
    std::uint32_t mob_type_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::uint32_t count = 0;
    float radius = 0.0f;
    std::uint32_t line = 0; // 1-based source line
};

struct PackageReport {
    std::filesystem::path root; // canonical package directory
    ValidationDepth depth = ValidationDepth::Startup;
    PackageManifest manifest;
    std::vector<PackageIssue> issues;
    std::uint32_t files_read = 0;
    std::uint64_t bytes_read = 0;
    std::uint32_t chunks_checked = 0;
    std::uint64_t resident_terrain_bytes = 0; // server-kept terrain arrays
    double elapsed_ms = 0.0;

    bool Ok() const noexcept; // no Error issue
    std::size_t Count(IssueSeverity severity) const noexcept;
    const PackageIssue* FirstError() const noexcept;
};

// What a server keeps from a package: heights + attributes (no splat, no
// palette), the worldlogic and the spawn table.
struct ServerWorldData {
    HeightField terrain;
    WorldLogic logic;
    std::vector<SpawnRecord> spawns;
    bool spawn_layer_present = false;
};

// Loads and validates everything a server needs from `package_root` at the
// requested depth. nullopt <=> report has at least one Error issue; nothing
// partially loaded is returned. Never throws (internal failures become
// Internal issues).
std::optional<ServerWorldData> LoadServerWorld(const std::filesystem::path& package_root,
                                               ValidationDepth depth,
                                               PackageReport& report);

// Offline check of a whole package (Full depth), discarding the data.
PackageReport ValidatePackage(const std::filesystem::path& package_root);

// Package-relative reference -> absolute path inside the package, or nullopt
// with the reason (absolute, drive/stream syntax, backslash, '.'/'..'
// components, or a canonical location outside the canonical root -- compared
// component by component, never as a string prefix). Exposed for tests.
std::optional<std::filesystem::path> ResolvePackageReference(const std::filesystem::path& canonical_root,
                                                             std::string_view reference,
                                                             std::string& why_not);

// CRC-32 (IEEE 802.3 polynomial, identical to zlib crc32()).
std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) noexcept;

} // namespace mx::map
