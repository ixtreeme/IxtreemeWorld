#include "platform/trash.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace platform
{
bool move_to_trash(const std::filesystem::path& path, std::string* errorOut)
{
    std::wstring from = std::filesystem::absolute(path).wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    const int result = SHFileOperationW(&op);
    if (result == 0 && !op.fAnyOperationsAborted)
        return true;

    if (errorOut)
    {
        if (op.fAnyOperationsAborted)
            *errorOut = "trash operation aborted";
        else
            *errorOut = "SHFileOperationW failed: " + std::to_string(result);
    }
    return false;
}
}
#endif
