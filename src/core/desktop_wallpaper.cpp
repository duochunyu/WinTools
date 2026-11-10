#include "core/desktop_wallpaper.h"

#include <Windows.h>
#include <ShlObj.h>

#include "util/log.h"
#include "util/text.h"

namespace wintools::core
{

namespace
{

// Defined here rather than pulled from a GUID header so the build never depends on uuid.lib
// being in the right order. The value is the documented CLSID for the desktop wallpaper COM
// object, the same one the PowerToys C# implementation hard-codes.
const CLSID kDesktopWallpaperClsid = {
    0xC2CF3110, 0x460E, 0x4fc1, { 0xB9, 0xD0, 0x8A, 0x1C, 0x0C, 0x9C, 0xC4, 0xBD }
};

/// Takes ownership of a CoTaskMemAlloc'd string produced by the shell.
std::wstring TakeCoString(LPWSTR raw)
{
    if (raw == nullptr)
    {
        return {};
    }

    std::wstring result{ raw };
    ::CoTaskMemFree(raw);
    return result;
}

} // namespace

DesktopWallpaper::DesktopWallpaper()
{
    const HRESULT hr = ::CoCreateInstance(
        kDesktopWallpaperClsid,
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(m_wallpaper.ReleaseAndGetAddressOf()));

    if (FAILED(hr))
    {
        util::LogError(L"cannot create the desktop wallpaper COM object: {}", util::FormatHresult(hr));
    }
}

std::vector<Monitor> DesktopWallpaper::Enumerate() const
{
    std::vector<Monitor> monitors;

    if (m_wallpaper == nullptr)
    {
        return monitors;
    }

    UINT count = 0;
    HRESULT hr = m_wallpaper->GetMonitorDevicePathCount(&count);
    if (FAILED(hr))
    {
        util::LogError(L"GetMonitorDevicePathCount failed: {}", util::FormatHresult(hr));
        return monitors;
    }

    monitors.reserve(count);

    for (UINT index = 0; index < count; ++index)
    {
        LPWSTR rawPath = nullptr;
        hr = m_wallpaper->GetMonitorDevicePathAt(index, &rawPath);
        if (FAILED(hr))
        {
            util::LogWarn(L"GetMonitorDevicePathAt({}) failed: {}", index, util::FormatHresult(hr));
            continue;
        }

        const std::wstring devicePath = TakeCoString(rawPath);
        if (devicePath.empty())
        {
            continue;
        }

        RECT rect{};
        hr = m_wallpaper->GetMonitorRECT(devicePath.c_str(), &rect);
        if (FAILED(hr))
        {
            util::LogWarn(L"GetMonitorRECT('{}') failed: {}", devicePath, util::FormatHresult(hr));
            continue;
        }

        Monitor monitor;
        monitor.devicePath = devicePath;
        monitor.monitorKey = Monitor::BuildKey(devicePath);
        monitor.left = rect.left;
        monitor.top = rect.top;
        monitor.width = rect.right - rect.left;
        monitor.height = rect.bottom - rect.top;
        monitors.push_back(std::move(monitor));
    }

    return monitors;
}

bool DesktopWallpaper::GetPosition(WallpaperPosition& position) const
{
    position = WallpaperPosition::Fill;

    if (m_wallpaper == nullptr)
    {
        return false;
    }

    DESKTOP_WALLPAPER_POSITION raw{};
    const HRESULT hr = m_wallpaper->GetPosition(&raw);
    if (FAILED(hr))
    {
        util::LogError(L"GetPosition failed: {}", util::FormatHresult(hr));
        return false;
    }

    position = static_cast<WallpaperPosition>(raw);
    return true;
}

bool DesktopWallpaper::SetPosition(WallpaperPosition position)
{
    if (m_wallpaper == nullptr)
    {
        return false;
    }

    const HRESULT hr = m_wallpaper->SetPosition(static_cast<DESKTOP_WALLPAPER_POSITION>(position));
    if (FAILED(hr))
    {
        util::LogError(
            L"SetPosition({}) failed: {}", static_cast<int>(position), util::FormatHresult(hr));
        return false;
    }

    return true;
}

bool DesktopWallpaper::SetWallpaper(const std::wstring& devicePath, const std::filesystem::path& image)
{
    if (m_wallpaper == nullptr)
    {
        return false;
    }

    if (devicePath.empty() || image.empty())
    {
        return false;
    }

    const HRESULT hr = m_wallpaper->SetWallpaper(devicePath.c_str(), image.c_str());
    if (FAILED(hr))
    {
        util::LogError(
            L"SetWallpaper('{}', '{}') failed: {}",
            devicePath,
            image.filename().wstring(),
            util::FormatHresult(hr));
        return false;
    }

    return true;
}

bool DesktopWallpaper::SetWallpaperForAll(const std::filesystem::path& image)
{
    if (m_wallpaper == nullptr || image.empty())
    {
        return false;
    }

    const HRESULT hr = m_wallpaper->SetWallpaper(nullptr, image.c_str());
    if (FAILED(hr))
    {
        util::LogError(L"SetWallpaper(all) failed: {}", util::FormatHresult(hr));
        return false;
    }

    return true;
}

std::wstring DesktopWallpaper::GetWallpaper(const std::wstring* devicePath) const
{
    if (m_wallpaper == nullptr)
    {
        return {};
    }

    LPWSTR raw = nullptr;
    const HRESULT hr = m_wallpaper->GetWallpaper(devicePath != nullptr ? devicePath->c_str() : nullptr, &raw);
    if (FAILED(hr))
    {
        // Not worth an error line: the shell returns a failure for "nothing assigned here",
        // which is a normal state and not a malfunction.
        if (raw != nullptr)
        {
            ::CoTaskMemFree(raw);
        }
        return {};
    }

    return TakeCoString(raw);
}

std::wstring GetShellWallpaperFile()
{
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)))
    {
        return {};
    }

    std::filesystem::path path{ raw };
    ::CoTaskMemFree(raw);

    path /= L"Microsoft";
    path /= L"Windows";
    path /= L"Themes";
    path /= L"TranscodedWallpaper";

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return {};
    }

    return path.wstring();
}

} // namespace wintools::core
