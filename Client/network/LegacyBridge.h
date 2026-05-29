#pragma once

#include <array>
#include <cstdint>

inline constexpr int CHARACTER_NAME_MAX_LEN = 24;
inline constexpr int PLAYER_PER_ACCOUNT4 = 4;
inline constexpr int POINT_MAX_NUM = 255;

struct TWorldEnterInfo {
    std::uint32_t vid = 0;
    std::uint16_t race = 0;
    std::uint8_t empire = 0;
    std::uint8_t skillGroup = 0;
    char name[CHARACTER_NAME_MAX_LEN + 1] = {};
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
};

struct TWorldMoveSample {
    std::uint32_t sequence = 0;
    std::uint8_t func = 0;
    std::uint32_t durationMs = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    float angle = 0.0f;
};

struct TWorldEntityInfo {
    std::uint32_t vid = 0;
    std::uint16_t race = 0;
    std::uint8_t type = 0;
    char name[CHARACTER_NAME_MAX_LEN + 1] = {};
    std::uint32_t level = 0;
    std::int32_t alignment = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    float angle = 0.0f;
    std::uint8_t moveFunc = 0;
    std::uint32_t moveDurationMs = 0;
    std::uint32_t moveSequence = 0;
    std::uint32_t moveSampleCount = 0;
    std::array<TWorldMoveSample, 8> moveSamples{};
    bool hasAdditionalInfo = false;
    bool hasPosition = false;
};

struct TQuestInfo {
    std::uint16_t index = 0;
    std::uint16_t categoryIndex = 0;
    char title[32] = {};
    char counterName[18] = {};
    std::int32_t counterValue = 0;
    char clockName[18] = {};
    std::int32_t clockValue = 0;
    char iconFile[26] = {};
    int startTime = 0;
    bool hasCounter = false;
    bool hasClock = false;
};

enum EPlayerPointIndex : std::uint32_t {
    POINT_LEVEL = 1,
    POINT_EXP = 3,
    POINT_NEXT_EXP = 4,
    POINT_HP = 5,
    POINT_MAX_HP = 6,
    POINT_SP = 7,
    POINT_MAX_SP = 8,
    POINT_GOLD = 11,
    POINT_ST = 12,
    POINT_HT = 13,
    POINT_DX = 14,
    POINT_IQ = 15,
    POINT_DEF_GRADE = 16,
    POINT_ATT_SPEED = 17,
    POINT_ATT_GRADE = 18,
    POINT_MOV_SPEED = 19,
    POINT_CASTING_SPEED = 21,
    POINT_MAGIC_ATT_GRADE = 22,
    POINT_MAGIC_DEF_GRADE = 23,
    POINT_STAT = 26,
    POINT_WEAPON_MIN = 29,
    POINT_WEAPON_MAX = 30,
    POINT_DODGE = 68,
};
