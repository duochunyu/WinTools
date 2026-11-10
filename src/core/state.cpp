#include "core/state.h"

#include <format>
#include <fstream>
#include <system_error>

#include "util/log.h"
#include "util/text.h"

namespace wintools::core
{

namespace
{

constexpr wchar_t kKeyPosition[] = L"position=";
constexpr wchar_t kKeyGlobal[] = L"global=";
constexpr wchar_t kKeyMonitor[] = L"monitor=";

} // namespace

bool WallpaperState::Save(const std::filesystem::path& file) const
{
    std::error_code ec;
    if (file.has_parent_path())
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec)
        {
            util::LogError(L"cannot create '{}': {}", file.parent_path().wstring(), util::FromUtf8(ec.message()));
            return false;
        }
    }

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        util::LogError(L"cannot open '{}' for writing", file.wstring());
        return false;
    }

    // devicePath contains no tabs and no newlines, so tab-separated lines are unambiguous.
    std::wstring text;
    text += std::format(L"{}{}\n", kKeyPosition, static_cast<int>(position));
    text += std::format(L"{}{}\n", kKeyGlobal, globalWallpaper);
    for (const auto& [devicePath, image] : perMonitor)
    {
        text += std::format(L"{}{}\t{}\n", kKeyMonitor, devicePath, image);
    }

    const std::string utf8 = util::ToUtf8(text);
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    out.flush();

    if (!out)
    {
        util::LogError(L"cannot write '{}'", file.wstring());
        return false;
    }

    return true;
}

std::optional<WallpaperState> WallpaperState::Load(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }

    const std::string utf8{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    std::wstring text = util::FromUtf8(utf8);

    WallpaperState state;
    size_t offset = 0;

    while (offset < text.size())
    {
        const size_t end = text.find(L'\n', offset);
        const std::wstring line = text.substr(offset, end == std::wstring::npos ? std::wstring::npos : end - offset);
        offset = (end == std::wstring::npos) ? text.size() : end + 1;

        if (line.starts_with(kKeyPosition))
        {
            state.position = static_cast<WallpaperPosition>(std::stoi(line.substr(std::size(kKeyPosition) - 1)));
        }
        else if (line.starts_with(kKeyGlobal))
        {
            state.globalWallpaper = line.substr(std::size(kKeyGlobal) - 1);
        }
        else if (line.starts_with(kKeyMonitor))
        {
            const std::wstring body = line.substr(std::size(kKeyMonitor) - 1);
            const size_t tab = body.find(L'\t');
            if (tab != std::wstring::npos && tab > 0)
            {
                state.perMonitor.emplace_back(body.substr(0, tab), body.substr(tab + 1));
            }
        }
    }

    return state;
}

WallpaperState CaptureState(const DesktopWallpaper& wallpaper)
{
    WallpaperState state;
    if (!wallpaper.GetPosition(state.position))
    {
        util::LogWarn(L"could not read the current wallpaper position; assuming Fill");
    }

    for (const auto& monitor : wallpaper.Enumerate())
    {
        const std::wstring path = wallpaper.GetWallpaper(&monitor.devicePath);
        if (!path.empty())
        {
            state.perMonitor.emplace_back(monitor.devicePath, path);
        }
    }

    state.globalWallpaper = wallpaper.GetWallpaper(nullptr);

    if (state.IsEmpty())
    {
        // Nothing readable through COM. Fall back to the shell's transcoded copy so a restore
        // still has an image to point at instead of silently doing nothing.
        state.globalWallpaper = GetShellWallpaperFile();
    }

    return state;
}

bool RestoreState(DesktopWallpaper& wallpaper, const WallpaperState& state)
{
    if (!wallpaper.IsValid())
    {
        return false;
    }

    bool ok = wallpaper.SetPosition(state.position);

    if (state.IsEmpty())
    {
        util::LogWarn(L"nothing was captured to restore");
        return ok;
    }

    // Assigning the global wallpaper clears every per-monitor assignment, so it has to happen
    // first; the per-monitor entries below then re-apply whatever was individual on top.
    if (!state.globalWallpaper.empty())
    {
        ok = wallpaper.SetWallpaperForAll(state.globalWallpaper) && ok;
    }

    for (const auto& [devicePath, image] : state.perMonitor)
    {
        ok = wallpaper.SetWallpaper(devicePath, image) && ok;
    }

    return ok;
}

} // namespace wintools::core
