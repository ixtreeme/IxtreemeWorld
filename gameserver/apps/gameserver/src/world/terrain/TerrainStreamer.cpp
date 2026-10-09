#include "TerrainStreamer.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "common/Logging.h"

#include "../WorldConstants.h"

namespace gs::game {
namespace {
struct ElapsedCounter {
    std::uint64_t& value;
    std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    ~ElapsedCounter(){value+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());}
};
}
namespace {

// Per-chunk bookkeeping beyond the payload: the TerrainChunk object, three
// vector headers and the shared_ptr control block.
constexpr std::size_t kChunkOverheadBytes = sizeof(mx::map::TerrainChunk) + 64;
constexpr std::size_t kLatencyWindow = 512;
constexpr std::size_t kQuarantineLimit = 4096; // poison_freed_for_test: oldest released beyond this

void PushWindow(std::vector<double>& ring, std::size_t& next, double value)
{
    if (ring.size() < kLatencyWindow) {
        ring.push_back(value);
    } else {
        ring[next] = value;
        next = (next + 1) % kLatencyWindow;
    }
}

double Percentile(std::vector<double> values, double p)
{
    if (values.empty()) {
        return 0.0;
    }
    const std::size_t k = std::min(values.size() - 1, static_cast<std::size_t>(p * static_cast<double>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

std::uint64_t NextGeneration() noexcept
{
    static std::atomic<std::uint64_t> generation{0};
    return generation.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace

const char* ToString(TerrainStreamer::ChunkState state) noexcept
{
    switch (state) {
    case TerrainStreamer::ChunkState::Unloaded:
        return "unloaded";
    case TerrainStreamer::ChunkState::Waiting:
        return "waiting";
    case TerrainStreamer::ChunkState::Loading:
        return "loading";
    case TerrainStreamer::ChunkState::Ready:
        return "ready";
    case TerrainStreamer::ChunkState::Failed:
        return "failed";
    }
    return "?";
}

TerrainStreamer::TerrainStreamer(mx::map::ServerTerrain& terrain,
                                 std::shared_ptr<const mx::map::ChunkSource> source,
                                 TerrainStreamingConfig config,
                                 std::function<void()> wake)
    : terrain_(&terrain)
    , source_(std::move(source))
    , config_(config)
    , wake_(std::move(wake))
    , generation_(NextGeneration())
    , slots_(terrain.ChunkCount())
{
    config_.io_threads = std::clamp<std::uint32_t>(config_.io_threads, 1u, 16u);
    config_.max_in_flight = std::max<std::uint32_t>(config_.max_in_flight, 1u);
    config_.max_waiting = std::max<std::uint32_t>(config_.max_waiting, 1u);
    // Consumers re-demand every tick (50 ms): a retention below that would
    // expire a demand before it could be admitted.
    config_.retain_seconds = std::max(config_.retain_seconds, 0.1);
    // Fixed metadata: streamer slots, the terrain's slot arrays (published
    // pointer + ownership) and the chunk index kept by the chunk source.
    metadata_bytes_ = slots_.size() * (sizeof(Slot) + sizeof(void*) + sizeof(std::shared_ptr<int>) +
                                       sizeof(mx::map::ChunkEntry) + 32);
    metadata_bytes_ += sizeof(requests_) + 2*kMaxRequests * (sizeof(TerrainRequest) + 128) +
        slots_.size() * sizeof(std::uint32_t) * 3 + config_.max_waiting * sizeof(std::uint32_t) * 5 +
        kLatencyWindow * sizeof(double) * 2 + sizeof(window_miss_histogram_) + 2*sizeof(Stats);
    waiting_.reserve(config_.max_waiting);
    counters_.window_started_us=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
    pressure_candidates_.reserve(slots_.size());
    // The loader may already have published chunks (streaming startup set).
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        if (const auto* chunk = terrain_->Published(i)) {
            slots_[i].state = ChunkState::Ready;
            resident_bytes_ += chunk->Bytes() + kChunkOverheadBytes;
        }
    }
    NotePeaks();
    latency_ms_.reserve(kLatencyWindow);
    miss_ms_.reserve(kLatencyWindow);
    lookahead_seconds_.store(static_cast<float>(config_.lookahead_min_seconds), std::memory_order_relaxed);
    for (std::uint32_t t = 0; t < config_.io_threads; ++t) {
        io_threads_.emplace_back([this] { IoWorker(); });
    }
    UpdateStats();
}

TerrainStreamer::~TerrainStreamer()
{
    Stop();
}

void TerrainStreamer::Stop()
{
    {
        std::lock_guard lock(io_mutex_);
        if (io_stop_) {
            return;
        }
        io_stop_ = true;
        for (auto& job : io_queue_) {
            job->cancelled.store(true, std::memory_order_relaxed);
        }
        io_queue_.clear();
    }
    io_cv_.notify_all();
    for (auto& thread : io_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    io_threads_.clear();
    // Late results: rejected, never published (the generation is closed).
    std::vector<Completion> late;
    {
        std::lock_guard lock(completion_mutex_);
        late.swap(completions_);
    }
    counters_.stale_completions += late.size();
    ++generation_;
    in_flight_bytes_ = 0;
    in_flight_jobs_ = 0;
    for (auto& slot : slots_) {
        if (slot.state == ChunkState::Loading || slot.state == ChunkState::Waiting) {
            slot.state = ChunkState::Unloaded;
            slot.reserved_bytes = 0;
            slot.in_waiting = false;
        }
    }
    waiting_.clear();
    for(auto& entry:requests_) {
        if(entry.count==0) continue;
        if(auto request=entry.consumer) request->SetStatus(TerrainRequestStatus::Cancelled);
        entry.pins={}; entry.consumer.reset(); entry.count=0;
        ++counters_.requests_cancelled;
    }
    UpdateStats();
}

std::size_t TerrainStreamer::Reservation(std::uint32_t chunk_index) const noexcept
{
    // Decoded payload + the read buffer the worker holds while decoding.
    return terrain_->ChunkBytes(chunk_index) + static_cast<std::size_t>(source_->FileBytes(chunk_index)) +
           kChunkOverheadBytes;
}

void TerrainStreamer::Demand(std::uint32_t chunk_index, Clock::time_point now, TerrainPriority priority)
{
    DemandImpl(chunk_index, now, priority, false);
}

void TerrainStreamer::DemandImpl(std::uint32_t chunk_index, Clock::time_point now,
                                 TerrainPriority priority, bool operation)
{
    if (!Enabled() || chunk_index >= slots_.size()) {
        return;
    }
    ++counters_.demands;
    Slot& slot = slots_[chunk_index];
    if(slot.observed_request_window!=counters_.window_id) {
        slot.observed_request_window=counters_.window_id; ++counters_.unique_requested;
    }
    if(slot.evicted_window==counters_.window_id && slot.observed_redemand==Clock::time_point{}) slot.observed_redemand=Clock::now();
    // A rejected anonymous demand has no queue entry to expire. End its
    // episode after the same demand lease, rather than inheriting stale age
    // when a genuinely new consumer visits the chunk much later.
    if(slot.ingress_rejected &&
       std::chrono::duration<double>(now-slot.last_demand).count()>=config_.retain_seconds) {
        slot.ingress_rejected=false;
        slot.ingress_retry={};
    }
    slot.last_demand = now;
    if (!operation) slot.last_by_class[static_cast<unsigned>(priority)] = now;
    switch (slot.state) {
    case ChunkState::Ready:
        ++counters_.hits;
        return;
    case ChunkState::Waiting:
    case ChunkState::Loading:
        ++counters_.deduplicated;
        return;
    case ChunkState::Failed:
        if (slot.attempts >= config_.max_attempts || now < slot.retry_after) {
            return; // permanent (published invalid) or backing off
        }
        [[fallthrough]];
    case ChunkState::Unloaded:
        if (now < slot.ingress_retry) {
            ++counters_.retry_suppressed;
            return;
        }
        // Reserve a bounded part of the SAME queue for blocking operations.
        // No accepted request is displaced and there is no overflow queue.
        const auto reserved = config_.max_waiting >= 8 ? std::min(64u, config_.max_waiting / 4) : 0u;
        const auto limit = priority == TerrainPriority::Admission ? config_.max_waiting : config_.max_waiting-reserved;
        if (waiting_.size() >= limit) {
            ++counters_.admission_rejects;
            if (!slot.ingress_rejected) {
                ++counters_.ingress_unique_rejected;
                slot.first_miss = now;
                slot.ingress_rejected = true;
            }
            slot.ingress_retry = now + std::chrono::milliseconds(100);
            return; // bounded: the consumer demands again later
        }
        ++counters_.misses;
        slot.state = ChunkState::Waiting;
        slot.in_waiting = true;
        if (!slot.ingress_rejected) slot.first_miss = now;
        slot.ingress_rejected = false;
        waiting_.push_back(chunk_index);
        counters_.waiting_high_water = std::max(counters_.waiting_high_water, waiting_.size());
        return;
    }
}

unsigned TerrainStreamer::PriorityOf(const Slot& slot, Clock::time_point now) const
{
    unsigned priority = slot.operation_priority;
    for (unsigned p=0; p<3; ++p) {
        if (std::chrono::duration<double>(now-slot.last_by_class[p]).count() < config_.retain_seconds)
            priority = std::min(priority,p);
    }
    return std::min(priority,2u);
}

void TerrainStreamer::Request(const TerrainRequestHandle& request,
                               const std::vector<std::uint32_t>& chunks, TerrainPriority priority)
{
    if (!request) return;
    // Refresh the existing operation, never register the same identity
    // twice or overwrite an accepted handle with an ingress rejection.
    // Its original chunk set/start/deadline stay fixed; only class may change.
    for(auto& entry:requests_) {
        if(entry.count!=0 && entry.consumer==request) {
            entry.priority=priority;
            request->priority.store(priority);
            return;
        }
    }
    if(request->Status()!=TerrainRequestStatus::Pending) return;
    auto reject = [&](TerrainRequestStatus status) {
        request->SetStatus(status);
        ++counters_.requests_rejected;
    };
    if (!Enabled() || chunks.empty()) { reject(TerrainRequestStatus::OutsideWorld); return; }
    if (chunks.size()>kMaxRequestChunks) { reject(TerrainRequestStatus::CapacityRejected); return; }
    std::vector<std::uint32_t> unique=chunks;
    std::sort(unique.begin(),unique.end());
    unique.erase(std::unique(unique.begin(),unique.end()),unique.end());
    if (unique.size()>kMaxRequestChunks) { reject(TerrainRequestStatus::CapacityRejected); return; }
    std::size_t minimum=metadata_bytes_;
    std::size_t read_buffer=0;
    for (const auto index:unique) {
        if(index>=slots_.size()) { reject(TerrainRequestStatus::OutsideWorld); return; }
        const auto bytes=terrain_->ChunkBytes(index)+kChunkOverheadBytes;
        if(slots_[index].state!=ChunkState::Ready)
            read_buffer=std::max(read_buffer,static_cast<std::size_t>(source_->FileBytes(index)));
        if(bytes>config_.budget_bytes || minimum>config_.budget_bytes-bytes) {
            reject(TerrainRequestStatus::CapacityRejected); return;
        }
        minimum+=bytes;
    }
    for(std::uint32_t i=0;i<slots_.size();++i) {
        if(slots_[i].permanent_pin && !std::binary_search(unique.begin(),unique.end(),i)) {
            if(const auto* chunk=terrain_->Published(i)) minimum+=chunk->Bytes()+kChunkOverheadBytes;
        }
    }
    // A set may load sequentially: charging every read buffer simultaneously
    // would mislabel a serviceable operation as a physical capacity failure.
    if(read_buffer>config_.budget_bytes || minimum>config_.budget_bytes-read_buffer) {
        reject(TerrainRequestStatus::CapacityRejected); return;
    }
    for(auto& entry:requests_) {
        if(entry.count!=0) continue;
        request->MarkOnce(request->registered_us);
        request->priority.store(priority);
        entry.consumer=request;
        entry.count=unique.size();
        entry.priority=priority;
        entry.generation=generation_;
        std::copy(unique.begin(),unique.end(),entry.chunks.begin());
        ++counters_.requests_accepted;
        return;
    }
    reject(TerrainRequestStatus::CapacityRejected);
}

void TerrainStreamer::PumpRequests(Clock::time_point now)
{
    for(auto& slot:slots_) {
        if(slot.operation_priority<3) {
            slot.last_demand=*std::max_element(slot.last_by_class.begin(),slot.last_by_class.end());
            slot.operation_priority=3;
        }
    }
    for(auto& entry:requests_) {
        if(entry.count==0) continue;
        const bool abandoned=entry.consumer.use_count()==1;
        auto consumer=entry.consumer;
        TerrainRequestStatus terminal=TerrainRequestStatus::Pending;
        if(!consumer || consumer->cancelled.load(std::memory_order_acquire) || entry.generation!=generation_) {
            terminal=TerrainRequestStatus::Cancelled; ++counters_.requests_cancelled;
        } else if(consumer->consumed.load(std::memory_order_acquire)) {
            terminal=TerrainRequestStatus::Consumed; ++counters_.requests_consumed;
        } else if(abandoned) {
            terminal=TerrainRequestStatus::Cancelled; ++counters_.requests_cancelled;
        } else if(now>=consumer->deadline) {
            terminal=TerrainRequestStatus::TimedOut; ++counters_.requests_timed_out;
        }
        bool ready=true;
        if(terminal==TerrainRequestStatus::Pending) {
            for(std::size_t n=0;n<entry.count;++n) {
                auto& slot=slots_[entry.chunks[n]];
                if(slot.state==ChunkState::Failed && slot.attempts>=config_.max_attempts) {
                    terminal=TerrainRequestStatus::InvalidData; ++counters_.requests_invalid; break;
                }
                ready &= slot.state==ChunkState::Ready;
            }
        }
        if(terminal!=TerrainRequestStatus::Pending) {
            if(terminal==TerrainRequestStatus::Consumed) {
                for(std::size_t n=0;n<entry.count;++n)
                    slots_[entry.chunks[n]].ready_until=now+std::chrono::milliseconds(100);
            }
            entry.pins={}; entry.consumer.reset(); entry.count=0;
            if(consumer) consumer->SetStatus(terminal);
            continue;
        }
        for(std::size_t n=0;n<entry.count;++n) {
            const auto index=entry.chunks[n];
            slots_[index].operation_priority=std::min(slots_[index].operation_priority,static_cast<unsigned>(entry.priority));
            DemandImpl(index,now,entry.priority,true);
            // Acquire the complete set together; no partial hold-and-wait.
            if(ready && !entry.pins[n]) entry.pins[n]=terrain_->Owned(index);
        }
        if(ready) consumer->SetStatus(TerrainRequestStatus::Ready);
    }
}

void TerrainStreamer::PinPermanently(std::uint32_t chunk_index)
{
    if (Enabled() && chunk_index < slots_.size()) {
        slots_[chunk_index].permanent_pin = true;
    }
}

bool TerrainStreamer::MakeRoom(std::size_t bytes, Clock::time_point now, bool quiescent, bool pressure)
{
    ElapsedCounter elapsed{counters_.room_us};
    ++counters_.room_checks;
    if (Accounted() + bytes <= config_.budget_bytes) {
        return true;
    }
    // Least recently demanded first. Admission pressure overrides soft
    // retention, never a pin or the short ready-to-consumer handoff lease.
    std::vector<std::uint32_t> ordinary_candidates;
    auto& candidates = pressure ? pressure_candidates_ : ordinary_candidates;
    const auto retain = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(config_.retain_seconds));
    if (!pressure || !pressure_candidates_valid_) {
    candidates.clear();
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        if (slot.state == ChunkState::Ready && !slot.permanent_pin &&
            (pressure ? now >= slot.ready_until : now - slot.last_demand >= retain)) {
            candidates.push_back(i);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [this](std::uint32_t a, std::uint32_t b) {
        return slots_[a].last_demand == slots_[b].last_demand ? a<b : slots_[a].last_demand < slots_[b].last_demand;
    });
    if(pressure) { pressure_candidates_valid_=true; pressure_cursor_=0; }
    }
    // Outside a quiescent window an evicted chunk only moves to the retired
    // list: its bytes stay accounted until the next quiescent free (retired
    // chunks are unpinned by construction, so that free is certain). Evict
    // only until the PROJECTED accounting (minus reclaimable retired) makes
    // room -- no over-eviction across passes; the admission itself still
    // waits for the actual free.
    auto projected = [this] {
        std::size_t reclaimable=0;
        for(const auto& retired:retired_) if(retired.chunk.use_count()==1) reclaimable+=retired.bytes;
        return Accounted()-reclaimable;
    };
    std::size_t ordinary_cursor=0;
    auto& cursor=pressure ? pressure_cursor_ : ordinary_cursor;
    while(cursor<candidates.size()) {
        if (projected() + bytes <= config_.budget_bytes) {
            break;
        }
        const auto index=candidates[cursor++];
        // Pins are taken on this thread only: the count cannot change
        // between this check and the unpublish below.
        if (slots_[index].state!=ChunkState::Ready || terrain_->PinCount(index) > 0) {
            continue;
        }
        counters_.pressure_evictions += pressure && now-slots_[index].last_demand<retain;
        EvictSlot(index, quiescent);
    }
    const bool room = Accounted() + bytes <= config_.budget_bytes;
    counters_.room_blocked += !room;
    return room;
}

void TerrainStreamer::EvictSlot(std::uint32_t chunk_index, bool quiescent)
{
    ElapsedCounter elapsed{counters_.unpublish_us};
    auto chunk = terrain_->Unpublish(chunk_index);
    Slot& slot = slots_[chunk_index];
    slot.state = ChunkState::Unloaded;
    slot.attempts = 0;
    if (!chunk) {
        return;
    }
    const std::size_t bytes = chunk->Bytes() + kChunkOverheadBytes;
    resident_bytes_ -= std::min(resident_bytes_, bytes);
    ++counters_.evictions;
    slot.evicted_window=counters_.window_id;
    slot.observed_eviction=Clock::now(); slot.observed_redemand={};
    Retire(std::move(chunk));
    // Freed at once only when no zone tick can hold the raw pointer.
    if (quiescent || config_.unsafe_free_without_quiescence_for_test) {
        FreeRetired();
    }
}

void TerrainStreamer::Retire(std::shared_ptr<const mx::map::TerrainChunk> chunk)
{
    const std::size_t bytes = chunk->Bytes() + kChunkOverheadBytes;
    retired_.push_back(Retired{std::move(chunk), bytes});
    retired_bytes_ += bytes;
    NotePeaks();
}

void TerrainStreamer::FreeRetired()
{
    ElapsedCounter elapsed{counters_.reclaim_us};
    auto keep=retired_.begin();
    for (auto it=retired_.begin();it!=retired_.end();++it) {
        auto& retired=*it;
        if(retired.chunk.use_count()>1) {
            if(keep!=it) *keep=std::move(retired);
            ++keep;
            continue; // a reset generation may still have a live reader pin
        }
        if (config_.poison_freed_for_test && retired.chunk.use_count() == 1) {
            // The chunk object was allocated mutable (loader / chunk source /
            // PublishInvalid); only its published view is const.
            auto* chunk = const_cast<mx::map::TerrainChunk*>(retired.chunk.get());
            std::fill(chunk->heights16.begin(), chunk->heights16.end(), kPoisonHeight16);
            std::fill(chunk->heights32.begin(), chunk->heights32.end(), std::numeric_limits<std::int32_t>::min());
            std::fill(chunk->attributes.begin(), chunk->attributes.end(), kPoisonAttributes);
            quarantine_.push_back(std::move(retired.chunk));
            while (quarantine_.size() > kQuarantineLimit) {
                quarantine_.pop_front();
            }
        }
        retired.chunk.reset();
        retired_bytes_ -= std::min(retired_bytes_, retired.bytes);
        ++counters_.frees;
    }
    retired_.erase(keep,retired_.end());
}

void TerrainStreamer::NotePeaks() noexcept
{
    counters_.peak_accounted_bytes = std::max(counters_.peak_accounted_bytes, Accounted());
    counters_.peak_resident_bytes = std::max(counters_.peak_resident_bytes, resident_bytes_);
    counters_.peak_in_flight_bytes = std::max(counters_.peak_in_flight_bytes, in_flight_bytes_);
    counters_.peak_retired_bytes = std::max(counters_.peak_retired_bytes, retired_bytes_);
}

bool TerrainStreamer::Admit(std::uint32_t chunk_index, Clock::time_point now, bool quiescent)
{
    ElapsedCounter elapsed{counters_.admission_us};
    const std::size_t bytes = Reservation(chunk_index);
    if (!MakeRoom(bytes, now, quiescent, true)) {
        return false;
    }
    Slot& slot = slots_[chunk_index];
    slot.state = ChunkState::Loading;
    slot.request_epoch = ++next_epoch_;
    slot.reserved_bytes = bytes;
    slot.admitted_at = now;
    in_flight_bytes_ += bytes;
    ++in_flight_jobs_;
    ++counters_.loads_started;
    ++counters_.admitted_by_class[PriorityOf(slot,now)];
    counters_.queue_high_water = std::max(counters_.queue_high_water, in_flight_jobs_);
    NotePeaks();
    auto job = std::make_shared<Job>();
    job->chunk_index = chunk_index;
    job->generation = generation_;
    job->request_epoch = slot.request_epoch;
    job->reservation = bytes;
    job->admitted_class=PriorityOf(slot,now);
    {
        std::lock_guard lock(io_mutex_);
        io_queue_.push_back(std::move(job));
    }
    io_cv_.notify_one();
    return true;
}

void TerrainStreamer::PublishInvalid(std::uint32_t chunk_index)
{
    // An empty chunk: every query in it answers InvalidData (no samples),
    // never NotResident-then-retry and never "free space".
    auto invalid = std::make_shared<mx::map::TerrainChunk>();
    const auto& g = terrain_->Geometry();
    invalid->x = chunk_index % g.chunks_x;
    invalid->y = chunk_index / g.chunks_x;
    invalid->cells_x = g.ChunkCellsX(invalid->x);
    invalid->cells_y = g.ChunkCellsY(invalid->y);
    if (auto replaced = terrain_->Publish(chunk_index, std::move(invalid))) {
        // Not reachable in the current state machine (a failing slot is
        // empty), kept safe anyway: readers may hold the old one.
        resident_bytes_ -= std::min(resident_bytes_, replaced->Bytes() + kChunkOverheadBytes);
        ++counters_.replaced;
        Retire(std::move(replaced));
    }
    resident_bytes_ += kChunkOverheadBytes;
    NotePeaks();
}

void TerrainStreamer::Complete(Completion completion, Clock::time_point now)
{
    const auto& job = *completion.job;
    const std::uint32_t index = job.chunk_index;
    Slot* slot = index < slots_.size() ? &slots_[index] : nullptr;
    const bool current = slot != nullptr && job.generation == generation_ &&
                         slot->request_epoch == job.request_epoch && slot->state == ChunkState::Loading;
    // The JOB owns its reservation until its buffers/completion arrive, even
    // after a generation reset. A new slot epoch must never release it early.
    in_flight_bytes_ -= job.reservation;
    --in_flight_jobs_;
    if (!current) {
        ++counters_.stale_completions; // result (if any) released here, never published
        return;
    }
    slot->reserved_bytes = 0;
    if (completion.cancelled) {
        ++counters_.cancelled;
        slot->state = ChunkState::Unloaded;
        return;
    }
    counters_.bytes_read += completion.result.bytes_read;
    std::string failure;
    if (completion.result.ok) {
        const std::uint64_t seams = terrain_->SeamMismatches(index, *completion.result.chunk);
        if (seams == 0) {
            const std::size_t bytes = completion.result.chunk->Bytes() + kChunkOverheadBytes;
            if (auto replaced = terrain_->Publish(index, completion.result.chunk)) {
                // A slot that is Loading is empty in the state machine; if
                // it ever was not, the old chunk is retired -- never freed
                // under a reader.
                resident_bytes_ -= std::min(resident_bytes_, replaced->Bytes() + kChunkOverheadBytes);
                ++counters_.replaced;
                Retire(std::move(replaced));
            }
            resident_bytes_ += bytes;
            NotePeaks();
            slot->state = ChunkState::Ready;
            slot->ready_until = now + std::chrono::milliseconds(100);
            slot->attempts = 0;
            ++counters_.loads_completed;
            ++counters_.completed_by_class[completion.job->admitted_class];
            if(slot->observed_load_window!=counters_.window_id) {
                slot->observed_load_window=counters_.window_id; ++counters_.unique_loaded;
            }
            if(slot->evicted_window==counters_.window_id && slot->observed_redemand!=Clock::time_point{} &&
               slot->observed_redemand>=slot->observed_eviction && now>=slot->observed_redemand) {
                ++counters_.reloads;
                counters_.short_reloads += now-slot->observed_eviction<std::chrono::seconds(1);
                if(counters_.reload_trace_count<counters_.reload_trace.size()) {
                    auto us=[](auto t){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count());};
                    counters_.reload_trace[counters_.reload_trace_count++]={index,generation_,us(slot->observed_eviction),us(slot->observed_redemand),us(now)};
                }
                slot->evicted_window=~0ull;
            }
            PushWindow(latency_ms_, latency_next_,
                       std::chrono::duration<double, std::milli>(now - slot->admitted_at).count());
            PushWindow(miss_ms_, miss_next_, std::chrono::duration<double, std::milli>(now - slot->first_miss).count());
            const double miss_ms=std::max(0.0,std::chrono::duration<double,std::milli>(now-slot->first_miss).count());
            const auto bin=std::min<std::size_t>(255,static_cast<std::size_t>(std::log1p(miss_ms*100.0)/std::log(1.1)));
            ++window_miss_histogram_[bin];
            ++counters_.window_miss_samples;
            return;
        }
        ++counters_.seam_rejects;
        failure = "CHUNK_EDGE_MISMATCH(307): " + std::to_string(seams) +
                  " shared border sample(s) differ from a published neighbour";
    } else {
        failure = completion.result.error;
    }
    ++counters_.load_failures;
    ++slot->attempts;
    slot->state = ChunkState::Failed;
    slot->retry_after =
        now + std::chrono::duration_cast<Clock::duration>(
                  std::chrono::duration<double>(config_.retry_backoff_seconds * slot->attempts));
    if (slot->attempts >= config_.max_attempts) {
        ++counters_.permanent_failures;
        PublishInvalid(index);
        LOG_ERROR("terrain: chunk {} failed {} time(s), published as invalid data: {}", index, slot->attempts,
                  failure);
    } else {
        LOG_WARN("terrain: chunk {} load failed (attempt {}/{}), retry after backoff: {}", index, slot->attempts,
                 config_.max_attempts, failure);
    }
}

void TerrainStreamer::DrainCompletions(Clock::time_point now)
{
    ElapsedCounter elapsed{counters_.completion_us};
    std::vector<Completion> batch;
    {
        std::lock_guard lock(completion_mutex_);
        batch.swap(completions_);
    }
    for (auto& completion : batch) {
        Complete(std::move(completion), now);
    }
}

void TerrainStreamer::Pump(Clock::time_point now, bool quiescent)
{
    if (!Enabled()) {
        return;
    }
    const auto pump_started = Clock::now();
    ++counters_.pump_calls;
    counters_.quiescent_pumps += quiescent;
    // Negative-control test mode only: pretend every pass is quiescent.
    quiescent = quiescent || config_.unsafe_free_without_quiescence_for_test;
    pressure_candidates_valid_=false;
    const auto loads_before=counters_.loads_completed;
    DrainCompletions(now);
    PumpRequests(now);
    if(quiescent) FreeRetired();
    const auto retain = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(config_.retain_seconds));
    // Cancel queued-not-started loads whose demand expired.
    {
        std::lock_guard lock(io_mutex_);
        for (auto& job : io_queue_) {
            if (now - slots_[job->chunk_index].last_demand >= retain) {
                job->cancelled.store(true, std::memory_order_relaxed);
            }
        }
    }
    // Build bounded class queues once per pass; the pressure candidate list
    // is also shared across attempts, avoiding O(waiting x chunks) rescans.
    bool room_exhausted = false;
    std::vector<std::uint32_t> still_waiting;
    still_waiting.reserve(config_.max_waiting);
    std::array<std::vector<std::uint32_t>,3> classes;
    for(auto& queue:classes) queue.reserve(config_.max_waiting);
    for (const std::uint32_t index : waiting_) {
        Slot& slot = slots_[index];
        if (slot.state != ChunkState::Waiting) {
            slot.in_waiting = false;
            continue;
        }
        if (now - slot.last_demand >= retain) {
            slot.state = ChunkState::Unloaded; // nobody needs it any more
            slot.in_waiting = false;
            ++counters_.cancelled;
            continue;
        }
        classes[PriorityOf(slot,now)].push_back(index);
    }
    // Weighted round robin, including a nonzero share for old active and
    // prefetch work. At most 64 candidates examined, never an unbounded
    // requeue-until-empty loop. Unsatisfied large work is rotated behind
    // unexamined work, with its ORIGINAL request age preserved.
    constexpr unsigned shares[]{0,0,0,0,1,1,2};
    std::array<std::size_t,3> cursor{};
    std::size_t examined=0;
    std::size_t failed_reservation=std::numeric_limits<std::size_t>::max();
    if(now>=retry_admission_at_ || counters_.loads_completed!=loads_before) {
        while(examined<64 && in_flight_jobs_<config_.max_in_flight) {
            unsigned chosen=3;
            for(unsigned attempt=0;attempt<7;++attempt) {
                const auto p=shares[fairness_cursor_++ % 7];
                if(cursor[p]<classes[p].size()) { chosen=p; break; }
            }
            if(chosen==3) break;
            const auto index=classes[chosen][cursor[chosen]++];
            ++examined;
            ++counters_.examined_by_class[chosen];
            const auto bytes=Reservation(index);
            // In this pass pins/completions cannot improve after failure.
            // Reuse that result for equal/larger reservations, while still
            // trying smaller work and preserving every original queue age.
            if(bytes>=failed_reservation) {
                ++counters_.retry_suppressed;
                slots_[index].admission_deferred=true;
            } else if(Admit(index,now,quiescent)) slots_[index].in_waiting=false;
            else {
                failed_reservation=bytes;
                room_exhausted=true;
                slots_[index].admission_deferred=true;
            }
        }
    } else ++counters_.retry_suppressed;
    for(const auto index:waiting_) {
        auto& slot=slots_[index];
        if(slot.state==ChunkState::Waiting && !slot.admission_deferred) still_waiting.push_back(index);
    }
    for(const auto index:waiting_) {
        auto& slot=slots_[index];
        if(slot.state==ChunkState::Waiting && slot.admission_deferred) still_waiting.push_back(index);
        slot.admission_deferred=false;
    }
    counters_.admission_waits+=still_waiting.size();
    if(room_exhausted) retry_admission_at_=now+std::chrono::milliseconds(10);
    waiting_.swap(still_waiting);
    // Proactive eviction above the high-water mark (down to the low-water
    // mark) keeps admission from stalling on the next burst. Skipped when
    // making room already failed in this pass (same candidates, nothing
    // more to gain from a second scan).
    if (waiting_.empty() && !room_exhausted &&
        static_cast<double>(Accounted()) > config_.high_water * static_cast<double>(config_.budget_bytes)) {
        const auto target = static_cast<std::size_t>(config_.low_water * static_cast<double>(config_.budget_bytes));
        (void)MakeRoom(config_.budget_bytes - std::min(config_.budget_bytes, target), now, quiescent);
    }
    if (quiescent) {
        FreeRetired();
    }
    // Prefetch window from the measured load latency.
    const double p99 = Percentile(latency_ms_, 0.99) / 1000.0;
    const double lookahead = std::clamp(3.0 * p99 + static_cast<double>(kTickDtSeconds),
                                        config_.lookahead_min_seconds, config_.lookahead_max_seconds);
    lookahead_seconds_.store(static_cast<float>(lookahead), std::memory_order_relaxed);
    counters_.pump_us += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - pump_started).count());
    // The stats snapshot scans every slot: publish it at most 10x per second.
    if (now - last_stats_ >= std::chrono::milliseconds(100)) {
        last_stats_ = now;
        UpdateStats();
    }
}

