#include "platform/tcp_stream.h"

#if !defined(_WIN32)
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace platform
{
namespace
{
// Input is pulled while the owner keeps up; beyond this the socket's own buffer applies backpressure.
constexpr std::size_t kMaxBufferedInput = 16u * 1024u * 1024u;

int SendFlags()
{
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;  // a closed peer must not raise SIGPIPE (Linux/FreeBSD)
#else
    return 0;
#endif
}
} // namespace

struct TcpStream::Impl
{
    int fd = -1;
    State state = State::Failed;
    std::string error;
    std::vector<std::uint8_t> out;
    std::size_t outOffset = 0;
    std::vector<std::uint8_t> in;

    void Fail(const char* what, int code)
    {
        error = std::string(what) + ": " + std::strerror(code);
        state = State::Failed;
        CloseSocket();
    }
    void CloseSocket()
    {
        if (fd >= 0)
            ::close(fd);
        fd = -1;
    }
    void Flush()
    {
        while (state == State::Connected && outOffset < out.size())
        {
            const ssize_t n = ::send(fd, out.data() + outOffset, out.size() - outOffset, SendFlags());
            if (n > 0)
            {
                outOffset += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                break;
            Fail("send", errno);
            return;
        }
        if (outOffset == out.size())
        {
            std::fill(out.begin(), out.end(), std::uint8_t{0});  // sent bytes may include credentials
            out.clear();
            outOffset = 0;
        }
    }
    void Pull()
    {
        std::uint8_t chunk[16384];
        while (state == State::Connected && in.size() < kMaxBufferedInput)
        {
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n > 0)
            {
                in.insert(in.end(), chunk, chunk + n);
                continue;
            }
            if (n == 0)
            {
                state = State::Closed;
                CloseSocket();
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            Fail("recv", errno);
            return;
        }
    }
};

TcpStream::TcpStream(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

TcpStream::~TcpStream()
{
    if (m_impl)
        m_impl->CloseSocket();
}

std::unique_ptr<TcpStream> TcpStream::Connect(const std::string& host, std::uint16_t port, std::string* errorOut)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    const std::string service = std::to_string(port);
    if (const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &list); rc != 0 || list == nullptr)
    {
        if (errorOut)
            *errorOut = std::string("resolve ") + host + ": " + ::gai_strerror(rc);
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    std::string lastError = "no usable address";
    for (addrinfo* a = list; a != nullptr; a = a->ai_next)
    {
        const int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
        {
            lastError = std::string("socket: ") + std::strerror(errno);
            continue;
        }
#if defined(SO_NOSIGPIPE)
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));  // BSD/macOS SIGPIPE guard
#endif
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0)
        {
            impl->fd = fd;
            impl->state = State::Connected;
            break;
        }
        if (errno == EINPROGRESS)
        {
            impl->fd = fd;
            impl->state = State::Connecting;
            break;
        }
        lastError = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
    }
    ::freeaddrinfo(list);
    if (impl->fd < 0)
    {
        if (errorOut)
            *errorOut = lastError;
        return nullptr;
    }
    return std::unique_ptr<TcpStream>(new TcpStream(std::move(impl)));
}

TcpStream::State TcpStream::Poll()
{
    Impl& s = *m_impl;
    if (s.state == State::Connecting)
    {
        pollfd p{};
        p.fd = s.fd;
        p.events = POLLOUT;
        if (::poll(&p, 1, 0) > 0)
        {
            int code = 0;
            socklen_t length = sizeof(code);
            ::getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &code, &length);
            if (code == 0)
                s.state = State::Connected;
            else
                s.Fail("connect", code);
        }
    }
    if (s.state == State::Connected)
    {
        s.Flush();
        s.Pull();
    }
    return s.state;
}

TcpStream::State TcpStream::GetState() const noexcept { return m_impl->state; }

bool TcpStream::Send(const std::uint8_t* data, std::size_t size)
{
    Impl& s = *m_impl;
    if (s.state == State::Closed || s.state == State::Failed)
        return false;
    s.out.insert(s.out.end(), data, data + size);
    if (s.state == State::Connected)
        s.Flush();
    return s.state != State::Failed;
}

std::size_t TcpStream::Receive(std::uint8_t* out, std::size_t capacity)
{
    Impl& s = *m_impl;
    const std::size_t n = std::min(capacity, s.in.size());
    if (n > 0)
    {
        std::memcpy(out, s.in.data(), n);
        s.in.erase(s.in.begin(), s.in.begin() + static_cast<std::ptrdiff_t>(n));
    }
    return n;
}

std::size_t TcpStream::Available() const noexcept { return m_impl->in.size(); }

void TcpStream::Close()
{
    m_impl->CloseSocket();
    if (m_impl->state != State::Failed)
        m_impl->state = State::Closed;
}

const std::string& TcpStream::Error() const noexcept { return m_impl->error; }
} // namespace platform
#endif
