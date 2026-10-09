#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// Timed respawn lifecycle. Push() is called from zone ticks (combat deaths);
// TakeReady() is called from the supervisor when no zone tick is in progress.
// Owns only (spawn_point, countdown) pairs; entity creation stays in
// SpawnSystem so this class never touches flecs or zones.
namespace gs::game {

class RespawnSystem {
public:
    void Push(std::size_t spawn_point_index, float delay_sec);
    std::vector<std::size_t> TakeReady(float dt);
    std::size_t PendingCount() const;
    void Clear();

private:
    struct RespawnPending {
        std::size_t spawn_point_index = 0;
        float remaining_sec = 0.0f;
    };

    mutable std::mutex mutex_;
    std::vector<RespawnPending> pending_;
};

} // namespace gs::game
