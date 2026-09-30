#include "ClosureBench.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_set>
#include "../world/zone/Zone.h"
#include "../world/zone/ZoneManager.h"
#include "../world/zone/ZoneScheduler.h"
#include "../world/activity/SpatialActivityField.h"
#include "../world/activity/WakeCaptureProfile.h"

namespace gs::bench {
int RunCaptureProbe(int players,int phases,const char* scenario)
{
    using namespace gs::game;
    if(players<0 || players>7000 || phases<1 || phases>2000) return 2;
    const std::string pattern=scenario;
    if(pattern!="static" && pattern!="rare" && pattern!="dynamic" && pattern!="topology") return 2;
    ZoneManager zones;PartitionLayout layout;layout.regions_x=8;layout.regions_y=8;
    InitialPartition initial;std::string error;
    if(!BuildInitialPartition({0,0,100000,100000},layout,240,initial,error)) return 2;
    zones.BuildInitialPartition(initial);
    struct Source {std::uint32_t net;std::size_t zone;Position position;};
    std::vector<Source> trace;trace.reserve(players);
    for(int i=0;i<players;++i) {
        Position p{1000.0f+float((i*7919)%98000),1000.0f+float((i*3571)%98000),0};
        trace.push_back({std::uint32_t(i+1),zones.FindIndexForPosition(p.x,p.y),p});
    }
    ZoneScheduler scheduler;
    const auto before=wake_profile::Snapshot();
    const auto window_start=wake_profile::Now();
    std::uint64_t capture_ns=0,source_writes=0,expected_total=0,observed_total=0;
    bool correct=true,immutable=true;std::shared_ptr<const ActivityWakeFrame> previous;
    const auto split_parent=zones.GetZone(0).Id();
    for(int phase=0;phase<phases;++phase) {
        if(pattern=="topology" && phase==phases/2) {
            const auto original=zones.GetZone(0).Id();
            ZoneManager::SplitPlan plan;std::vector<ZoneId> children;
            const bool split=zones.PlanSplit(original,plan) && zones.CreateStagedSplit(plan,children) && zones.CommitSplit(original,children);
            correct=correct && split;
            // Source-only replay: move immutable input identities to the new
            // leaf buffers; real entity transfer is tested by activitytemporal.
            for(std::size_t i=0;i<zones.ZoneCount();++i)zones.GetZone(i).ClearActivitySources();
            for(auto& p:trace) {
                p.zone=zones.FindIndexForPosition(p.position.x,p.position.y);
                auto& zone=zones.GetZone(p.zone);std::lock_guard lock(zone.ActivityMutex());
                zone.UpsertActivitySourceLocked(p.net,p.position);++source_writes;
            }
        }
        if(phase==0 || pattern=="dynamic" || (pattern=="rare" && phase%20==0)) {
            for(auto& p:trace) {
                if(phase) p.position.x+=0.015625f;
                auto& zone=zones.GetZone(p.zone);std::lock_guard lock(zone.ActivityMutex());
                zone.UpsertActivitySourceLocked(p.net,p.position);++source_writes;
            }
        }
        if(pattern=="topology" && phase==phases*3/4) {
            ZoneManager::MergePlan plan;ZoneId merged=0;
            const bool done=zones.PlanMerge(split_parent,plan) && zones.CreateStagedMergeTarget(plan,merged) && zones.CommitMerge(plan,merged);
            correct=correct && done;
            for(std::size_t i=0;i<zones.ZoneCount();++i)zones.GetZone(i).ClearActivitySources();
            for(auto& p:trace) {
                p.zone=zones.FindIndexForPosition(p.position.x,p.position.y);
                auto& zone=zones.GetZone(p.zone);std::lock_guard lock(zone.ActivityMutex());
                zone.UpsertActivitySourceLocked(p.net,p.position);++source_writes;
            }
        }
        // Independent flat rectangle oracle, outside every timed capture call.
        std::unordered_set<std::uint32_t> expected;
        for(const auto* leaf:zones.GetActiveLeaves()) for(const auto& p:trace) {
            const auto& b=leaf->bounds;
            const float dx=std::max({b.min_x-p.position.x,0.0f,p.position.x-b.max_x});
            const float dy=std::max({b.min_y-p.position.y,0.0f,p.position.y-b.max_y});
            if(dx*dx+dy*dy<=500.0f*500.0f){expected.insert(leaf->zone_id);}
        }
        const auto old_size=previous?previous->sources.size():0;
        const auto old_x=old_size?previous->sources.front().x:0;
        const auto a=wake_profile::Now();
        auto frame=scheduler.CaptureWakeInput(zones,500);
        const auto b=wake_profile::Now();capture_ns+=b-a;
        correct=correct && frame->influenced==expected && frame->sources.size()==trace.size();
        if(previous) immutable=immutable && previous->sources.size()==old_size && (!old_size || previous->sources.front().x==old_x);
        for(const auto& p:frame->sources) {
            const auto& input=trace.at(p.net_id-1);
            correct=correct && p.x==input.position.x && p.y==input.position.y;
        }
        expected_total+=expected.size();observed_total+=frame->influenced.size();previous=frame;
    }
    const auto window_end=wake_profile::Now();
    wake_profile::Print("captureprobe",before,wake_profile::Snapshot());
    std::printf("CAPTUREPROBE version=1 pattern=%s players=%d phases=%d source_writes=%llu expected_sum=%llu observed_sum=%llu capture_ns=%llu start_ns=%llu end_ns=%llu timed_population=capture_calls_only oracle_and_producer_excluded=1\n",
        scenario,players,phases,source_writes,expected_total,observed_total,capture_ns,window_start,window_end);
    std::printf("CAPTUREPROBE independent-geometry-source-oracle: %s\n",correct?"PASS":"FAIL");
    std::printf("CAPTUREPROBE retained-frame-immutable: %s\n",immutable?"PASS":"FAIL");
    return correct && immutable?0:2;
}
} // namespace gs::bench
