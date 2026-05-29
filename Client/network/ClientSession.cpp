#include "ClientSession.h"

#include <boost/asio.hpp>
#include <capnp/message.h>

#include <array>
#include <cstdio>
#include <deque>
#include <optional>
#include <utility>
#include <vector>
#include <windows.h>

#include <protocol/Serialization.h>
#include <kj/exception.h>
#include "schema/packet.capnp.h"

namespace client::net {
namespace {

constexpr std::size_t kHeaderSize = 4;
constexpr std::size_t kMaxPayloadSize = 64 * 1024;

void LogNet(const char* message)
{
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
    std::fprintf(stderr, "%s\n", message);
}

void LogNetFormat(const char* format, const std::string& value)
{
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), format, value.c_str());
    LogNet(buffer);
}

std::vector<std::uint8_t> SerializeToFrame(capnp::MessageBuilder& builder)
{
    auto payload = gs::protocol::SerializeToBytes(builder);
    std::vector<std::uint8_t> frame;
    frame.reserve(kHeaderSize + payload.size());

    const auto length = static_cast<std::uint32_t>(payload.size());
    frame.push_back(static_cast<std::uint8_t>((length >> 24) & 0xff));
    frame.push_back(static_cast<std::uint8_t>((length >> 16) & 0xff));
    frame.push_back(static_cast<std::uint8_t>((length >> 8) & 0xff));
    frame.push_back(static_cast<std::uint8_t>(length & 0xff));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::optional<std::size_t> ParseLengthPrefix(const std::uint8_t* bytes)
{
    const auto length = (static_cast<std::uint32_t>(bytes[0]) << 24) |
                        (static_cast<std::uint32_t>(bytes[1]) << 16) |
                        (static_cast<std::uint32_t>(bytes[2]) << 8) |
                        static_cast<std::uint32_t>(bytes[3]);
    if (length > kMaxPayloadSize) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(length);
}

} // namespace

struct ClientSession::Impl {
    enum class State {
        Offline,
        Connecting,
        WaitingHandshake,
        HandshakeAccepted,
        Authenticating,
        Authenticated,
    };

    explicit Impl(IClientHandler& h)
        : socket(io)
        , resolver(io)
        , handler(h)
    {
    }

    void Connect(const std::string& host, std::uint16_t port)
    {
        Disconnect(false);
        io.restart();
        state = State::Connecting;

        resolver.async_resolve(
            host,
            std::to_string(port),
            [this](const boost::system::error_code& error,
                   boost::asio::ip::tcp::resolver::results_type results) {
                if (error) {
                    state = State::Offline;
                    handler.OnConnectionFailed(error.message());
                    return;
                }

                boost::asio::async_connect(
                    socket,
                    results,
                    [this](const boost::system::error_code& connect_error,
                           const boost::asio::ip::tcp::endpoint&) {
                        if (connect_error) {
                            state = State::Offline;
                            handler.OnConnectionFailed(connect_error.message());
                            return;
                        }

                        state = State::WaitingHandshake;
                        StartReadHeader();
                        SendHandshake();
                    });
            });
    }

    void Disconnect(bool notify = true)
    {
        const bool was_online = state != State::Offline;
        state = State::Offline;

        boost::system::error_code ignored;
        resolver.cancel();
        if (socket.is_open()) {
            socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
            socket.close(ignored);
        }
        send_queue.clear();
        writing = false;

        if (notify && was_online) {
            handler.OnDisconnected();
        }
    }

    bool IsConnected() const
    {
        return state != State::Offline && socket.is_open();
    }

    bool IsAuthenticated() const
    {
        return state == State::Authenticated;
    }

    void SendHandshake(std::uint32_t protocol_version = 1,
                       const std::string& client_build = "vulkan-client-0.1")
    {
        if (state != State::WaitingHandshake) {
            return;
        }

        capnp::MallocMessageBuilder msg;
        auto packet = msg.initRoot<gs::protocol::Packet>();
        auto request = packet.initHandshakeRequest();
        request.setProtocolVersion(protocol_version);
        request.setClientBuild(client_build);
        EnqueueFrame(SerializeToFrame(msg));
    }

    void SendLogin(const std::string& username, const std::string& password)
    {
        if (state != State::HandshakeAccepted) {
            return;
        }

        state = State::Authenticating;
        capnp::MallocMessageBuilder msg;
        auto packet = msg.initRoot<gs::protocol::Packet>();
        auto request = packet.initLoginRequest();
        request.setUsername(username);
        request.setPassword(password);
        EnqueueFrame(SerializeToFrame(msg));
    }

    void SendCharacterListRequest()
    {
        if (state != State::Authenticated) {
            return;
        }

        capnp::MallocMessageBuilder msg;
        auto packet = msg.initRoot<gs::protocol::Packet>();
        packet.initCharacterListRequest();
        EnqueueFrame(SerializeToFrame(msg));
    }

    void Update()
    {
        if (state == State::Offline) {
            return;
        }
        io.poll();
    }

    void EnqueueFrame(std::vector<std::uint8_t> frame)
    {
        const bool already_writing = writing;
        send_queue.push_back(std::move(frame));
        if (!already_writing) {
            StartWrite();
        }
    }

