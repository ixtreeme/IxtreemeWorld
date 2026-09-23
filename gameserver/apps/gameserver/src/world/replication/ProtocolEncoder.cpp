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

void WriteU32At(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value)
{
    out[offset] = static_cast<std::uint8_t>(value & 0xff);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    out[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    out[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
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

TransformRecord EncodeTransformRecord(std::uint32_t net_id,
                                      const Position& position,
                                      const Heading& heading,
                                      MoveState move_state)
{
    // Same field order/encoding as WriteTransformRecord: one canonical
    // 19-byte record, reused by every interested recipient this tick.
    TransformRecord record{};
    std::memcpy(record.data(), &net_id, sizeof(net_id));
    std::memcpy(record.data() + 4, &position.x, sizeof(position.x));
    std::memcpy(record.data() + 8, &position.y, sizeof(position.y));
    std::memcpy(record.data() + 12, &position.z, sizeof(position.z));
    const std::uint16_t heading_q = QuantizeHeading(heading.angle);
    record[16] = static_cast<std::uint8_t>(heading_q & 0xff);
    record[17] = static_cast<std::uint8_t>((heading_q >> 8) & 0xff);
    record[18] = static_cast<std::uint8_t>(move_state);
    return record;
}

void AppendTransformDelta(std::vector<std::uint8_t>& payload,
                          std::uint32_t net_id,
                          std::uint8_t mask,
                          const Position& position,
                          float heading_angle,
                          MoveState move_state)
{
    WriteU32(payload, net_id);
    payload.push_back(mask);
    if ((mask & kTransformFieldPosition) != 0) {
        WriteF32(payload, position.x);
        WriteF32(payload, position.y);
        WriteF32(payload, position.z);
    }
    if ((mask & kTransformFieldHeading) != 0) {
        WriteU16(payload, QuantizeHeading(heading_angle));
    }
    if ((mask & kTransformFieldMoveState) != 0) {
        payload.push_back(static_cast<std::uint8_t>(move_state));
    }
}

std::vector<std::uint8_t> EncodeTransformFrameV2(const TransformRecord& viewer_record,
                                                 const std::vector<std::uint8_t>& delta_payload,
                                                 std::uint32_t delta_count,
                                                 std::uint32_t zone_tick)
{
    const std::size_t record_count = 1 + delta_count;
    std::vector<std::uint8_t> payload;
    payload.reserve(1 + 1 + 4 + 2 + kTransformRecordSize + delta_payload.size());
    payload.push_back(gs::protocol::kCodecBinary);
    payload.push_back(kTransformFrameV2Opcode);
    WriteU32(payload, zone_tick);
    WriteU16(payload, static_cast<std::uint16_t>(record_count));
    payload.insert(payload.end(), viewer_record.begin(), viewer_record.end());
    payload.insert(payload.end(), delta_payload.begin(), delta_payload.end());
    return payload;
}

std::vector<std::uint8_t> EncodeTransformFrameFromRecords(
    const TransformRecord& viewer_record,
    const std::vector<TransformRecord>& records,
    const std::vector<std::uint32_t>& record_slots,
    std::uint32_t zone_tick)
{
    const std::size_t record_count = 1 + record_slots.size();
    std::vector<std::uint8_t> payload;
    payload.resize(1 + 1 + 4 + 2 + record_count * kTransformRecordSize);
    std::size_t offset = 0;
    payload[offset++] = gs::protocol::kCodecBinary;
    payload[offset++] = 0x10;
    WriteU32At(payload, offset, zone_tick);
    offset += 4;
    const auto count = static_cast<std::uint16_t>(record_count);
    payload[offset++] = static_cast<std::uint8_t>(count & 0xff);
    payload[offset++] = static_cast<std::uint8_t>((count >> 8) & 0xff);
    std::memcpy(payload.data() + offset, viewer_record.data(), kTransformRecordSize);
    offset += kTransformRecordSize;
    for (const std::uint32_t slot : record_slots) {
        std::memcpy(payload.data() + offset, records[slot].data(), kTransformRecordSize);
        offset += kTransformRecordSize;
    }
    return payload;
}

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
