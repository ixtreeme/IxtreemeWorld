#include "MaterialGraphIO.h"
#include <fstream>
#include <random>
#include <system_error>
#include <utility>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace material_tool
{
namespace
{
constexpr std::size_t MaxFileBytes = 4 * 1024 * 1024;
struct TemporaryFile
{
    std::filesystem::path path;
    ~TemporaryFile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};
struct NativeFile
{
#ifdef _WIN32
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit NativeFile(HANDLE handle) : value(handle) {}
    bool Close() { return CloseHandle(std::exchange(value, INVALID_HANDLE_VALUE)) != 0; }
    ~NativeFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
#else
    int value = -1;
    explicit NativeFile(int descriptor) : value(descriptor) {}
    bool Close() { return close(std::exchange(value, -1)) == 0; }
    ~NativeFile() { if (value >= 0) close(value); }
#endif
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
};
}
bool SaveMaterialTextAtomic(const std::filesystem::path& path, const std::string& text, std::string& error)
{
    error.clear();
    if (path.empty() || text.size() > MaxFileBytes) { error = "Invalid material destination or file size"; return false; }
    std::random_device random;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        auto temporary = path;
        temporary += ".tmp-" + std::to_string(random()) + "-" + std::to_string(random());
#ifdef _WIN32
        NativeFile file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            error = "Cannot create temporary material file"; return false;
        }
        TemporaryFile cleanup{temporary};
        DWORD written = 0;
        const bool wrote = WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
        const bool flushed = wrote && FlushFileBuffers(file.value);
        const bool closed = file.Close();
        if (!flushed || !closed) { error = "Cannot finish material file"; return false; }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        { error = "Cannot atomically replace material file"; return false; }
#else
        NativeFile file{open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600)};
        if (file.value < 0)
        {
            if (errno == EEXIST) continue;
            error = "Cannot create temporary material file"; return false;
        }
        TemporaryFile cleanup{temporary};
        std::size_t written = 0;
        while (written < text.size())
        {
            const auto bytes = write(file.value, text.data() + written, text.size() - written);
            if (bytes < 0 && errno == EINTR) continue;
            if (bytes <= 0) break;
            written += static_cast<std::size_t>(bytes);
        }
        const bool flushed = written == text.size() && fsync(file.value) == 0;
        const bool closed = file.Close();
        if (!flushed || !closed) { error = "Cannot finish material file"; return false; }
        std::error_code failure;
        std::filesystem::rename(temporary, path, failure);
        if (failure) { error = "Cannot atomically replace material file: " + failure.message(); return false; }
#endif
        return true;
    }
    error = "Cannot allocate a unique temporary material file"; return false;
}
bool LoadMaterialText(const std::filesystem::path& path, std::string& text, std::string& error)
{
    error.clear();
    std::error_code failure;
    const auto size = std::filesystem::file_size(path, failure);
    if (failure || size > MaxFileBytes) { error = "Cannot open material file, or file exceeds 4 MB"; return false; }
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "Cannot open material file"; return false; }
    std::string draft(static_cast<std::size_t>(size), '\0');
    if (!input.read(draft.data(), static_cast<std::streamsize>(draft.size())) || input.peek() != std::char_traits<char>::eof())
    { error = "Material file changed or could not be read completely"; return false; }
    text = std::move(draft); return true;
}
}
