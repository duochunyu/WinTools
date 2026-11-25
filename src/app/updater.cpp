#include "app/updater.h"

#include "app/commands.h"

#include <Windows.h>

#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <system_error>

#include "util/log.h"

namespace wintools::app
{
namespace
{

constexpr DWORD kExitWaitMs = 30'000;

bool ParsePid(std::wstring_view text, DWORD& pid)
{
    if (text.empty() || text.size() > 10)
    {
        return false;
    }
    unsigned long long value = 0;
    for (const wchar_t c : text)
    {
        if (c < L'0' || c > L'9')
        {
            return false;
        }
        value = value * 10 + static_cast<unsigned long long>(c - L'0');
        if (value > 0xFFFFFFFFULL)
        {
            return false;
        }
    }
    pid = static_cast<DWORD>(value);
    return true;
}

std::wstring Quoted(const std::filesystem::path& path)
{
    return std::format(L"\"{}\"", path.wstring());
}

bool Launch(const std::filesystem::path& executable, std::wstring_view arguments,
    std::wstring& error)
{
    std::wstring command = Quoted(executable);
    if (!arguments.empty())
    {
        command += L" ";
        command += arguments;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)
        == FALSE)
    {
        error = std::format(L"cannot start {}: Win32 {}", executable.wstring(), ::GetLastError());
        return false;
    }

    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

} // namespace

bool StartUpdaterProcess(const std::filesystem::path& target,
    const std::filesystem::path& package, unsigned long parentPid, std::wstring& error)
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / std::format(L"wintools-updater-{}", parentPid);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec)
    {
        error = L"cannot create the updater folder";
        return false;
    }

    const std::filesystem::path copy = folder / L"WinTools.exe";
    std::filesystem::copy_file(target, copy, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        error = L"cannot copy the running executable";
        return false;
    }

    const std::wstring arguments = std::format(L"--apply-update {} {} {}", parentPid,
        Quoted(target), Quoted(package));
    if (!Launch(copy, arguments, error))
    {
        return false;
    }

    util::LogInfo(L"updater started from {}", copy.wstring());
    return true;
}

int RunUpdater(std::wstring_view pidText, std::wstring_view target, std::wstring_view package)
{
    DWORD pid = 0;
    if (!ParsePid(pidText, pid))
    {
        util::LogError(L"updater received an invalid parent pid");
        return kBadArguments;
    }
    if (target.empty() || package.empty())
    {
        util::LogError(L"updater received an empty path");
        return kBadArguments;
    }

    util::LogInfo(L"updater waiting for parent {}", pid);
    HANDLE parent = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (parent == nullptr)
    {
        const DWORD error = ::GetLastError();
        if (error == ERROR_INVALID_PARAMETER)
        {
            // The parent has already gone; there is nothing left to lock the exe.
            util::LogInfo(L"parent {} already exited", pid);
        }
        else
        {
            util::LogError(L"updater cannot wait for parent {}: Win32 {}", pid, error);
            return kEnvironmentProblem;
        }
    }
    else
    {
        const DWORD waited = ::WaitForSingleObject(parent, kExitWaitMs);
        ::CloseHandle(parent);
        if (waited == WAIT_TIMEOUT)
        {
            util::LogError(L"parent {} is still running; leaving the package untouched", pid);
            return kRuntimeFailure;
        }
        if (waited != WAIT_OBJECT_0)
        {
            util::LogError(L"waiting for parent {} failed: Win32 {}", pid, ::GetLastError());
            return kRuntimeFailure;
        }
    }

    const std::filesystem::path targetPath{ target };
    const std::filesystem::path packagePath{ package };
    const std::filesystem::path backup = targetPath.wstring() + L".bak";

    if (::CopyFileW(targetPath.c_str(), backup.c_str(), FALSE) == FALSE)
    {
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_EXISTS)
        {
            util::LogError(L"updater cannot back up the old exe: Win32 {}", error);
            return kRuntimeFailure;
        }
        ::DeleteFileW(backup.c_str());
        if (::CopyFileW(targetPath.c_str(), backup.c_str(), FALSE) == FALSE)
        {
            util::LogError(L"updater cannot replace the old backup: Win32 {}", ::GetLastError());
            return kRuntimeFailure;
        }
    }

    if (::MoveFileExW(packagePath.c_str(), targetPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
        == FALSE)
    {
        util::LogError(L"updater cannot replace the exe: Win32 {}", ::GetLastError());
        return kRuntimeFailure;
    }

    std::wstring error;
    if (!Launch(targetPath, L"--tray", error))
    {
        util::LogError(L"{}", error);
        return kRuntimeFailure;
    }

    util::LogInfo(L"update applied; old exe kept at {}", backup.wstring());
    return kSuccess;
}

} // namespace wintools::app
