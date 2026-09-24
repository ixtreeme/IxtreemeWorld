#include "network/Session.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <span>
#include <utility>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include "common/Logging.h"
#include "network/Framing.h"

namespace gs::network {
namespace asio = boost::asio;
using boost::asio::ip::tcp;

namespace {

void RaiseHighWater(std::atomic<std::uint64_t>& mark, std::uint64_t value) noexcept
{
    std::uint64_t observed = mark.load(std::memory_order_relaxed);
    while (value > observed &&
           !mark.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
    }
}

} // namespace

SessionCounters& GlobalSessionCounters() noexcept
{
    static SessionCounters counters;
    return counters;
}

Session::Session(tcp::socket socket, gs::common::SessionId id, SessionLimits limits)
    : socket_(std::move(socket))
    , strand_(asio::make_strand(socket_.get_executor()))
    , housekeeping_(strand_)
    , id_(id)
    , limits_(limits)
{
}

void Session::Start(PayloadHandler on_payload, DisconnectHandler on_disconnect)
{
    on_payload_ = std::move(on_payload);
    on_disconnect_ = std::move(on_disconnect);

    auto self = shared_from_this();
    // The read loop lives on the session strand: every resume, and therefore
    // every payload/disconnect handler call, is serialized with the writes.
    asio::co_spawn(
        strand_,
        [self]() -> asio::awaitable<void> {
            co_await self->ReadLoop();
        },
        asio::detached);
    asio::dispatch(strand_, [self] {
        self->started_at_ = Clock::now();
        self->last_receive_ = self->started_at_;
        self->ArmHousekeeping();
    });
}

void Session::Stop()
{
    // Callable from any thread; inline when already on the strand (e.g. a
    // payload handler disconnecting its own session).
    auto self = shared_from_this();
    asio::dispatch(strand_, [self] {
        self->StopOnStrand();
    });
}

void Session::StopOnStrand()
{
    stopped_.store(true, std::memory_order_release);
    boost::system::error_code ignored;
    housekeeping_.cancel();
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
    // The write queue is NOT cleared here: an in-flight async_write still
    // references the front frame's buffer until its (aborted) completion
    // runs. The queue dies with the session.

    if (!disconnect_notified_) {
        disconnect_notified_ = true;
        if (on_disconnect_) {
            on_disconnect_(shared_from_this());
        }
    }
}

void Session::ArmHousekeeping()
{
    // One cheap periodic check per session instead of re-arming a timer on
    // every received packet. Period follows the tightest enabled limit.
    auto period = std::chrono::milliseconds(1000);
    for (const auto limit : {limits_.idle_timeout, limits_.setup_timeout, limits_.send_hard_age}) {
        if (limit.count() > 0) {
            period = std::min(period, limit / 4);
        }
    }
    period = std::max(period, std::chrono::milliseconds(20));
    if (limits_.idle_timeout.count() <= 0 && limits_.setup_timeout.count() <= 0 &&
        limits_.send_hard_age.count() <= 0) {
        return; // nothing to police
    }
    housekeeping_.expires_after(period);
    auto self = shared_from_this();
    housekeeping_.async_wait(asio::bind_executor(strand_, [self](const boost::system::error_code& ec) {
        if (ec || self->IsStopped()) {
            return;
        }
        self->Housekeep();
        if (!self->IsStopped()) {
            self->ArmHousekeeping();
        }
    }));
}

void Session::Housekeep()
{
    const auto now = Clock::now();
    auto& counters = GlobalSessionCounters();
    if (limits_.setup_timeout.count() > 0 && !established_.load(std::memory_order_acquire) &&
        now - started_at_ > limits_.setup_timeout) {
        counters.setup_timeouts.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("Session {} closed: setup not completed within {} ms", id_,
                 limits_.setup_timeout.count());
        StopOnStrand();
        return;
    }
    if (limits_.idle_timeout.count() > 0 && now - last_receive_ > limits_.idle_timeout) {
        counters.idle_timeouts.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("Session {} closed: idle for {} ms", id_, limits_.idle_timeout.count());
        StopOnStrand();
        return;
    }
    if (limits_.send_hard_age.count() > 0 && !write_queue_.empty() &&
        now - write_queue_.front().enqueued_at > limits_.send_hard_age) {
        counters.stalled_queue_disconnects.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("Session {} closed: send queue stalled {} ms ({} bytes, {} frames)", id_,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - write_queue_.front().enqueued_at)
                     .count(),
                 queued_bytes_,
                 write_queue_.size());
        StopOnStrand();
    }
}

void Session::SendPayload(std::vector<std::uint8_t> payload)
{
    SendPayloadInternal(std::move(payload), false);
}

void Session::SendPayloadAndClose(std::vector<std::uint8_t> payload)
{
    SendPayloadInternal(std::move(payload), true);
}

