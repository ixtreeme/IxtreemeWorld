#include "ProtocolEncoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <capnp/message.h>

#include "../WorldConstants.h"
#include "protocol/Serialization.h"
#include "schema/packet.capnp.h"

namespace gs::game {
namespace {

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

void WriteTransformRecord(std::vector<std::uint8_t>& payload, const BorderEntitySnapshot& snapshot)
{
    WriteU32(payload, snapshot.net_id);
    WriteF32(payload, snapshot.position.x);
    WriteF32(payload, snapshot.position.y);
    WriteF32(payload, snapshot.position.z);
    WriteU16(payload, QuantizeHeading(snapshot.heading.angle));
    payload.push_back(static_cast<std::uint8_t>(snapshot.move_state));
}

} // namespace

std::uint16_t QuantizeHeading(float angle)
{
    while (angle < 0.0f) {
        angle += kTwoPi;
    }
    while (angle >= kTwoPi) {
        angle -= kTwoPi;
    }
    return static_cast<std::uint16_t>(std::lround((angle / kTwoPi) * 65535.0f));
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

std::vector<std::uint8_t> MakeSpawn(const BorderEntitySnapshot& snapshot)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto spawn = packet.initEntitySpawn();
    spawn.setNetId(snapshot.net_id);
    spawn.setName(snapshot.name);
    spawn.setClassId(snapshot.class_id);
    FillVec3(spawn.initSpawnPos(), snapshot.position);
    spawn.setHeading(QuantizeHeading(snapshot.heading.angle));
    spawn.setMobType(snapshot.mob_type_id);
    spawn.setLevel(snapshot.level);
    spawn.setHpCurrent(snapshot.hp_current);
    spawn.setHpMax(snapshot.hp_max);
    return gs::protocol::SerializeToBytes(msg);
}

std::vector<std::uint8_t> MakeDespawn(std::uint32_t net_id)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto despawn = packet.initEntityDespawn();
    despawn.setNetId(net_id);
    return gs::protocol::SerializeToBytes(msg);
}

std::vector<std::uint8_t> MakeHealthUpdate(std::uint32_t net_id, const Hp& hp)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto update = packet.initEntityHealthUpdate();
    update.setNetId(net_id);
    update.setHpCurrent(std::max(0.0f, hp.current));
    update.setHpMax(std::max(1.0f, hp.max));
    return gs::protocol::SerializeToBytes(msg);
}

std::vector<std::uint8_t> MakeDeath(std::uint32_t net_id, std::uint32_t killer_net_id)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto death = packet.initEntityDeath();
    death.setNetId(net_id);
    death.setKillerNetId(killer_net_id);
    return gs::protocol::SerializeToBytes(msg);
}

std::vector<std::uint8_t> EncodeTransformFrame(const BorderEntitySnapshot& viewer,
                                               const std::vector<BorderEntitySnapshot>& visible,
                                               std::uint32_t zone_tick)
{
    const std::size_t record_count = 1 + visible.size();
    std::vector<std::uint8_t> payload;
    payload.reserve(1 + 1 + 4 + 2 + record_count * 19);
    payload.push_back(gs::protocol::kCodecBinary);
    payload.push_back(0x10);
    WriteU32(payload, zone_tick);
    WriteU16(payload, static_cast<std::uint16_t>(record_count));
    WriteTransformRecord(payload, viewer);
    for (const auto& visible_entity : visible) {
        WriteTransformRecord(payload, visible_entity);
    }
    return payload;
}

} // namespace gs::game
