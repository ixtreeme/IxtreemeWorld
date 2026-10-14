#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/CharacterRepository.h"
#include "protocol/Protocol.h"
#include "db/HandoffTokenRepository.h"
#include "network/Session.h"
#include "protocol/Serialization.h"
#include "schema/packet.capnp.h"

#include "world/WorldRuntime.h"

namespace gs::game {

enum class GameSessionState {
    WaitingHandshake,
    ConnectionEstablished,
    EnteringWorld,
    InWorld,
};

// Network-edge input policy (hardening H3). Moves are latest-state and the
// world coalesces them to one applied input per tick, so the edge never drops
// a move below the abuse threshold (dropping the NEWEST could lose a "stop").
// Only traffic far beyond any real client is refused: move packets above the
// hard per-second rate disconnect the session. Attacks are events: a token
// bucket admits them; excess is dropped and counted.
struct ConnectionPolicy {
    std::uint32_t max_move_packets_per_second = 1000; // 0 = unlimited
    float attacks_per_second = 20.0f;                 // 0 = unlimited
    float attack_burst = 20.0f;
};

// Fixed one-second window event counter (pure; unit-tested).
struct RateWindow {
    std::chrono::steady_clock::time_point window_start{};
    std::uint32_t count = 0;
    // Records one event at `now`; returns the count in the current window.
    std::uint32_t Note(std::chrono::steady_clock::time_point now) noexcept
    {
        if (window_start == std::chrono::steady_clock::time_point{} ||
            now - window_start >= std::chrono::seconds(1)) {
            window_start = now;
            count = 0;
        }
        return ++count;
    }
};

// Token bucket, starts full (pure; unit-tested).
struct TokenBucket {
    float tokens = -1.0f; // < 0: not yet initialized
    std::chrono::steady_clock::time_point last{};
    bool Take(std::chrono::steady_clock::time_point now, float rate, float burst) noexcept
    {
        if (!(rate > 0.0f)) {
            return true;
        }
        if (tokens < 0.0f) {
            tokens = burst;
            last = now;
        }
        const float elapsed = std::chrono::duration<float>(now - last).count();
        last = now;
        tokens = std::min(burst, tokens + elapsed * rate);
        if (tokens >= 1.0f) {
            tokens -= 1.0f;
            return true;
        }
        return false;
    }
};

struct GameSessionContext {
    GameSessionState state = GameSessionState::WaitingHandshake;
    RateWindow move_window;
    TokenBucket attack_bucket;
};

struct ConnectionStats {
    std::uint64_t moves_accepted = 0;
    std::uint64_t moves_dropped_entering = 0; // state gate: EnteringWorld (race-tolerant drop)
    std::uint64_t moves_rejected_state = 0;   // state gate: before world entry -> disconnect
    std::uint64_t move_flood_disconnects = 0;
    std::uint64_t attacks_accepted = 0;
    std::uint64_t attacks_rate_limited = 0;
};

class GameConnectionHandler {
public:
    GameConnectionHandler(gs::db::HandoffTokenRepository& handoff_tokens,
                           gs::db::CharacterRepository& characters,
                           WorldRuntime& sim,
                           std::string game_server,
                           ConnectionPolicy policy = {});

    void OnPayload(std::shared_ptr<gs::network::Session> session,
                   std::vector<std::uint8_t> payload);
    void OnDisconnect(std::shared_ptr<gs::network::Session> session);

    // Observability (tests/admin): live per-session contexts and completed
    // disconnect cleanups (context erased + world despawn posted).
    std::size_t ContextCount() const;
    std::uint64_t DisconnectCleanups() const noexcept
    {
        return disconnect_cleanups_.load(std::memory_order_relaxed);
    }
    ConnectionStats Stats() const noexcept;

    // The one parse entry point for client packets (the handler and the
    // protocol hardening tests share it).
    static std::optional<gs::protocol::ParsedPacket> ParseClientPacket(
        const std::vector<std::uint8_t>& payload);

private:
    // Runs under contexts_mutex_; returns a disconnect reason instead of
    // disconnecting (the caller disconnects after releasing the lock).
    std::optional<std::string> ProcessPacketLocked(
        const std::shared_ptr<gs::network::Session>& session,
        GameSessionContext& ctx,
        const std::vector<std::uint8_t>& payload);
    void HandleHandshakeRequest(std::shared_ptr<gs::network::Session> session,
                                GameSessionContext& ctx,
                                gs::protocol::HandshakeRequest::Reader request);
    void HandleEnterWorld(std::shared_ptr<gs::network::Session> session,
                          GameSessionContext& ctx,
                          gs::protocol::C2sEnterWorld::Reader request);
    void SendHandshakeResponse(std::shared_ptr<gs::network::Session> session,
                               gs::protocol::HandshakeResult result,
                               const std::string& message,
                               bool close_after_send = false,
                               std::uint32_t protocol_version = gs::protocol::kProtocolVersion);
    void SendEnterWorldReject(std::shared_ptr<gs::network::Session> session,
                              gs::protocol::S2cEnterWorldReject::RejectReason reason,
                              bool close_after_send = true);
    void Disconnect(std::shared_ptr<gs::network::Session> session, const std::string& reason);

    gs::db::HandoffTokenRepository& handoff_tokens_;
    gs::db::CharacterRepository& characters_;
    WorldRuntime& sim_;
    std::string game_server_;
    mutable std::mutex contexts_mutex_;
    std::unordered_map<gs::common::SessionId, GameSessionContext> contexts_;
    std::atomic<std::uint64_t> disconnect_cleanups_{0};
    ConnectionPolicy policy_;
    std::atomic<std::uint64_t> moves_accepted_{0};
    std::atomic<std::uint64_t> moves_dropped_entering_{0};
    std::atomic<std::uint64_t> moves_rejected_state_{0};
    std::atomic<std::uint64_t> move_flood_disconnects_{0};
    std::atomic<std::uint64_t> attacks_accepted_{0};
    std::atomic<std::uint64_t> attacks_rate_limited_{0};
};

} // namespace gs::game
