#include "platform/open_external.h"

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>

namespace platform
{
bool OpenInDefaultApp(const std::filesystem::path& path, std::string* errorOut)
{
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    const std::string p = (ec ? path : abs).string();
#if defined(__APPLE__)
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    const pid_t pid = ::fork();
    if (pid < 0)
    {
        if (errorOut)
            *errorOut = "fork failed";
        return false;
    }
    if (pid == 0)
    {
        ::execlp(opener, opener, p.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);  // exec failed
    }
    // Reap the short-lived launcher (xdg-open/open exits after handing off to the real viewer) so it
    // doesn't linger as a zombie. The actual editor window is the launcher's child — unaffected.
    ::waitpid(pid, nullptr, 0);
    return true;
}
}
#endif
