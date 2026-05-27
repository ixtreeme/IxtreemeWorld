#include "common/Logging.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace gs::common {
namespace {

spdlog::level::level_enum ParseLevel(std::string_view level)
{
    std::string lowered(level);
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lowered == "trace") {
        return spdlog::level::trace;
    }
    if (lowered == "debug") {
        return spdlog::level::debug;
    }
    if (lowered == "warn" || lowered == "warning") {
        return spdlog::level::warn;
    }
    if (lowered == "error") {
        return spdlog::level::err;
    }
    if (lowered == "critical") {
        return spdlog::level::critical;
    }
    if (lowered == "off") {
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

} // namespace

void InitLogging(std::string_view level, const std::filesystem::path& log_file_path)
{
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    if (!log_file_path.empty()) {
        if (const auto parent = log_file_path.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            log_file_path.string(), true));
    }

    auto logger = std::make_shared<spdlog::logger>("gameserver", sinks.begin(), sinks.end());
    logger->set_level(ParseLevel(level));
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    logger->flush_on(spdlog::level::info);

    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(ParseLevel(level));
}

} // namespace gs::common
