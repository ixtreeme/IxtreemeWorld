#include "network/Session.h"

#include <array>
#include <cstring>
#include <exception>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
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

Session::Session(tcp::socket socket, gs::common::SessionId id)
    : socket_(std::move(socket))
    , id_(id)
{
}

void Session::Start()
{
    auto self = shared_from_this();
    asio::co_spawn(
        socket_.get_executor(),
        [self]() -> asio::awaitable<void> {
            co_await self->ReadLoop();
        },
        asio::detached);
}

void Session::Stop()
{
    if (stopped_) {
        return;
    }

    stopped_ = true;
    boost::system::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
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

            LOG_DEBUG("Session {} received {} bytes", id_, length);
            co_await WriteFrame(std::move(payload));
        }
    } catch (const boost::system::system_error& error) {
        if (!stopped_) {
            LOG_DEBUG("Session {} closed: {}", id_, error.code().message());
        }
    } catch (const std::exception& error) {
        LOG_ERROR("Session {} error: {}", id_, error.what());
    }

    Stop();
    LOG_INFO("Session {} stopped", id_);
}

asio::awaitable<void> Session::WriteFrame(std::vector<std::uint8_t> payload)
{
    auto frame = Framing::Encode(payload);
    co_await asio::async_write(socket_, asio::buffer(frame), asio::use_awaitable);
}

} // namespace gs::network
