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
    rebuilt.reserve(zones.ZoneCount());
    for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
        const ZoneId id = zones.GetZone(i).Id();
        const auto override_it = assignments_.find(id);
        if (override_it != assignments_.end() && !IsLocal(override_it->second)) {
            // Keep an explicit (emulated/balancer) remote assignment.
            rebuilt.emplace(id, override_it->second);
        } else {
            rebuilt.emplace(id, LocalZoneLocation(identity_, id));
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
}

void WorldDirectory::ClearAssignment(ZoneId zone)
{
    std::lock_guard lock(mutex_);
    assignments_[zone] = LocalZoneLocation(identity_, zone);
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
