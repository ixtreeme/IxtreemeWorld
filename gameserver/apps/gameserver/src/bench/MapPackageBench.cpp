#include "HardeningBench.h"

#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "map/MapData.h"
#include "map/WorldPackage.h"
#include "map/WorldPackageWriter.h"
#include "schema/map_manifest.capnp.h"

#include "../world/package/WorldPackageLoader.h"

// MAP-1: world package corpus. Positive packages (generated v3 with and
// without client data, generated + checked-in v2) and one fixture per
// rejection rule, each loaded through the server's real loader
// (gs::game::LoadWorldPackage). A negative case passes only when the load is
// refused AND the expected stable code is reported; a positive case only when
// it loads with no error. Terrain of the checked-in test map is compared
// sample by sample against the legacy loader (independent oracle).
namespace gs::bench {
namespace {

namespace fs = std::filesystem;
using mx::map::PackageErrorCode;
using mx::map::ValidationDepth;

std::string Fmt(const char* format, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

struct Corpus {
    fs::path root;
    int failures = 0;
    int passes = 0;
    int skipped = 0;

    void Report(const std::string& name, bool pass, const std::string& detail)
    {
        std::printf("WORLDPACKAGE %s %s: %s\n", name.c_str(), detail.c_str(), pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        (pass ? passes : failures) += 1;
    }
    void Skip(const std::string& name, const std::string& why)
    {
        std::printf("WORLDPACKAGE %s %s: SKIPPED\n", name.c_str(), why.c_str());
        std::fflush(stdout);
        ++skipped;
    }
    fs::path Dir(const std::string& name) const
    {
        const fs::path dir = root / name;
        std::error_code ec;
        fs::remove_all(dir, ec);
        return dir;
    }
};

struct Loaded {
    std::optional<gs::game::LoadedWorld> world;
    mx::map::PackageReport report;

    bool HasError(PackageErrorCode code) const
    {
        for (const auto& issue : report.issues) {
            if (issue.code == code && issue.severity == mx::map::IssueSeverity::Error) {
                return true;
            }
        }
        return false;
    }
    bool Has(PackageErrorCode code, mx::map::IssueSeverity severity) const
    {
        for (const auto& issue : report.issues) {
            if (issue.code == code && issue.severity == severity) {
                return true;
            }
        }
        return false;
    }
    std::string First() const
    {
        const auto* e = report.FirstError();
        return e != nullptr ? e->Format() : std::string("no error");
    }
};

Loaded Load(const fs::path& dir, ValidationDepth depth = ValidationDepth::Startup,
            std::string mob_types = IXTREEME_DEFAULT_MOB_TYPES_CONFIG)
{
    Loaded out;
    gs::game::WorldLoadRequest request;
    request.package_root = dir;
    request.mob_types_config = std::move(mob_types);
    request.depth = depth;
    out.world = gs::game::LoadWorldPackage(request, out.report);
    return out;
}

// Base fixture: 64 x 64 cells of 4 m (256 m world), 32-cell chunks (2x2),
// gentle slope, a blocked 3x3 cell patch at cells 10..12, two bootstrap
// zones, one player spawn region, one warp, one mob spawn line.
mx::map::PackageWriteSpec BaseSpec(std::uint32_t format = 3, bool splats = false)
{
    mx::map::PackageWriteSpec spec;
    spec.format_version = format;
    spec.world_id = "corpus";
    spec.world_name = "MAP-1 corpus";
    spec.size_cells = 64;
    spec.cell_size_m = 4.0f;
    spec.chunk_size_cells = 32;
    spec.height_cm = [](std::uint32_t vx, std::uint32_t vy) {
        return static_cast<std::int16_t>(100 + vx * 3 + vy * 2);
    };
    spec.attributes = [](std::uint32_t cx, std::uint32_t cy) -> std::uint16_t {
        return cx >= 10 && cx <= 12 && cy >= 10 && cy <= 12 ? 0x0001 : 0;
    };
    if (splats || format == 2) {
        spec.splat_size = 8;
        spec.splat = [](std::uint32_t, std::uint32_t, std::vector<std::uint8_t>& a, std::vector<std::uint8_t>& b) {
            a.assign(8 * 8 * 4, 0x40);
            b.assign(8 * 8 * 4, 0x80);
        };
        spec.texture_palette = {"assets/Textures/a.dds"};
    }
    spec.logic.zones = {{1, "west", {0.0f, 0.0f, 128.0f, 256.0f}}, {2, "east", {128.0f, 0.0f, 256.0f, 256.0f}}};
    spec.logic.spawns = {{1, 1, {10.0f, 60.0f, 20.0f, 70.0f}}};
    spec.logic.warps = {{1, {60.0f, 60.0f, 64.0f, 64.0f}, 200.0f, 200.0f}};
    spec.mob_spawns = std::string("# corpus\nmob_type_id=1 x=50 y=50 count=2 radius=5\n");
    spec.overwrite = true;
    return spec;
}

bool Write(const fs::path& dir, const mx::map::PackageWriteSpec& spec)
{
    const auto result = mx::map::WritePackage(dir, spec);
    if (!result.ok) {
        std::printf("WORLDPACKAGE fixture write failed at %s: %s\n", dir.string().c_str(), result.error.c_str());
    }
    return result.ok;
}

std::vector<std::uint8_t> ReadAll(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteAll(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void PutU16At(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void PutU32At(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        bytes[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
    }
}

std::uint32_t GetU32At(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

// ---- negative case helpers ----------------------------------------------------------

void ExpectRejected(Corpus& c, const std::string& name, const fs::path& dir, PackageErrorCode code,
                    ValidationDepth depth = ValidationDepth::Startup,
                    std::string mob_types = IXTREEME_DEFAULT_MOB_TYPES_CONFIG)
{
    const auto loaded = Load(dir, depth, std::move(mob_types));
    const bool pass = !loaded.world && loaded.HasError(code);
    c.Report(name, pass,
             Fmt("expect %s -> %s; first: %s", ToString(code), loaded.world ? "LOADED" : "refused",
                 loaded.First().c_str()));
}

void ExpectSpecRejected(Corpus& c, const std::string& name, const mx::map::PackageWriteSpec& spec,
                        PackageErrorCode code)
{
    const auto dir = c.Dir(name);
    if (!Write(dir, spec)) {
        c.Report(name, false, "fixture not written");
        return;
    }
    ExpectRejected(c, name, dir, code);
}

void ExpectLoaded(Corpus& c, const std::string& name, const fs::path& dir,
                  ValidationDepth depth = ValidationDepth::Startup)
{
    const auto loaded = Load(dir, depth);
    c.Report(name, loaded.world.has_value(),
             Fmt("load=%s warnings=%zu infos=%zu first_error=%s", loaded.world ? "ok" : "REFUSED",
                 loaded.report.Count(mx::map::IssueSeverity::Warning),
                 loaded.report.Count(mx::map::IssueSeverity::Info), loaded.First().c_str()));
}

// Offsets inside a writer-produced chunk (TOC order: height, attributes,
// [splatA, splatB]); read back from the file so the tests follow the format.
struct TocEntry {
    std::size_t toc_offset;
    std::uint32_t offset;
    std::uint32_t length;
};
TocEntry TocAt(const std::vector<std::uint8_t>& chunk, int index)
{
    const std::size_t toc = mx::map::kChunkHeaderBytes + static_cast<std::size_t>(index) * mx::map::kChunkTocEntryBytes;
    return TocEntry{toc, GetU32At(chunk, toc + 4), GetU32At(chunk, toc + 8)};
}

} // namespace

int RunWorldPackageScenario(const std::string& fixtures_out)
{
    Corpus c;
    c.root = fs::temp_directory_path() / "ixw_worldpackage";
    std::error_code ec;
    fs::remove_all(c.root, ec);
    fs::create_directories(c.root, ec);

    // ===== positive ===========================================================
    {
        const auto dir = c.Dir("valid_v3_server_only");
        Write(dir, BaseSpec());
        const auto loaded = Load(dir);
        const bool ok = loaded.world && loaded.world->terrain.splat_a_rgba8.empty() &&
                        loaded.world->spawn_points.size() == 1 && loaded.world->logic.zones.size() == 2 &&
                        loaded.report.resident_terrain_bytes == 65ull * 65 * 2 + 64ull * 64 * 2;
        c.Report("valid-v3-server-only", ok,
                 Fmt("load=%s chunks=%u resident=%lluB splat_bytes=%zu spawn_points=%zu first=%s",
                     loaded.world ? "ok" : "REFUSED", loaded.report.chunks_checked,
                     static_cast<unsigned long long>(loaded.report.resident_terrain_bytes),
                     loaded.world ? loaded.world->terrain.splat_a_rgba8.size() : 0,
                     loaded.world ? loaded.world->spawn_points.size() : 0, loaded.First().c_str()));
        // Terrain content is what the writer put in (height + blocked patch).
        if (loaded.world) {
            const auto& t = loaded.world->terrain;
            const bool heights = t.HeightMetersAt(10, 20) == static_cast<float>(100 + 30 + 40) * 0.01f &&
                                 t.HeightMetersAt(64, 64) == static_cast<float>(100 + 192 + 128) * 0.01f;
            const bool walk = !t.IsWalkable(11 * 4.0f + 1.0f, 11 * 4.0f + 1.0f) && t.IsWalkable(100.0f, 100.0f);
            c.Report("valid-v3-content", heights && walk,
                     Fmt("height(10,20)=%.2fm height(64,64)=%.2fm blocked(cell 11,11)=%d walkable(100,100)=%d",
                         t.HeightMetersAt(10, 20), t.HeightMetersAt(64, 64),
                         t.IsWalkable(45.0f, 45.0f) ? 0 : 1, t.IsWalkable(100.0f, 100.0f) ? 1 : 0));
        }
    }
    {
        const auto dir = c.Dir("valid_v3_client");
        Write(dir, BaseSpec(3, true));
        const auto startup = Load(dir, ValidationDepth::Startup);
        const auto full = Load(dir, ValidationDepth::Full);
        const auto* startup_splat = startup.report.manifest.FindLayer(mx::map::LayerKind::SplatA);
        const auto* full_splat = full.report.manifest.FindLayer(mx::map::LayerKind::SplatA);
        const bool ok = startup.world && full.world && startup.world->terrain.splat_a_rgba8.empty() &&
                        startup_splat != nullptr && startup_splat->status == mx::map::LayerStatus::RangeChecked &&
                        full_splat != nullptr && full_splat->status == mx::map::LayerStatus::Validated;
        c.Report("valid-v3-with-client-data", ok,
                 Fmt("startup: load=%s splatA=%s kept=%zuB; full: load=%s splatA=%s",
                     startup.world ? "ok" : "REFUSED",
                     startup_splat != nullptr ? mx::map::ToString(startup_splat->status) : "-",
                     startup.world ? startup.world->terrain.splat_a_rgba8.size() : 0,
                     full.world ? "ok" : "REFUSED",
                     full_splat != nullptr ? mx::map::ToString(full_splat->status) : "-"));
    }
    {
        const auto dir = c.Dir("valid_v2");
        Write(dir, BaseSpec(2));
        ExpectLoaded(c, "valid-v2-legacy-layout", dir, ValidationDepth::Full);
    }
    {
        // The checked-in test map (v2, editor-edited chunks): loads, and its
        // terrain equals the legacy loader's sample for sample.
        const fs::path test_map = IXTREEME_TEST_MAP_ROOT;
        const auto loaded = Load(test_map, ValidationDepth::Full);
        const auto legacy = mx::map::LoadHeightField(
            [&](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
                std::ifstream file(test_map / fs::path(path).relative_path(), std::ios::binary);
                if (!file) {
                    return std::nullopt;
                }
                return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
                                                 std::istreambuf_iterator<char>());
            },
            ".");
        std::size_t height_diff = 0;
        std::size_t attr_diff = 0;
        if (loaded.world && legacy) {
            const auto& t = loaded.world->terrain;
            height_diff = t.heights_cm.size() == legacy->heights_cm.size() ? 0 : 1;
            for (std::size_t i = 0; height_diff == 0 && i < t.heights_cm.size(); ++i) {
                height_diff += t.heights_cm[i] != legacy->heights_cm[i] ? 1 : 0;
            }
            attr_diff = t.attributes.size() == legacy->attributes.size() ? 0 : 1;
            for (std::size_t i = 0; attr_diff == 0 && i < t.attributes.size(); ++i) {
                attr_diff += t.attributes[i] != legacy->attributes[i] ? 1 : 0;
            }
        }
        c.Report("checked-in-test-map", loaded.world && legacy && height_diff == 0 && attr_diff == 0 &&
                                            loaded.world->spawn_points.size() == 4,
                 Fmt("load=%s legacy=%s height_mismatches=%zu attribute_mismatches=%zu spawn_points=%zu "
                     "warnings=%zu (%s)",
                     loaded.world ? "ok" : "REFUSED", legacy ? "ok" : "failed", height_diff, attr_diff,
                     loaded.world ? loaded.world->spawn_points.size() : 0,
                     loaded.report.Count(mx::map::IssueSeverity::Warning),
                     loaded.Has(PackageErrorCode::WorldLogicWarpTargetInTrigger, mx::map::IssueSeverity::Warning)
                         ? "warp 1 -> warp 2 chain"
                         : "-"));
    }
    {
        auto spec = BaseSpec();
        spec.mob_spawns.reset();
        const auto dir = c.Dir("valid_no_spawns");
        Write(dir, spec);
        const auto loaded = Load(dir);
        c.Report("optional-spawn-layer-absent", loaded.world && loaded.world->spawn_points.empty(),
                 Fmt("load=%s spawn_points=%zu", loaded.world ? "ok" : "REFUSED",
                     loaded.world ? loaded.world->spawn_points.size() : 0));
    }
    {
        // Optional layer of an unsupported version: skipped with a warning.
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            auto layers = m.getLayers();
            for (auto layer : layers) {
                if (layer.getKind() == mx::map::schema::LayerKind::MOB_SPAWNS) {
                    layer.setVersion(9);
                }
            }
        };
        const auto dir = c.Dir("optional_layer_unsupported");
        Write(dir, spec);
        const auto loaded = Load(dir);
        c.Report("optional-layer-unsupported-version-skipped",
                 loaded.world && loaded.Has(PackageErrorCode::LayerSkipped, mx::map::IssueSeverity::Warning),
                 Fmt("load=%s", loaded.world ? "ok (spawn layer skipped + warned)" : "REFUSED"));
    }
    {
        // A warp chain (target inside another trigger, no cycle): warning.
        auto spec = BaseSpec();
        spec.logic.warps = {{1, {60.0f, 60.0f, 64.0f, 64.0f}, 201.0f, 201.0f},
                            {2, {200.0f, 200.0f, 204.0f, 204.0f}, 30.0f, 30.0f}};
        const auto dir = c.Dir("warp_chain");
        Write(dir, spec);
        const auto loaded = Load(dir);
        c.Report("warp-chain-warning",
                 loaded.world &&
                     loaded.Has(PackageErrorCode::WorldLogicWarpTargetInTrigger, mx::map::IssueSeverity::Warning),
                 Fmt("load=%s", loaded.world ? "ok + WARP_TARGET_IN_TRIGGER warning" : "REFUSED"));
    }

    // ===== package / path =======================================================
    ExpectRejected(c, "root-missing", c.root / "does_not_exist", PackageErrorCode::PackageRootMissing);
    {
        const auto file = c.root / "not_a_dir";
        WriteAll(file, {1, 2, 3});
        ExpectRejected(c, "root-is-a-file", file, PackageErrorCode::PackageRootNotDirectory);
    }
    {
        const auto dir = c.Dir("manifest_missing");
        Write(dir, BaseSpec());
        fs::remove(dir / "map.manifest", ec);
        ExpectRejected(c, "manifest-missing", dir, PackageErrorCode::FileMissing);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            m.getChunks()[1].setFile("../escape.mxchunk");
        };
        ExpectSpecRejected(c, "path-traversal", spec, PackageErrorCode::PathInvalid);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            m.getChunks()[1].setFile("C:/Windows/win.ini");
        };
        ExpectSpecRejected(c, "path-absolute-drive", spec, PackageErrorCode::PathInvalid);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            m.getChunks()[1].setFile("/etc/passwd");
        };
        ExpectSpecRejected(c, "path-absolute-root", spec, PackageErrorCode::PathInvalid);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            m.getChunks()[1].setFile("chunks\\chunk_1_0.mxchunk");
        };
        ExpectSpecRejected(c, "path-backslash", spec, PackageErrorCode::PathInvalid);
    }
    {
        // A directory symlink inside the package that leads outside it:
        // canonical containment (component-wise) must refuse it.
        const auto dir = c.Dir("path_symlink");
        const auto outside = c.Dir("path_symlink_outside");
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            m.getChunks()[0].setFile("link/chunk_0_0.mxchunk");
        };
        Write(dir, spec);
        fs::create_directories(outside, ec);
        fs::copy_file(dir / "chunks" / "chunk_0_0.mxchunk", outside / "chunk_0_0.mxchunk", ec);
        std::error_code link_ec;
        fs::create_directory_symlink(outside, dir / "link", link_ec);
        if (link_ec) {
            c.Skip("path-symlink-escape", "cannot create a directory symlink here (" + link_ec.message() +
                                              "); containment still covered by the '..' cases");
        } else {
            ExpectRejected(c, "path-symlink-escape", dir, PackageErrorCode::PathOutsidePackage);
        }
    }
    {
        // Sibling directory with the same name prefix: rejected by the lexical
        // ".." rule (the canonical component-wise check is only reachable
        // through links: see path-symlink-escape).
        const auto pkg = c.Dir("pkg");
        const auto evil = c.Dir("pkg-evil");
        fs::create_directories(pkg, ec);
        fs::create_directories(evil, ec);
        WriteAll(evil / "x.bin", {1});
        std::string why;
        const auto canonical = fs::canonical(pkg);
        const auto resolved = mx::map::ResolvePackageReference(canonical, "../pkg-evil/x.bin", why);
        const auto inside = mx::map::ResolvePackageReference(canonical, "sub/x.bin", why);
        c.Report("path-dotdot-sibling", !resolved && inside.has_value(),
                 Fmt("'../pkg-evil/x.bin' -> %s; 'sub/x.bin' -> %s", resolved ? "ACCEPTED" : "refused",
                     inside ? "inside" : "refused"));
    }

