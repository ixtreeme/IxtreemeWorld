#include "ScriptWireBench.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "db/CharacterRepository.h"
#include "db/DbPool.h"
#include "db/HandoffTokenRepository.h"
#include "network/Server.h"
#include "network/Session.h"
#include "protocol/Protocol.h"
#include "protocol/Serialization.h"

#include "../GameConnectionHandler.h"
#include "../world/WorldRuntime.h"
#include "../world/replication/ProtocolEncoder.h"

#include "platform/tcp_stream.h"  // the engine's script transport (Client/libs/platform)

// The scene script itself, codec part only (no engine SDK needed).
#define MMO_CLIENT_CODEC_ONLY 1
#include IXTREEME_MMO_CLIENT_SCRIPT

namespace gs::bench {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::string Fmt(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

struct Check {
    int failures = 0;
    int passes = 0;
    void operator()(const char* name, bool ok, const std::string& detail = {})
    {
        std::printf("SCRIPTWIRE %-58s %s%s%s\n", name, ok ? "PASS" : "FAIL", detail.empty() ? "" : "  ",
                    detail.c_str());
        ok ? ++passes : ++failures;
    }
};

std::optional<gs::protocol::ParsedPacket> ParseUntrusted(const std::vector<std::uint8_t>& payload)
{
    try {
        return gs::protocol::ParseUntrustedPacket(payload);
    } catch (const kj::Exception&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> Serialize(capnp::MessageBuilder& builder)
{
    return gs::protocol::SerializeToBytes(builder);
}

std::size_t SegmentCount(const std::vector<std::uint8_t>& payload)
{
    return payload.size() >= 5 ? static_cast<std::size_t>(mmowire::LoadU32(payload.data() + 1)) + 1 : 0;
}

std::string TokenString(std::size_t n, std::uint8_t seed)
{
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(static_cast<char>(seed + i * 13));
    }
    return s;
}

// ---- server packets built with REAL Cap'n Proto (optionally forced multi-segment) ----

template <typename Fill>
std::vector<std::uint8_t> Build(Fill fill, unsigned firstSegmentWords = 0)
{
    if (firstSegmentWords == 0) {
        capnp::MallocMessageBuilder msg;
        fill(msg.initRoot<gs::protocol::Packet>());
        return Serialize(msg);
    }
    // FIXED_SIZE tiny segments force every object into its own segment: far
    // and double-far pointers on (almost) every edge.
    capnp::MallocMessageBuilder msg(firstSegmentWords, capnp::AllocationStrategy::FIXED_SIZE);
    fill(msg.initRoot<gs::protocol::Packet>());
    return Serialize(msg);
}

void FillCharacterList(gs::protocol::Packet::Builder packet)
{
    auto response = packet.initCharacterListResponse();
    response.setResult(gs::protocol::CharacterListResult::OK);
    response.setMessage("three heroes");
    auto list = response.initCharacters(3);
    for (unsigned i = 0; i < 3; ++i) {
        auto c = list[i];
        c.setId(0x0123456789ABCDEFull + i);
        c.setSlot(static_cast<std::uint8_t>(i + 1));
        c.setName(i == 1 ? "Hős Árvíztűrő" : (i == 0 ? "Alpha" : "A rather long character name here"));
        c.setLevel(10u * i + 7u);
        c.setClassId(static_cast<std::uint16_t>(300 + i));
        c.setAppearance(static_cast<std::uint16_t>(0xBEE0 + i));
        c.setPosX(-1234 * static_cast<std::int32_t>(i + 1));
        c.setPosY(5678 + static_cast<std::int32_t>(i));
        c.setMapId(static_cast<std::uint16_t>(40 + i));
    }
}

bool CharacterListMatches(const mmowire::ServerPacket& p)
{
    if (p.tag != mmowire::Tag::CharacterListResponse || p.result != 0 || p.message != "three heroes" ||
        p.characters.size() != 3) {
        return false;
    }
    for (unsigned i = 0; i < 3; ++i) {
        const auto& c = p.characters[i];
        const char* name = i == 1 ? "Hős Árvíztűrő" : (i == 0 ? "Alpha" : "A rather long character name here");
        if (c.id != 0x0123456789ABCDEFull + i || c.slot != i + 1 || c.name != name || c.level != 10u * i + 7u ||
            c.classId != 300 + i || c.appearance != 0xBEE0 + i || c.posX != -1234 * static_cast<std::int32_t>(i + 1) ||
            c.posY != 5678 + static_cast<std::int32_t>(i) || c.mapId != 40 + i) {
            return false;
        }
    }
    return true;
}

// ---- loopback client (blocking with timeouts on a private io_context) ----

class WireClient {
public:
    bool Connect(std::uint16_t port)
    {
        boost::system::error_code ec;
        socket_.connect(tcp::endpoint(asio::ip::address_v4::loopback(), port), ec);
        return !ec;
    }
    bool SendRaw(const std::vector<std::uint8_t>& bytes)
    {
        boost::system::error_code ec;
        asio::write(socket_, asio::buffer(bytes), ec);
        return !ec;
    }
    // Feeds the script FrameReader one byte at a time until it yields a payload.
    bool ReadPayload(mmowire::FrameReader& reader, std::vector<std::uint8_t>& payload, std::chrono::milliseconds timeout)
    {
        const auto deadline = Clock::now() + timeout;
        for (;;) {
            if (reader.Next(payload) == 1) {
                return true;
            }
            if (Clock::now() >= deadline) {
                return false;
            }
            std::uint8_t byte = 0;
            if (!ReadByte(byte, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()) + 1ms)) {
                return false;
            }
            reader.Feed(&byte, 1);
        }
    }
    // True if the server closed the connection within the timeout.
    bool Closed(std::chrono::milliseconds timeout)
    {
        std::uint8_t byte = 0;
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            if (!ReadByte(byte, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()) + 1ms)) {
                return closed_;
            }
        }
        return false;
    }
    void Close()
    {
        boost::system::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

private:
    bool ReadByte(std::uint8_t& out, std::chrono::milliseconds timeout)
    {
        boost::system::error_code result = asio::error::would_block;
        asio::async_read(socket_, asio::buffer(&out, 1),
                         [&result](const boost::system::error_code& ec, std::size_t) { result = ec; });
        ctx_.restart();
        ctx_.run_for(timeout);
        if (result == asio::error::would_block) {
            boost::system::error_code ignored;
            socket_.cancel(ignored);
            ctx_.restart();
            ctx_.run();
            return false;
        }
        closed_ = static_cast<bool>(result);
        return !result;
    }
    asio::io_context ctx_;
    tcp::socket socket_{ctx_};
    bool closed_ = false;
};

class IoThreads {
public:
    explicit IoThreads(int n)
    {
        for (int i = 0; i < n; ++i) {
            threads_.emplace_back([this] { io.run(); });
        }
    }
    ~IoThreads()
    {
        work_.reset();
        io.stop();
        for (auto& t : threads_) {
            t.join();
        }
    }
    asio::io_context io;

private:
    asio::executor_work_guard<asio::io_context::executor_type> work_{asio::make_work_guard(io)};
    std::vector<std::thread> threads_;
};

void ClientEncoderChecks(Check& check)
{
    // Handshake
    {
        const auto payload = mmowire::EncodeHandshakeRequest(2, "ixtreeme-scene-script");
        auto parsed = ParseUntrusted(payload);
        bool ok = parsed && parsed->packet.isHandshakeRequest();
        if (ok) {
            const auto r = parsed->packet.getHandshakeRequest();
            ok = r.getProtocolVersion() == 2 && std::string(r.getClientBuild().cStr()) == "ixtreeme-scene-script";
        }
        capnp::MallocMessageBuilder msg;
        auto request = msg.initRoot<gs::protocol::Packet>().initHandshakeRequest();
        request.setProtocolVersion(2);
        request.setClientBuild("ixtreeme-scene-script");
        const bool identical = Serialize(msg) == payload;
        check("client-handshake-parses-in-real-capnp", ok, Fmt("bytes=%zu canonical_identical=%d", payload.size(), identical ? 1 : 0));
    }
    // Login (incl. empty, non-ASCII and long credentials)
    {
        bool ok = true;
        std::string detail;
        const std::vector<std::pair<std::string, std::string>> cases = {
            {"user", "secret"}, {"", ""}, {"felhasználó", "jelszó ÁÉŐŰ"}, {std::string(200, 'u'), TokenString(255, 33)},
            {"x", std::string(1, '\0') + "after-nul"}};
        for (const auto& [user, pass] : cases) {
            auto payload = mmowire::EncodeLoginRequest(user, pass);
            auto parsed = ParseUntrusted(payload);
            bool one = parsed && parsed->packet.isLoginRequest();
            if (one) {
                const auto r = parsed->packet.getLoginRequest();
                const auto u = r.getUsername();
                const auto p = r.getPassword();
                one = std::string(u.begin(), u.end()) == user && std::string(p.begin(), p.end()) == pass;
            }
            ok = ok && one;
            detail += Fmt(" %zu/%zu:%d", user.size(), pass.size(), one ? 1 : 0);
            mmowire::Wipe(payload);
        }
        check("client-login-request-parses-in-real-capnp", ok, detail);
    }
    {
        const auto payload = mmowire::EncodeCharacterListRequest();
        auto parsed = ParseUntrusted(payload);
        check("client-character-list-request-parses", parsed && parsed->packet.isCharacterListRequest(),
              Fmt("bytes=%zu", payload.size()));
    }
    {
        bool ok = true;
        for (const std::uint64_t id : {0ull, 1ull, 0x0123456789ABCDEFull, ~0ull}) {
            auto parsed = ParseUntrusted(mmowire::EncodeCharacterSelect(id));
            ok = ok && parsed && parsed->packet.isCharacterSelect() &&
                 parsed->packet.getCharacterSelect().getCharacterId() == id;
        }
        check("client-character-select-parses", ok);
    }
    {
        bool ok = true;
        std::string detail;
        for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{32}, std::size_t{200}}) {
            std::vector<std::uint8_t> token;
            for (std::size_t i = 0; i < n; ++i) {
                token.push_back(static_cast<std::uint8_t>(i * 7 + 3));
            }
            auto parsed = ParseUntrusted(mmowire::EncodeEnterWorld(token));
            bool one = parsed && parsed->packet.isEnterWorld();
            if (one) {
                const auto r = parsed->packet.getEnterWorld();
                const auto t = r.getToken();
                one = std::vector<std::uint8_t>(t.begin(), t.end()) == token && !r.hasDebugSpawnOverride();
            }
            ok = ok && one;
            detail += Fmt(" %zu:%d", n, one ? 1 : 0);
        }
        check("client-enter-world-parses-token-exact", ok, detail);
    }
    {
        // The server's binary move parser: size 9, [1][0x01][u32 seq LE][u16 heading LE][u8 state].
        const auto move = mmowire::EncodeMove(0xA1B2C3D4u, 0xBEEF, 2);
        const bool ok = move.size() == 9 && move[0] == 1 && move[1] == 0x01 && mmowire::LoadU32(move.data() + 2) == 0xA1B2C3D4u &&
                        mmowire::LoadU16(move.data() + 6) == 0xBEEF && move[8] == 2;
        check("client-move-packet-layout", ok);
    }
    {
        // Heading quantization: the script vs the server (QuantizeHeading) over a sweep.
        int exact = 0;
        int within1 = 0;
        int total = 0;
        for (int i = -4000; i <= 4000; ++i) {
            const float angle = static_cast<float>(i) * 0.0031f;
            const int a = mmowire::QuantizeHeading(angle);
            const int b = gs::game::QuantizeHeading(angle);
            const int d = std::min(std::abs(a - b), 65535 - std::abs(a - b));
            exact += a == b ? 1 : 0;
            within1 += d <= 1 ? 1 : 0;
            ++total;
        }
        check("heading-quantization-matches-server", within1 == total, Fmt("exact=%d within1=%d total=%d", exact, within1, total));
    }
}

