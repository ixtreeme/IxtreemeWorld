// MmoClient — the IxtreemeWorld MMO client protocol, written as a SCENE SCRIPT (native C++ game module).
//
// The engine knows nothing about this game's network protocol: it only offers a generic, non-blocking
// TCP byte stream (IScriptApi::NetConnect..NetClose) and a one-line text prompt (PromptText). Everything
// protocol-specific lives here, in the game's own script:
//
//   * framing           4-byte big-endian length prefix, payload[0] = codec (0 Cap'n Proto, 1 binary)
//   * Cap'n Proto       a small hand-written single-message codec for the packets this client uses
//                       (the game-module build has the SDK headers only, no third-party libraries)
//   * login server      handshake (protocol 2) -> login -> character list -> select -> enter-world token
//   * game server       handshake (protocol 2) -> EnterWorld(token) -> accept -> transform frames
//   * transform frames  v1 0x10, v2 0x11 and the layered v3 0x12 (viewer volume/layer + layer deltas)
//   * movement          WASD -> binary move packet [1][0x01][u32 seq][u16 heading_q][u8 state]
//   * proxies           other entities become SpawnMesh proxies; this script's entity is the player
//
// Attach it to the entity that should represent the local player (a plain mesh, no character
// controller). Script parameters (string key/value, all optional):
//   loginHost  (127.0.0.1)   loginPort (11000)   character (first character on the account)
//   proxyMesh  mesh asset id for other players/mobs (empty: others are tracked but not drawn)
//   visualLift metres added to the drawn height (e.g. half the mesh height when its pivot is centred)
//
// Coordinates: the server is z-up (x, y, z); the engine is y-up. server (x, y, z) == engine (x, z, y).
// Heading 0 faces +server-y (engine +Z), pi/2 faces +x. Up/Down (or W/S) move along +-engine Z,
// Right/Left (or D/A) along +-X; Shift walks instead of running. In editor Play without a character
// controller WASD also flies the editor camera, so prefer the arrow keys there.
//
// Credentials are typed into engine prompts during Play, are sent only to the login server, are never
// logged, and are wiped from this script's buffers once the login request is queued. (The protocol is
// plain TCP, as the servers are today; use it only against servers you trust.)
//
// Build: drop this file into a project's script folder (the editor builds it), or build it standalone
// with the CMakeLists.txt next to it. Define MMO_CLIENT_CODEC_ONLY to compile only the wire codec
// (namespace mmowire) — used by the server-side cross-check test that compares it to real Cap'n Proto.

#if !defined(MMO_CLIENT_CODEC_ONLY)
#ifndef IXTREEME_GAME_MODULE
#define IXTREEME_GAME_MODULE 1
#endif
#include "ixtreeme/NativeScript.h"
#include "ixtreeme/IxModuleRegistry.inl"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mmowire
{

constexpr std::uint32_t kProtocolVersion = 2;          // layered frames (0x12) need >= 2
constexpr std::uint8_t kCodecCapnp = 0;
constexpr std::uint8_t kCodecBinary = 1;
constexpr std::size_t kMaxPayloadSize = 64u * 1024u;    // the servers' framing limit
constexpr std::uint8_t kMoveOpcode = 0x01;
constexpr std::uint8_t kFrameV1 = 0x10;
constexpr std::uint8_t kFrameV2 = 0x11;
constexpr std::uint8_t kFrameV3 = 0x12;
constexpr std::size_t kTransformRecordSize = 19;
constexpr std::uint8_t kFieldPosition = 0x01;
constexpr std::uint8_t kFieldHeading = 0x02;
constexpr std::uint8_t kFieldMoveState = 0x04;
constexpr std::uint8_t kFieldLayer = 0x08;

// Packet union tags (shared/protocol/schema/packet.capnp, Packet @0x9cb9eb642b266be4).
enum class Tag : std::uint16_t
{
    HandshakeRequest = 0,
    HandshakeResponse = 1,
    LoginRequest = 2,
    LoginResponse = 3,
    CharacterListRequest = 4,
    CharacterListResponse = 5,
    CharacterSelect = 6,
    EnterWorldToken = 7,
    EnterWorld = 8,
    EnterWorldAccept = 9,
    EnterWorldReject = 10,
    EntitySpawn = 11,
    EntityDespawn = 12,
    AttackTarget = 13,
    EntityHealthUpdate = 14,
    EntityDeath = 15,
};

inline void Wipe(std::string& s)
{
    std::fill(s.begin(), s.end(), '\0');
    s.clear();
}
inline void Wipe(std::vector<std::uint8_t>& v)
{
    std::fill(v.begin(), v.end(), std::uint8_t{0});
    v.clear();
}

inline std::uint16_t LoadU16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t LoadU32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t LoadU64(const std::uint8_t* p)
{
    return static_cast<std::uint64_t>(LoadU32(p)) | (static_cast<std::uint64_t>(LoadU32(p + 4)) << 32);
}
inline float LoadF32(const std::uint8_t* p)
{
    const std::uint32_t bits = LoadU32(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
inline float BitsToF32(std::uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// ---- framing --------------------------------------------------------------------------------------

// Appends one length-prefixed frame (u32 big-endian payload length, then the payload).
inline void AppendFrame(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& payload)
{
    const auto n = static_cast<std::uint32_t>(payload.size());
    out.push_back(static_cast<std::uint8_t>(n >> 24));
    out.push_back(static_cast<std::uint8_t>(n >> 16));
    out.push_back(static_cast<std::uint8_t>(n >> 8));
    out.push_back(static_cast<std::uint8_t>(n));
    out.insert(out.end(), payload.begin(), payload.end());
}

// Reassembles frames from the received byte stream.
class FrameReader
{
public:
    void Feed(const std::uint8_t* data, std::size_t size) { m_buffer.insert(m_buffer.end(), data, data + size); }

    // 1 = one payload extracted, 0 = need more bytes, -1 = protocol error (zero/oversized frame).
    int Next(std::vector<std::uint8_t>& payload)
    {
        const std::size_t available = m_buffer.size() - m_offset;
        if (available < 4)
            return Compact(), 0;
        const std::uint8_t* p = m_buffer.data() + m_offset;
        const std::uint32_t length = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
            (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
        if (length == 0 || length > kMaxPayloadSize)
            return -1;
        if (available < 4u + length)
            return Compact(), 0;
        payload.assign(p + 4, p + 4 + length);
        m_offset += 4u + length;
        return 1;
    }

    void Clear()
    {
        Wipe(m_buffer);
        m_offset = 0;
    }

private:
    void Compact()
    {
        if (m_offset == 0)
            return;
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_offset));
        m_offset = 0;
    }
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_offset = 0;
};

// ---- Cap'n Proto: single-segment message builder --------------------------------------------------

class CapnpBuilder
{
public:
    CapnpBuilder() { m_words.push_back(0); }  // word 0 = root pointer
    ~CapnpBuilder() { std::fill(m_words.begin(), m_words.end(), std::uint64_t{0}); }  // may hold a password
    CapnpBuilder(const CapnpBuilder&) = delete;
    CapnpBuilder& operator=(const CapnpBuilder&) = delete;

    std::size_t Allocate(std::size_t words)
    {
        const std::size_t at = m_words.size();
        m_words.resize(at + words, 0);
        return at;
    }
    // Allocates a struct and points `pointerWord` at it. Returns the struct's first (data) word.
    std::size_t InitStruct(std::size_t pointerWord, std::uint16_t dataWords, std::uint16_t pointerCount)
    {
        const std::size_t at = Allocate(static_cast<std::size_t>(dataWords) + pointerCount);
        if (dataWords == 0 && pointerCount == 0)
        {
            // A zero-sized struct is encoded with offset -1 (never as the null pointer).
            m_words[pointerWord] = 0xFFFFFFFCull;
            return at;
        }
        m_words[pointerWord] = StructPointer(pointerWord, at, dataWords, pointerCount);
        return at;
    }
    // Writes `bits` low bits of `value` at bit offset `bitOffset` of the data section at `dataWord`.
    void SetBits(std::size_t dataWord, unsigned bitOffset, unsigned bits, std::uint64_t value)
    {
        const std::size_t word = dataWord + bitOffset / 64;
        const unsigned shift = bitOffset % 64;
        const std::uint64_t mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1ull);
        m_words[word] = (m_words[word] & ~(mask << shift)) | ((value & mask) << shift);
    }
    // Text (NUL-terminated byte list) or Data (raw byte list) at `pointerWord`.
    void SetBytes(std::size_t pointerWord, const std::uint8_t* data, std::size_t size, bool text)
    {
        const std::size_t count = size + (text ? 1u : 0u);
        const std::size_t at = Allocate((count + 7u) / 8u);
        if (size > 0)
            std::memcpy(reinterpret_cast<std::uint8_t*>(m_words.data() + at), data, size);  // LE host
        const auto offset = static_cast<std::int64_t>(at) - static_cast<std::int64_t>(pointerWord + 1);
        m_words[pointerWord] = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(offset << 2)) | 1u) |
            (2ull << 32) | (static_cast<std::uint64_t>(count) << 35);
    }
    void SetText(std::size_t pointerWord, const std::string& text)
    {
        SetBytes(pointerWord, reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), true);
    }

    // codec byte + segment table (1 segment) + the segment, all little-endian.
    std::vector<std::uint8_t> Finish() const
    {
        std::vector<std::uint8_t> out;
        out.reserve(1 + 8 + m_words.size() * 8);
        out.push_back(kCodecCapnp);
        const auto words = static_cast<std::uint32_t>(m_words.size());
        for (std::uint32_t v : {0u, words})
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        for (std::uint64_t w : m_words)
            for (int i = 0; i < 8; ++i)
                out.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
        return out;
    }