    void StartWrite()
    {
        if (send_queue.empty() || state == State::Offline) {
            writing = false;
            return;
        }

        writing = true;
        boost::asio::async_write(
            socket,
            boost::asio::buffer(send_queue.front()),
            [this](const boost::system::error_code& error, std::size_t) {
                if (error) {
                    Disconnect();
                    return;
                }

                send_queue.pop_front();
                StartWrite();
            });
    }

    void StartReadHeader()
    {
        boost::asio::async_read(
            socket,
            boost::asio::buffer(read_header),
            [this](const boost::system::error_code& error, std::size_t) {
                if (error) {
                    Disconnect();
                    return;
                }

                auto length = ParseLengthPrefix(read_header.data());
                if (!length) {
                    Disconnect();
                    return;
                }

                read_payload.assign(*length, 0);
                StartReadPayload();
            });
    }

    void StartReadPayload()
    {
        boost::asio::async_read(
            socket,
            boost::asio::buffer(read_payload),
            [this](const boost::system::error_code& error, std::size_t) {
                if (error) {
                    Disconnect();
                    return;
                }

                HandlePayload(read_payload);
                if (state != State::Offline) {
                    StartReadHeader();
                }
            });
    }

    void HandlePayload(const std::vector<std::uint8_t>& payload)
    {
        try {
            auto parsed = gs::protocol::ParsePacket(payload);
            if (!parsed) {
                Disconnect();
                return;
            }

            auto packet = parsed->packet;
            if (packet.isHandshakeResponse()) {
                HandleHandshakeResponse(packet.getHandshakeResponse());
            } else if (packet.isLoginResponse()) {
                HandleLoginResponse(packet.getLoginResponse());
            } else if (packet.isCharacterListResponse()) {
                HandleCharacterListResponse(packet.getCharacterListResponse());
            } else {
                LogNet("[NET] unexpected packet type");
            }
        } catch (const kj::Exception& exception) {
            LogNetFormat("[NET] invalid packet: %s", exception.getDescription().cStr());
            Disconnect();
        }
    }

    void HandleHandshakeResponse(gs::protocol::HandshakeResponse::Reader response)
    {
        if (response.getResult() == gs::protocol::HandshakeResult::OK) {
            state = State::HandshakeAccepted;
            handler.OnHandshakeAccepted();
            return;
        }

        const std::string message = response.getMessage().cStr();
        handler.OnHandshakeRejected(message.empty() ? "Handshake rejected" : message);
        Disconnect(false);
    }

    void HandleLoginResponse(gs::protocol::LoginResponse::Reader response)
    {
        if (response.getResult() == gs::protocol::LoginResult::OK) {
            state = State::Authenticated;
            handler.OnLoginAccepted(response.getAccountId());
            return;
        }

        state = State::HandshakeAccepted;
        const std::string message = response.getMessage().cStr();
        handler.OnLoginRejected(message.empty() ? "Login failed" : message);
    }

    void HandleCharacterListResponse(gs::protocol::CharacterListResponse::Reader response)
    {
        if (response.getResult() != gs::protocol::CharacterListResult::OK) {
            handler.OnCharacterList({});
            return;
        }

        std::vector<CharacterListItem> characters;
        auto list = response.getCharacters();
        characters.reserve(list.size());
        for (auto item : list) {
            CharacterListItem character;
            character.id = item.getId();
            character.slot = item.getSlot();
            character.name = item.getName().cStr();
            character.level = item.getLevel();
            character.classId = item.getClassId();
            character.appearance = item.getAppearance();
            character.posX = item.getPosX();
            character.posY = item.getPosY();
            character.mapId = item.getMapId();
            characters.push_back(std::move(character));
        }
        handler.OnCharacterList(characters);
    }

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket;
    boost::asio::ip::tcp::resolver resolver;
    IClientHandler& handler;
    State state = State::Offline;
    std::array<std::uint8_t, kHeaderSize> read_header{};
    std::vector<std::uint8_t> read_payload;
    std::deque<std::vector<std::uint8_t>> send_queue;
    bool writing = false;
};

ClientSession::ClientSession(IClientHandler& handler)
    : m_impl(std::make_unique<Impl>(handler))
{
}

ClientSession::~ClientSession() = default;

void ClientSession::Connect(const std::string& host, std::uint16_t port)
{
    m_impl->Connect(host, port);
}

void ClientSession::Disconnect()
{
    m_impl->Disconnect();
}

bool ClientSession::IsConnected() const
{
    return m_impl->IsConnected();
}

bool ClientSession::IsAuthenticated() const
{
    return m_impl->IsAuthenticated();
}

void ClientSession::SendHandshake(std::uint32_t protocol_version, const std::string& client_build)
{
    m_impl->SendHandshake(protocol_version, client_build);
}

void ClientSession::SendLogin(const std::string& username, const std::string& password)
{
    m_impl->SendLogin(username, password);
}

void ClientSession::SendCharacterListRequest()
{
    m_impl->SendCharacterListRequest();
}

void ClientSession::Update()
{
    m_impl->Update();
}

} // namespace client::net
