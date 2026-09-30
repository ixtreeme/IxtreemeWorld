#pragma once

#include <filesystem>
#include <string_view>

#include <spdlog/spdlog.h>

namespace gs::common {

void InitLogging(std::string_view level, const std::filesystem::path& log_file_path);

} // namespace gs::common

#define LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