private:
    static std::uint64_t StructPointer(std::size_t pointerWord, std::size_t target, std::uint16_t dataWords,
        std::uint16_t pointerCount)
    {
        const auto offset = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pointerWord + 1);
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(offset << 2)) |
            (static_cast<std::uint64_t>(dataWords) << 32) | (static_cast<std::uint64_t>(pointerCount) << 48);
    }
    std::vector<std::uint64_t> m_words;
};

// Packet { union tag bits[0,16) ; ptr[0] -> member struct } (8 bytes, 1 ptr). Returns the member struct.
inline std::size_t InitPacket(CapnpBuilder& b, Tag tag, std::uint16_t dataWords, std::uint16_t pointerCount)
{
    const std::size_t packet = b.InitStruct(0, 1, 1);
    b.SetBits(packet, 0, 16, static_cast<std::uint16_t>(tag));
    return b.InitStruct(packet + 1, dataWords, pointerCount);
}

// HandshakeRequest { protocolVersion u32 bits[0,32) ; clientBuild Text ptr[0] } (8 bytes, 1 ptr)
inline std::vector<std::uint8_t> EncodeHandshakeRequest(std::uint32_t protocolVersion, const std::string& clientBuild)
{
    CapnpBuilder b;
    const std::size_t s = InitPacket(b, Tag::HandshakeRequest, 1, 1);
    b.SetBits(s, 0, 32, protocolVersion);
    b.SetText(s + 1, clientBuild);
    return b.Finish();
}

// LoginRequest { username Text ptr[0] ; password Text ptr[1] } (0 bytes, 2 ptrs). Wipe the result after use.
inline std::vector<std::uint8_t> EncodeLoginRequest(const std::string& username, const std::string& password)
{
    CapnpBuilder b;
    const std::size_t s = InitPacket(b, Tag::LoginRequest, 0, 2);
    b.SetText(s, username);
    b.SetText(s + 1, password);
    return b.Finish();
}

// CharacterListRequest {} (0 bytes, 0 ptrs)
inline std::vector<std::uint8_t> EncodeCharacterListRequest()
{
    CapnpBuilder b;
    InitPacket(b, Tag::CharacterListRequest, 0, 0);
    return b.Finish();
}

// C2sCharacterSelect { characterId u64 bits[0,64) } (8 bytes, 0 ptrs)
inline std::vector<std::uint8_t> EncodeCharacterSelect(std::uint64_t characterId)
{
    CapnpBuilder b;
    const std::size_t s = InitPacket(b, Tag::CharacterSelect, 1, 0);
    b.SetBits(s, 0, 64, characterId);
    return b.Finish();
}

// C2sEnterWorld { token Data ptr[0] ; debugSpawnOverride ptr[1] (left null) } (0 bytes, 2 ptrs)
inline std::vector<std::uint8_t> EncodeEnterWorld(const std::vector<std::uint8_t>& token)
{
    CapnpBuilder b;
    const std::size_t s = InitPacket(b, Tag::EnterWorld, 0, 2);
    b.SetBytes(s, token.data(), token.size(), false);
    return b.Finish();
}

// Binary move packet: [codec 1][0x01][u32 seq][u16 heading_q][u8 state] — exactly 9 bytes.
inline std::vector<std::uint8_t> EncodeMove(std::uint32_t sequence, std::uint16_t headingQ, std::uint8_t moveState)
{
    std::vector<std::uint8_t> out{kCodecBinary, kMoveOpcode};
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(sequence >> (8 * i)));
    out.push_back(static_cast<std::uint8_t>(headingQ));
    out.push_back(static_cast<std::uint8_t>(headingQ >> 8));
    out.push_back(moveState);
    return out;
}

constexpr float kTwoPi = 6.28318530717958647692f;
inline std::uint16_t QuantizeHeading(float radians)
{
    if (!std::isfinite(radians))
        return 0;
    float a = std::fmod(radians, kTwoPi);
    if (a < 0.0f)
        a += kTwoPi;
    return static_cast<std::uint16_t>(std::lround(a / kTwoPi * 65535.0f) & 0xFFFF);
}
inline float DequantizeHeading(std::uint16_t q) { return static_cast<float>(q) / 65535.0f * kTwoPi; }

// ---- Cap'n Proto: bounds-checked reader (multi-segment, far pointers) ------------------------------

class CapnpReader
{
public:
    struct StructRef
    {
        bool valid = false;
        std::uint32_t segment = 0;
        std::size_t data = 0;       // first data word
        std::uint16_t dataWords = 0;
        std::uint16_t pointerCount = 0;
        std::size_t Pointers() const { return data + dataWords; }
    };
    struct ListRef
    {
        bool valid = false;
        std::uint32_t segment = 0;
        std::size_t start = 0;      // first element word (composite) / first byte word (byte list)
        std::uint32_t count = 0;    // elements (composite) / bytes (byte list)
        std::uint16_t dataWords = 0;
        std::uint16_t pointerCount = 0;
    };

