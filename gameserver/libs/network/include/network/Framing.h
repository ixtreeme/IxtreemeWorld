#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gs::network::Framing {

constexpr std::size_t kHeaderSize = sizeof(std::uint32_t);
constexpr std::size_t kMaxPayloadSize = 64 * 1024;

std::vector<std::uint8_t> Encode(std::span<const std::uint8_t> payload);
void EncodeInto(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> payload);

} // namespace gs::network::Framing
