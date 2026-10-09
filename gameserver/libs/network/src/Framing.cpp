#include "network/Framing.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace gs::network::Framing {

std::vector<std::uint8_t> Encode(std::span<const std::uint8_t> payload)
{
    std::vector<std::uint8_t> frame;
    EncodeInto(frame, payload);
    return frame;
}

void EncodeInto(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> payload)
{
    if (payload.size() > kMaxPayloadSize) {
        throw std::length_error("payload exceeds maximum frame size");
    }

    const auto length = htonl(static_cast<std::uint32_t>(payload.size()));
    const auto old_size = out.size();
    out.resize(old_size + kHeaderSize + payload.size());

    auto* write = out.data() + old_size;
    std::memcpy(write, &length, kHeaderSize);
    std::ranges::copy(payload, write + kHeaderSize);
}

} // namespace gs::network::Framing