void ServerDecoderChecks(Check& check)
{
    std::string error;
    {
        const auto payload = Build([](gs::protocol::Packet::Builder p) {
            auto r = p.initHandshakeResponse();
            r.setResult(gs::protocol::HandshakeResult::OK);
            r.setServerProtocolVersion(2);
            r.setMessage("Welcome to world");
        });
        mmowire::ServerPacket out;
        const bool ok = mmowire::DecodeServerPacket(payload.data(), payload.size(), out, error) &&
                        out.tag == mmowire::Tag::HandshakeResponse && out.result == 0 && out.serverProtocolVersion == 2 &&
                        out.message == "Welcome to world";
        check("server-handshake-response-decodes", ok, error);
    }
    {
        const auto payload = Build([](gs::protocol::Packet::Builder p) {
            auto r = p.initLoginResponse();
            r.setResult(gs::protocol::LoginResult::ALREADY_LOGGED_IN);
            r.setMessage("busy");
            r.setAccountId(0xFEDCBA9876543210ull);
        });
        mmowire::ServerPacket out;
        const bool ok = mmowire::DecodeServerPacket(payload.data(), payload.size(), out, error) &&
                        out.tag == mmowire::Tag::LoginResponse && out.result == 3 && out.message == "busy" &&
                        out.accountId == 0xFEDCBA9876543210ull;
        check("server-login-response-decodes", ok, error);
    }
    {
        const auto payload = Build(FillCharacterList);
        mmowire::ServerPacket out;
        const bool ok = mmowire::DecodeServerPacket(payload.data(), payload.size(), out, error) && CharacterListMatches(out);
        check("server-character-list-decodes-all-fields", ok, error);
    }
    {
        // Multi-segment messages: every edge becomes a far / double-far pointer.
        bool ok = true;
        std::size_t maxSegments = 0;
        std::string detail;
        for (unsigned words = 1; words <= 16; ++words) {
            const auto payload = Build(FillCharacterList, words);
            maxSegments = std::max(maxSegments, SegmentCount(payload));
            mmowire::ServerPacket out;
            const bool one = mmowire::DecodeServerPacket(payload.data(), payload.size(), out, error) && CharacterListMatches(out);
            ok = ok && one;
            if (!one) {
                detail += Fmt(" words=%u:%s", words, error.c_str());
            }
            // The real reader agrees on the same bytes.
            auto parsed = ParseUntrusted(payload);
            ok = ok && parsed && parsed->packet.isCharacterListResponse() &&
                 parsed->packet.getCharacterListResponse().getCharacters().size() == 3;
        }
        check("server-multisegment-far-pointers-decode", ok && maxSegments > 3,
              Fmt("max_segments=%zu%s", maxSegments, detail.c_str()));
    }
    {
        const std::string token = TokenString(32, 5);
        const auto payload = Build([&](gs::protocol::Packet::Builder p) {
            auto r = p.initEnterWorldToken();
            r.setToken(kj::ArrayPtr<const kj::byte>(reinterpret_cast<const kj::byte*>(token.data()), token.size()));
            r.setGameHost("127.0.0.1");
            r.setGamePort(11020);
        });
        mmowire::ServerPacket out;
        const bool ok = mmowire::DecodeServerPacket(payload.data(), payload.size(), out, error) &&
                        out.tag == mmowire::Tag::EnterWorldToken &&
                        std::string(out.token.begin(), out.token.end()) == token && out.gameHost == "127.0.0.1" &&
                        out.gamePort == 11020;
        check("server-enter-world-token-decodes", ok, error);
    }
    {
        const auto payload = gs::game::MakeEnterWorldAccept(77, gs::game::Position{12.5f, -3.25f, 4.0f}, 9001, 3, 2);
        mmowire::ServerPacket out;
        const bool ok = mmowire::DecodeServerPacket(payload.data(), payload.size(), out, error) &&
                        out.tag == mmowire::Tag::EnterWorldAccept && out.netId == 77 && out.position[0] == 12.5f &&
                        out.position[1] == -3.25f && out.position[2] == 4.0f && out.serverTick == 9001 &&
                        out.volumeId == 3 && out.layerId == 2;
        check("server-enter-world-accept-decodes-layered-spawn", ok, error);
    }
    {
        const auto payload = gs::game::MakeEnterWorldRejectAlreadyInWorld();
        mmowire::ServerPacket out;
        const bool ok = mmowire::DecodeServerPacket(payload.data(), payload.size(), out, error) &&
                        out.tag == mmowire::Tag::EnterWorldReject && out.result == 4;
        check("server-enter-world-reject-decodes", ok, error);
    }
    {
        gs::game::BorderEntitySnapshot s;
        s.net_id = 4242;
        s.position = gs::game::Position{-7.0f, 100.25f, 2.5f};
        s.heading.angle = 1.25f;
        s.name = "Mob Ősz";
        s.class_id = 12;
        s.mob_type_id = 0xABCDEF;
        s.level = 33;
        s.hp_current = 17.5f;
        s.hp_max = 40.0f;
        s.volume_id = 9;
        s.layer_id = 4;
        const auto payload = gs::game::MakeSpawn(s);
        mmowire::ServerPacket out;
        const bool ok = mmowire::DecodeServerPacket(payload.data(), payload.size(), out, error) &&
                        out.tag == mmowire::Tag::EntitySpawn && out.netId == 4242 && out.name == "Mob Ősz" &&
                        out.classId == 12 && out.position[0] == -7.0f && out.position[1] == 100.25f &&
                        out.position[2] == 2.5f && out.heading == gs::game::QuantizeHeading(1.25f) &&
                        out.mobType == 0xABCDEF && out.level == 33 && out.hpCurrent == 17.5f && out.hpMax == 40.0f &&
                        out.volumeId == 9 && out.layerId == 4;
        check("server-entity-spawn-decodes-all-fields", ok, error);
    }
    {
        mmowire::ServerPacket a, b, c;
        const auto despawn = gs::game::MakeDespawn(31);
        const auto health = gs::game::MakeHealthUpdate(32, gs::game::Hp{3.5f, 9.0f});
        const auto death = gs::game::MakeDeath(33, 34);
        const bool ok = mmowire::DecodeServerPacket(despawn.data(), despawn.size(), a, error) && a.tag == mmowire::Tag::EntityDespawn &&
                        a.netId == 31 && mmowire::DecodeServerPacket(health.data(), health.size(), b, error) &&
                        b.tag == mmowire::Tag::EntityHealthUpdate && b.netId == 32 && b.hpCurrent == 3.5f && b.hpMax == 9.0f &&
                        mmowire::DecodeServerPacket(death.data(), death.size(), c, error) && c.tag == mmowire::Tag::EntityDeath &&
                        c.netId == 33 && c.killerNetId == 34;
        check("server-despawn-health-death-decode", ok, error);
    }
    {
        // Robustness: every truncation and 20000 random corruptions of real
        // packets (single- and multi-segment) must be rejected or decoded
        // without a fault; a truncated message is never accepted.
        std::vector<std::vector<std::uint8_t>> samples = {
            Build(FillCharacterList), Build(FillCharacterList, 2), Build(FillCharacterList, 5),
            gs::game::MakeEnterWorldAccept(1, gs::game::Position{1, 2, 3}, 4, 5, 6)};
        {
            gs::game::BorderEntitySnapshot s;
            s.net_id = 1;
            s.name = "x";
            samples.push_back(gs::game::MakeSpawn(s));
        }
        std::size_t truncationsAccepted = 0;
        std::size_t truncations = 0;
        for (const auto& sample : samples) {
            for (std::size_t n = 0; n < sample.size(); ++n) {
                mmowire::ServerPacket out;
                ++truncations;
                truncationsAccepted += mmowire::DecodeServerPacket(sample.data(), n, out, error) ? 1 : 0;
            }
        }
        std::mt19937 rng(0x5C2);
        std::size_t decoded = 0;
        std::size_t mutated = 0;
        for (int i = 0; i < 20000; ++i) {
            std::vector<std::uint8_t> m = samples[static_cast<std::size_t>(i) % samples.size()];
            const int flips = 1 + static_cast<int>(rng() % 4);
            for (int f = 0; f < flips; ++f) {
                const std::size_t at = 1 + rng() % (m.size() - 1);
                m[at] = static_cast<std::uint8_t>(rng());
            }
            mmowire::ServerPacket out;
            decoded += mmowire::DecodeServerPacket(m.data(), m.size(), out, error) ? 1 : 0;
            ++mutated;
        }
        check("server-decoder-rejects-truncation-survives-corruption", truncationsAccepted == 0,
              Fmt("truncations=%zu accepted=%zu mutations=%zu decoded_anyway=%zu", truncations, truncationsAccepted,
                  mutated, decoded));
    }
}

