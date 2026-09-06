#pragma once

#include <functional>
#include <mutex>
#include <queue>

// Thread-safe inbound command queue for a single zone. Producers are the
// supervisor thread (spawn/despawn/input routing) and any input path;
// the single consumer is the worker currently owning the zone, draining
// inside Zone::Tick. Commands execute under zone ownership.
namespace gs::game {

class Zone;

class ZoneCommandQueue {
public:
    using Command = std::function<void(Zone&)>;

    void Push(Command command)
    {
        std::lock_guard lock(mutex_);
        commands_.push(std::move(command));
    }

    bool Empty() const
    {
        std::lock_guard lock(mutex_);
        return commands_.empty();
    }

    std::queue<Command> TakeAll()
    {
        std::lock_guard lock(mutex_);
        std::queue<Command> taken;
        taken.swap(commands_);
        return taken;
    }

private:
    mutable std::mutex mutex_;
    std::queue<Command> commands_;
};

} // namespace gs::game
