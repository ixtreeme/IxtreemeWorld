#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX  // <windows.h> min/max macros would break std::min below
#endif
#include "platform/tcp_stream.h"

#if defined(_WIN32)
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace platform
{
namespace
{
constexpr std::size_t kMaxBufferedInput = 16u * 1024u * 1024u;

// Winsock is initialised once for the process (and never torn down while streams may exist).
bool EnsureWinsock(std::string* errorOut)
{
    static std::once_flag once;
    static int startupResult = 0;
    std::call_once(once, [] {
        WSADATA data{};
        startupResult = ::WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (startupResult != 0 && errorOut)
        *errorOut = "WSAStartup failed: " + std::to_string(startupResult);
    return startupResult == 0;
}

std::string WinsockError(const char* what, int code)
{
    return std::string(what) + ": winsock error " + std::to_string(code);
}
} // namespace

struct TcpStream::Impl
{
    SOCKET fd = INVALID_SOCKET;
    State state = State::Failed;
    std::string error;
    std::vector<std::uint8_t> out;
    std::size_t outOffset = 0;
    std::vector<std::uint8_t> in;

    void Fail(const char* what, int code)
    {
        error = WinsockError(what, code);
        state = State::Failed;
        CloseSocket();
    }
    void CloseSocket()
    {
        if (fd != INVALID_SOCKET)
            ::closesocket(fd);
        fd = INVALID_SOCKET;
    }
    void Flush()
    {
        while (state == State::Connected && outOffset < out.size())
        {
            const int chunk = static_cast<int>(std::min<std::size_t>(out.size() - outOffset, 1u << 20));
            const int n = ::send(fd, reinterpret_cast<const char*>(out.data() + outOffset), chunk, 0);
            if (n > 0)
            {
                outOffset += static_cast<std::size_t>(n);
                continue;
            }
            const int code = ::WSAGetLastError();
            if (n == SOCKET_ERROR && code == WSAEWOULDBLOCK)
                break;
            Fail("send", code);
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
        char chunk[16384];
        while (state == State::Connected && in.size() < kMaxBufferedInput)
        {
            const int n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n > 0)
            {
                in.insert(in.end(), reinterpret_cast<std::uint8_t*>(chunk), reinterpret_cast<std::uint8_t*>(chunk) + n);
                continue;
            }
            if (n == 0)
            {
                state = State::Closed;
                CloseSocket();
                return;
            }
            const int code = ::WSAGetLastError();
            if (code == WSAEWOULDBLOCK)
                return;
            Fail("recv", code);
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
    if (!EnsureWinsock(errorOut))
        return nullptr;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* list = nullptr;
    const std::string service = std::to_string(port);
    if (const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &list); rc != 0 || list == nullptr)
    {
        if (errorOut)
            *errorOut = "resolve " + host + ": error " + std::to_string(rc);
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    std::string lastError = "no usable address";
    for (addrinfo* a = list; a != nullptr; a = a->ai_next)
    {
        const SOCKET fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd == INVALID_SOCKET)
        {
            lastError = WinsockError("socket", ::WSAGetLastError());
            continue;
        }
        u_long nonBlocking = 1;
        ::ioctlsocket(fd, FIONBIO, &nonBlocking);
        if (::connect(fd, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0)
        {
            impl->fd = fd;
            impl->state = State::Connected;
            break;
        }
        const int code = ::WSAGetLastError();
        if (code == WSAEWOULDBLOCK)
        {
            impl->fd = fd;
            impl->state = State::Connecting;
            break;
        }
        lastError = WinsockError("connect", code);
        ::closesocket(fd);
    }
    ::freeaddrinfo(list);
    if (impl->fd == INVALID_SOCKET)
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
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s.fd, &writable);
        FD_SET(s.fd, &failed);
        timeval zero{0, 0};
        if (::select(0, nullptr, &writable, &failed, &zero) > 0)
        {
            int code = 0;
            int length = sizeof(code);
            ::getsockopt(s.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length);
            if (code == 0 && FD_ISSET(s.fd, &writable))
                s.state = State::Connected;
            else
                s.Fail("connect", code != 0 ? code : WSAECONNREFUSED);
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
