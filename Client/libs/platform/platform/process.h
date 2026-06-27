#pragma once

// Cross-platform child-process runner (Win32 CreateProcess / POSIX fork+exec), behind the platform
// abstraction. Used to drive an external build tool (cmake) from the editor. BLOCKS until the child
// exits and captures its merged stdout+stderr — the caller is expected to run it on a worker thread.

#include <filesystem>
#include <string>
#include <vector>

namespace platform
{
struct ProcessResult
{
    bool launched = false;  // false if the executable couldn't be spawned (e.g. not on PATH)
    int exitCode = -1;      // the child's exit code (only meaningful when launched)
    std::string output;     // merged stdout + stderr
};

// Runs argv[0] with argv[1..] in workingDir (current dir if empty). argv[0] is resolved via PATH.
ProcessResult RunProcess(const std::vector<std::string>& argv,
                         const std::filesystem::path& workingDir = {});
}