bool TerrainStreamer::LoadBlocking(const std::vector<std::uint32_t>& chunks, std::string& error)
{
    if (!Enabled()) {
        return true;
    }
    const auto now = Clock::now();
    std::size_t needed = 0;
    for (const std::uint32_t index : chunks) {
        if (index < slots_.size() && slots_[index].state != ChunkState::Ready) {
            needed += Reservation(index);
        }
    }
    if (Accounted() + needed > config_.budget_bytes) {
        // Try to make room from undemanded chunks first.
        if (!MakeRoom(needed, now, true)) {
            error = "blocking load of " + std::to_string(chunks.size()) + " chunk(s) needs " + std::to_string(needed) +
                    " B, above the terrain budget (" + std::to_string(Accounted()) + " of " +
                    std::to_string(config_.budget_bytes) + " B in use)";
            return false;
        }
    }
    for (const std::uint32_t index : chunks) {
        Demand(index, now);
    }
    const auto deadline = Clock::now() + std::chrono::seconds(120);
    for (;;) {
        Pump(Clock::now(), true);
        bool done = true;
        std::vector<std::uint32_t> failed;
        for (const std::uint32_t index : chunks) {
            if (index >= slots_.size()) {
                continue;
            }
            const Slot& slot = slots_[index];
            if (slot.state == ChunkState::Failed && slot.attempts >= config_.max_attempts) {
                failed.push_back(index); // final: published invalid
                continue;
            }
            if (slot.state != ChunkState::Ready) {
                done = false;
                if (slot.state == ChunkState::Unloaded || slot.state == ChunkState::Failed) {
                    Demand(index, Clock::now()); // (re)start; a failed one after its backoff
                } else {
                    slots_[index].last_demand = Clock::now(); // keep it wanted, not a new demand
                }
            }
        }
        if (done) {
            UpdateStats();
            if (failed.empty()) {
                return true;
            }
            error = std::to_string(failed.size()) + " chunk(s) failed to load and are published invalid (first: " +
                    std::to_string(failed.front()) + ")";
            return false;
        }
        if (Clock::now() > deadline) {
            error = "blocking load timed out";
            UpdateStats();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::size_t TerrainStreamer::EvictUndemanded(Clock::time_point older_than, bool quiescent)
{
    if (!Enabled()) {
        return 0;
    }
    std::size_t evicted = 0;
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        if (slot.state == ChunkState::Ready && !slot.permanent_pin && slot.last_demand < older_than &&
            terrain_->PinCount(i) == 0) {
            EvictSlot(i, quiescent);
            ++evicted;
        }
    }
    if (quiescent) {
        FreeRetired();
    }
    UpdateStats();
    return evicted;
}

void TerrainStreamer::ResetGenerationForTest(bool quiescent)
{
    ++generation_;
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (terrain_->Published(i) != nullptr) {
            // Valid (Ready) or permanently failed (published invalid): both
            // belong to the old generation and are retired, not freed.
            EvictSlot(i, quiescent);
        }
        if (slot.state == ChunkState::Failed) {
            slot.state = ChunkState::Unloaded;
        }
        if (slot.state == ChunkState::Loading) {
            // Its completion now carries an old generation: rejected when it
            // arrives. The reservation is released with it.
            slot.state = ChunkState::Unloaded;
            slot.reserved_bytes = 0;
        }
        if(slot.state==ChunkState::Waiting) slot.state=ChunkState::Unloaded;
        slot.in_waiting=false;
        slot.last_by_class={};
        slot.last_demand={};
        slot.operation_priority=3;
        slot.ingress_rejected=false;
        slot.ingress_retry={};
        slot.attempts = 0;
    }
    waiting_.clear();
    UpdateStats();
}

TerrainStreamer::ChunkState TerrainStreamer::State(std::uint32_t chunk_index) const
{
    return chunk_index < slots_.size() ? slots_[chunk_index].state : ChunkState::Unloaded;
}

void TerrainStreamer::UpdateStats()
{
    counters_.captured_us=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
    NotePeaks();
    Stats s = counters_;
    const auto now = Clock::now();
    s.chunks_total = slots_.size();
    for(const auto& entry:requests_) {
        if(entry.count==0) continue;
        if(const auto consumer=entry.consumer) {
            if(consumer->Status()==TerrainRequestStatus::Ready) ++s.requests_ready;
            else ++s.requests_pending;
            s.oldest_request_ms=std::max(s.oldest_request_ms,
                std::chrono::duration<double,std::milli>(now-consumer->started).count());
        }
    }
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        switch (slot.state) {
        case ChunkState::Ready: {
            ++s.resident;
            const bool reader_pin = terrain_ != nullptr && terrain_->PinCount(i) > 0;
            s.permanent_pinned += slot.permanent_pin ? 1u : 0u;
            s.reader_pinned += reader_pin ? 1u : 0u;
            if (slot.permanent_pin || reader_pin) {
                ++s.pinned;
                if (const auto* chunk = terrain_->Published(i)) {
                    s.pinned_bytes += chunk->Bytes() + kChunkOverheadBytes;
                }
            } else if (const auto* chunk = terrain_->Published(i)) {
                auto& bytes = std::chrono::duration<double>(now - slot.last_demand).count() < config_.retain_seconds
                    ? s.soft_retained_bytes : s.evictable_bytes;
                bytes += chunk->Bytes() + kChunkOverheadBytes;
            }
            break;
        }
        case ChunkState::Waiting:
            ++s.waiting;
            ++s.waiting_by_class[PriorityOf(slot,now)];
            s.oldest_wait_ms = std::max(s.oldest_wait_ms,
                std::chrono::duration<double, std::milli>(now - slot.first_miss).count());
            break;
        case ChunkState::Loading:
            ++s.loading;
            break;
        case ChunkState::Failed:
            ++s.failed;
            s.invalid += terrain_ != nullptr && terrain_->Published(i) != nullptr ? 1u : 0u;
            break;
        case ChunkState::Unloaded:
            break;
        }
    }
    counters_.peak_pinned_bytes = std::max(counters_.peak_pinned_bytes, s.pinned_bytes);
    s.peak_pinned_bytes = counters_.peak_pinned_bytes;
    s.quarantined = quarantine_.size();
    s.retired = retired_.size();
    s.budget_bytes = config_.budget_bytes;
    s.resident_bytes = resident_bytes_;
    s.in_flight_bytes = in_flight_bytes_;
    s.retired_bytes = retired_bytes_;
    s.metadata_bytes = metadata_bytes_;
    s.accounted_bytes = Accounted();
    s.load_ms_p50 = Percentile(latency_ms_, 0.50);
    s.load_ms_p99 = Percentile(latency_ms_, 0.99);
    s.load_ms_max = latency_ms_.empty() ? 0.0 : *std::max_element(latency_ms_.begin(), latency_ms_.end());
    s.miss_ms_p50 = Percentile(miss_ms_, 0.50);
    s.miss_ms_p99 = Percentile(miss_ms_, 0.99);
    s.miss_ms_max = miss_ms_.empty() ? 0.0 : *std::max_element(miss_ms_.begin(), miss_ms_.end());
    s.window_miss_p99_ms=std::numeric_limits<double>::quiet_NaN();
    if(s.window_miss_samples) {
        const auto rank=(s.window_miss_samples*99+99)/100;
        std::uint64_t count=0;
        for(std::size_t i=0;i<window_miss_histogram_.size();++i) {
            count+=window_miss_histogram_[i];
            if(count>=rank) {s.window_miss_p99_ms=(std::pow(1.1,double(i+1))-1.0)/100.0; break;}
        }
    }
    s.lookahead_seconds = lookahead_seconds_.load(std::memory_order_relaxed);
    s.generation = generation_;
    {
        std::lock_guard lock(completion_mutex_);
        s.completion_pending = completions_.size();
    }
    {
        std::lock_guard lock(io_mutex_);
        s.io_queued = io_queue_.size();
    }
    std::lock_guard lock(stats_mutex_);
    published_stats_ = s;
}

