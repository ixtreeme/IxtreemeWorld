#include "platform/process.h"

#if !defined(_WIN32)
#include <array>
#include <cstring>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace platform
{
ProcessResult RunProcess(const std::vector<std::string>& argv, const std::filesystem::path& workingDir)
{
    ProcessResult result;
    if (argv.empty())
        return result;

    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0)
        return result;

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(fds[0]);
        ::close(fds[1]);
        return result;
    }

    if (pid == 0)
    {
        // Child: redirect stdout+stderr into the pipe, then exec.
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        if (!workingDir.empty())
        {
            if (::chdir(workingDir.c_str()) != 0)
                ::_exit(127);
        }
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const std::string& a : argv)
            cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        ::_exit(127);  // exec failed
    }

    // Parent: drain the read end until EOF, then reap.
    ::close(fds[1]);
    result.launched = true;
    std::array<char, 4096> buffer{};
    ssize_t n = 0;
    while ((n = ::read(fds[0], buffer.data(), buffer.size())) > 0)
        result.output.append(buffer.data(), static_cast<size_t>(n));
    ::close(fds[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}
}
#endif
