#include "AssetWatcher.h"

#include "AssetDatabase.h"
#include "Debug.h"

#include <algorithm>

AssetWatcher& AssetWatcher::Instance()
{
    static AssetWatcher watcher(AssetDatabase::Instance());
    return watcher;
}

AssetWatcher::AssetWatcher(AssetDatabase& db)
    : db_(db)
{
}

AssetWatcher::~AssetWatcher()
{
    stop();
}

void AssetWatcher::start(const std::filesystem::path& projectRoot)
{
    stop();

    std::error_code ec;
    projectRoot_ = std::filesystem::weakly_canonical(std::filesystem::absolute(projectRoot, ec), ec);
    if (ec)
        projectRoot_ = projectRoot;
    if (!std::filesystem::exists(projectRoot_, ec))
    {
        TraceError("[ASSET-DB] watcher_start_failed root=%s reason=missing_root",
            projectRoot_.generic_string().c_str());
        projectRoot_.clear();
        return;
    }

    watcher_ = std::make_unique<efsw::FileWatcher>();
    watchId_ = watcher_->addWatch(projectRoot_.string(), this, true);
    if (watchId_ < 0)
    {
        TraceError("[ASSET-DB] watcher_start_failed root=%s reason=%s",
            projectRoot_.generic_string().c_str(),
            efsw::Errors::Log::getLastErrorLog().c_str());
        watcher_.reset();
        watchId_ = 0;
        projectRoot_.clear();
        return;
    }

    watcher_->watch();
    Tracenf("[ASSET-DB] watcher_started root=%s backend=efsw",
        projectRoot_.generic_string().c_str());
}

void AssetWatcher::stop()
{
    if (watcher_ && watchId_ > 0)
        watcher_->removeWatch(watchId_);
    if (watcher_)
        Tracen("[ASSET-DB] watcher_stopped");

    watcher_.reset();
    watchId_ = 0;
    projectRoot_.clear();

    std::scoped_lock lock(queueMutex_);
    queue_.clear();
}

bool AssetWatcher::processPendingEvents()
{
    std::vector<PendingEvent> events;
    {
        std::scoped_lock lock(queueMutex_);
        if (queue_.empty())
            return false;
        events.swap(queue_);
    }

    bool changed = false;
    for (const PendingEvent& event : events)
    {
        switch (event.action)
        {
        case efsw::Actions::Add:
            changed = db_.runtimeAdd(event.path) || changed;
            break;
        case efsw::Actions::Delete:
            changed = db_.runtimeRemove(event.path) || changed;
            break;
        case efsw::Actions::Modified:
            changed = db_.runtimeModified(event.path) || changed;
            break;
        case efsw::Actions::Moved:
            changed = db_.runtimeMove(event.oldPath, event.path) || changed;
            break;
        default:
            break;
        }
    }
    return changed;
}

void AssetWatcher::handleFileAction(efsw::WatchID,
                                    const std::string& dir,
                                    const std::string& filename,
                                    efsw::Action action,
                                    const std::string& oldFilename)
{
    const std::filesystem::path directory(dir);
    PendingEvent event;
    event.action = action;
    event.path = directory / filename;
    if (!oldFilename.empty())
        event.oldPath = directory / oldFilename;

    std::scoped_lock lock(queueMutex_);
    queue_.push_back(std::move(event));
}
