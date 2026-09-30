#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include "common/Types.h"
#include "db/Types.h"

// World presence registry (hardening H4). Gameserver invariant:
//
//     one CharacterId -> at most one authoritative world presence
//
// A presence is claimed when the supervisor accepts a spawn for a session and
// released only by that same session's despawn. Zone, net id and routing stay
// in the owner map (session -> OwnerInfo, the single source of truth for WHERE
// the entity lives); this registry answers WHO holds a character, so the full
// presence is CharacterId -> SessionId -> OwnerInfo.
//
// Policy today: first presence wins -- a second session entering with a
// character that is already present is refused (the client gets
// EnterWorldReject::alreadyInWorld and can retry once the old session is
// gone). Reconnect/takeover (kick the old session, then admit the new one) is
// a later policy on top of this seam; it must preserve the invariant by
// admitting the new session only after the old presence is released.
//
// Supervisor-thread only (same as the owner map). Concrete class, no interface.
namespace gs::game {

struct PresenceRecord {
    gs::common::SessionId session_id = 0;
    std::uint32_t net_id = 0;
    std::uint32_t claimed_world_tick = 0;
};

class PresenceRegistry {
public:
    enum class ClaimResult {
        Claimed,
        HeldByOtherSession,
    };

    ClaimResult Claim(gs::db::CharacterId character,
                      gs::common::SessionId session_id,
                      std::uint32_t net_id,
                      std::uint32_t world_tick)
    {
        const auto existing = by_character_.find(character);
        if (existing != by_character_.end() && existing->second.session_id != session_id) {
            rejected_duplicates_.fetch_add(1, std::memory_order_relaxed);
            return ClaimResult::HeldByOtherSession;
        }
        // Same session re-entering keeps a single record (its previous
        // presence was already despawned by the caller).
        if (const auto previous = character_by_session_.find(session_id);
            previous != character_by_session_.end() && previous->second != character) {
            by_character_.erase(previous->second);
        }
        by_character_[character] = PresenceRecord{session_id, net_id, world_tick};
        character_by_session_[session_id] = character;
        claims_.fetch_add(1, std::memory_order_relaxed);
        present_.store(by_character_.size(), std::memory_order_relaxed);
        return ClaimResult::Claimed;
    }

    // Admission check before anything is allocated: false (and counted as a
    // refused duplicate) when another session already holds the character.
    bool Admit(gs::db::CharacterId character, gs::common::SessionId session_id)
    {
        const auto existing = by_character_.find(character);
        if (existing != by_character_.end() && existing->second.session_id != session_id) {
            rejected_duplicates_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    // Releases the presence held by `session_id`, if any. A session can only
    // ever release its own presence, never another session's.
    std::optional<gs::db::CharacterId> ReleaseBySession(gs::common::SessionId session_id)
    {
        const auto it = character_by_session_.find(session_id);
        if (it == character_by_session_.end()) {
            return std::nullopt;
        }
        const gs::db::CharacterId character = it->second;
        character_by_session_.erase(it);
        const auto record = by_character_.find(character);
        if (record != by_character_.end() && record->second.session_id == session_id) {
            by_character_.erase(record);
        }
        releases_.fetch_add(1, std::memory_order_relaxed);
        present_.store(by_character_.size(), std::memory_order_relaxed);
        return character;
    }

    const PresenceRecord* Find(gs::db::CharacterId character) const
    {
        const auto it = by_character_.find(character);
        return it == by_character_.end() ? nullptr : &it->second;
    }

    std::optional<gs::db::CharacterId> CharacterOf(gs::common::SessionId session_id) const
    {
        const auto it = character_by_session_.find(session_id);
        if (it == character_by_session_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    const std::unordered_map<gs::db::CharacterId, PresenceRecord>& Records() const noexcept
    {
        return by_character_;
    }

    void Clear()
    {
        by_character_.clear();
        character_by_session_.clear();
        present_.store(0, std::memory_order_relaxed);
    }

    // Counters are atomics: readable from any thread (benchmarks, admin).
    struct Stats {
        std::uint64_t claims = 0;
        std::uint64_t releases = 0;
        std::uint64_t rejected_duplicates = 0;
        std::uint64_t present = 0; // gauge
    };
    Stats GetStats() const noexcept
    {
        return Stats{claims_.load(std::memory_order_relaxed),
                     releases_.load(std::memory_order_relaxed),
                     rejected_duplicates_.load(std::memory_order_relaxed),
                     present_.load(std::memory_order_relaxed)};
    }

private:
    std::unordered_map<gs::db::CharacterId, PresenceRecord> by_character_;
    std::unordered_map<gs::common::SessionId, gs::db::CharacterId> character_by_session_;
    std::atomic<std::uint64_t> claims_{0};
    std::atomic<std::uint64_t> releases_{0};
    std::atomic<std::uint64_t> rejected_duplicates_{0};
    std::atomic<std::uint64_t> present_{0};
};

} // namespace gs::game