    // `bytes` = the message after the codec byte (segment table + segments).
    bool Init(const std::uint8_t* bytes, std::size_t size)
    {
        m_segments.clear();
        if (size < 8)
            return false;
        const std::uint32_t segmentCount = LoadU32(bytes) + 1u;
        if (segmentCount == 0 || segmentCount > 64)
            return false;
        std::size_t tableBytes = 4u + 4u * segmentCount;
        tableBytes = (tableBytes + 7u) & ~std::size_t{7};
        if (size < tableBytes)
            return false;
        std::size_t offset = tableBytes;
        std::size_t totalWords = 0;
        for (std::uint32_t i = 0; i < segmentCount; ++i)
        {
            const std::size_t words = LoadU32(bytes + 4 + 4 * i);
            if (words > (size - offset) / 8)
                return false;
            m_segments.push_back({bytes + offset, words});
            offset += words * 8;
            totalWords += words;
        }
        if (offset != size || m_segments[0].words == 0)
            return false;
        // Traversal budget proportional to the message (amplification guard, like the server's).
        m_budget = std::max<std::size_t>(64, totalWords * 4);
        return true;
    }

    StructRef Root()
    {
        StructRef r;
        ReadStruct(0, 0, r);
        return r;
    }
    std::uint64_t Bits(const StructRef& s, unsigned bitOffset, unsigned bits) const
    {
        if (!s.valid || bitOffset / 64 >= s.dataWords)
            return 0;  // outside the (older/smaller) data section: the schema default (0)
        const std::uint64_t word = Word(s.segment, s.data + bitOffset / 64);
        const std::uint64_t mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1ull);
        return (word >> (bitOffset % 64)) & mask;
    }
    StructRef Struct(const StructRef& parent, unsigned pointerIndex)
    {
        StructRef r;
        if (parent.valid && pointerIndex < parent.pointerCount)
            ReadStruct(parent.segment, parent.Pointers() + pointerIndex, r);
        return r;
    }
    bool Text(const StructRef& parent, unsigned pointerIndex, std::string& out)
    {
        out.clear();
        ListRef list;
        if (!ReadByteList(parent, pointerIndex, list))
            return false;
        if (list.count == 0)
            return true;
        const std::uint8_t* p = Bytes(list.segment, list.start);
        if (p[list.count - 1] != 0)
            return false;  // Text must be NUL-terminated
        out.assign(reinterpret_cast<const char*>(p), list.count - 1);
        return true;
    }
    bool Data(const StructRef& parent, unsigned pointerIndex, std::vector<std::uint8_t>& out)
    {
        out.clear();
        ListRef list;
        if (!ReadByteList(parent, pointerIndex, list))
            return false;
        const std::uint8_t* p = Bytes(list.segment, list.start);
        out.assign(p, p + list.count);
        return true;
    }
    ListRef StructList(const StructRef& parent, unsigned pointerIndex)
    {
        ListRef out;
        if (!parent.valid || pointerIndex >= parent.pointerCount)
            return out;
        std::uint64_t pointer = 0;
        std::uint32_t segment = 0;
        std::size_t target = 0;
        if (!Resolve(parent.segment, parent.Pointers() + pointerIndex, pointer, segment, target))
            return out;
        if (pointer == 0)
        {
            out.valid = true;  // null list == empty
            return out;
        }
        if ((pointer & 3u) != 1u || ((pointer >> 32) & 7u) != 7u)
            return out;  // struct lists are always composite
        const std::size_t words = static_cast<std::size_t>(pointer >> 35);
        if (!InSegment(segment, target, words + 1) || !Spend(words + 1))
            return out;
        const std::uint64_t tag = Word(segment, target);
        if ((tag & 3u) != 0u)
            return out;
        out.count = static_cast<std::uint32_t>((tag >> 2) & 0x3FFFFFFFu);
        out.dataWords = static_cast<std::uint16_t>(tag >> 32);
        out.pointerCount = static_cast<std::uint16_t>(tag >> 48);
        const std::size_t stride = static_cast<std::size_t>(out.dataWords) + out.pointerCount;
        if (static_cast<std::uint64_t>(out.count) * stride > words)
            return out;
        out.segment = segment;
        out.start = target + 1;
        out.valid = true;
        return out;
    }
    StructRef Element(const ListRef& list, std::uint32_t index) const
    {
        StructRef r;
        if (!list.valid || index >= list.count)
            return r;
        r.valid = true;
        r.segment = list.segment;
        r.data = list.start + static_cast<std::size_t>(index) * (static_cast<std::size_t>(list.dataWords) + list.pointerCount);
        r.dataWords = list.dataWords;
        r.pointerCount = list.pointerCount;
        return r;
    }

