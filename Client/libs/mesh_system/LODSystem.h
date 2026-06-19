#pragma once

#include "SpatialIndex.h"
#include "StaticMeshRenderer.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

std::uint64_t HashLodConfig(const LodConfig& config);
float DistanceToAabb(const WorldVec3& point, const SpatialIndex::Aabb& bounds);
std::uint32_t SelectLodLevel(const LodConfig& config, float distanceMeters, std::uint32_t previousLevel);

struct StaticMeshLodBatchKey
{
    StaticMeshRenderer* renderer = nullptr;
    std::uint64_t configHash = 0;
    std::uint32_t lodLevel = 0;

    bool operator==(const StaticMeshLodBatchKey& rhs) const
    {
        return renderer == rhs.renderer && configHash == rhs.configHash && lodLevel == rhs.lodLevel;
    }
};

struct StaticMeshLodBatchKeyHash
{
    std::size_t operator()(const StaticMeshLodBatchKey& key) const;
};

struct StaticMeshLodBatch
{
    struct LodDispositionRecord
    {
        std::uint32_t entityId = 0;
        float distance = 0.0f;
        std::uint32_t previousLevel = 0;
        std::uint32_t selectedLevel = 0;
        std::uint32_t levelCount = 0;
        LodConfig configSnapshot;
        bool overrideEnabled = false;
        bool bufferValid = false;
        const char* bufferSource = "none";
        std::size_t selectedVertices = 0;
        std::size_t selectedIndices = 0;
        SpatialIndex::Aabb bounds{};
        const char* bboxSource = "entity";
        bool culled = false;
        const char* cullReason = "none";
        bool submitted = false;
        bool fullResFallback = false;
        std::uint32_t drawIndexCount = 0;
    };

    LodConfig config;
    std::vector<StaticMeshRenderer::Instance> instances;
    std::vector<LodDispositionRecord> lodDispositionRecords;
};

struct LodDispositionState
{
    std::uint32_t selectedLevel = std::numeric_limits<std::uint32_t>::max();
    bool bufferValid = false;
    bool culled = false;
    bool submitted = false;
    bool fullResFallback = false;
    std::string disposition;
};

struct LodCfgLogState
{
    std::uint32_t levelCount = 0;
    std::array<float, LodConfig::MaxLevels> ratios{};
    std::array<float, LodConfig::MaxLevels> distances{};
    bool overrideEnabled = false;
    bool initialized = false;
};

struct LodPickLogState
{
    std::uint32_t selectedLevel = 0;
    std::uint32_t levelCount = 0;
    std::array<std::size_t, LodConfig::MaxLevels> levelTris{};
    std::size_t selectedTris = 0;
    bool selectedBufferValid = false;
    std::string source;
    bool initialized = false;
};

struct MPerfMainState
{
    std::size_t drawcalls = 0;
    std::size_t tris = 0;
    bool initialized = false;
};

struct MPerfOverrideState
{
    std::size_t uniformUpdates = 0;
    std::size_t activeOverrideDraws = 0;
    bool initialized = false;
};

struct MPerfMeshesState
{
    std::size_t total = 0;
    std::size_t culled = 0;
    std::size_t drawn = 0;
    bool initialized = false;
};

struct InstSummaryState
{
    std::size_t batches = 0;
    std::size_t draws = 0;
    std::size_t instances = 0;
    std::size_t maxBatch = 0;
    bool initialized = false;
};

struct InstBufferState
{
    std::size_t bytes = 0;
    bool initialized = false;
};
