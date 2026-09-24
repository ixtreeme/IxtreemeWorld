#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include "common/Types.h"

namespace gs::network {

// Per-session resource policy (hardening H3). The library defaults keep every
// timeout OFF (the login server shares this library and has its own
// protocol); the game server enables them from config.
//
// Send queue: frames are never dropped individually -- replication v2 deltas
// are relative to the recipient's known state, so a silently dropped frame
// would desynchronize the client. Pressure is therefore explicit:
//   soft  -> counted + logged once per episode (the client is falling behind);
//   hard  -> the slow client is disconnected (bytes, frames or oldest-frame
//            age over the limit). Server state stays consistent; the client
//            reconnects.
struct SessionLimits {
    std::size_t send_soft_bytes = 512 * 1024;
    std::size_t send_hard_bytes = 8 * 1024 * 1024;
    std::size_t send_hard_frames = 20000;
    std::chrono::milliseconds send_hard_age{10000}; // 0 = off
    // Close when nothing was received for this long (0 = off).
    std::chrono::milliseconds idle_timeout{0};
    // Close unless MarkEstablished() happens within this long after Start
    // (e.g. a completed handshake; 0 = off).
    std::chrono::milliseconds setup_timeout{0};
};

// Process-wide network counters (cumulative).
struct SessionCounters {
    std::atomic<std::uint64_t> send_soft_pressure_events{0};
    std::atomic<std::uint64_t> slow_client_disconnects{0};    // bytes/frames hard limit
    std::atomic<std::uint64_t> stalled_queue_disconnects{0};  // oldest frame over the age limit
    std::atomic<std::uint64_t> idle_timeouts{0};
    std::atomic<std::uint64_t> setup_timeouts{0};
    std::atomic<std::uint64_t> max_queued_bytes{0};  // high-water mark, any session
    std::atomic<std::uint64_t> max_queued_frames{0}; // high-water mark, any session
};
SessionCounters& GlobalSessionCounters() noexcept;

// Threading contract (hardening H2.2): all mutable session state -- socket,
// write queue, writing flag, disconnect notification -- is touched ONLY on
// the session's own strand, so the session is safe on a multi-threaded
// io_context (production runs io_threads > 1). Public entry points may be
// called from any thread (IO threads, zone workers, DB callbacks):
//   SendPayload*  posts onto the strand;
//   Stop          dispatches onto the strand; idempotent, and the disconnect
//                 handler runs exactly once, on the strand.
// Handlers (payload/disconnect) are invoked on the strand; a payload handler
// may call Stop() re-entrantly (it runs inline).
class Session : public std::enable_shared_from_this<Session> {
public:
    using PayloadHandler = std::function<void(std::shared_ptr<Session> session,
                                              std::vector<std::uint8_t> payload)>;
    using DisconnectHandler = std::function<void(std::shared_ptr<Session> session)>;

    explicit Session(boost::asio::ip::tcp::socket socket,
                     gs::common::SessionId id,
                     SessionLimits limits = {});

    void Start(PayloadHandler on_payload, DisconnectHandler on_disconnect);
    void Stop();
    void SendPayload(std::vector<std::uint8_t> payload);
    void SendPayloadAndClose(std::vector<std::uint8_t> payload);
    // Clears the setup deadline (any thread).
    void MarkEstablished() noexcept
    {
        established_.store(true, std::memory_order_release);
    }

    [[nodiscard]] gs::common::SessionId Id() const { return id_; }
    [[nodiscard]] bool IsStopped() const noexcept
    {
        return stopped_.load(std::memory_order_acquire);
    }
    // Send-queue observability (point-in-time, any thread).
    [[nodiscard]] std::size_t QueuedBytes() const noexcept
    {
        return queued_bytes_now_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t MaxQueuedBytes() const noexcept
    {
        return max_queued_bytes_.load(std::memory_order_relaxed);
    }

private:
    using Clock = std::chrono::steady_clock;

    boost::asio::awaitable<void> ReadLoop();
    void SendPayloadInternal(std::vector<std::uint8_t> payload, bool close_after_send);
    void StartWriteQueue();
    void StopOnStrand();
    void ArmHousekeeping();
    void Housekeep();

    struct PendingWrite {
        std::vector<std::uint8_t> frame;
        bool close_after_send = false;
        Clock::time_point enqueued_at{};
    };

    boost::asio::ip::tcp::socket socket_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer housekeeping_;
    gs::common::SessionId id_;
    SessionLimits limits_;
    PayloadHandler on_payload_;
    DisconnectHandler on_disconnect_;
    // Written on the strand, read anywhere (cheap early-out for senders).
    std::atomic<bool> stopped_{false};
    std::atomic<bool> established_{false};
    std::atomic<std::size_t> queued_bytes_now_{0};
    std::atomic<std::size_t> max_queued_bytes_{0};
    // Strand-only state.
    bool disconnect_notified_ = false;
    bool writing_ = false;
    bool soft_pressure_ = false;
    std::size_t queued_bytes_ = 0;
    Clock::time_point started_at_{};
    Clock::time_point last_receive_{};
    std::deque<PendingWrite> write_queue_;
};

} // namespace gs::network
