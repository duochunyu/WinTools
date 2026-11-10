#include "app/startup.h"

#include <Windows.h>

#include <filesystem>
#include <vector>

#include "app/panel.h"
#include "util/log.h"
#include "util/text.h"

namespace wintools::app
{

namespace
{

constexpr wchar_t kMutexName[] = L"Local\\WinTools.SingleInstance.4f1d9c72";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"WinTools";

/// Held for the life of the process. Deliberately leaked: closing the handle at exit would
/// release the lock while our windows are still being destroyed, letting a second copy start
/// and race with the teardown.
HANDLE g_instanceMutex = nullptr;

std::wstring ExecutablePath()
{
    std::vector<wchar_t> buffer(MAX_PATH);

    while (true)
    {
        const DWORD length =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            return {};
        }

        // Since Windows 10 the call no longer null-terminates when the buffer is exactly full;
        // a length strictly below the capacity is the only reliable success signal.
        if (length < buffer.size() - 1)
        {
            return std::wstring{ buffer.data(), length };
        }

        buffer.resize(buffer.size() * 2);
    }
}

} // namespace

bool ClaimSingleInstance()
{
    // Local\ rather than Global\: two different users may legitimately run their own copy.
    g_instanceMutex = ::CreateMutexW(nullptr, TRUE, kMutexName);
    if (g_instanceMutex == nullptr)
    {
        util::LogWarn(L"CreateMutex failed, continuing without the single-instance lock");
        return true;
    }

    if (::GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return true;
    }

    // Another copy is running. Its panel may be hidden, so ask it to come forward.
    ActivateExistingPanel();
    ::CloseHandle(g_instanceMutex);
    g_instanceMutex = nullptr;
    return false;
}

bool IsAutostartEnabled()
{
    HKEY key{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD type{};
    DWORD bytes{};
    LONG status = ::RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0)
    {
        ::RegCloseKey(key);
        return false;
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    status = ::RegQueryValueExW(
        key, kRunValue, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    ::RegCloseKey(key);

    if (status != ERROR_SUCCESS)
    {
        return false;
    }

    // Only an entry that points at this executable counts. A stale value left behind by a copy
    // that has since moved would otherwise read as "enabled" while never starting us.
    const std::wstring self = ExecutablePath();
    if (self.empty())
    {
        return false;
    }

    const std::wstring value{ buffer.data() };
    return value.find(self) != std::wstring::npos;
}

bool SetAutostart(bool enabled)
{
    HKEY key{};
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
            nullptr)
        != ERROR_SUCCESS)
    {
        util::LogError(L"cannot open the Run key for writing");
        return false;
    }

    LONG status = ERROR_SUCCESS;
    if (enabled)
    {
        const std::wstring command = L"\"" + ExecutablePath() + L"\" --tray";
        status = ::RegSetValueExW(key, kRunValue, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        status = ::RegDeleteValueW(key, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND)
        {
            status = ERROR_SUCCESS;
        }
    }

    ::RegCloseKey(key);

    if (status != ERROR_SUCCESS)
    {
        util::LogError(L"cannot update the Run value: {}", util::FormatHresult(status));
        return false;
    }

    return true;
}

} // namespace wintools::app
