#include "RespawnSystem.h"

namespace gs::game {

void RespawnSystem::Push(std::size_t spawn_point_index, float delay_sec)
{
    std::lock_guard lock(mutex_);
    pending_.push_back(RespawnPending{spawn_point_index, delay_sec});
}

std::vector<std::size_t> RespawnSystem::TakeReady(float dt)
{
    std::vector<std::size_t> ready;
    {
        std::lock_guard lock(mutex_);
        for (auto& pending : pending_) {
            pending.remaining_sec -= dt;
        }

        auto it = pending_.begin();
        while (it != pending_.end()) {
            if (it->remaining_sec <= 0.0f) {
                ready.push_back(it->spawn_point_index);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    return ready;
}

std::size_t RespawnSystem::PendingCount() const
{
    std::lock_guard lock(mutex_);
    return pending_.size();
}

void RespawnSystem::Clear()
{
    std::lock_guard lock(mutex_);
    pending_.clear();
}

} // namespace gs::game