void Session::SendPayloadInternal(std::vector<std::uint8_t> payload, bool close_after_send)
{
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }

    PendingWrite pending{Framing::Encode(std::span<const std::uint8_t>(payload.data(), payload.size())),
                         close_after_send,
                         Clock::now()};

    auto self = shared_from_this();
    asio::post(strand_, [self, pending = std::move(pending)]() mutable {
        if (self->stopped_.load(std::memory_order_relaxed)) {
            return;
        }

        self->queued_bytes_ += pending.frame.size();
        self->write_queue_.push_back(std::move(pending));
        auto& counters = GlobalSessionCounters();
        self->queued_bytes_now_.store(self->queued_bytes_, std::memory_order_relaxed);
        if (self->queued_bytes_ > self->max_queued_bytes_.load(std::memory_order_relaxed)) {
            self->max_queued_bytes_.store(self->queued_bytes_, std::memory_order_relaxed);
        }
        RaiseHighWater(counters.max_queued_bytes, self->queued_bytes_);
        RaiseHighWater(counters.max_queued_frames, self->write_queue_.size());

        // Hard limit: the client cannot keep up -- disconnect it rather than
        // grow without bound (or drop frames and desync v2 state).
        const auto& limits = self->limits_;
        if ((limits.send_hard_bytes > 0 && self->queued_bytes_ > limits.send_hard_bytes) ||
            (limits.send_hard_frames > 0 && self->write_queue_.size() > limits.send_hard_frames)) {
            counters.slow_client_disconnects.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("Session {} closed: send queue over hard limit ({} bytes, {} frames)",
                     self->Id(),
                     self->queued_bytes_,
                     self->write_queue_.size());
            self->StopOnStrand();
            return;
        }
        if (limits.send_soft_bytes > 0 && !self->soft_pressure_ &&
            self->queued_bytes_ > limits.send_soft_bytes) {
            self->soft_pressure_ = true;
            counters.send_soft_pressure_events.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("Session {} send queue under pressure ({} bytes queued)",
                     self->Id(),
                     self->queued_bytes_);
        }

        if (!self->writing_) {
            self->StartWriteQueue();
        }
    });
}

void Session::StartWriteQueue()
{
    if (stopped_.load(std::memory_order_relaxed)) {
        return;
    }

    if (write_queue_.empty()) {
        writing_ = false;
        return;
    }

    writing_ = true;
    auto self = shared_from_this();
    asio::async_write(
        socket_,
        asio::buffer(write_queue_.front().frame),
        asio::bind_executor(strand_, [self](const boost::system::error_code& error, std::size_t) {
            if (error) {
                if (!self->IsStopped()) {
                    LOG_ERROR("Session {} send error: {}", self->Id(), error.message());
                }
                self->StopOnStrand();
                return;
            }

            const bool close_after_send = self->write_queue_.front().close_after_send;
            self->queued_bytes_ -= self->write_queue_.front().frame.size();
            self->write_queue_.pop_front();
            self->queued_bytes_now_.store(self->queued_bytes_, std::memory_order_relaxed);
            if (self->soft_pressure_ && self->queued_bytes_ <= self->limits_.send_soft_bytes / 2) {
                self->soft_pressure_ = false; // episode over (hysteresis)
            }
            if (close_after_send) {
                self->StopOnStrand();
                return;
            }

            self->StartWriteQueue();
        }));
}

asio::awaitable<void> Session::ReadLoop()
{
    try {
        for (;;) {
            std::array<std::uint8_t, Framing::kHeaderSize> header{};
            co_await asio::async_read(socket_, asio::buffer(header), asio::use_awaitable);

            std::uint32_t network_length = 0;
            std::memcpy(&network_length, header.data(), header.size());
            const auto length = ntohl(network_length);

            if (length > Framing::kMaxPayloadSize) {
                LOG_ERROR("Session {} rejected oversized payload: {} bytes", id_, length);
                break;
            }

            std::vector<std::uint8_t> payload(length);
            if (length > 0) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }

            // A Stop() queued on the strand may have run while this read was
            // completing: never hand a payload of a stopped session to the
            // application (it would recreate state OnDisconnect already
            // cleaned up).
            if (stopped_.load(std::memory_order_relaxed)) {
                break;
            }
            last_receive_ = Clock::now();

            LOG_DEBUG("Session {} received {} bytes", id_, length);
            if (on_payload_) {
                on_payload_(shared_from_this(), std::move(payload));
            }
        }
    } catch (const boost::system::system_error& error) {
        if (!stopped_.load(std::memory_order_relaxed)) {
            LOG_DEBUG("Session {} closed: {}", id_, error.code().message());
        }
    } catch (const std::exception& error) {
        LOG_ERROR("Session {} error: {}", id_, error.what());
    }

    StopOnStrand();
    LOG_INFO("Session {} stopped", id_);
}

} // namespace gs::network