private:
    struct Segment
    {
        const std::uint8_t* bytes;
        std::size_t words;
    };

    bool InSegment(std::uint32_t segment, std::size_t word, std::size_t words) const
    {
        return segment < m_segments.size() && word <= m_segments[segment].words &&
            words <= m_segments[segment].words - word;
    }
    std::uint64_t Word(std::uint32_t segment, std::size_t word) const
    {
        return LoadU64(m_segments[segment].bytes + word * 8);
    }
    const std::uint8_t* Bytes(std::uint32_t segment, std::size_t word) const
    {
        return m_segments[segment].bytes + word * 8;
    }
    bool Spend(std::size_t words)
    {
        if (words > m_budget)
            return false;
        m_budget -= words;
        return true;
    }

    // Follows the pointer stored at (segment, pointerWord), through far / double-far landing pads.
    // Out: the effective struct/list pointer word (0 = null) and where its content starts.
    bool Resolve(std::uint32_t segment, std::size_t pointerWord, std::uint64_t& pointer, std::uint32_t& targetSegment,
        std::size_t& target) const
    {
        if (!InSegment(segment, pointerWord, 1))
            return false;
        pointer = Word(segment, pointerWord);
        if (pointer == 0)
            return true;
        const unsigned kind = static_cast<unsigned>(pointer & 3u);
        if (kind == 3u)
            return false;  // capabilities are not used by this protocol
        if (kind != 2u)
        {
            const auto offset = static_cast<std::int32_t>(static_cast<std::uint32_t>(pointer)) >> 2;
            const std::int64_t at = static_cast<std::int64_t>(pointerWord) + 1 + offset;
            if (at < 0)
                return false;
            targetSegment = segment;
            target = static_cast<std::size_t>(at);
            return true;
        }
        const bool doubleFar = ((pointer >> 2) & 1u) != 0;
        const std::size_t padWord = static_cast<std::size_t>((pointer >> 3) & 0x1FFFFFFFu);
        const auto padSegment = static_cast<std::uint32_t>(pointer >> 32);
        if (!doubleFar)
        {
            if (!InSegment(padSegment, padWord, 1))
                return false;
            const std::uint64_t pad = Word(padSegment, padWord);
            if ((pad & 3u) == 2u || (pad & 3u) == 3u)
                return false;
            const auto offset = static_cast<std::int32_t>(static_cast<std::uint32_t>(pad)) >> 2;
            const std::int64_t at = static_cast<std::int64_t>(padWord) + 1 + offset;
            if (at < 0)
                return false;
            pointer = pad;
            targetSegment = padSegment;
            target = static_cast<std::size_t>(at);
            return true;
        }
        if (!InSegment(padSegment, padWord, 2))
            return false;
        const std::uint64_t landing = Word(padSegment, padWord);
        const std::uint64_t tag = Word(padSegment, padWord + 1);
        if ((landing & 7u) != 2u || (tag & 3u) == 2u || (tag & 3u) == 3u)
            return false;  // the landing pad must be a single-far pointer + a content tag
        pointer = tag;
        targetSegment = static_cast<std::uint32_t>(landing >> 32);
        target = static_cast<std::size_t>((landing >> 3) & 0x1FFFFFFFu);
        return true;
    }

    bool ReadStruct(std::uint32_t segment, std::size_t pointerWord, StructRef& out)
    {
        std::uint64_t pointer = 0;
        std::uint32_t targetSegment = 0;
        std::size_t target = 0;
        if (!Resolve(segment, pointerWord, pointer, targetSegment, target) || pointer == 0 || (pointer & 3u) != 0u)
            return false;
        out.dataWords = static_cast<std::uint16_t>(pointer >> 32);
        out.pointerCount = static_cast<std::uint16_t>(pointer >> 48);
        const std::size_t words = static_cast<std::size_t>(out.dataWords) + out.pointerCount;
        if (!InSegment(targetSegment, target, words) || !Spend(std::max<std::size_t>(words, 1)))
            return false;
        out.segment = targetSegment;
        out.data = target;
        out.valid = true;
        return true;
    }

    bool ReadByteList(const StructRef& parent, unsigned pointerIndex, ListRef& out)
    {
        if (!parent.valid || pointerIndex >= parent.pointerCount)
            return false;
        std::uint64_t pointer = 0;
        std::uint32_t segment = 0;
        std::size_t target = 0;
        if (!Resolve(parent.segment, parent.Pointers() + pointerIndex, pointer, segment, target))
            return false;
        if (pointer == 0)
        {
            out.valid = true;  // null Text/Data reads as empty
            return true;
        }
        if ((pointer & 3u) != 1u || ((pointer >> 32) & 7u) != 2u)
            return false;
        out.count = static_cast<std::uint32_t>(pointer >> 35);
        const std::size_t words = (static_cast<std::size_t>(out.count) + 7u) / 8u;
        if (!InSegment(segment, target, words) || !Spend(std::max<std::size_t>(words, 1)))
            return false;
        out.segment = segment;
        out.start = target;
        out.valid = true;
        return true;
    }

    std::vector<Segment> m_segments;
    std::size_t m_budget = 0;
};

// ---- decoded server packets -----------------------------------------------------------------------

struct CharacterInfo
{
    std::uint64_t id = 0;
    std::uint8_t slot = 0;
    std::string name;
    std::uint32_t level = 0;
    std::uint16_t classId = 0;
    std::uint16_t appearance = 0;
    std::int32_t posX = 0;
    std::int32_t posY = 0;
    std::uint16_t mapId = 0;
};

struct ServerPacket
{
    Tag tag = Tag::HandshakeResponse;
    std::uint16_t result = 0;                // handshake/login/character-list result, reject reason
    std::uint32_t serverProtocolVersion = 0; // handshake response
    std::string message;
    std::uint64_t accountId = 0;
    std::vector<CharacterInfo> characters;
    std::vector<std::uint8_t> token;         // enter-world token (secret: wipe after use)
    std::string gameHost;
    std::uint16_t gamePort = 0;
    std::uint32_t netId = 0;                 // accept: your net id; spawn/despawn/health/death: entity
    float position[3] = {0.0f, 0.0f, 0.0f};  // server coordinates (z up)
    std::uint32_t serverTick = 0;
    std::uint32_t volumeId = 0;
    std::uint32_t layerId = 0;
    std::string name;
    std::uint16_t classId = 0;
    std::uint16_t heading = 0;
    std::uint32_t mobType = 0;
    std::uint32_t level = 0;
    float hpCurrent = 0.0f;
    float hpMax = 0.0f;
    std::uint32_t killerNetId = 0;
};

inline bool ReadVec3(CapnpReader& r, const CapnpReader::StructRef& parent, unsigned pointerIndex, float out[3])
{
    const CapnpReader::StructRef v = r.Struct(parent, pointerIndex);
    if (!v.valid)
        return false;
    out[0] = BitsToF32(static_cast<std::uint32_t>(r.Bits(v, 0, 32)));
    out[1] = BitsToF32(static_cast<std::uint32_t>(r.Bits(v, 32, 32)));
    out[2] = BitsToF32(static_cast<std::uint32_t>(r.Bits(v, 64, 32)));
    return true;
}

