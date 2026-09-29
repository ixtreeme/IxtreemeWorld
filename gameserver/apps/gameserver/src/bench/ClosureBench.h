#pragma once
#include <cstddef>
#include <cstdint>
namespace gs::bench {
int RunSchedulerContract(std::size_t workers, bool fixed_work, std::uint64_t iterations);
int RunActivityTemporalReproduction(bool fixed = false);
int RunTerrainWaitTests();
int RunTC4TemporalTests();
int RunCaptureProbe(int players,int phases,const char* scenario);
}
