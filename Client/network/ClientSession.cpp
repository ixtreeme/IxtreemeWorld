#include "ClientSession.h"

#include <boost/asio.hpp>
#include <capnp/message.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <protocol/Serialization.h>
#include <kj/exception.h>
#include "schema/packet.capnp.h"

namespace client::net {
namespace {

constexpr std::size_t kHeaderSize = 4;
constexpr std::size_t kMaxPayloadSize = 64 * 1024;
constexpr float kTwoPi = 6.28318530717958647692f;

void LogNet(const char* message)
{
#if defined(_WIN32)
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
#endif
    std::fprintf(stderr, "%s\n", message);
}

void LogNetFormat(const char* format, const std::string& value)
{
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), format, value.c_str());
    LogNet(buffer);
}

void LogNetFormat(const char* format, const std::string& first, const std::string& second)
{
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), format, first.c_str(), second.c_str());
    LogNet(buffer);
}

void LogNetFormat(const char* format,
                  std::uint32_t first,
                  std::uint32_t second,
                  std::uint32_t third,
                  std::uint32_t fourth,
                  std::uint32_t fifth)
{
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), format, first, second, third, fourth, fifth);
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

void WriteU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void WriteU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

std::uint16_t ReadU16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t ReadU32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

float ReadF32(const std::uint8_t* bytes)
{
    std::uint32_t bits = ReadU32(bytes);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint16_t QuantizeHeading(float angle)
{
    while (angle < 0.0f) {
        angle += kTwoPi;
    }
    while (angle >= kTwoPi) {
        angle -= kTwoPi;
    }
    return static_cast<std::uint16_t>(std::lround((angle / kTwoPi) * 65535.0f));
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
        EnteringWorld,
        InWorld,
    };

    struct RxDiagnostics {
        std::uint32_t entity_spawns = 0;
        std::uint32_t entity_despawns = 0;
        std::uint32_t transform_packets = 0;
        std::uint32_t transform_records = 0;
        std::uint32_t packet_errors = 0;
        std::chrono::steady_clock::time_point next_log =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
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
        const std::string port_string = std::to_string(port);
        LogNetFormat("[NET] connect begin %s:%s", host, port_string);

        resolver.async_resolve(
            host,
            port_string,
            [this, host, port_string](const boost::system::error_code& error,
                   boost::asio::ip::tcp::resolver::results_type results) {
                if (error) {
                    LogNetFormat("[NET] resolve failed: %s", error.message());
                    state = State::Offline;
                    handler.OnConnectionFailed(error.message());
                    return;
                }

                boost::asio::async_connect(
                    socket,
                    results,
                    [this, host, port_string](const boost::system::error_code& connect_error,
                           const boost::asio::ip::tcp::endpoint& endpoint) {
                        if (connect_error) {
                            LogNetFormat("[NET] connect failed: %s", connect_error.message());
                            state = State::Offline;
                            handler.OnConnectionFailed(connect_error.message());
                            return;
                        }

                        LogNetFormat("[NET] connect OK %s:%s",
                            endpoint.address().to_string(),
                            std::to_string(endpoint.port()));
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
        return state == State::Authenticated || state == State::InWorld;
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

    void SendCharacterSelect(std::uint64_t character_id)
    {
        if (state != State::Authenticated) {
            return;
        }

        capnp::MallocMessageBuilder msg;
        auto packet = msg.initRoot<gs::protocol::Packet>();
        auto request = packet.initCharacterSelect();
        request.setCharacterId(character_id);
        EnqueueFrame(SerializeToFrame(msg));
    }

    void SendEnterWorld(const std::vector<std::uint8_t>& token)
    {
        if (state != State::HandshakeAccepted) {
            return;
        }

        state = State::EnteringWorld;
        capnp::MallocMessageBuilder msg;
        auto packet = msg.initRoot<gs::protocol::Packet>();
        auto request = packet.initEnterWorld();
        request.setToken(kj::ArrayPtr<const kj::byte>(
            reinterpret_cast<const kj::byte*>(token.data()), token.size()));
        if (debug_spawn_override) {
            auto spawn = request.initDebugSpawnOverride();
            spawn.setX(debug_spawn_override->x);
            spawn.setY(debug_spawn_override->y);
            LogNet("[NET] sending debug spawn override");
        }
        EnqueueFrame(SerializeToFrame(msg));
    }

    void SetDebugSpawnOverride(std::optional<DebugSpawnOverride> spawn)
    {
        debug_spawn_override = spawn;
        if (debug_spawn_override) {
            char buffer[160];
            std::snprintf(buffer,
                          sizeof(buffer),
                          "[NET] debug spawn override configured: %.2f, %.2f",
                          debug_spawn_override->x,
                          debug_spawn_override->y);
            LogNet(buffer);
        } else {
            LogNet("[NET] debug spawn override disabled");
        }
    }

    void SendMoveInput(float dir_angle, MoveState move_state)
    {
        if (state != State::InWorld) {
            return;
        }

        std::vector<std::uint8_t> payload;
        payload.reserve(9);
        payload.push_back(gs::protocol::kCodecBinary);
        payload.push_back(0x01);
        WriteU32(payload, ++move_sequence);
        WriteU16(payload, QuantizeHeading(dir_angle));
        payload.push_back(static_cast<std::uint8_t>(move_state));

        std::vector<std::uint8_t> frame;
        frame.reserve(kHeaderSize + payload.size());
        const auto length = static_cast<std::uint32_t>(payload.size());
        frame.push_back(static_cast<std::uint8_t>((length >> 24) & 0xff));
        frame.push_back(static_cast<std::uint8_t>((length >> 16) & 0xff));
        frame.push_back(static_cast<std::uint8_t>((length >> 8) & 0xff));
        frame.push_back(static_cast<std::uint8_t>(length & 0xff));
        frame.insert(frame.end(), payload.begin(), payload.end());
        EnqueueFrame(std::move(frame));
    }

    void Update()
    {
        if (state == State::Offline) {
            return;
        }
        io.poll();
        MaybeLogRxDiagnostics();
    }

    void MaybeLogRxDiagnostics()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < rx_diagnostics.next_log) {
            return;
        }

        LogNetFormat("[NET] rx diag: spawns=%u despawns=%u transform_packets=%u transform_records=%u errors=%u",
            rx_diagnostics.entity_spawns,
            rx_diagnostics.entity_despawns,
            rx_diagnostics.transform_packets,
            rx_diagnostics.transform_records,
            rx_diagnostics.packet_errors);
        rx_diagnostics.entity_spawns = 0;
        rx_diagnostics.entity_despawns = 0;
        rx_diagnostics.transform_packets = 0;
        rx_diagnostics.transform_records = 0;
        rx_diagnostics.packet_errors = 0;
        do {
            rx_diagnostics.next_log += std::chrono::seconds(1);
        } while (now >= rx_diagnostics.next_log);
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

                try {
                    HandlePayload(read_payload);
                } catch (const std::exception& exception) {
                    ++rx_diagnostics.packet_errors;
                    LogNetFormat("[NET] packet handler exception: %s", std::string(exception.what()));
                } catch (...) {
                    ++rx_diagnostics.packet_errors;
                    LogNet("[NET] packet handler exception: unknown");
                }
                if (state != State::Offline) {
                    StartReadHeader();
                }
            });
    }

    void HandlePayload(const std::vector<std::uint8_t>& payload)
    {
        if (!payload.empty() && payload[0] == gs::protocol::kCodecBinary) {
            HandleBinaryPayload(payload);
            return;
        }

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
            } else if (packet.isEnterWorldToken()) {
                HandleEnterWorldToken(packet.getEnterWorldToken());
            } else if (packet.isEnterWorldAccept()) {
                HandleEnterWorldAccept(packet.getEnterWorldAccept());
            } else if (packet.isEnterWorldReject()) {
                HandleEnterWorldReject(packet.getEnterWorldReject());
            } else if (packet.isEntitySpawn()) {
                ++rx_diagnostics.entity_spawns;
                HandleEntitySpawn(packet.getEntitySpawn());
            } else if (packet.isEntityDespawn()) {
                ++rx_diagnostics.entity_despawns;
                handler.OnEntityDespawn(packet.getEntityDespawn().getNetId());
            } else {
                LogNet("[NET] unexpected packet type");
            }
        } catch (const kj::Exception& exception) {
            ++rx_diagnostics.packet_errors;
            LogNetFormat("[NET] invalid packet: %s", exception.getDescription().cStr());
            Disconnect();
        }
    }

    void HandleBinaryPayload(const std::vector<std::uint8_t>& payload)
    {
        if (payload.size() < 2 || payload[1] != 0x10 || payload.size() < 8) {
            ++rx_diagnostics.packet_errors;
            LogNet("[NET] invalid binary payload");
            return;
        }

        const auto server_tick = ReadU32(payload.data() + 2);
        const auto count = ReadU16(payload.data() + 6);
        constexpr std::size_t header_size = 8;
        constexpr std::size_t record_size = 19;
        if (payload.size() != header_size + static_cast<std::size_t>(count) * record_size) {
            ++rx_diagnostics.packet_errors;
            LogNet("[NET] invalid transform payload size");
            return;
        }

        std::vector<EntityTransform> transforms;
        transforms.reserve(count);
        std::size_t offset = header_size;
        for (std::uint16_t i = 0; i < count; ++i) {
            EntityTransform transform;
            transform.netId = ReadU32(payload.data() + offset);
            offset += 4;
            transform.position.x = ReadF32(payload.data() + offset);
            offset += 4;
            transform.position.y = ReadF32(payload.data() + offset);
            offset += 4;
            transform.position.z = ReadF32(payload.data() + offset);
            offset += 4;
            transform.heading = ReadU16(payload.data() + offset);
            offset += 2;
            transform.moveState = static_cast<MoveState>(payload[offset++]);
            transforms.push_back(transform);
        }

        handler.OnEntityTransforms(server_tick, transforms);
        ++rx_diagnostics.transform_packets;
        rx_diagnostics.transform_records += count;
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

    void HandleEnterWorldToken(gs::protocol::S2cEnterWorldToken::Reader response)
    {
        std::vector<std::uint8_t> token;
        auto data = response.getToken();
        token.assign(data.begin(), data.end());
        handler.OnEnterWorldToken(std::move(token), response.getGameHost().cStr(), response.getGamePort());
    }

    void HandleEnterWorldAccept(gs::protocol::S2cEnterWorldAccept::Reader response)
    {
        state = State::InWorld;
        auto pos = response.getSpawnPos();
        handler.OnEnterWorldAccepted(response.getYourNetId(), Vec3{pos.getX(), pos.getY(), pos.getZ()});
    }

    void HandleEnterWorldReject(gs::protocol::S2cEnterWorldReject::Reader response)
    {
        state = State::HandshakeAccepted;
        switch (response.getReason()) {
        case gs::protocol::S2cEnterWorldReject::RejectReason::INVALID_TOKEN:
            handler.OnEnterWorldRejected("Invalid token");
            break;
        case gs::protocol::S2cEnterWorldReject::RejectReason::EXPIRED_TOKEN:
            handler.OnEnterWorldRejected("Expired token");
            break;
        case gs::protocol::S2cEnterWorldReject::RejectReason::ALREADY_USED:
            handler.OnEnterWorldRejected("Token already used");
            break;
        default:
            handler.OnEnterWorldRejected("Enter world failed");
            break;
        }
    }

    void HandleEntitySpawn(gs::protocol::S2cEntitySpawn::Reader response)
    {
        auto pos = response.getSpawnPos();
        EntitySpawnInfo spawn;
        spawn.netId = response.getNetId();
        spawn.name = response.getName().cStr();
        spawn.classId = response.getClassId();
        spawn.spawnPos = Vec3{pos.getX(), pos.getY(), pos.getZ()};
        spawn.heading = response.getHeading();
        handler.OnEntitySpawn(spawn);
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
    std::uint32_t move_sequence = 0;
    RxDiagnostics rx_diagnostics;
    std::optional<DebugSpawnOverride> debug_spawn_override;
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

void ClientSession::SendCharacterSelect(std::uint64_t character_id)
{
    m_impl->SendCharacterSelect(character_id);
}

void ClientSession::SendEnterWorld(const std::vector<std::uint8_t>& token)
{
    m_impl->SendEnterWorld(token);
}

void ClientSession::SetDebugSpawnOverride(std::optional<DebugSpawnOverride> spawn)
{
    m_impl->SetDebugSpawnOverride(spawn);
}

void ClientSession::SendMoveInput(float dir_angle, MoveState state)
{
    m_impl->SendMoveInput(dir_angle, state);
}

void ClientSession::Update()
{
    m_impl->Update();
}

} // namespace client::net
