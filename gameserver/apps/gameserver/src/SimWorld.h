#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <flecs.h>

#include "db/CharacterRepository.h"
#include "network/Session.h"

namespace gs::game {

struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Heading {
    float angle = 0.0f;
};

struct Velocity {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class MoveState : std::uint8_t {
    Idle = 0,
    Walking = 1,
    Running = 2,
};

struct MoveIntent {
    float dir_angle = 0.0f;
    MoveState state = MoveState::Idle;
    std::uint32_t last_input_seq = 0;
};

struct MoveSpeed {
    float walk = 3.0f;
    float run = 6.0f;
};

struct NetId {
    std::uint32_t value = 0;
};

struct SessionRef {
    gs::common::SessionId session = 0;
};

struct PlayerTag {
};

struct SimPlayer;

class SimWorld {
public:
    explicit SimWorld(boost::asio::io_context& io);
    ~SimWorld();

    SimWorld(const SimWorld&) = delete;
    SimWorld& operator=(const SimWorld&) = delete;

    void Start();
    void Stop();

    void PostSpawn(std::shared_ptr<gs::network::Session> session, gs::db::Character character);
    void PostDespawn(gs::common::SessionId session_id);
    void PostMoveInput(gs::common::SessionId session_id,
                       std::uint32_t sequence,
                       float dir_angle,
                       MoveState state);

private:
    struct MoveInput {
        gs::common::SessionId session_id = 0;
        std::uint32_t sequence = 0;
        float dir_angle = 0.0f;
        MoveState state = MoveState::Idle;
    };

    void Enqueue(std::function<void()> command);
    void Run();
    void DrainCommands();
    void DrainMoveInputs();
    void StepMovement(float dt);
    void BroadcastTransforms();
    void Spawn(std::shared_ptr<gs::network::Session> session, gs::db::Character character);
    void Despawn(gs::common::SessionId session_id);
    void AssertSimThread() const;

    boost::asio::io_context& io_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::thread::id sim_thread_id_{};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> commands_;

    std::mutex input_mutex_;
    std::vector<MoveInput> pending_inputs_;

    std::vector<SimPlayer> players_;
    std::unique_ptr<flecs::world> world_;
    std::uint32_t next_net_id_ = 1;
    std::uint32_t world_tick_ = 0;
};

} // namespace gs::game
