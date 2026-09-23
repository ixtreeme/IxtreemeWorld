#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "../visibility/BorderSnapshot.h"
#include "../components/CombatComponents.h"
#include "../components/MovementComponents.h"
#include "../components/TransformComponents.h"

// Sole owner of wire-format knowledge (Cap'n Proto packets + the binary
// transform codec). The WorldRuntime and Zone never format packets; they hand
// snapshots/payloads to this module. Protocol FORMAT is unchanged (parity).
namespace gs::game {

// kTransformRecordSize / TransformRecord live in BorderSnapshot.h (shared
// with Zone state; the canonical record is produced and consumed here).

// Phase 6 field-level delta (v2). The mask selects which fields follow the
// NetId; a delta is always relative to the recipient's known state.
inline constexpr std::uint8_t kTransformFieldPosition = 0x01;
inline constexpr std::uint8_t kTransformFieldHeading = 0x02;
inline constexpr std::uint8_t kTransformFieldMoveState = 0x04;
inline constexpr std::uint8_t kTransformFieldAll = 0x07;
// v2 frame opcode (v1 transform frames stay 0x10).
inline constexpr std::uint8_t kTransformFrameV2Opcode = 0x11;

// Appends one v2 delta record: u32 NetId, u8 mask, then the masked fields
// (position 3xf32, heading u16, move state u8).
void AppendTransformDelta(std::vector<std::uint8_t>& payload,
                          std::uint32_t net_id,
                          std::uint8_t mask,
                          const Position& position,
                          float heading_angle,
                          MoveState move_state);

// v2 frame: codec, opcode 0x11, tick, record count (viewer + deltas), the
// viewer's own full 19-byte record, then the delta payload.
std::vector<std::uint8_t> EncodeTransformFrameV2(const TransformRecord& viewer_record,
                                                 const std::vector<std::uint8_t>& delta_payload,
                                                 std::uint32_t delta_count,
                                                 std::uint32_t zone_tick);

std::vector<std::uint8_t> MakeEnterWorldAccept(std::uint32_t net_id,
                                               const Position& pos,
                                               std::uint32_t world_tick);

std::vector<std::uint8_t> MakeSpawn(const BorderEntitySnapshot& snapshot);
std::vector<std::uint8_t> MakeDespawn(std::uint32_t net_id);
std::vector<std::uint8_t> MakeHealthUpdate(std::uint32_t net_id, const Hp& hp);
std::vector<std::uint8_t> MakeDeath(std::uint32_t net_id, std::uint32_t killer_net_id);

// Binary per-tick transform frame: self record + visible records.
std::vector<std::uint8_t> EncodeTransformFrame(const BorderEntitySnapshot& viewer,
                                               const std::vector<BorderEntitySnapshot>& visible,
                                               std::uint32_t zone_tick);

// Phase 5C: canonical record encode (one entity -> 19 bytes) and frame
// assembly from cached records. The frame layout is byte-identical to
// EncodeTransformFrame.
TransformRecord EncodeTransformRecord(std::uint32_t net_id,
                                      const Position& position,
                                      const Heading& heading,
                                      MoveState move_state);
std::vector<std::uint8_t> EncodeTransformFrameFromRecords(
    const TransformRecord& viewer_record,
    const std::vector<TransformRecord>& records,
    const std::vector<std::uint32_t>& record_slots,
    std::uint32_t zone_tick);

std::uint16_t QuantizeHeading(float angle);

} // namespace gs::game
