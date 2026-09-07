#include "WorldDirectory.h"

#include "../zone/ZoneManager.h"

namespace gs::game {

WorldDirectory::WorldDirectory(RuntimeIdentity identity)
    : identity_(identity)
{
}

void WorldDirectory::RebuildFromManager(const ZoneManager& zones)
{
    std::lock_guard lock(mutex_);
    std::unordered_map<ZoneId, ZoneLocation> rebuilt;
    // Only simulating active leaves receive fresh local assignments.
    // Retired entries survive for in-flight migration completion.
    for (ZonePartition* leaf : zones.GetActiveLeaves()) {
        const ZoneId id = leaf->zone_id;
        const auto override_it = assignments_.find(id);
        if (override_it != assignments_.end() && !IsLocal(override_it->second)) {
            // Keep an explicit (emulated/balancer) remote assignment.
            rebuilt.emplace(id, override_it->second);
        } else {
            rebuilt.emplace(id, LocalZoneLocation(identity_, id));
        }
    }
    for (const auto& [id, loc] : assignments_) {
        if (retired_zones_.find(id) != retired_zones_.end()) {
            rebuilt.emplace(id, loc);
        }
    }
    assignments_.swap(rebuilt);
}

std::optional<ZoneLocation> WorldDirectory::ResolveZone(ZoneId zone) const
{
    std::lock_guard lock(mutex_);
    const auto it = assignments_.find(zone);
    if (it == assignments_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<ZoneLocation> WorldDirectory::ResolveZoneForPosition(
    float world_x,
    float world_y,
    const std::vector<std::unique_ptr<ZonePartition>>& partition_roots) const
{
    ZoneId leaf_id = 0;
    for (const auto& root : partition_roots) {
        if (auto* leaf = FindLeaf(root.get(), world_x, world_y)) {
            leaf_id = leaf->zone_id;
            break;
        }
    }
    if (leaf_id == 0) {
        return std::nullopt;
    }
    return ResolveZone(leaf_id);
}

bool WorldDirectory::IsLocal(ZoneLocation location) const
{
    return gs::game::IsLocal(location, identity_);
}

bool WorldDirectory::IsDraining(ZoneLocation location) const
{
    std::lock_guard lock(mutex_);
    // An explicit per-zone drain flag is authoritative at any locality: it is
    // exactly how a future directory sync would publish remote drain state,
    // and what the benchmark emulation uses to model it.
    if (drained_zones_.find(location.zone) != drained_zones_.end()) {
        return true;
    }
    if (IsLocal(location)) {
        return local_status_ == ProcessStatus::Draining;
    }
    // Unknown remotes default to draining (safe: no new work is sent where
    // health is unknown); noted remotes accept work.
    return alive_remotes_.find({location.node.value, location.process.value}) == alive_remotes_.end();
}

ProcessStatus WorldDirectory::LocalStatus() const
{
    std::lock_guard lock(mutex_);
    return local_status_;
}

void WorldDirectory::SetLocalStatus(ProcessStatus status)
{
    std::lock_guard lock(mutex_);
    local_status_ = status;
}

void WorldDirectory::SetAssignment(ZoneId zone, ZoneLocation location)
{
    std::lock_guard lock(mutex_);
    assignments_[zone] = location;
    retired_zones_.erase(zone);
}

void WorldDirectory::ClearAssignment(ZoneId zone)
{
    std::lock_guard lock(mutex_);
    assignments_[zone] = LocalZoneLocation(identity_, zone);
    retired_zones_.erase(zone);
}

void WorldDirectory::RetireZones(const std::vector<ZoneId>& zone_ids)
{
    std::lock_guard lock(mutex_);
    for (const ZoneId id : zone_ids) {
        retired_zones_.insert(id);
    }
}

void WorldDirectory::SetZoneDrained(ZoneId zone, bool drained)
{
    std::lock_guard lock(mutex_);
    if (drained) {
        drained_zones_.insert(zone);
    } else {
        drained_zones_.erase(zone);
    }
}

void WorldDirectory::NoteRemoteAlive(NodeId node, ProcessId process)
{
    std::lock_guard lock(mutex_);
    alive_remotes_.emplace(node.value, process.value);
}

void WorldDirectory::ForgetRemote(NodeId node, ProcessId process)
{
    std::lock_guard lock(mutex_);
    alive_remotes_.erase({node.value, process.value});
}

std::size_t WorldDirectory::KnownZoneCount() const
{
    std::lock_guard lock(mutex_);
    return assignments_.size();
}

} // namespace gs::game
