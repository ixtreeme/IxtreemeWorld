#pragma once

#include <cstddef>
#include <memory>
#include <vector>

// Zone adjacency graph. Owns ONLY the neighbor topology (index-based), built
// from zone bounds touch/overlap. Kept separate from ZoneManager so future
// link types (portals, sea routes, dynamic splits) can extend the graph
// without touching zone lifetime management.
namespace gs::game {

class Zone;

class ZoneGraph {
public:
    void Rebuild(const std::vector<std::unique_ptr<Zone>>& zones);

    const std::vector<std::size_t>& Neighbors(std::size_t zone_index) const;

private:
    std::vector<std::vector<std::size_t>> adjacency_;
};

} // namespace gs::game
