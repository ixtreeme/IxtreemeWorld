#include "WorldPackageLoader.h"

#include <system_error>

#include "common/Logging.h"

#include "../spawn/MobPrototypeRegistry.h"

namespace gs::game {

namespace {

void AddIssue(mx::map::PackageReport& report,
              mx::map::PackageErrorCode code,
              std::string layer,
              std::string file,
              std::int64_t offset,
              std::string reason,
              std::string expected)
{
    mx::map::PackageIssue issue;
    issue.severity = mx::map::IssueSeverity::Error;
    issue.code = code;
    issue.package = report.manifest.world_id;
    issue.layer = std::move(layer);
    issue.file = std::move(file);
    issue.offset = offset;
    issue.reason = std::move(reason);
    issue.expected = std::move(expected);
    report.issues.push_back(std::move(issue));
}

} // namespace

std::optional<LoadedWorld> LoadWorldPackage(const WorldLoadRequest& request, mx::map::PackageReport& report)
{
    auto data = mx::map::LoadServerWorld(request.package_root, request.depth, report);

    // Server startup data: the mob type registry the spawn table refers to.
    std::error_code ec;
    const auto types_path = std::filesystem::absolute(request.mob_types_config, ec);
    MobPrototypeRegistry registry;
    if (request.mob_types_config.empty()) {
        AddIssue(report, mx::map::PackageErrorCode::StartupDataInvalid, "mobTypes", {}, -1,
                 "no mob type registry configured", "mob_types_config = <path>");
    } else if (ec || !std::filesystem::is_regular_file(types_path, ec) ||
               !registry.LoadFromFile(types_path.string()) || registry.Empty()) {
        AddIssue(report, mx::map::PackageErrorCode::StartupDataInvalid, "mobTypes", types_path.string(), -1,
                 "mob type registry missing, unreadable or without a valid type",
                 "a readable mob_types.conf with at least one valid [mob]");
    }
    if (!data || !report.Ok()) {
        return std::nullopt;
    }

    LoadedWorld world;
    const auto* spawn_layer = report.manifest.FindLayer(mx::map::LayerKind::MobSpawns);
    const std::string spawn_file = spawn_layer != nullptr ? spawn_layer->file : std::string{};
    world.spawn_points.reserve(data->spawns.size());
    for (const auto& record : data->spawns) {
        if (registry.Find(record.mob_type_id) == nullptr) {
            AddIssue(report, mx::map::PackageErrorCode::SpawnsMobTypeUnknown, "mobSpawns", spawn_file,
                     static_cast<std::int64_t>(record.line),
                     "mob_type_id=" + std::to_string(record.mob_type_id) + " is not in the mob type registry",
                     "an id defined in " + types_path.string());
            continue;
        }
        world.spawn_points.push_back(
            MobSpawnPoint{record.mob_type_id, record.x, record.y, record.count, record.radius});
    }
    if (!report.Ok()) {
        return std::nullopt;
    }
    world.terrain = std::move(data->terrain);
    world.logic = std::move(data->logic);
    world.mob_types_config = types_path.string();
    world.mob_type_count = registry.Size();
    world.report = report;
    return world;
}

void LogPackageReport(const mx::map::PackageReport& report)
{
    for (const auto& issue : report.issues) {
        const std::string line = issue.Format();
        switch (issue.severity) {
        case mx::map::IssueSeverity::Error:
            LOG_ERROR("world package: {}", line);
            break;
        case mx::map::IssueSeverity::Warning:
            LOG_WARN("world package: {}", line);
            break;
        case mx::map::IssueSeverity::Info:
            LOG_INFO("world package: {}", line);
            break;
        }
    }
}

} // namespace gs::game