// Decodes a Cap'n Proto payload (payload[0] == 0) sent by the login or game server.
inline bool DecodeServerPacket(const std::uint8_t* payload, std::size_t size, ServerPacket& out, std::string& error)
{
    out = ServerPacket{};
    if (size < 1 || payload[0] != kCodecCapnp)
        return error = "not a capnp payload", false;
    CapnpReader r;
    if (!r.Init(payload + 1, size - 1))
        return error = "bad capnp framing", false;
    const CapnpReader::StructRef packet = r.Root();
    if (!packet.valid)
        return error = "missing root", false;
    out.tag = static_cast<Tag>(r.Bits(packet, 0, 16));
    const CapnpReader::StructRef s = r.Struct(packet, 0);
    if (!s.valid)
        return error = "missing packet body", false;
    switch (out.tag)
    {
    case Tag::HandshakeResponse:
        out.result = static_cast<std::uint16_t>(r.Bits(s, 0, 16));
        out.serverProtocolVersion = static_cast<std::uint32_t>(r.Bits(s, 32, 32));
        if (!r.Text(s, 0, out.message))
            return error = "bad handshake message", false;
        return true;
    case Tag::LoginResponse:
        out.result = static_cast<std::uint16_t>(r.Bits(s, 0, 16));
        out.accountId = r.Bits(s, 64, 64);
        if (!r.Text(s, 0, out.message))
            return error = "bad login message", false;
        return true;
    case Tag::CharacterListResponse:
    {
        out.result = static_cast<std::uint16_t>(r.Bits(s, 0, 16));
        if (!r.Text(s, 0, out.message))
            return error = "bad character list message", false;
        const CapnpReader::ListRef list = r.StructList(s, 1);
        if (!list.valid || list.count > 64)
            return error = "bad character list", false;
        for (std::uint32_t i = 0; i < list.count; ++i)
        {
            const CapnpReader::StructRef c = r.Element(list, i);
            CharacterInfo info;
            info.id = r.Bits(c, 0, 64);
            info.slot = static_cast<std::uint8_t>(r.Bits(c, 64, 8));
            info.classId = static_cast<std::uint16_t>(r.Bits(c, 80, 16));
            info.level = static_cast<std::uint32_t>(r.Bits(c, 96, 32));
            info.appearance = static_cast<std::uint16_t>(r.Bits(c, 128, 16));
            info.mapId = static_cast<std::uint16_t>(r.Bits(c, 144, 16));
            info.posX = static_cast<std::int32_t>(static_cast<std::uint32_t>(r.Bits(c, 160, 32)));
            info.posY = static_cast<std::int32_t>(static_cast<std::uint32_t>(r.Bits(c, 192, 32)));
            if (!r.Text(c, 0, info.name))
                return error = "bad character name", false;
            out.characters.push_back(std::move(info));
        }
        return true;
    }
    case Tag::EnterWorldToken:
        out.gamePort = static_cast<std::uint16_t>(r.Bits(s, 0, 16));
        if (!r.Data(s, 0, out.token) || !r.Text(s, 1, out.gameHost))
            return error = "bad enter-world token", false;
        return true;
    case Tag::EnterWorldAccept:
        out.netId = static_cast<std::uint32_t>(r.Bits(s, 0, 32));
        out.serverTick = static_cast<std::uint32_t>(r.Bits(s, 32, 32));
        out.volumeId = static_cast<std::uint32_t>(r.Bits(s, 64, 32));
        out.layerId = static_cast<std::uint32_t>(r.Bits(s, 96, 32));
        if (!ReadVec3(r, s, 0, out.position))
            return error = "accept without spawn position", false;
        return true;
    case Tag::EnterWorldReject:
        out.result = static_cast<std::uint16_t>(r.Bits(s, 0, 16));
        return true;
    case Tag::EntitySpawn:
        out.netId = static_cast<std::uint32_t>(r.Bits(s, 0, 32));
        out.classId = static_cast<std::uint16_t>(r.Bits(s, 32, 16));
        out.heading = static_cast<std::uint16_t>(r.Bits(s, 48, 16));
        out.mobType = static_cast<std::uint32_t>(r.Bits(s, 64, 32));
        out.level = static_cast<std::uint32_t>(r.Bits(s, 96, 32));
        out.hpCurrent = BitsToF32(static_cast<std::uint32_t>(r.Bits(s, 128, 32)));
        out.hpMax = BitsToF32(static_cast<std::uint32_t>(r.Bits(s, 160, 32)));
        out.volumeId = static_cast<std::uint32_t>(r.Bits(s, 192, 32));
        out.layerId = static_cast<std::uint32_t>(r.Bits(s, 224, 32));
        if (!r.Text(s, 0, out.name) || !ReadVec3(r, s, 1, out.position))
            return error = "bad entity spawn", false;
        return true;
    case Tag::EntityDespawn:
        out.netId = static_cast<std::uint32_t>(r.Bits(s, 0, 32));
        return true;
    case Tag::EntityHealthUpdate:
        out.netId = static_cast<std::uint32_t>(r.Bits(s, 0, 32));
        out.hpCurrent = BitsToF32(static_cast<std::uint32_t>(r.Bits(s, 32, 32)));
        out.hpMax = BitsToF32(static_cast<std::uint32_t>(r.Bits(s, 64, 32)));
        return true;
    case Tag::EntityDeath:
        out.netId = static_cast<std::uint32_t>(r.Bits(s, 0, 32));
        out.killerNetId = static_cast<std::uint32_t>(r.Bits(s, 32, 32));
        return true;
    default:
        return error = "unexpected packet tag " + std::to_string(static_cast<unsigned>(out.tag)), false;
    }
}

// ---- binary transform frames ----------------------------------------------------------------------

struct TransformRecord
{
    std::uint32_t netId = 0;
    std::uint8_t mask = 0;                    // which fields below are present
    float position[3] = {0.0f, 0.0f, 0.0f};   // server coordinates
    std::uint16_t heading = 0;
    std::uint8_t moveState = 0;
    std::uint32_t volumeId = 0;
    std::uint32_t layerId = 0;
};

struct TransformFrame
{
    std::uint8_t opcode = 0;
    std::uint32_t tick = 0;
    TransformRecord viewer;                   // always complete (mask 0x07, or 0x0F in a v3 frame)
    std::vector<TransformRecord> records;     // others: full (v1) or deltas (v2/v3)
};

// Decodes a 0x10/0x11/0x12 frame. The payload must be consumed exactly.
inline bool DecodeTransformFrame(const std::uint8_t* p, std::size_t size, TransformFrame& out, std::string& error)
{
    out = TransformFrame{};
    if (size < 8 || p[0] != kCodecBinary)
        return error = "short frame", false;
    out.opcode = p[1];
    if (out.opcode != kFrameV1 && out.opcode != kFrameV2 && out.opcode != kFrameV3)
        return error = "unknown binary opcode " + std::to_string(out.opcode), false;
    out.tick = LoadU32(p + 2);
    const std::uint16_t count = LoadU16(p + 6);
    std::size_t at = 8;
    auto fullRecord = [&](TransformRecord& rec) {
        if (size - at < kTransformRecordSize)
            return false;
        rec.netId = LoadU32(p + at);
        rec.position[0] = LoadF32(p + at + 4);
        rec.position[1] = LoadF32(p + at + 8);
        rec.position[2] = LoadF32(p + at + 12);
        rec.heading = LoadU16(p + at + 16);
        rec.moveState = p[at + 18];
        rec.mask = kFieldPosition | kFieldHeading | kFieldMoveState;
        at += kTransformRecordSize;
        return true;
    };
    if (count == 0)
        return error = "frame without viewer record", false;
    if (!fullRecord(out.viewer))
        return error = "truncated viewer record", false;
    if (out.opcode == kFrameV3)
    {
        if (size - at < 8)
            return error = "truncated viewer layer", false;
        out.viewer.volumeId = LoadU32(p + at);
        out.viewer.layerId = LoadU32(p + at + 4);
        out.viewer.mask |= kFieldLayer;
        at += 8;
    }
    out.records.reserve(count - 1u);
    for (std::uint32_t i = 1; i < count; ++i)
    {
        TransformRecord rec;
        if (out.opcode == kFrameV1)
        {
            if (!fullRecord(rec))
                return error = "truncated record", false;
            out.records.push_back(rec);
            continue;
        }
        if (size - at < 5)
            return error = "truncated delta header", false;
        rec.netId = LoadU32(p + at);
        rec.mask = p[at + 4];
        at += 5;
        const std::uint8_t allowed = out.opcode == kFrameV3 ? 0x0F : 0x07;
        if ((rec.mask & ~allowed) != 0)
            return error = "delta with unknown field bits", false;
        if (rec.mask & kFieldPosition)
        {
            if (size - at < 12)
                return error = "truncated delta position", false;
            rec.position[0] = LoadF32(p + at);
            rec.position[1] = LoadF32(p + at + 4);
            rec.position[2] = LoadF32(p + at + 8);
            at += 12;
        }
        if (rec.mask & kFieldHeading)
        {
            if (size - at < 2)
                return error = "truncated delta heading", false;
            rec.heading = LoadU16(p + at);
            at += 2;
        }
        if (rec.mask & kFieldMoveState)
        {
            if (size - at < 1)
                return error = "truncated delta move state", false;
            rec.moveState = p[at];
            at += 1;
        }
        if (rec.mask & kFieldLayer)
        {
            if (size - at < 8)
                return error = "truncated delta layer", false;
            rec.volumeId = LoadU32(p + at);
            rec.layerId = LoadU32(p + at + 4);
            at += 8;
        }
        out.records.push_back(rec);
    }
    if (at != size)
        return error = "trailing bytes in frame", false;
    return true;
}