    // ===== manifest ===============================================================
    {
        const auto dir = c.Dir("manifest_not_words");
        Write(dir, BaseSpec());
        WriteAll(dir / "map.manifest", {1, 2, 3, 4, 5});
        ExpectRejected(c, "manifest-not-word-aligned", dir, PackageErrorCode::ManifestCorrupt);
    }
    {
        const auto dir = c.Dir("manifest_garbage");
        Write(dir, BaseSpec());
        std::vector<std::uint8_t> garbage(64);
        for (std::size_t i = 0; i < garbage.size(); ++i) {
            garbage[i] = static_cast<std::uint8_t>(0xA5 ^ (i * 37));
        }
        WriteAll(dir / "map.manifest", garbage);
        ExpectRejected(c, "manifest-garbage", dir, PackageErrorCode::ManifestCorrupt);
    }
    {
        const auto dir = c.Dir("manifest_truncated");
        Write(dir, BaseSpec());
        auto bytes = ReadAll(dir / "map.manifest");
        bytes.resize((bytes.size() / 2) & ~std::size_t{7});
        WriteAll(dir / "map.manifest", bytes);
        ExpectRejected(c, "manifest-truncated", dir, PackageErrorCode::ManifestCorrupt);
    }
    {
        const auto dir = c.Dir("manifest_trailing");
        Write(dir, BaseSpec());
        auto bytes = ReadAll(dir / "map.manifest");
        bytes.insert(bytes.end(), 8, 0);
        WriteAll(dir / "map.manifest", bytes);
        ExpectRejected(c, "manifest-trailing-bytes", dir, PackageErrorCode::ManifestCorrupt);
    }
    for (const std::uint32_t version : {1u, 4u, 7u}) {
        auto spec = BaseSpec();
        spec.patch_manifest = [version](mx::map::schema::MapManifest::Builder& m) { m.setFormatVersion(version); };
        ExpectSpecRejected(c, "manifest-version-" + std::to_string(version), spec,
                           PackageErrorCode::ManifestVersionUnsupported);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            m.initZoneGridDims().setX(2);
            m.getZoneGridDims().setY(2);
        };
        ExpectSpecRejected(c, "manifest-v3-zone-grid", spec, PackageErrorCode::ManifestFieldForbidden);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) { m.setWorldLogicFile("worldlogic.dat"); };
        ExpectSpecRejected(c, "manifest-v3-legacy-reference", spec, PackageErrorCode::ManifestFieldForbidden);
    }
    {
        auto spec = BaseSpec(2);
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) { m.setWorldSizeCellsY(64); };
        ExpectSpecRejected(c, "manifest-v2-with-v3-fields", spec, PackageErrorCode::ManifestFieldForbidden);
    }
    for (const float cell : {std::numeric_limits<float>::quiet_NaN(), 0.0f, -4.0f,
                             std::numeric_limits<float>::infinity()}) {
        auto spec = BaseSpec();
        spec.patch_manifest = [cell](mx::map::schema::MapManifest::Builder& m) { m.setCellSizeMeters(cell); };
        ExpectSpecRejected(c, "manifest-cell-size-" + std::to_string(cell), spec,
                           PackageErrorCode::ManifestFieldInvalid);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) { m.setWorldId("bad id!"); };
        ExpectSpecRejected(c, "manifest-world-id", spec, PackageErrorCode::ManifestFieldInvalid);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) { m.setWorldSizeCells(0); };
        ExpectSpecRejected(c, "manifest-world-size-zero", spec, PackageErrorCode::ManifestFieldInvalid);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            m.setWorldSizeCells(65536);
            m.setWorldSizeCellsY(65536);
            m.setChunkSizeCells(4096);
            m.getChunkGrid().setX(16);
            m.getChunkGrid().setY(16);
        };
        ExpectSpecRejected(c, "manifest-size-overflow", spec, PackageErrorCode::ManifestSizeOverflow);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) { m.setWorldSizeCellsY(32); };
        ExpectSpecRejected(c, "manifest-non-square", spec, PackageErrorCode::UnsupportedFeature);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) { m.getOrigin().setX(1000.0); };
        ExpectSpecRejected(c, "manifest-origin-nonzero", spec, PackageErrorCode::UnsupportedFeature);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            m.setWorldSizeCells(60);
            m.setWorldSizeCellsY(60);
        };
        ExpectSpecRejected(c, "manifest-partial-chunk", spec, PackageErrorCode::UnsupportedFeature);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) { m.getChunkGrid().setX(3); };
        ExpectSpecRejected(c, "manifest-chunk-grid", spec, PackageErrorCode::ManifestFieldInvalid);
    }

    // ===== layers ===================================================================
    auto patch_layer = [](mx::map::schema::LayerKind kind, std::function<void(mx::map::schema::LayerDecl::Builder)> fn) {
        return [kind, fn](mx::map::schema::MapManifest::Builder& m) {
            for (auto layer : m.getLayers()) {
                if (layer.getKind() == kind) {
                    fn(layer);
                }
            }
        };
    };
    {
        auto spec = BaseSpec();
        spec.patch_manifest = patch_layer(mx::map::schema::LayerKind::MOB_SPAWNS, [](auto layer) {
            layer.setKind(mx::map::schema::LayerKind::HEIGHT);
            layer.setFile("");
        });
        ExpectSpecRejected(c, "layer-duplicate", spec, PackageErrorCode::LayerDuplicate);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = patch_layer(mx::map::schema::LayerKind::MOB_SPAWNS, [](auto layer) {
            layer.setKind(static_cast<mx::map::schema::LayerKind>(42));
            layer.setRequired(true);
        });
        ExpectSpecRejected(c, "layer-unknown-required", spec, PackageErrorCode::LayerUnsupported);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = patch_layer(mx::map::schema::LayerKind::HEIGHT, [](auto layer) { layer.setVersion(2); });
        ExpectSpecRejected(c, "layer-required-version", spec, PackageErrorCode::LayerUnsupported);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            auto old = m.getLayers();
            std::vector<std::tuple<mx::map::schema::LayerKind, bool, mx::map::schema::LayerAudience, std::string>> keep;
            for (auto layer : old) {
                if (layer.getKind() != mx::map::schema::LayerKind::HEIGHT) {
                    keep.emplace_back(layer.getKind(), layer.getRequired(), layer.getAudience(), layer.getFile().cStr());
                }
            }
            auto layers = m.initLayers(static_cast<unsigned>(keep.size()));
            for (unsigned i = 0; i < keep.size(); ++i) {
                layers[i].setKind(std::get<0>(keep[i]));
                layers[i].setRequired(std::get<1>(keep[i]));
                layers[i].setAudience(std::get<2>(keep[i]));
                layers[i].setVersion(1);
                layers[i].setFile(std::get<3>(keep[i]));
            }
        };
        ExpectSpecRejected(c, "layer-height-missing", spec, PackageErrorCode::LayerRequiredMissing);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = patch_layer(mx::map::schema::LayerKind::WORLD_LOGIC,
                                          [](auto layer) { layer.setAudience(mx::map::schema::LayerAudience::CLIENT); });
        ExpectSpecRejected(c, "layer-server-data-client-only", spec, PackageErrorCode::LayerDeclInvalid);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = patch_layer(mx::map::schema::LayerKind::HEIGHT, [](auto layer) { layer.setFile("h.bin"); });
        ExpectSpecRejected(c, "layer-chunk-section-with-file", spec, PackageErrorCode::LayerDeclInvalid);
    }
    {
        auto spec = BaseSpec();
        spec.mob_spawns_required = true;
        const auto dir = c.Dir("layer_required_file_missing");
        Write(dir, spec);
        fs::remove(dir / "mob_spawns.conf", ec);
        ExpectRejected(c, "layer-required-file-missing", dir, PackageErrorCode::FileMissing);
    }
    {
        const auto dir = c.Dir("worldlogic_missing");
        Write(dir, BaseSpec());
        fs::remove(dir / "worldlogic.dat", ec);
        ExpectRejected(c, "worldlogic-file-missing", dir, PackageErrorCode::FileMissing);
    }

    // ===== chunk index + integrity (v3) ===================================================
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            auto old = m.getChunks();
            std::vector<std::tuple<std::uint32_t, std::uint32_t, std::string, std::uint64_t, std::uint32_t>> keep;
            for (unsigned i = 0; i + 1 < old.size(); ++i) {
                keep.emplace_back(old[i].getX(), old[i].getY(), old[i].getFile().cStr(), old[i].getByteSize(),
                                  old[i].getCrc32());
            }
            auto chunks = m.initChunks(static_cast<unsigned>(keep.size()));
            for (unsigned i = 0; i < keep.size(); ++i) {
                chunks[i].setX(std::get<0>(keep[i]));
                chunks[i].setY(std::get<1>(keep[i]));
                chunks[i].setFile(std::get<2>(keep[i]));
                chunks[i].setByteSize(std::get<3>(keep[i]));
                chunks[i].setCrc32(std::get<4>(keep[i]));
            }
        };
        ExpectSpecRejected(c, "index-missing-entry", spec, PackageErrorCode::ChunkIndexInvalid);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) { m.getChunks()[1].setX(0); };
        ExpectSpecRejected(c, "index-duplicate-cell", spec, PackageErrorCode::ChunkIndexInvalid);
    }
    {
        auto spec = BaseSpec();
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) { m.getChunks()[1].setX(7); };
        ExpectSpecRejected(c, "index-outside-grid", spec, PackageErrorCode::ChunkIndexInvalid);
    }
    {
        const auto dir = c.Dir("chunk_missing");
        Write(dir, BaseSpec());
        fs::remove(dir / "chunks" / "chunk_1_1.mxchunk", ec);
        ExpectRejected(c, "chunk-file-missing", dir, PackageErrorCode::FileMissing);
    }
    {
        const auto dir = c.Dir("chunk_truncated_v3");
        Write(dir, BaseSpec());
        auto bytes = ReadAll(dir / "chunks" / "chunk_1_0.mxchunk");
        bytes.resize(bytes.size() - 100);
        WriteAll(dir / "chunks" / "chunk_1_0.mxchunk", bytes);
        ExpectRejected(c, "chunk-truncated-v3", dir, PackageErrorCode::ChunkSizeMismatch);
    }
    {
        const auto dir = c.Dir("chunk_bitflip_v3");
        Write(dir, BaseSpec());
        auto bytes = ReadAll(dir / "chunks" / "chunk_0_1.mxchunk");
        bytes[bytes.size() / 2] ^= 0x10;
        WriteAll(dir / "chunks" / "chunk_0_1.mxchunk", bytes);
        ExpectRejected(c, "chunk-content-bitflip-v3", dir, PackageErrorCode::ChunkChecksumMismatch);
    }

    // ===== chunk structure (v2: no CRC, so the structural rule itself fires) ==============
    auto v2_chunk_case = [&](const std::string& name, PackageErrorCode code,
                             const std::function<void(std::vector<std::uint8_t>&)>& mutate,
                             ValidationDepth depth = ValidationDepth::Startup) {
        const auto dir = c.Dir(name);
        Write(dir, BaseSpec(2));
        auto bytes = ReadAll(dir / "chunks" / "chunk_1_0.mxchunk");
        mutate(bytes);
        WriteAll(dir / "chunks" / "chunk_1_0.mxchunk", bytes);
        ExpectRejected(c, name, dir, code, depth);
    };
    v2_chunk_case("chunk-bad-magic", PackageErrorCode::ChunkHeaderInvalid, [](auto& b) { b[0] = 'X'; });
    v2_chunk_case("chunk-bad-version", PackageErrorCode::ChunkHeaderInvalid, [](auto& b) { PutU16At(b, 4, 3); });
    v2_chunk_case("chunk-wrong-coordinates", PackageErrorCode::ChunkHeaderInvalid, [](auto& b) { PutU16At(b, 6, 0); });
    v2_chunk_case("chunk-wrong-cells", PackageErrorCode::ChunkHeaderInvalid, [](auto& b) { PutU16At(b, 10, 31); });
    v2_chunk_case("chunk-truncated-header", PackageErrorCode::ChunkHeaderInvalid, [](auto& b) { b.resize(10); });
    v2_chunk_case("chunk-truncated-toc", PackageErrorCode::ChunkTocInvalid, [](auto& b) { b.resize(30); });
    v2_chunk_case("chunk-zero-sections", PackageErrorCode::ChunkTocInvalid, [](auto& b) { PutU16At(b, 12, 0); });
    v2_chunk_case("chunk-section-offset-beyond-file", PackageErrorCode::ChunkSectionRange,
                  [](auto& b) { PutU32At(b, TocAt(b, 0).toc_offset + 4, static_cast<std::uint32_t>(b.size())); });
    v2_chunk_case("chunk-section-inside-toc", PackageErrorCode::ChunkSectionRange,
                  [](auto& b) { PutU32At(b, TocAt(b, 1).toc_offset + 4, 20); });
    v2_chunk_case("chunk-truncated-body", PackageErrorCode::ChunkSectionRange, [](auto& b) { b.resize(b.size() - 64); });
    v2_chunk_case("chunk-trailing-bytes", PackageErrorCode::ChunkSectionRange,
                  [](auto& b) { b.insert(b.end(), 16, 0); });
    v2_chunk_case("chunk-duplicate-section", PackageErrorCode::ChunkTocInvalid,
                  [](auto& b) { PutU16At(b, TocAt(b, 1).toc_offset, mx::map::kSectionHeight); });
    v2_chunk_case("chunk-wrong-element-format", PackageErrorCode::ChunkTocInvalid,
                  [](auto& b) { b[TocAt(b, 0).toc_offset + 2] = 3; });
    v2_chunk_case("chunk-height-length", PackageErrorCode::ChunkSectionRange, [](auto& b) {
        // Height section claims 2 bytes less: a gap appears before attributes.
        const auto h = TocAt(b, 0);
        PutU32At(b, h.toc_offset + 8, h.length - 2);
    });
    v2_chunk_case("chunk-attributes-missing", PackageErrorCode::ChunkSectionMissing, [](auto& b) {
        // Retype the attribute section as an unknown type (v2: ignored) -> no attributes.
        PutU16At(b, TocAt(b, 1).toc_offset, 9);
    });
    v2_chunk_case("chunk-edge-mismatch", PackageErrorCode::ChunkEdgeMismatch, [](auto& b) {
        // Chunk (1,0)'s west column is shared with chunk (0,0)'s east column.
        const auto h = TocAt(b, 0);
        PutU16At(b, h.offset + 5 * (32 + 1) * 2, 9999);
    });
    {
        // Reserved attribute bits: an error in v3, a warning in v2.
        auto spec = BaseSpec();
        spec.attributes = [](std::uint32_t cx, std::uint32_t) -> std::uint16_t { return cx == 3 ? 0x0004 : 0; };
        ExpectSpecRejected(c, "chunk-attribute-reserved-bits-v3", spec, PackageErrorCode::ChunkAttributeReservedBits);
        auto v2 = BaseSpec(2);
        v2.attributes = spec.attributes;
        const auto dir = c.Dir("attribute_reserved_v2");
        Write(dir, v2);
        const auto loaded = Load(dir);
        c.Report("chunk-attribute-reserved-bits-v2-warning",
                 loaded.world && loaded.Has(PackageErrorCode::ChunkAttributeReservedBits, mx::map::IssueSeverity::Warning),
                 Fmt("load=%s", loaded.world ? "ok + warning" : "REFUSED"));
    }
    {
        // Splat geometry: client data, only decoded at Full depth.
        const auto dir = c.Dir("splat_geometry");
        Write(dir, BaseSpec(2));
        auto bytes = ReadAll(dir / "chunks" / "chunk_1_1.mxchunk");
        PutU16At(bytes, TocAt(bytes, 3).offset, 7); // splat B claims 7 wide
        WriteAll(dir / "chunks" / "chunk_1_1.mxchunk", bytes);
        const auto startup = Load(dir, ValidationDepth::Startup);
        const auto full = Load(dir, ValidationDepth::Full);
        c.Report("splat-geometry-depth", startup.world && !full.world &&
                                              full.HasError(PackageErrorCode::ChunkSplatGeometry),
                 Fmt("startup (range-checked only) -> %s; full -> %s (%s)",
                     startup.world ? "loads" : "REFUSED", full.world ? "LOADS" : "refused", full.First().c_str()));
    }
    {
        // v3 splat section present although no splat layer is declared.
        auto spec = BaseSpec(3, true);
        spec.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) {
            auto old = m.getLayers();
            std::vector<std::tuple<mx::map::schema::LayerKind, bool, mx::map::schema::LayerAudience, std::string>> keep;
            for (auto layer : old) {
                if (layer.getKind() != mx::map::schema::LayerKind::SPLAT_A &&
                    layer.getKind() != mx::map::schema::LayerKind::SPLAT_B) {
                    keep.emplace_back(layer.getKind(), layer.getRequired(), layer.getAudience(), layer.getFile().cStr());
                }
            }
            auto layers = m.initLayers(static_cast<unsigned>(keep.size()));
            for (unsigned i = 0; i < keep.size(); ++i) {
                layers[i].setKind(std::get<0>(keep[i]));
                layers[i].setRequired(std::get<1>(keep[i]));
                layers[i].setAudience(std::get<2>(keep[i]));
                layers[i].setVersion(1);
                layers[i].setFile(std::get<3>(keep[i]));
            }
        };
        ExpectSpecRejected(c, "chunk-undeclared-section-v3", spec, PackageErrorCode::ChunkTocInvalid);
    }

    // ===== worldlogic ==================================================================
    auto logic_bytes_case = [&](const std::string& name, PackageErrorCode code,
                                const std::function<void(std::vector<std::uint8_t>&)>& mutate) {
        const auto dir = c.Dir(name);
        Write(dir, BaseSpec());
        auto bytes = ReadAll(dir / "worldlogic.dat");
        mutate(bytes);
        WriteAll(dir / "worldlogic.dat", bytes);
        ExpectRejected(c, name, dir, code);
    };
    logic_bytes_case("worldlogic-truncated", PackageErrorCode::WorldLogicTruncated, [](auto& b) { b.resize(b.size() - 3); });
    logic_bytes_case("worldlogic-short-header", PackageErrorCode::WorldLogicTruncated, [](auto& b) { b.resize(12); });
    logic_bytes_case("worldlogic-trailing", PackageErrorCode::WorldLogicTrailingData, [](auto& b) { b.push_back(0); });
    logic_bytes_case("worldlogic-bad-magic", PackageErrorCode::WorldLogicHeaderInvalid, [](auto& b) { b[0] ^= 0xff; });
    logic_bytes_case("worldlogic-bad-version", PackageErrorCode::WorldLogicHeaderInvalid, [](auto& b) { PutU32At(b, 4, 2); });
    logic_bytes_case("worldlogic-too-many-records", PackageErrorCode::WorldLogicHeaderInvalid,
                     [](auto& b) { PutU32At(b, 8, 5000); });
    const float nan = std::numeric_limits<float>::quiet_NaN();
    auto logic_case = [&](const std::string& name, PackageErrorCode code,
                          const std::function<void(mx::map::WorldLogic&)>& mutate) {
        auto spec = BaseSpec();
        mutate(spec.logic);
        ExpectSpecRejected(c, name, spec, code);
    };
    logic_case("worldlogic-no-zones", PackageErrorCode::WorldLogicNoZones, [](auto& l) {
        l.zones.clear();
        l.spawns.clear();
    });
    logic_case("worldlogic-zone-id-zero", PackageErrorCode::WorldLogicIdInvalid, [](auto& l) {
        l.zones[1].id = 0;
    });
    logic_case("worldlogic-zone-id-reserved-range", PackageErrorCode::WorldLogicIdInvalid, [](auto& l) {
        l.zones[1].id = 0x01000000;
    });
    logic_case("worldlogic-zone-id-duplicate", PackageErrorCode::WorldLogicIdDuplicate, [](auto& l) {
        l.zones[1].id = 1;
    });
    logic_case("worldlogic-zone-nan", PackageErrorCode::WorldLogicRectInvalid, [nan](auto& l) {
        l.zones[1].bounds.max_x = nan;
    });
    logic_case("worldlogic-zone-inverted", PackageErrorCode::WorldLogicRectInvalid, [](auto& l) {
        l.zones[1].bounds = {256.0f, 0.0f, 128.0f, 256.0f};
    });
    logic_case("worldlogic-zone-out-of-world", PackageErrorCode::WorldLogicOutOfBounds, [](auto& l) {
        l.zones[1].bounds.max_x = 300.0f;
    });
    logic_case("worldlogic-zone-overlap", PackageErrorCode::WorldLogicZoneOverlap, [](auto& l) {
        l.zones[1].bounds.min_x = 100.0f;
    });
    logic_case("worldlogic-zone-gap", PackageErrorCode::WorldLogicCoverageGap, [](auto& l) {
        l.zones[1].bounds.min_x = 140.0f;
    });
    logic_case("worldlogic-spawn-unknown-zone", PackageErrorCode::WorldLogicReferenceInvalid, [](auto& l) {
        l.spawns[0].zone_id = 42;
    });
    logic_case("worldlogic-spawn-outside-zone", PackageErrorCode::WorldLogicSpawnOutsideZone, [](auto& l) {
        l.spawns[0].bounds = {120.0f, 60.0f, 140.0f, 70.0f}; // crosses into zone 2
    });
    logic_case("worldlogic-spawn-id-duplicate", PackageErrorCode::WorldLogicIdDuplicate, [](auto& l) {
        l.spawns.push_back(l.spawns[0]);
    });
    logic_case("worldlogic-warp-target-nan", PackageErrorCode::WorldLogicWarpTargetInvalid, [nan](auto& l) {
        l.warps[0].target_x = nan;
    });
    logic_case("worldlogic-warp-target-outside", PackageErrorCode::WorldLogicWarpTargetInvalid, [](auto& l) {
        l.warps[0].target_x = 256.0f; // half-open: extent itself is outside
    });
    logic_case("worldlogic-warp-target-blocked", PackageErrorCode::WorldLogicWarpTargetInvalid, [](auto& l) {
        l.warps[0].target_x = 45.0f; // cell 11,11 is blocked
        l.warps[0].target_y = 45.0f;
    });
    logic_case("worldlogic-warp-self-cycle", PackageErrorCode::WorldLogicWarpCycle, [](auto& l) {
        l.warps[0].target_x = 62.0f;
        l.warps[0].target_y = 62.0f;
    });
    logic_case("worldlogic-warp-two-cycle", PackageErrorCode::WorldLogicWarpCycle, [](auto& l) {
        l.warps = {{1, {60.0f, 60.0f, 64.0f, 64.0f}, 201.0f, 201.0f},
                   {2, {200.0f, 200.0f, 204.0f, 204.0f}, 61.0f, 61.0f}};
    });
    logic_case("worldlogic-warp-id-zero", PackageErrorCode::WorldLogicIdInvalid, [](auto& l) { l.warps[0].id = 0; });

    // ===== spawn table ==================================================================
    auto spawn_case = [&](const std::string& name, PackageErrorCode code, const std::string& text) {
        auto spec = BaseSpec();
        spec.mob_spawns = text;
        ExpectSpecRejected(c, name, spec, code);
    };
    spawn_case("spawns-no-equals", PackageErrorCode::SpawnsSyntax, "mob_type_id=1 x=5 y5 count=1 radius=1\n");
    spawn_case("spawns-unknown-key", PackageErrorCode::SpawnsSyntax, "mob_type_id=1 x=5 y=5 count=1 radius=1 z=3\n");
    spawn_case("spawns-missing-key", PackageErrorCode::SpawnsSyntax, "mob_type_id=1 x=5 y=5 count=1\n");
    spawn_case("spawns-repeated-key", PackageErrorCode::SpawnsSyntax, "mob_type_id=1 x=5 x=6 y=5 count=1 radius=1\n");
    spawn_case("spawns-nan", PackageErrorCode::SpawnsFieldInvalid, "mob_type_id=1 x=nan y=5 count=1 radius=1\n");
    spawn_case("spawns-count-zero", PackageErrorCode::SpawnsFieldInvalid, "mob_type_id=1 x=5 y=5 count=0 radius=1\n");
    spawn_case("spawns-negative-radius", PackageErrorCode::SpawnsFieldInvalid, "mob_type_id=1 x=5 y=5 count=1 radius=-1\n");
    spawn_case("spawns-quoted-value", PackageErrorCode::SpawnsFieldInvalid, "mob_type_id=\"1\" x=5 y=5 count=1 radius=1\n");
    spawn_case("spawns-type-zero", PackageErrorCode::SpawnsFieldInvalid, "mob_type_id=0 x=5 y=5 count=1 radius=1\n");
    spawn_case("spawns-out-of-bounds", PackageErrorCode::SpawnsOutOfBounds, "mob_type_id=1 x=256 y=5 count=1 radius=1\n");
    spawn_case("spawns-unknown-mob-type", PackageErrorCode::SpawnsMobTypeUnknown, "mob_type_id=77 x=5 y=5 count=1 radius=1\n");
    {
        const auto dir = c.Dir("mob_types_missing");
        Write(dir, BaseSpec());
        ExpectRejected(c, "mob-types-registry-missing", dir, PackageErrorCode::StartupDataInvalid,
                       ValidationDepth::Startup, (c.root / "no_such_mob_types.conf").string());
    }

    // ===== map-load baseline (checked-in test map, warm OS cache) =========================
    {
        const fs::path test_map = IXTREEME_TEST_MAP_ROOT;
        constexpr int kRuns = 20;
        double legacy_ms = 0.0;
        double startup_ms = 0.0;
        double full_ms = 0.0;
        std::size_t legacy_bytes = 0;
        std::uint64_t server_bytes = 0;
        for (int run = 0; run < kRuns; ++run) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto legacy = mx::map::LoadHeightField(
                [&](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
                    std::ifstream file(test_map / fs::path(path).relative_path(), std::ios::binary);
                    if (!file) {
                        return std::nullopt;
                    }
                    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
                                                     std::istreambuf_iterator<char>());
                },
                ".");
            const auto t1 = std::chrono::steady_clock::now();
            mx::map::PackageReport startup;
            const auto data = mx::map::LoadServerWorld(test_map, ValidationDepth::Startup, startup);
            const auto t2 = std::chrono::steady_clock::now();
            mx::map::PackageReport full;
            (void)mx::map::LoadServerWorld(test_map, ValidationDepth::Full, full);
            const auto t3 = std::chrono::steady_clock::now();
            legacy_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            startup_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
            full_ms += std::chrono::duration<double, std::milli>(t3 - t2).count();
            if (legacy) {
                legacy_bytes = legacy->heights_cm.size() * 2 + legacy->attributes.size() * 2 +
                               legacy->splat_a_rgba8.size() + legacy->splat_b_rgba8.size();
            }
            server_bytes = startup.resident_terrain_bytes;
            (void)data;
        }
        std::printf("WORLDPACKAGE map-load-baseline test_zone (500x500 cells, 16 chunks, 3.1 MB on disk, warm OS "
                    "cache, avg of %d): legacy_loader=%.2fms resident=%zuB | strict_startup=%.2fms "
                    "resident=%lluB | strict_full=%.2fms\n",
                    kRuns, legacy_ms / kRuns, legacy_bytes, startup_ms / kRuns,
                    static_cast<unsigned long long>(server_bytes), full_ms / kRuns);
    }

    // ===== fixtures for the gameserver startup acceptance ==================================
    if (!fixtures_out.empty()) {
        const fs::path out(fixtures_out);
        fs::create_directories(out, ec);
        auto emit = [&](const std::string& name, mx::map::PackageWriteSpec spec,
                        const std::function<void(const fs::path&)>& post = {}) {
            spec.overwrite = true;
            const auto dir = out / name;
            fs::remove_all(dir, ec);
            const bool ok = Write(dir, spec);
            if (ok && post) {
                post(dir);
            }
            std::printf("WORLDPACKAGE fixture %s -> %s\n", name.c_str(), ok ? dir.string().c_str() : "FAILED");
        };
        emit("valid_v3", BaseSpec(3, true));
        emit("server_only_v3", BaseSpec());
        auto bad_version = BaseSpec();
        bad_version.patch_manifest = [](mx::map::schema::MapManifest::Builder& m) { m.setFormatVersion(7); };
        emit("bad_version", bad_version);
        emit("corrupt_manifest", BaseSpec(), [](const fs::path& dir) {
            auto bytes = ReadAll(dir / "map.manifest");
            bytes.resize((bytes.size() / 2) & ~std::size_t{7});
            WriteAll(dir / "map.manifest", bytes);
        });
        auto bad_logic = BaseSpec();
        bad_logic.logic.zones[1].bounds.min_x = 100.0f; // overlapping bootstrap zones
        emit("invalid_worldlogic", bad_logic);
    }

    fs::remove_all(c.root, ec);
    std::printf("WORLDPACKAGE-DONE passes=%d failures=%d skipped=%d\n", c.passes, c.failures, c.skipped);
    return c.failures;
}

} // namespace gs::bench
