#include "map/ServerTerrain.h"
#include "map/WorldPackage.h"
#include "map/WorldPackageWriter.h"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace mx::map;
namespace fs = std::filesystem;
int checks = 0;
int failures = 0;

void Check(const char* name, bool pass)
{
    ++checks;
    if (!pass) ++failures;
    std::cout << "TERRAIN SURFACE " << name << ": " << (pass ? "PASS" : "FAIL") << '\n';
}

bool Near(double actual, double expected)
{
    return std::isfinite(actual) && std::abs(actual - expected) < 0.000001;
}

struct Scratch {
    fs::path parent = fs::weakly_canonical(fs::temp_directory_path());
    fs::path root = parent / ("ixw_terrain_surface_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    Scratch()
    {
        if (!fs::create_directory(root)) throw std::runtime_error("cannot create unique terrain surface scratch directory");
    }
    ~Scratch()
    {
        if (root.parent_path() == parent && root.filename().string().starts_with("ixw_terrain_surface_")) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }
};

// Independent barycentric oracle on actual triangles. It does not call the
// production interpolation helper or rearrange its piecewise expression.
double TriangleOracle(const std::array<double, 4>& heights, double x, double y)
{
    const std::array<std::array<double, 2>, 4> corners{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
    constexpr std::array<std::array<std::size_t, 3>, 2> triangles{{{0, 1, 3}, {0, 3, 2}}};
    for (const auto& triangle : triangles) {
        const auto& a = corners[triangle[0]];
        const auto& b = corners[triangle[1]];
        const auto& c = corners[triangle[2]];
        const double denominator = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        const double wa = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / denominator;
        const double wb = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / denominator;
        const double wc = 1.0 - wa - wb;
        if (wa >= -1e-12 && wb >= -1e-12 && wc >= -1e-12) {
            return wa * heights[triangle[0]] + wb * heights[triangle[1]] + wc * heights[triangle[2]];
        }
    }
    throw std::runtime_error("oracle point outside cell");
}

PackageWriteSpec Spec()
{
    PackageWriteSpec spec;
    spec.world_id = "terrain_surface_fixture";
    spec.world_name = "Declared terrain surface fixture";
    spec.size_cells_x = 2;
    spec.size_cells_y = 2;
    spec.chunk_size_cells = 1;
    spec.cell_size_m = 1;
    spec.height_raw = [](std::uint32_t x, std::uint32_t y) { return x == 1 && y == 0 ? 400 : 0; };
    spec.attributes = [](std::uint32_t, std::uint32_t) { return std::uint16_t{0}; };
    spec.logic.spawns.push_back(SpawnRegion{1, 0, Rect{0.1f, 0.1f, 0.2f, 0.2f}});
    return spec;
}

HeightEncoding Physical(bool int32 = false)
{
    return HeightEncoding{3, int32, 0.01, 0.0, HeightInterpolation::TriangleMainDiagonal};
}

void SetHeightVersion(package_schema::MapManifest::Builder root, std::uint32_t version)
{
    for (auto layer : root.getLayers()) {
        if (layer.getKind() == package_schema::LayerKind::HEIGHT) layer.setVersion(version);
    }
}

bool HasError(const PackageReport& report, PackageErrorCode expected)
{
    for (const auto& issue : report.issues) {
        if (issue.severity == IssueSeverity::Error && issue.code == expected) return true;
    }
    return false;
}

void RejectFixture(const fs::path& root, const char* name, PackageWriteSpec spec, PackageErrorCode error)
{
    const auto written = WritePackage(root / name, spec);
    PackageReport report;
    const auto loaded = written.ok ? LoadServerWorld(root / name, ValidationDepth::Full, report) : std::nullopt;
    Check(name, written.ok && !loaded && !report.Ok() && HasError(report, error));
}

void PureSurfaceTests()
{
    const std::array<double, 4> saddle{0, 4, 0, 0};
    Check("saddle-physical-diagonal-zero", Near(InterpolateTerrainHeight(0, 4, 0, 0, 0.5, 0.5,
        HeightInterpolation::TriangleMainDiagonal), 0));
    Check("legacy-bilinear-saddle-one", Near(InterpolateTerrainHeight(0, 4, 0, 0, 0.5, 0.5,
        HeightInterpolation::Bilinear), 1));
    bool oracle = true;
    for (const auto heights : {saddle, std::array<double, 4>{-5, -2, 7, 3}}) {
        for (int y = 0; y <= 16; ++y) {
            for (int x = 0; x <= 16; ++x) {
                const double fx = static_cast<double>(x) / 16;
                const double fy = static_cast<double>(y) / 16;
                oracle = oracle && Near(InterpolateTerrainHeight(heights[0], heights[1], heights[2], heights[3],
                    fx, fy, HeightInterpolation::TriangleMainDiagonal), TriangleOracle(heights, fx, fy));
            }
        }
    }
    Check("both-triangles-diagonal-edges-barycentric-oracle", oracle);
    Check("unknown-interpolation-not-guessed", std::isnan(InterpolateTerrainHeight(0, 4, 0, 0, 0.5, 0.5,
        static_cast<HeightInterpolation>(255))));
}

void PackageTests(const fs::path& root)
{
    auto physical = Spec();
    physical.height_encoding = Physical();
    const auto written = WritePackage(root / "physical", physical);
    Check("write-opt-in-height-layer-v3", written.ok);
    PackageReport report;
    auto loaded = written.ok ? LoadServerWorld(root / "physical", ValidationDepth::Full, report) : std::nullopt;
    Check("strict-v3-physical-package-load", loaded.has_value() && report.Ok());

    // Native filesystem paths are independent of ASCII wire references/world
    // identities. Even diagnostic paths must survive the Windows code page.
    const auto unicode_path = root / fs::path(u8"világ_árvíztűrő_世界");
    const auto unicode_written = WritePackage(unicode_path, physical);
    PackageReport unicode_report;
    const auto unicode_loaded = unicode_written.ok
        ? LoadServerWorld(unicode_path, ValidationDepth::Full, unicode_report) : std::nullopt;
    Check("unicode-native-package-root-load", unicode_loaded && unicode_report.Ok());
    const auto overwrite = WritePackage(unicode_path, physical);
    Check("unicode-writer-overwrite-diagnostic", !overwrite.ok &&
        overwrite.error.find("refusing to overwrite") != std::string::npos);
    auto unicode_missing = unicode_path;
    unicode_missing += ".missing";
    PackageReport missing_report;
    const auto missing = LoadServerWorld(unicode_missing, ValidationDepth::Full, missing_report);
    Check("unicode-missing-root-diagnostic-preserves-error-class", !missing && missing_report.FirstError() &&
        missing_report.FirstError()->code == PackageErrorCode::PackageRootMissing);
    if (loaded) {
        const auto* height = report.manifest.FindLayer(LayerKind::Height);
        Check("required-version-discriminator-for-old-readers", height && height->required && height->version == 3 &&
            height->version > 2);
        Check("decoded-encoding-and-chunk-mode", loaded->terrain.Encoding().layer_version == 3 &&
            loaded->terrain.Encoding().interpolation == HeightInterpolation::TriangleMainDiagonal &&
            !loaded->terrain.Encoding().int32_samples && report.chunks_decoded == 4);
        bool oracle = true;
        for (int y = 1; y < 16; ++y) {
            for (int x = 1; x < 16; ++x) {
                const double fx = static_cast<double>(x) / 16;
                const double fy = static_cast<double>(y) / 16;
                const auto sample = loaded->terrain.Height(fx, fy);
                oracle = oracle && sample.Ok() && Near(sample.meters, TriangleOracle({0, 4, 0, 0}, fx, fy));
            }
        }
        Check("decoded-physical-height-independent-oracle", oracle);
        auto clone = loaded->terrain.Clone();
        Check("clone-keeps-physical-mode", clone.Encoding().interpolation == HeightInterpolation::TriangleMainDiagonal &&
            Near(clone.Height(0.5, 0.5).meters, 0));
        Check("outer-edge-and-nonfinite-unchanged", loaded->terrain.Height(2, 1).status == TerrainStatus::OutsideWorld &&
            loaded->terrain.Height(std::numeric_limits<double>::quiet_NaN(), 1).status == TerrainStatus::OutsideWorld);
    }

    for (const bool explicit_encoding : {false, true}) {
        auto legacy = Spec();
        if (explicit_encoding) legacy.height_encoding = HeightEncoding{2, false, 0.01, 0.0};
        const auto name = explicit_encoding ? "height_v2" : "height_v1";
        const auto legacy_written = WritePackage(root / name, legacy);
        PackageReport legacy_report;
        auto old = legacy_written.ok ? LoadServerWorld(root / name, ValidationDepth::Full, legacy_report) : std::nullopt;
        Check(explicit_encoding ? "height-v2-remains-bilinear" : "height-v1-remains-bilinear", old && legacy_report.Ok() &&
            old->terrain.Encoding().layer_version == (explicit_encoding ? 2u : 1u) &&
            old->terrain.Encoding().interpolation == HeightInterpolation::Bilinear &&
            Near(old->terrain.Height(0.5, 0.5).meters, 1));
    }
    auto legacy_manifest = Spec();
    legacy_manifest.format_version = 2;
    legacy_manifest.splat_size = 1;
    const auto legacy_written = WritePackage(root / "manifest_v2", legacy_manifest);
    PackageReport legacy_report;
    const auto old = legacy_written.ok ? LoadServerWorld(root / "manifest_v2", ValidationDepth::Full, legacy_report) : std::nullopt;
    Check("historic-manifest-v2-content-unchanged", old && legacy_report.Ok() &&
        old->terrain.Encoding().interpolation == HeightInterpolation::Bilinear && Near(old->terrain.Height(0.5, 0.5).meters, 1));

    auto int32 = physical;
    int32.height_encoding = Physical(true);
    int32.height_raw = [](std::uint32_t x, std::uint32_t y) { return static_cast<std::int32_t>(100000 + x * 100 + y * 200); };
    const auto int32_written = WritePackage(root / "physical_int32", int32);
    PackageReport int32_report;
    const auto range = int32_written.ok ? LoadServerWorld(root / "physical_int32", ValidationDepth::Full, int32_report) : std::nullopt;
    Check("physical-int32-height-storage", range && int32_report.Ok() && range->terrain.Encoding().int32_samples &&
        range->terrain.Encoding().interpolation == HeightInterpolation::TriangleMainDiagonal &&
        Near(range->terrain.Height(0.25, 0.75).meters, 1001.75));

    // Non-square negative-origin world with partial edge chunks exercises
    // the same mode through duplicated seam samples and chunk-local reads.
    auto partial = physical;
    partial.size_cells_x = 6;
    partial.size_cells_y = 4;
    partial.cell_size_m = 2;
    partial.origin_x = -6;
    partial.origin_y = -4;
    partial.chunk_size_cells = 3;
    partial.height_raw = [](std::uint32_t x, std::uint32_t y) { return -700 + static_cast<std::int32_t>(23 * x + 71 * y); };
    partial.logic.spawns[0].bounds = Rect{-5.8f, -3.8f, -5.6f, -3.6f};
    const auto partial_written = WritePackage(root / "partial", partial);
    PackageReport partial_report;
    const auto offset = partial_written.ok ? LoadServerWorld(root / "partial", ValidationDepth::Full, partial_report) : std::nullopt;
    Check("negative-origin-partial-chunks-physical-mode", offset && partial_report.Ok() &&
        offset->terrain.Encoding().interpolation == HeightInterpolation::TriangleMainDiagonal &&
        offset->terrain.Geometry().ChunkCellsY(1) == 1 && Near(offset->terrain.Height(-5, -3).meters, -6.53));
    Check("partial-chunk-seam-continuity", offset &&
        std::abs(offset->terrain.Height(-1e-6, 1).meters - offset->terrain.Height(0, 1).meters) < 0.00001);

    LoadOptions streaming_options;
    streaming_options.residency = ResidencyMode::Streaming;
    streaming_options.depth = ValidationDepth::Startup;
    PackageReport streaming_report;
    auto streaming = LoadServerWorld(root / "physical", streaming_options, streaming_report);
    Check("streaming-retains-height-mode", streaming && streaming_report.Ok() &&
        streaming->terrain.Encoding().interpolation == HeightInterpolation::TriangleMainDiagonal && streaming->chunk_source);
    if (streaming && streaming->chunk_source) {
        const auto index = streaming->terrain.ChunkIndexOf(1.25, 1.25);
        const auto before = streaming->terrain.Height(1.25, 1.25);
        const auto chunk = streaming->chunk_source->Load(index);
        if (chunk.ok) (void)streaming->terrain.Publish(index, chunk.chunk);
        const auto after = streaming->terrain.Height(1.25, 1.25);
        Check("later-decoded-stream-chunk-uses-declared-surface", before.status == TerrainStatus::NotResident &&
            chunk.ok && after.Ok() && Near(after.meters, 0));
    }

    auto bad = physical;
    bad.patch_manifest = [](auto root) { root.getHeightEncoding().setInterpolation(static_cast<package_schema::HeightInterpolation>(99)); };
    RejectFixture(root, "unknown-mode-refused", bad, PackageErrorCode::ManifestFieldInvalid);
    bad = physical;
    bad.patch_manifest = [](auto root) { root.getHeightEncoding().setInterpolation(package_schema::HeightInterpolation::BILINEAR); };
    RejectFixture(root, "v3-missing-explicit-mode-refused", bad, PackageErrorCode::ManifestFieldInvalid);
    bad = physical;
    bad.patch_manifest = [](auto root) { (void)root.disownHeightEncoding(); };
    RejectFixture(root, "v3-missing-encoding-refused", bad, PackageErrorCode::ManifestFieldInvalid);
    bad = physical;
    bad.patch_manifest = [](auto root) { SetHeightVersion(root, 2); };
    RejectFixture(root, "v2-nonbilinear-forbidden", bad, PackageErrorCode::ManifestFieldForbidden);
    bad = physical;
    bad.patch_manifest = [](auto root) { SetHeightVersion(root, 1); };
    RejectFixture(root, "v1-nonbilinear-encoding-forbidden", bad, PackageErrorCode::ManifestFieldForbidden);
    bad = physical;
    bad.patch_manifest = [](auto root) {
        for (auto layer : root.getLayers()) {
            if (layer.getKind() == package_schema::LayerKind::HEIGHT) layer.setRequired(false);
        }
    };
    RejectFixture(root, "v3-optional-contract-forbidden", bad, PackageErrorCode::LayerDeclInvalid);
    bad = physical;
    bad.patch_manifest = [](auto root) { SetHeightVersion(root, 4); };
    RejectFixture(root, "unsupported-required-height-version-refused", bad, PackageErrorCode::LayerUnsupported);
    bad = physical;
    bad.patch_manifest = [](auto root) { root.getHeightEncoding().setSampleType(package_schema::HeightSampleType::INT32); };
    RejectFixture(root, "physical-chunk-element-type-mismatch-refused", bad, PackageErrorCode::ChunkTocInvalid);
    bad = physical;
    bad.height_encoding->interpolation = static_cast<HeightInterpolation>(99);
    const auto invalid_write = WritePackage(root / "invalid_writer_mode", bad);
    Check("writer-refuses-unknown-interpolation", !invalid_write.ok && !fs::exists(root / "invalid_writer_mode"));
}

} // namespace

int main()
{
    try {
        PureSurfaceTests();
        Scratch scratch;
        PackageTests(scratch.root);
    } catch (const std::exception& exception) {
        Check("unexpected-test-exception", false);
        std::cout << "test exception: " << exception.what() << '\n';
    }
    std::cout << "TERRAIN SURFACE summary: checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
