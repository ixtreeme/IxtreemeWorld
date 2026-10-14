#pragma once

// Cross-platform, non-blocking TCP client byte stream (Win32 Winsock / POSIX sockets), behind the
// platform abstraction so the rest of the engine never touches an OS socket API. It is a TRANSPORT
// only: no framing, no protocol, no threads. The owner polls it once per frame; nothing ever blocks
// the game loop (connect, send and receive are all non-blocking).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace platform
{
class TcpStream
{
public:
    enum class State
    {
        Connecting,  // non-blocking connect in progress
        Connected,   // bytes flow
        Closed,      // the peer closed the stream (or Close() was called)
        Failed,      // resolve/connect/IO error (see Error())
    };

    // Resolves host (DNS name or IPv4/IPv6 literal) and starts a non-blocking connect. Returns nullptr
    // only when the attempt cannot even start (resolution failure, no socket); errorOut explains why.
    static std::unique_ptr<TcpStream> Connect(const std::string& host, std::uint16_t port,
                                              std::string* errorOut = nullptr);

    ~TcpStream();
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    // Advances the connect, flushes queued output and pulls available input. Returns the state.
    State Poll();
    State GetState() const noexcept;
    // Queues bytes for sending (flushed now as far as the socket accepts, the rest on later polls).
    // False once the stream is Closed/Failed.
    bool Send(const std::uint8_t* data, std::size_t size);
    // Moves up to `capacity` received bytes into `out`; returns how many (0 = nothing yet).
    std::size_t Receive(std::uint8_t* out, std::size_t capacity);
    // Bytes received and not yet taken by Receive.
    std::size_t Available() const noexcept;
    void Close();
    const std::string& Error() const noexcept;

    struct Impl;

private:
    explicit TcpStream(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace platform
