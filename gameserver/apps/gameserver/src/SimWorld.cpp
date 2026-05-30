#include "SimWorld.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

#include <boost/asio/post.hpp>
#include <capnp/message.h>

#include "common/Logging.h"
#include "db/Types.h"
#include "protocol/Serialization.h"
#include "schema/packet.capnp.h"

namespace gs::game {
namespace {

constexpr float kDbUnitsPerMeter = 1000.0f;

float DbToMeters(std::int32_t value)
{
    return static_cast<float>(value) / kDbUnitsPerMeter;
}

std::uint16_t QuantizeHeading(float angle)
{
    constexpr float kTwoPi = 6.28318530717958647692f;
    while (angle < 0.0f) {
        angle += kTwoPi;
    }
    while (angle >= kTwoPi) {
        angle -= kTwoPi;
    }
    return static_cast<std::uint16_t>(std::lround((angle / kTwoPi) * 65535.0f));
}

void FillVec3(gs::protocol::Vec3::Builder out, const Position& pos)
{
    out.setX(pos.x);
    out.setY(pos.y);
    out.setZ(pos.z);
}

void WriteU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void WriteU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

void WriteF32(std::vector<std::uint8_t>& out, float value)
{
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(out, bits);
}

std::vector<std::uint8_t> MakeEnterWorldAccept(std::uint32_t net_id,
                                               const Position& pos,
                                               std::uint32_t world_tick)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto accept = packet.initEnterWorldAccept();
    accept.setYourNetId(net_id);
    FillVec3(accept.initSpawnPos(), pos);
    accept.setServerTick(world_tick);
    return gs::protocol::SerializeToBytes(msg);
}

std::vector<std::uint8_t> MakeSpawn(const SimPlayer& player);

std::vector<std::uint8_t> MakeDespawn(std::uint32_t net_id)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto despawn = packet.initEntityDespawn();
    despawn.setNetId(net_id);
    return gs::protocol::SerializeToBytes(msg);
}

void Send(boost::asio::io_context& io,
          const std::shared_ptr<gs::network::Session>& session,
          std::vector<std::uint8_t> payload)
{
    boost::asio::post(io, [session, payload = std::move(payload)]() mutable {
        session->SendPayload(std::move(payload));
    });
}

} // namespace

struct SimPlayer {
    flecs::entity entity;
    std::shared_ptr<gs::network::Session> session;
    gs::db::Character character;
    Position position;
    Heading heading;
    Velocity velocity;
    MoveIntent move_intent;
    MoveSpeed move_speed;
    std::uint32_t net_id = 0;
};

namespace {

std::vector<std::uint8_t> MakeSpawn(const SimPlayer& player)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto spawn = packet.initEntitySpawn();
    spawn.setNetId(player.net_id);
    spawn.setName(player.character.name);
    spawn.setClassId(player.character.class_id);
    FillVec3(spawn.initSpawnPos(), player.position);
    spawn.setHeading(QuantizeHeading(player.heading.angle));
    return gs::protocol::SerializeToBytes(msg);
}

} // namespace

SimWorld::SimWorld(boost::asio::io_context& io)
    : io_(io)
{
}

SimWorld::~SimWorld()
{
    Stop();
}

void SimWorld::Start()
{
    if (thread_.joinable()) {
        return;
    }

    stopping_ = false;
    thread_ = std::thread([this] {
        Run();
    });
}

void SimWorld::Stop()
{
    stopping_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SimWorld::PostSpawn(std::shared_ptr<gs::network::Session> session, gs::db::Character character)
{
    Enqueue([this, session = std::move(session), character = std::move(character)]() mutable {
        Spawn(std::move(session), std::move(character));
    });
}

void SimWorld::PostDespawn(gs::common::SessionId session_id)
{
    Enqueue([this, session_id] {
        Despawn(session_id);
    });
}

void SimWorld::PostMoveInput(gs::common::SessionId session_id,
                             std::uint32_t sequence,
                             float dir_angle,
                             MoveState state)
{
    {
        std::lock_guard lock(input_mutex_);
        pending_inputs_.push_back(MoveInput{session_id, sequence, dir_angle, state});
    }
    cv_.notify_one();
}

void SimWorld::Enqueue(std::function<void()> command)
{
    {
        std::lock_guard lock(mutex_);
        commands_.push(std::move(command));
    }
    cv_.notify_one();
}

void SimWorld::Run()
{
    sim_thread_id_ = std::this_thread::get_id();

    world_ = std::make_unique<flecs::world>();
    world_->component<Position>();
    world_->component<Heading>();
    world_->component<NetId>();
    world_->component<SessionRef>();
    world_->component<PlayerTag>();

    LOG_INFO("Game sim thread started");
    const auto tick_dt = std::chrono::milliseconds(50);
    auto next_tick = std::chrono::steady_clock::now();

    while (!stopping_) {
        next_tick += tick_dt;
        DrainCommands();
        DrainMoveInputs();
        StepMovement(0.05f);
        world_->progress(0.05f);
        BroadcastTransforms();
        ++world_tick_;

        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, next_tick, [this] {
            return stopping_.load() || !commands_.empty();
        });
    }

    DrainCommands();
    players_.clear();
    world_.reset();
    LOG_INFO("Game sim thread stopped");
}