void FrameChecks(Check& check)
{
    using gs::game::MoveState;
    std::string error;
    const gs::game::Position viewerPos{10.0f, 20.0f, 3.5f};
    const auto viewer = gs::game::EncodeTransformRecord(5, viewerPos, gs::game::Heading{0.75f}, MoveState::Running);
    for (const bool layered : {true, false}) {
        std::vector<std::uint8_t> deltas;
        std::vector<mmowire::TransformRecord> expected;
        const std::uint8_t maxMask = layered ? 0x0F : 0x07;
        for (std::uint8_t mask = 0; mask <= maxMask; ++mask) {
            const std::uint32_t net = 100u + mask;
            const gs::game::Position pos{static_cast<float>(mask) * 1.5f, -static_cast<float>(mask), 0.25f * mask};
            const float heading = 0.1f * mask;
            const auto state = static_cast<MoveState>(mask % 3);
            gs::game::AppendTransformDelta(deltas, net, mask, pos, heading, state, 1000u + mask, 7u + mask);
            mmowire::TransformRecord rec;
            rec.netId = net;
            rec.mask = mask;
            if (mask & 0x01) {
                rec.position[0] = pos.x;
                rec.position[1] = pos.y;
                rec.position[2] = pos.z;
            }
            if (mask & 0x02) {
                rec.heading = gs::game::QuantizeHeading(heading);
            }
            if (mask & 0x04) {
                rec.moveState = static_cast<std::uint8_t>(state);
            }
            if (mask & 0x08) {
                rec.volumeId = 1000u + mask;
                rec.layerId = 7u + mask;
            }
            expected.push_back(rec);
        }
        const auto frame = layered
                               ? gs::game::EncodeTransformFrameV3(viewer, 3, 2, deltas, static_cast<std::uint32_t>(expected.size()), 4321)
                               : gs::game::EncodeTransformFrameV2(viewer, deltas, static_cast<std::uint32_t>(expected.size()), 4321);
        mmowire::TransformFrame out;
        bool ok = mmowire::DecodeTransformFrame(frame.data(), frame.size(), out, error) && out.tick == 4321 &&
                  out.opcode == (layered ? 0x12 : 0x11) && out.viewer.netId == 5 && out.viewer.position[0] == 10.0f &&
                  out.viewer.position[1] == 20.0f && out.viewer.position[2] == 3.5f &&
                  out.viewer.heading == gs::game::QuantizeHeading(0.75f) && out.viewer.moveState == 2 &&
                  (!layered || (out.viewer.volumeId == 3 && out.viewer.layerId == 2)) && out.records.size() == expected.size();
        for (std::size_t i = 0; ok && i < expected.size(); ++i) {
            const auto& a = out.records[i];
            const auto& b = expected[i];
            ok = a.netId == b.netId && a.mask == b.mask && a.position[0] == b.position[0] && a.position[1] == b.position[1] &&
                 a.position[2] == b.position[2] && a.heading == b.heading && a.moveState == b.moveState &&
                 a.volumeId == b.volumeId && a.layerId == b.layerId;
        }
        std::size_t truncAccepted = 0;
        for (std::size_t n = 0; n < frame.size(); ++n) {
            mmowire::TransformFrame t;
            truncAccepted += mmowire::DecodeTransformFrame(frame.data(), n, t, error) ? 1 : 0;
        }
        auto trailing = frame;
        trailing.push_back(0);
        mmowire::TransformFrame t;
        const bool trailingRejected = !mmowire::DecodeTransformFrame(trailing.data(), trailing.size(), t, error);
        check(layered ? "frame-v3-0x12-every-mask-decodes-exactly" : "frame-v2-0x11-every-mask-decodes-exactly",
              ok && truncAccepted == 0 && trailingRejected,
              Fmt("bytes=%zu records=%zu truncations_accepted=%zu trailing_rejected=%d", frame.size(), out.records.size(),
                  truncAccepted, trailingRejected ? 1 : 0));
    }
    {
        // v2 frames must not carry the layer bit; a v3 decoder rejects unknown bits.
        std::vector<std::uint8_t> deltas;
        gs::game::AppendTransformDelta(deltas, 1, 0x08, gs::game::Position{}, 0.0f, MoveState::Idle, 1, 1);
        const auto v2 = gs::game::EncodeTransformFrameV2(viewer, deltas, 1, 1);
        mmowire::TransformFrame out;
        const bool rejected = !mmowire::DecodeTransformFrame(v2.data(), v2.size(), out, error);
        check("frame-v2-with-layer-bit-rejected", rejected, error);
    }
    {
        std::vector<gs::game::TransformRecord> records;
        std::vector<std::uint32_t> slots;
        for (std::uint32_t i = 0; i < 4; ++i) {
            records.push_back(gs::game::EncodeTransformRecord(200 + i, gs::game::Position{1.0f * i, 2.0f * i, 3.0f * i},
                                                              gs::game::Heading{0.5f * i}, MoveState::Walking));
            slots.push_back(i);
        }
        const auto frame = gs::game::EncodeTransformFrameFromRecords(viewer, records, slots, 77);
        mmowire::TransformFrame out;
        bool ok = mmowire::DecodeTransformFrame(frame.data(), frame.size(), out, error) && out.opcode == 0x10 &&
                  out.records.size() == 4;
        for (std::uint32_t i = 0; ok && i < 4; ++i) {
            ok = out.records[i].netId == 200 + i && out.records[i].position[2] == 3.0f * i && out.records[i].moveState == 1;
        }
        check("frame-v1-0x10-decodes", ok, ok ? std::string() : error);
    }
    {
        // Framing: payloads split across arbitrary chunk boundaries.
        std::vector<std::uint8_t> stream;
        std::vector<std::vector<std::uint8_t>> payloads;
        for (int i = 0; i < 50; ++i) {
            payloads.push_back(gs::game::MakeDespawn(static_cast<std::uint32_t>(i)));
            mmowire::AppendFrame(stream, payloads.back());
        }
        mmowire::FrameReader reader;
        std::mt19937 rng(7);
        std::size_t at = 0;
        std::size_t got = 0;
        bool ok = true;
        std::vector<std::uint8_t> payload;
        while (at < stream.size()) {
            const std::size_t n = std::min<std::size_t>(1 + rng() % 13, stream.size() - at);
            reader.Feed(stream.data() + at, n);
            at += n;
            while (reader.Next(payload) == 1) {
                ok = ok && got < payloads.size() && payload == payloads[got];
                ++got;
            }
        }
        std::vector<std::uint8_t> bad{0, 1, 0, 1};  // 65537 > 64 KiB limit
        mmowire::FrameReader badReader;
        badReader.Feed(bad.data(), bad.size());
        check("framing-reassembles-and-rejects-oversize", ok && got == payloads.size() && badReader.Next(payload) < 0,
              Fmt("payloads=%zu", got));
    }
}