TerrainStreamer::Stats TerrainStreamer::GetStats() const
{
    std::lock_guard lock(stats_mutex_);
    return published_stats_;
}

void TerrainStreamer::BeginMeasurementWindow(Clock::time_point now)
{
    ++counters_.window_id;
    counters_.window_started_us=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
    counters_.unique_requested=counters_.unique_loaded=counters_.reloads=counters_.short_reloads=0;
    counters_.reload_trace_count=0; counters_.reload_trace={};
    counters_.window_miss_samples=0;
    counters_.window_initial_waiting=waiting_.size();
    counters_.window_initial_oldest_ms=0;
    for(const auto index:waiting_) counters_.window_initial_oldest_ms=std::max(counters_.window_initial_oldest_ms,
        std::chrono::duration<double,std::milli>(now-slots_[index].first_miss).count());
    window_miss_histogram_.fill(0);
    UpdateStats();
}

void TerrainStreamer::IoWorker()
{
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(io_mutex_);
            io_cv_.wait(lock, [this] { return io_stop_ || !io_queue_.empty(); });
            if (io_stop_) {
                return;
            }
            job = std::move(io_queue_.front());
            io_queue_.pop_front();
        }
        Completion completion;
        completion.job = job;
        if (job->cancelled.load(std::memory_order_relaxed)) {
            completion.cancelled = true;
        } else {
            completion.result = source_->Load(job->chunk_index);
        }
        {
            std::lock_guard lock(completion_mutex_);
            completions_.push_back(std::move(completion));
        }
        if (wake_) {
            wake_();
        }
    }
}

} // namespace gs::game
