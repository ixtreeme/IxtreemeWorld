#include "AssetWatcher.h"

#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "Common.h"
#include "Debug.h"
#include "ProjectManager.h"

#include <algorithm>

namespace
{
// A path whose appearance, removal or move changes what the asset browser shows: a folder or an
// asset file under the asset root, not the library's own files (thumbnails, manifest, .meta, temp
// writes). A deleted path no longer exists, so one without an extension is taken for a folder.
bool AffectsAssetBrowser(const std::filesystem::path& path, const std::string& assetRootKey)
{
    if (path.empty() || assetRootKey.empty())
        return false;
    try
    {
        // Compared as lower-case generic text: the watcher and the project may spell the same root
        // differently (separators, drive-letter case).
        const std::string key = ixtreeme::common::ToLowerAscii(path.lexically_normal().generic_string());
        if (key.rfind(assetRootKey + "/", 0) != 0)
            return false;
        const std::string relative = key.substr(assetRootKey.size() + 1);
        const std::string first = relative.substr(0, relative.find('/'));
        if (first == "thumbnails" || first == "manifest.json")
            return false;
        const std::string ext = ixtreeme::common::ToLowerAscii(path.extension().generic_string());
        if (ext == ".meta" || ext == ".tmp")
            return false;
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec) || !path.has_extension())
            return true;
        return ext == ".scene" || AssetLibrary::DiscoverableCategory(path).has_value();
    }
    catch (const std::exception&)
    {
        return false;  // a name the narrow path API cannot represent
    }
}
} // namespace

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

    std::string assetRootKey;
    if (ProjectManager::Instance().HasProject())
    {
        std::error_code ec;
        std::filesystem::path assetRoot = ProjectManager::Instance().AssetRootPath();
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(assetRoot, ec);
        if (!ec)
            assetRoot = canonical;
        assetRootKey = ixtreeme::common::ToLowerAscii(assetRoot.lexically_normal().generic_string());
        while (!assetRootKey.empty() && assetRootKey.back() == '/')
            assetRootKey.pop_back();
    }

    bool changed = false;
    for (const PendingEvent& event : events)
    {
        if (!changed && event.action != efsw::Actions::Modified &&
            (AffectsAssetBrowser(event.path, assetRootKey) || AffectsAssetBrowser(event.oldPath, assetRootKey)))
        {
            changed = true;
        }
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