// server (x, y, z) z-up  <->  engine (x, y, z) y-up
inline void ServerToEngine(const float s[3], float e[3])
{
    e[0] = s[0];
    e[1] = s[2];
    e[2] = s[1];
}

} // namespace mmowire

#if !defined(MMO_CLIENT_CODEC_ONLY)

class MmoClient : public ixscript::NativeScript
{
public:
    void OnStart() override
    {
        m_loginHost = Param("loginHost", "127.0.0.1");
        m_loginPort = static_cast<std::uint32_t>(std::max(0.0f, ParamFloat("loginPort", 11000.0f)));
        m_characterName = Param("character");
        m_proxyMesh = Param("proxyMesh");
        m_visualLift = ParamFloat("visualLift", 0.0f);
        Log("[MMO] client script started (login " + m_loginHost + ":" + std::to_string(m_loginPort) +
            ", protocol " + std::to_string(mmowire::kProtocolVersion) + ")");
        Enter(Phase::AskUsername);
    }

    void OnUpdate(float dt) override
    {
        m_phaseTime += dt;
        switch (m_phase)
        {
        case Phase::AskUsername: AskUsername(); break;
        case Phase::WaitUsername: WaitUsername(); break;
        case Phase::WaitPassword: WaitPassword(); break;
        case Phase::Failed:
        case Phase::Idle: break;
        default: Pump(dt); break;
        }
        if (m_phase == Phase::InWorld)
            ApplyVisuals(dt);
    }

    void OnDestroy() override
    {
        Disconnect();
        mmowire::Wipe(m_username);
        mmowire::Wipe(m_password);
        mmowire::Wipe(m_token);
    }

private:
    enum class Phase
    {
        Idle,
        AskUsername,
        WaitUsername,
        WaitPassword,
        LoginConnecting,
        LoginHandshake,
        LoginAuth,
        LoginCharacters,
        LoginSelect,
        GameConnecting,
        GameHandshake,
        Entering,
        InWorld,
        Failed,
    };

    struct Proxy
    {
        std::uint32_t entity = 0;  // engine entity (0 = not drawn)
        float target[3] = {0.0f, 0.0f, 0.0f};  // engine coordinates
        float shown[3] = {0.0f, 0.0f, 0.0f};
        std::uint16_t heading = 0;
        std::uint32_t volumeId = 0;
        std::uint32_t layerId = 0;
        bool placed = false;
    };

    static const char* PhaseName(Phase p)
    {
        switch (p)
        {
        case Phase::Idle: return "idle";
        case Phase::AskUsername: return "ask-username";
        case Phase::WaitUsername: return "wait-username";
        case Phase::WaitPassword: return "wait-password";
        case Phase::LoginConnecting: return "login-connecting";
        case Phase::LoginHandshake: return "login-handshake";
        case Phase::LoginAuth: return "login-auth";
        case Phase::LoginCharacters: return "login-characters";
        case Phase::LoginSelect: return "login-select";
        case Phase::GameConnecting: return "game-connecting";
        case Phase::GameHandshake: return "game-handshake";
        case Phase::Entering: return "entering-world";
        case Phase::InWorld: return "in-world";
        case Phase::Failed: return "failed";
        }
        return "?";
    }

    void Enter(Phase p)
    {
        m_phase = p;
        m_phaseTime = 0.0f;
    }

    void Fail(const std::string& why)
    {
        LogError("[MMO] " + std::string(PhaseName(m_phase)) + ": " + why);
        Disconnect();
        mmowire::Wipe(m_password);
        mmowire::Wipe(m_token);
        Enter(Phase::Failed);
    }

    void Disconnect()
    {
        if (m_stream != 0)
            NetClose(m_stream);
        m_stream = 0;
        m_reader.Clear();
    }

    // ---- credentials (engine prompts; never logged) ----

    void AskUsername()
    {
        m_prompt = PromptText("Login", "Username", false);
        if (m_prompt == 0)
            return Fail("this host has no text prompt UI; run the scene in the editor");
        Enter(Phase::WaitUsername);
    }
    bool CollectPrompt(std::string& out, bool& cancelled)
    {
        char buffer[257] = {};
        const int status = PromptResult(m_prompt, buffer, sizeof(buffer));
        cancelled = status < 0;
        if (status == 1)
            out.assign(buffer);
        std::fill(std::begin(buffer), std::end(buffer), '\0');
        return status == 1;
    }
    void WaitUsername()
    {
        bool cancelled = false;
        if (!CollectPrompt(m_username, cancelled))
        {
            if (cancelled)
            {
                Log("[MMO] login cancelled");
                Enter(Phase::Idle);
            }
            return;
        }
        if (m_username.empty())
            return Enter(Phase::AskUsername);
        m_prompt = PromptText("Login", "Password for " + m_username, true);
        if (m_prompt == 0)
            return Fail("password prompt unavailable");
        Enter(Phase::WaitPassword);
    }
    void WaitPassword()
    {
        bool cancelled = false;
        if (!CollectPrompt(m_password, cancelled))
        {
            if (cancelled)
            {
                Log("[MMO] login cancelled");
                mmowire::Wipe(m_username);
                Enter(Phase::Idle);
            }
            return;
        }
        Connect(m_loginHost, m_loginPort, Phase::LoginConnecting);
    }

    // ---- transport ----

    void Connect(const std::string& host, std::uint32_t port, Phase next)
    {
        Disconnect();
        m_stream = NetConnect(host, port);
        if (m_stream == 0)
            return Fail("cannot connect to " + host + ":" + std::to_string(port));
        Log("[MMO] connecting to " + host + ":" + std::to_string(port));
        Enter(next);
    }

    void SendPayload(std::vector<std::uint8_t>& payload, bool secret = false)
    {
        std::vector<std::uint8_t> frame;
        mmowire::AppendFrame(frame, payload);
        const bool ok = NetSend(m_stream, frame.data(), static_cast<std::uint32_t>(frame.size()));
        if (secret)
        {
            mmowire::Wipe(frame);
            mmowire::Wipe(payload);
        }
        if (!ok)
            Fail("send failed (stream closed)");
    }

