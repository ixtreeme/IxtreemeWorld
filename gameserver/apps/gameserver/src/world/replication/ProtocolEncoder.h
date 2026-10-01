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
// snapshots/payloads to this module. Transform frames exist in two formats:
// v1 (opcode 0x10, full 19-byte records) and v2 (opcode 0x11, field-level
// deltas against the recipient's known state).
//
// WIRE TICK CONTRACT (hardening H7). Every tick field on the wire -- the u32
// in transform frame headers and S2cEnterWorldAccept.serverTick -- is the
// GLOBAL world tick: the supervisor's 20 Hz counter (kTickDt), shared by all
// zones. A frame carries the world tick at the start of the zone tick that
// produced it. Per recipient the sequence is monotonic non-decreasing across
// zones (migration, split, merge never move it backwards); under scheduling
// jitter consecutive frames may repeat a value or skip one. A zone's local
// tick counter (Zone::TickIndex, the LOD timebase) never reaches the wire:
// it restarts per zone and freezes while a zone sleeps.
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
                                               std::uint32_t world_tick,
                                               std::uint32_t volume_id = 0,
                                               std::uint32_t layer_id = 0);
// EnterWorldReject::alreadyInWorld (world presence invariant, hardening H4).
std::vector<std::uint8_t> MakeEnterWorldRejectAlreadyInWorld();
// No valid spawn position could be resolved (MAP-2 spawn rule): serverError.
std::vector<std::uint8_t> MakeEnterWorldRejectServerError();

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
