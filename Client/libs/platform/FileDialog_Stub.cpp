#include "platform/file_dialog.h"

#if !defined(_WIN32)
namespace platform
{
std::optional<std::filesystem::path> ShowFileDialog(const FileDialogOptions&)
{
    // No native file dialog on this platform yet; callers treat this as a cancelled dialog.
    return std::nullopt;
}
}
#endif
