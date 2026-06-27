#include "platform/process.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>

namespace platform
{
namespace
{
// Quote one argv element for the Windows command line per the CommandLineToArgvW rules (backslashes
// before a quote double; the whole token is wrapped in quotes if it contains spaces/quotes).
std::wstring QuoteArg(const std::string& arg)
{
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, arg.c_str(), -1, nullptr, 0);
    std::wstring w(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1)
        ::MultiByteToWideChar(CP_UTF8, 0, arg.c_str(), -1, w.data(), wlen);

    const bool needQuotes = w.empty() || w.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needQuotes)
        return w;

    std::wstring out = L"\"";
    for (size_t i = 0; i < w.size(); ++i)
    {
        size_t backslashes = 0;
        while (i < w.size() && w[i] == L'\\') { ++backslashes; ++i; }
        if (i == w.size())
        {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (w[i] == L'"')
        {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        }
        else
        {
            out.append(backslashes, L'\\');
            out.push_back(w[i]);
        }
    }
    out.push_back(L'"');
    return out;
}
} // namespace

ProcessResult RunProcess(const std::vector<std::string>& argv, const std::filesystem::path& workingDir)
{
    ProcessResult result;
    if (argv.empty())
        return result;

    std::wstring cmdLine;
    for (size_t i = 0; i < argv.size(); ++i)
    {
        if (i)
            cmdLine.push_back(L' ');
        cmdLine += QuoteArg(argv[i]);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!::CreatePipe(&readEnd, &writeEnd, &sa, 0))
        return result;
    // The read end stays in the parent only — never inherited by the child (else EOF never arrives).
    ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    // The editor is a GUI-subsystem process (WinMain, no console), so its std handles are NULL. With
    // STARTF_USESTDHANDLES a NULL stdin makes child tool-probes (cmake's compiler checks) hang/fail —
    // give the child a real, inheritable NUL device for stdin.
    HANDLE nulIn = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = nulIn;  // INVALID_HANDLE_VALUE is acceptable here if NUL couldn't open

    PROCESS_INFORMATION pi{};
    const std::wstring workingDirW = workingDir.empty() ? std::wstring() : workingDir.wstring();

    // CreateProcessW may modify the command-line buffer, so hand it a writable copy.
    std::wstring mutableCmd = cmdLine;
    const BOOL ok = ::CreateProcessW(
        nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        workingDirW.empty() ? nullptr : workingDirW.c_str(), &si, &pi);

    // The parent must drop its copy of the write end so the read loop sees EOF when the child exits.
    ::CloseHandle(writeEnd);
    if (nulIn != INVALID_HANDLE_VALUE)
        ::CloseHandle(nulIn);

    if (!ok)
    {
        ::CloseHandle(readEnd);
        return result;  // launched stays false
    }
    result.launched = true;

    std::array<char, 4096> buffer{};
    DWORD bytesRead = 0;
    while (::ReadFile(readEnd, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) &&
           bytesRead > 0)
        result.output.append(buffer.data(), bytesRead);
    ::CloseHandle(readEnd);

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);

    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return result;
}
}
#endif