void LoopbackChecks(Check& check)
{
    IoThreads pool(2);
    gs::game::WorldRuntime sim(pool.io, {}, gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
    sim.Start();
    // DB pool never started: EnterWorld parks in EnteringWorld (token lookup pending).
    gs::db::DbPool db(pool.io, gs::db::DbConfig{});
    gs::db::CharacterRepository characters(db);
    gs::db::HandoffTokenRepository tokens(db);
    gs::game::GameConnectionHandler handler(tokens, characters, sim, "127.0.0.1:0");
    gs::network::Server server(
        pool.io, tcp::endpoint(asio::ip::address_v4::loopback(), 0),
        [&handler](auto session, auto payload) { handler.OnPayload(session, std::move(payload)); },
        [&handler](auto session) { handler.OnDisconnect(session); });
    server.Start();
    const std::uint16_t port = server.LocalPort();

    WireClient client;
    mmowire::FrameReader reader;
    std::vector<std::uint8_t> bytes;
    bool ok = client.Connect(port);
    mmowire::AppendFrame(bytes, mmowire::EncodeHandshakeRequest(mmowire::kProtocolVersion, "scriptwire"));
    ok = ok && client.SendRaw(bytes);
    std::vector<std::uint8_t> payload;
    mmowire::ServerPacket response;
    std::string error;
    ok = ok && client.ReadPayload(reader, payload, 3000ms) &&
         mmowire::DecodeServerPacket(payload.data(), payload.size(), response, error);
    const bool handshakeOk = ok && response.tag == mmowire::Tag::HandshakeResponse && response.result == 0 &&
                             response.serverProtocolVersion == 2;
    check("loopback-script-handshake-real-handler-negotiates-2", handshakeOk,
          Fmt("result=%u version=%u message='%s'", response.result, response.serverProtocolVersion, response.message.c_str()));

    const auto statsBefore = handler.Stats();
    bytes.clear();
    std::vector<std::uint8_t> token(32);
    for (std::size_t i = 0; i < token.size(); ++i) {
        token[i] = static_cast<std::uint8_t>(i * 11 + 1);
    }
    mmowire::AppendFrame(bytes, mmowire::EncodeEnterWorld(token));
    // A move while EnteringWorld is counted and dropped -- it proves the
    // binary path accepted the script's 9-byte layout (a bad one disconnects).
    mmowire::AppendFrame(bytes, mmowire::EncodeMove(1, mmowire::QuantizeHeading(1.0f), 2));
    const bool sent = client.SendRaw(bytes);
    std::this_thread::sleep_for(300ms);
    const bool stillOpen = !client.Closed(200ms);
    const auto statsAfter = handler.Stats();
    check("loopback-script-enter-world-and-move-accepted-by-real-handler",
          sent && stillOpen && handler.ContextCount() == 1 &&
              statsAfter.moves_dropped_entering == statsBefore.moves_dropped_entering + 1,
          Fmt("open=%d contexts=%zu moves_dropped_entering=%llu", stillOpen ? 1 : 0, handler.ContextCount(),
              static_cast<unsigned long long>(statsAfter.moves_dropped_entering)));
    client.Close();

    // A version the servers refuse (3) gets the mismatch answer and a close.
    WireClient refused;
    mmowire::FrameReader refusedReader;
    bytes.clear();
    mmowire::AppendFrame(bytes, mmowire::EncodeHandshakeRequest(3, "scriptwire"));
    mmowire::ServerPacket mismatch;
    const bool mismatchOk = refused.Connect(port) && refused.SendRaw(bytes) &&
                            refused.ReadPayload(refusedReader, payload, 3000ms) &&
                            mmowire::DecodeServerPacket(payload.data(), payload.size(), mismatch, error) &&
                            mismatch.result == 1 && refused.Closed(2000ms);
    check("loopback-script-unsupported-version-refused", mismatchOk, Fmt("result=%u", mismatch.result));
    refused.Close();

    // The engine's own script transport (platform::TcpStream, what IScriptApi::Net* wraps): non-blocking
    // connect, framed handshake out, response in -- polled like the engine's frame loop does.
    {
        std::string connectError;
        auto stream = platform::TcpStream::Connect("127.0.0.1", port, &connectError);
        bool connected = false;
        bool answered = false;
        mmowire::ServerPacket hello;
        int polls = 0;
        if (stream) {
            bytes.clear();
            mmowire::AppendFrame(bytes, mmowire::EncodeHandshakeRequest(mmowire::kProtocolVersion, "tcpstream"));
            bool handshakeSent = false;
            mmowire::FrameReader streamReader;
            const auto deadline = Clock::now() + 3s;
            while (Clock::now() < deadline && !answered) {
                ++polls;
                const auto state = stream->Poll();
                if (state == platform::TcpStream::State::Connected) {
                    connected = true;
                    if (!handshakeSent) {
                        handshakeSent = stream->Send(bytes.data(), bytes.size());
                    }
                } else if (state != platform::TcpStream::State::Connecting) {
                    break;
                }
                std::uint8_t chunk[4096];
                for (std::size_t n; (n = stream->Receive(chunk, sizeof(chunk))) > 0;) {
                    streamReader.Feed(chunk, n);
                }
                if (streamReader.Next(payload) == 1) {
                    answered = mmowire::DecodeServerPacket(payload.data(), payload.size(), hello, error);
                }
                std::this_thread::sleep_for(2ms);
            }
            stream->Close();
        }
        check("engine-tcpstream-handshake-with-real-handler",
              connected && answered && hello.tag == mmowire::Tag::HandshakeResponse && hello.result == 0 &&
                  hello.serverProtocolVersion == 2,
              Fmt("connected=%d answered=%d polls=%d error='%s'", connected ? 1 : 0, answered ? 1 : 0, polls,
                  stream ? stream->Error().c_str() : connectError.c_str()));
    }
    {
        // A refused connect (nothing listens on a just-released port) ends Failed, never hangs.
        tcp::acceptor probe(pool.io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const std::uint16_t deadPort = probe.local_endpoint().port();
        probe.close();
        std::string connectError;
        auto stream = platform::TcpStream::Connect("127.0.0.1", deadPort, &connectError);
        auto state = stream ? stream->GetState() : platform::TcpStream::State::Failed;
        const auto deadline = Clock::now() + 3s;
        while (stream && state == platform::TcpStream::State::Connecting && Clock::now() < deadline) {
            state = stream->Poll();
            std::this_thread::sleep_for(2ms);
        }
        check("engine-tcpstream-refused-connect-fails", state == platform::TcpStream::State::Failed,
              stream ? stream->Error() : connectError);
    }

    server.Stop();
    sim.Stop();
}

} // namespace

int RunScriptWireScenario()
{
    Check check;
    ClientEncoderChecks(check);
    ServerDecoderChecks(check);
    FrameChecks(check);
    LoopbackChecks(check);
    std::printf("SCRIPTWIRE summary: %d/%d passed\n", check.passes, check.passes + check.failures);
    return check.failures;
}

} // namespace gs::bench
