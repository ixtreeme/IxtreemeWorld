#include "ixtreemetree/tree_options.h"

#include <cstdint>

namespace ixtreemetree::detail
{
class Rng
{
public:
    explicit Rng(std::uint64_t seed)
    {
        std::uint64_t state = seed;
        for (std::uint64_t& value : s_)
            value = splitmix64(state);
    }

    std::uint64_t next()
    {
        const std::uint64_t result = rotl(s_[1] * 5u, 7) * 9u;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    float uniform(float minValue, float maxValue)
    {
        constexpr double inv = 1.0 / static_cast<double>(UINT64_MAX);
        const float t = static_cast<float>(static_cast<double>(next()) * inv);
        return minValue + (maxValue - minValue) * t;
    }

    int uniformInt(int minValue, int maxValue)
    {
        if (maxValue <= minValue)
            return minValue;
        return minValue + static_cast<int>(next() % static_cast<std::uint64_t>(maxValue - minValue + 1));
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k)
    {
        return (x << k) | (x >> (64 - k));
    }

    static std::uint64_t splitmix64(std::uint64_t& x)
    {
        std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    std::uint64_t s_[4]{};
};
}
