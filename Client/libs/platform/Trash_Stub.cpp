#include "platform/trash.h"

#if !defined(_WIN32)
namespace platform
{
bool move_to_trash(const std::filesystem::path&, std::string* errorOut)
{
    if (errorOut)
        *errorOut = "Trash is not implemented on this platform yet";
    return false;
}
}
#endif