    void Pump(float dt)
    {
        if (m_stream == 0)
            return;
        const int state = NetState(m_stream);
        if (state == 0)
        {
            if (m_phaseTime > 10.0f)
                Fail("connect timed out");
            return;
        }
        if (m_phase == Phase::LoginConnecting && state == 1)
        {
            auto hello = mmowire::EncodeHandshakeRequest(mmowire::kProtocolVersion, "ixtreeme-scene-script");
            SendPayload(hello);
            Enter(Phase::LoginHandshake);
        }
        else if (m_phase == Phase::GameConnecting && state == 1)
        {
            auto hello = mmowire::EncodeHandshakeRequest(mmowire::kProtocolVersion, "ixtreeme-scene-script");
            SendPayload(hello);
            Enter(Phase::GameHandshake);
        }

        std::uint8_t chunk[16384];
        for (;;)
        {
            const std::uint32_t n = NetReceive(m_stream, chunk, sizeof(chunk));
            if (n == 0)
                break;
            m_reader.Feed(chunk, n);
        }
        std::vector<std::uint8_t> payload;
        for (;;)
        {
            const int got = m_reader.Next(payload);
            if (got < 0)
                return Fail("bad frame length from server");
            if (got == 0)
                break;
            HandlePayload(payload);
            if (m_stream == 0)
                return;  // the handler disconnected (failure or server switch)
        }
        if (m_stream != 0 && state >= 2 && m_phase != Phase::Failed)
        {
            // The stream ended. After the token the login server may hang up: that is fine.
            if (m_phase == Phase::GameConnecting)
                return;
            return Fail(state == 2 ? "server closed the connection" : "connection failed");
        }
        if (m_phase == Phase::InWorld)
            SendMovement(dt);
        else if (m_phaseTime > 15.0f && m_phase != Phase::InWorld)
            Fail("server did not answer in time");
    }

    void HandlePayload(std::vector<std::uint8_t>& payload)
    {
        if (!payload.empty() && payload[0] == mmowire::kCodecBinary)
            return HandleFrame(payload);
        mmowire::ServerPacket packet;
        std::string error;
        if (!mmowire::DecodeServerPacket(payload.data(), payload.size(), packet, error))
            return Fail("undecodable packet: " + error);
        using mmowire::Tag;
        switch (m_phase)
        {
        case Phase::LoginHandshake:
        case Phase::GameHandshake:
            if (packet.tag != Tag::HandshakeResponse)
                return Fail("expected handshake response");
            if (packet.result != 0)
                return Fail("handshake refused (" + std::to_string(packet.result) + "): " + packet.message);
            m_negotiated = packet.serverProtocolVersion;
            Log("[MMO] handshake ok: '" + packet.message + "', protocol " + std::to_string(m_negotiated));
            if (m_phase == Phase::LoginHandshake)
            {
                auto login = mmowire::EncodeLoginRequest(m_username, m_password);
                mmowire::Wipe(m_password);  // only the queued request still carries it
                SendPayload(login, true);
                Enter(Phase::LoginAuth);
            }
            else
            {
                auto enter = mmowire::EncodeEnterWorld(m_token);
                mmowire::Wipe(m_token);
                SendPayload(enter, true);
                Enter(Phase::Entering);
            }
            return;
        case Phase::LoginAuth:
            if (packet.tag != Tag::LoginResponse)
                return Fail("expected login response");
            if (packet.result != 0)
            {
                static const char* kReasons[] = {"ok", "invalid credentials", "account banned", "already logged in",
                                                 "internal error"};
                const char* reason = packet.result < 5 ? kReasons[packet.result] : "unknown";
                LogError(std::string("[MMO] login refused: ") + reason);
                Disconnect();
                return Enter(Phase::AskUsername);  // let the player try again
            }
            Log("[MMO] logged in as " + m_username);
            {
                auto list = mmowire::EncodeCharacterListRequest();
                SendPayload(list);
            }
            Enter(Phase::LoginCharacters);
            return;
        case Phase::LoginCharacters:
        {
            if (packet.tag != Tag::CharacterListResponse)
                return Fail("expected character list");
            if (packet.result != 0)
                return Fail("character list refused: " + packet.message);
            if (packet.characters.empty())
                return Fail("the account has no characters");
            const mmowire::CharacterInfo* chosen = &packet.characters.front();
            for (const auto& c : packet.characters)
            {
                Log("[MMO] character '" + c.name + "' level " + std::to_string(c.level) + " slot " +
                    std::to_string(c.slot));
                if (!m_characterName.empty() && c.name == m_characterName)
                    chosen = &c;
            }
            Log("[MMO] selecting '" + chosen->name + "'");
            auto select = mmowire::EncodeCharacterSelect(chosen->id);
            SendPayload(select);
            Enter(Phase::LoginSelect);
            return;
        }
        case Phase::LoginSelect:
            if (packet.tag != Tag::EnterWorldToken)
                return Fail("expected enter-world token");
            if (packet.token.empty() || packet.gamePort == 0)
                return Fail("invalid enter-world token");
            m_token = std::move(packet.token);  // single use; wiped once EnterWorld is queued
            Log("[MMO] world handoff to " + packet.gameHost + ":" + std::to_string(packet.gamePort));
            Connect(packet.gameHost.empty() ? m_loginHost : packet.gameHost, packet.gamePort, Phase::GameConnecting);
            return;
        default:
            break;
        }
        // In-world (and entering) game-server packets.
        switch (packet.tag)
        {
        case Tag::EnterWorldAccept:
        {
            m_myNetId = packet.netId;
            m_volumeId = packet.volumeId;
            m_layerId = packet.layerId;
            mmowire::ServerToEngine(packet.position, m_self.target);
            std::copy(std::begin(m_self.target), std::end(m_self.target), std::begin(m_self.shown));
            m_self.placed = false;
            char line[192];
            std::snprintf(line, sizeof(line),
                "[MMO] entered world: net %u at server (%.2f, %.2f, %.2f) volume %u layer %u tick %u protocol %u",
                packet.netId, packet.position[0], packet.position[1], packet.position[2], packet.volumeId,
                packet.layerId, packet.serverTick, m_negotiated);
            Log(line);
            Enter(Phase::InWorld);
            return;
        }
        case Tag::EnterWorldReject:
        {
            static const char* kReasons[] = {"invalid token", "expired token", "token already used", "server error",
                                             "character already in world"};
            return Fail(std::string("enter world rejected: ") + (packet.result < 5 ? kReasons[packet.result] : "unknown"));
        }
        case Tag::EntitySpawn:
        {
            Proxy& proxy = m_proxies[packet.netId];
            mmowire::ServerToEngine(packet.position, proxy.target);
            std::copy(std::begin(proxy.target), std::end(proxy.target), std::begin(proxy.shown));
            proxy.heading = packet.heading;
            proxy.volumeId = packet.volumeId;
            proxy.layerId = packet.layerId;
            if (proxy.entity == 0 && !m_proxyMesh.empty())
                proxy.entity = SpawnMesh(m_proxyMesh, proxy.target[0], proxy.target[1] + m_visualLift, proxy.target[2]);
            char line[192];
            std::snprintf(line, sizeof(line), "[MMO] spawn net %u '%s' at server (%.2f, %.2f, %.2f) volume %u layer %u",
                packet.netId, packet.name.c_str(), packet.position[0], packet.position[1], packet.position[2],
                packet.volumeId, packet.layerId);
            Log(line);
            return;
        }
        case Tag::EntityDespawn:
        {
            const auto it = m_proxies.find(packet.netId);
            if (it != m_proxies.end())
            {
                if (it->second.entity != 0)
                    DestroyEntity(it->second.entity);
                m_proxies.erase(it);
            }
            Log("[MMO] despawn net " + std::to_string(packet.netId));
            return;
        }
        case Tag::EntityHealthUpdate:
            return;
        case Tag::EntityDeath:
            Log("[MMO] net " + std::to_string(packet.netId) + " died (killer " + std::to_string(packet.killerNetId) + ")");
            return;
        default:
            Log("[MMO] ignored packet tag " + std::to_string(static_cast<unsigned>(packet.tag)));
            return;
        }
    }

