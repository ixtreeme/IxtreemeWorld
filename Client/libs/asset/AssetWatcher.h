#pragma once

#include <efsw/efsw.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

class AssetDatabase;

class AssetWatcher final : public efsw::FileWatchListener
{
public:
    static AssetWatcher& Instance();

    explicit AssetWatcher(AssetDatabase& db);
    ~AssetWatcher() override;

    void start(const std::filesystem::path& projectRoot);
    void stop();
    bool processPendingEvents();

    void handleFileAction(efsw::WatchID watchId,
                          const std::string& dir,
                          const std::string& filename,
                          efsw::Action action,
                          const std::string& oldFilename = "") override;

private:
    struct PendingEvent
    {
        efsw::Action action = efsw::Actions::Modified;
        std::filesystem::path path;
        std::filesystem::path oldPath;
    };

    AssetDatabase& db_;
    std::unique_ptr<efsw::FileWatcher> watcher_;
    efsw::WatchID watchId_ = 0;
    std::filesystem::path projectRoot_;
    std::mutex queueMutex_;
    std::vector<PendingEvent> queue_;
};