void SimWorld::DrainMoveInputs()
{
    AssertSimThread();
    std::vector<MoveInput> inputs;
    {
        std::lock_guard lock(input_mutex_);
        inputs.swap(pending_inputs_);
    }

    for (const auto& input : inputs) {
        auto it = std::find_if(players_.begin(), players_.end(), [&input](const SimPlayer& player) {
            return player.session && player.session->Id() == input.session_id;
        });
        if (it == players_.end()) {
            continue;
        }
        if (input.sequence < it->move_intent.last_input_seq) {
            continue;
        }
        it->move_intent.dir_angle = input.dir_angle;
        it->move_intent.state = input.state;
        it->move_intent.last_input_seq = input.sequence;
    }
}

void SimWorld::StepMovement(float dt)
{
    AssertSimThread();
    for (auto& player : players_) {
        const auto state = player.move_intent.state;
        const float speed = state == MoveState::Running
                                ? player.move_speed.run
                                : (state == MoveState::Walking ? player.move_speed.walk : 0.0f);
        player.heading.angle = player.move_intent.dir_angle;
        player.velocity.x = std::sin(player.move_intent.dir_angle) * speed;
        player.velocity.y = std::cos(player.move_intent.dir_angle) * speed;
        player.velocity.z = 0.0f;

        player.position.x = std::clamp(player.position.x + player.velocity.x * dt, 0.0f, 1000.0f);
        player.position.y = std::clamp(player.position.y + player.velocity.y * dt, 0.0f, 1000.0f);
        player.position.z = 0.0f;

        player.entity.set<Position>(player.position)
            .set<Heading>(player.heading)
            .set<Velocity>(player.velocity)
            .set<MoveIntent>(player.move_intent);
    }
}

void SimWorld::BroadcastTransforms()
{
    AssertSimThread();
    if (players_.empty()) {
        return;
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(1 + 1 + 4 + 2 + players_.size() * 19);
    payload.push_back(gs::protocol::kCodecBinary);
    payload.push_back(0x10);
    WriteU32(payload, world_tick_);
    WriteU16(payload, static_cast<std::uint16_t>(players_.size()));

    for (const auto& player : players_) {
        WriteU32(payload, player.net_id);
        WriteF32(payload, player.position.x);
        WriteF32(payload, player.position.y);
        WriteF32(payload, player.position.z);
        WriteU16(payload, QuantizeHeading(player.heading.angle));
        payload.push_back(static_cast<std::uint8_t>(player.move_intent.state));
    }

    for (const auto& player : players_) {
        Send(io_, player.session, payload);
    }
}

void SimWorld::DrainCommands()
{
    for (;;) {
        std::function<void()> command;
        {
            std::lock_guard lock(mutex_);
            if (commands_.empty()) {
                return;
            }
            command = std::move(commands_.front());
            commands_.pop();
        }
        command();
    }
}

void SimWorld::Spawn(std::shared_ptr<gs::network::Session> session, gs::db::Character character)
{
    AssertSimThread();
    Despawn(session->Id());

    SimPlayer player;
    player.session = std::move(session);
    player.character = std::move(character);
    player.position = Position{DbToMeters(player.character.pos_x), DbToMeters(player.character.pos_y), 0.0f};
    player.heading = Heading{0.0f};
    player.move_speed = MoveSpeed{3.0f, 6.0f};
    player.net_id = next_net_id_++;
    player.entity = world_->entity()
                        .set<Position>(player.position)
                        .set<Heading>(player.heading)
                        .set<Velocity>(player.velocity)
                        .set<MoveIntent>(player.move_intent)
                        .set<MoveSpeed>(player.move_speed)
                        .set<NetId>({player.net_id})
                        .set<SessionRef>({player.session->Id()})
                        .add<PlayerTag>();

    const auto incoming_session = player.session;
    const auto incoming_spawn = MakeSpawn(player);

    Send(io_, incoming_session, MakeEnterWorldAccept(player.net_id, player.position, world_tick_));
    for (const auto& existing : players_) {
        Send(io_, incoming_session, MakeSpawn(existing));
        Send(io_, existing.session, incoming_spawn);
    }

    LOG_INFO("Session {} spawned character '{}' as net_id {} at {}, {}",
             incoming_session->Id(),
             player.character.name,
             player.net_id,
             player.position.x,
             player.position.y);
    players_.push_back(std::move(player));
}

void SimWorld::Despawn(gs::common::SessionId session_id)
{
    AssertSimThread();
    const auto it = std::find_if(players_.begin(), players_.end(), [session_id](const SimPlayer& player) {
        return player.session && player.session->Id() == session_id;
    });
    if (it == players_.end()) {
        return;
    }

    const auto net_id = it->net_id;
    if (it->entity.is_valid()) {
        it->entity.destruct();
    }
    LOG_INFO("Session {} despawned net_id {}", session_id, net_id);
    players_.erase(it);

    auto payload = MakeDespawn(net_id);
    for (const auto& player : players_) {
        Send(io_, player.session, payload);
    }
}

void SimWorld::AssertSimThread() const
{
    assert(std::this_thread::get_id() == sim_thread_id_);
}

} // namespace gs::game