    void HandleFrame(const std::vector<std::uint8_t>& payload)
    {
        mmowire::TransformFrame frame;
        std::string error;
        if (!mmowire::DecodeTransformFrame(payload.data(), payload.size(), frame, error))
            return Fail("bad transform frame: " + error);
        ++m_frames;
        ++m_framesByOpcode[frame.opcode];
        m_lastTick = frame.tick;
        // The viewer record is this player's authoritative transform.
        mmowire::ServerToEngine(frame.viewer.position, m_self.target);
        m_self.heading = frame.viewer.heading;
        m_selfServer[0] = frame.viewer.position[0];
        m_selfServer[1] = frame.viewer.position[1];
        m_selfServer[2] = frame.viewer.position[2];
        if (frame.viewer.mask & mmowire::kFieldLayer)
        {
            if (frame.viewer.volumeId != m_volumeId || frame.viewer.layerId != m_layerId)
            {
                char line[192];
                std::snprintf(line, sizeof(line),
                    "[MMO] level change: volume %u/layer %u -> volume %u/layer %u at server (%.2f, %.2f, %.2f) tick %u",
                    m_volumeId, m_layerId, frame.viewer.volumeId, frame.viewer.layerId, frame.viewer.position[0],
                    frame.viewer.position[1], frame.viewer.position[2], frame.tick);
                Log(line);
            }
            m_volumeId = frame.viewer.volumeId;
            m_layerId = frame.viewer.layerId;
        }
        for (const mmowire::TransformRecord& rec : frame.records)
        {
            const auto it = m_proxies.find(rec.netId);
            if (it == m_proxies.end())
                continue;  // not spawned for us (yet)
            Proxy& proxy = it->second;
            if (rec.mask & mmowire::kFieldPosition)
                mmowire::ServerToEngine(rec.position, proxy.target);
            if (rec.mask & mmowire::kFieldHeading)
                proxy.heading = rec.heading;
            if (rec.mask & mmowire::kFieldLayer)
            {
                if (rec.volumeId != proxy.volumeId)
                    Log("[MMO] net " + std::to_string(rec.netId) + " level change: volume " +
                        std::to_string(proxy.volumeId) + " -> " + std::to_string(rec.volumeId));
                proxy.volumeId = rec.volumeId;
                proxy.layerId = rec.layerId;
            }
        }
    }

    // ---- input -> move packets ----

    void SendMovement(float dt)
    {
        using K = ixscript::ScriptKey;
        float dx = 0.0f, dy = 0.0f;  // server plane
        if (IsKeyDown(K::W) || IsKeyDown(K::Up)) dy += 1.0f;
        if (IsKeyDown(K::S) || IsKeyDown(K::Down)) dy -= 1.0f;
        if (IsKeyDown(K::D) || IsKeyDown(K::Right)) dx += 1.0f;
        if (IsKeyDown(K::A) || IsKeyDown(K::Left)) dx -= 1.0f;
        std::uint8_t state = 0;
        std::uint16_t headingQ = m_sentHeading;
        if (dx != 0.0f || dy != 0.0f)
        {
            state = IsKeyDown(K::Shift) ? 1 : 2;  // walking : running
            headingQ = mmowire::QuantizeHeading(std::atan2(dx, dy));
        }
        m_keepAlive += dt;
        if (state != m_sentState || headingQ != m_sentHeading || m_keepAlive >= 1.0f)
        {
            auto move = mmowire::EncodeMove(++m_sequence, headingQ, state);
            SendPayload(move);
            m_sentState = state;
            m_sentHeading = headingQ;
            m_keepAlive = 0.0f;
        }
        m_statusTimer += dt;
        if (m_statusTimer >= 5.0f)
        {
            m_statusTimer = 0.0f;
            char line[224];
            std::snprintf(line, sizeof(line),
                "[MMO] status: tick %u frames %u (v1 %u, v2 %u, v3 %u) server (%.2f, %.2f, %.2f) volume %u layer %u proxies %zu",
                m_lastTick, m_frames, m_framesByOpcode[mmowire::kFrameV1], m_framesByOpcode[mmowire::kFrameV2],
                m_framesByOpcode[mmowire::kFrameV3], m_selfServer[0], m_selfServer[1], m_selfServer[2], m_volumeId,
                m_layerId, m_proxies.size());
            Log(line);
        }
    }

    // ---- presentation: ease shown transforms toward the server's ----

    void Ease(float shown[3], const float target[3], float dt, bool& placed)
    {
        const float d[3] = {target[0] - shown[0], target[1] - shown[1], target[2] - shown[2]};
        const float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float k = (!placed || distance > 5.0f) ? 1.0f : std::min(1.0f, dt * 15.0f);
        for (int i = 0; i < 3; ++i)
            shown[i] += d[i] * k;
        placed = true;
    }

    void ApplyVisuals(float dt)
    {
        Ease(m_self.shown, m_self.target, dt, m_self.placed);
        const float self[3] = {m_self.shown[0], m_self.shown[1] + m_visualLift, m_self.shown[2]};
        SetPosition(self);
        const float yaw[3] = {0.0f, mmowire::DequantizeHeading(m_self.heading) * 57.2957795f, 0.0f};
        SetRotation(yaw);
        for (auto& [netId, proxy] : m_proxies)
        {
            if (proxy.entity == 0)
                continue;
            Ease(proxy.shown, proxy.target, dt, proxy.placed);
            const float drawn[3] = {proxy.shown[0], proxy.shown[1] + m_visualLift, proxy.shown[2]};
            api->SetPosition(proxy.entity, drawn);
            const float proxyYaw[3] = {0.0f, mmowire::DequantizeHeading(proxy.heading) * 57.2957795f, 0.0f};
            api->SetRotation(proxy.entity, proxyYaw);
        }
    }

    Phase m_phase = Phase::Idle;
    float m_phaseTime = 0.0f;
    std::string m_loginHost;
    std::uint32_t m_loginPort = 11000;
    std::string m_characterName;
    std::string m_proxyMesh;
    float m_visualLift = 0.0f;
    std::uint32_t m_prompt = 0;
    std::string m_username;
    std::string m_password;              // wiped as soon as the login request is queued
    std::vector<std::uint8_t> m_token;   // wiped as soon as EnterWorld is queued
    std::uint32_t m_stream = 0;
    mmowire::FrameReader m_reader;
    std::uint32_t m_negotiated = 0;
    std::uint32_t m_myNetId = 0;
    std::uint32_t m_volumeId = 0;
    std::uint32_t m_layerId = 0;
    Proxy m_self;
    float m_selfServer[3] = {0.0f, 0.0f, 0.0f};
    std::map<std::uint32_t, Proxy> m_proxies;
    std::uint32_t m_sequence = 0;
    std::uint8_t m_sentState = 255;      // forces the first move packet
    std::uint16_t m_sentHeading = 0;
    float m_keepAlive = 0.0f;
    float m_statusTimer = 0.0f;
    std::uint32_t m_frames = 0;
    std::uint32_t m_framesByOpcode[256] = {};
    std::uint32_t m_lastTick = 0;
};
IXSCRIPT_REGISTER(MmoClient)

#endif // !MMO_CLIENT_CODEC_ONLY
