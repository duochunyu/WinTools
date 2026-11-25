#include "core/wallpaper_store.h"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

#include "core/monitor.h"
#include "util/file.h"
#include "util/log.h"
#include "util/path.h"
#include "util/text.h"

namespace wintools::core
{

namespace
{

constexpr wchar_t kHeader[] = L"# WinTools wallpapers v1";

/// Tabs and newlines would break the record format; neither belongs in a file path.
std::wstring Sanitize(const std::wstring& value)
{
    std::wstring result = value;
    for (wchar_t& character : result)
    {
        if (character == L'\t' || character == L'\r' || character == L'\n')
        {
            character = L' ';
        }
    }
    return result;
}

} // namespace

std::filesystem::path DefaultWallpaperFile()
{
    return util::AppDataFolder() / L"wallpapers.txt";
}

bool SaveMonitorWallpapers(const MonitorWallpapers& list, const std::filesystem::path& file)
{
    std::error_code ec;
    if (file.has_parent_path())
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec)
        {
            util::LogError(L"cannot create '{}': {}", file.parent_path().wstring(),
                util::FromUtf8(ec.message()));
            return false;
        }
    }

    std::wstring text = kHeader;
    text += L'\n';

    for (const auto& entry : list)
    {
        // Rows with no picture are not written at all: "follows the spanning wallpaper" is the
        // absence of a row, which keeps the file readable and makes a stale entry impossible.
        if (entry.image.empty() || entry.devicePath.empty())
        {
            continue;
        }

        text += L"monitor\t";
        text += Sanitize(entry.devicePath);
        text += L'\t';
        text += Sanitize(entry.image);
        text += L'\n';
    }

    return util::AtomicWrite(file, text);
}

std::optional<MonitorWallpapers> LoadMonitorWallpapers(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }

    const std::string utf8{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    const std::wstring text = util::FromUtf8(utf8);

    MonitorWallpapers list;
    size_t offset = 0;

    while (offset < text.size())
    {
        const size_t end = text.find(L'\n', offset);
        std::wstring line =
            text.substr(offset, end == std::wstring::npos ? std::wstring::npos : end - offset);
        offset = (end == std::wstring::npos) ? text.size() : end + 1;

        // Anything that has been through an editor or a git checkout is CRLF, and the carriage
        // return would stick to the end of the path - which reads as "this display has no picture
        // file" rather than as a parse error. Stripped in a loop because a file saved by a tool
        // that translates newlines twice ends up with two of them, and one leftover is enough to
        // make the path invalid rather than merely wrong.
        while (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }

        if (line.empty() || line.starts_with(L"#"))
        {
            continue;
        }

        const size_t first = line.find(L'\t');
        if (first == std::wstring::npos || line.substr(0, first) != L"monitor")
        {
            continue;
        }

        const size_t second = line.find(L'\t', first + 1);
        if (second == std::wstring::npos)
        {
            continue;
        }

        MonitorWallpaper entry;
        entry.devicePath = line.substr(first + 1, second - first - 1);
        entry.image = line.substr(second + 1);

        if (entry.devicePath.empty() || entry.image.empty())
        {
            continue;
        }

        list.push_back(std::move(entry));
    }

    if (list.empty())
    {
        return std::nullopt;
    }

    return list;
}

std::wstring_view FindMonitorImage(const MonitorWallpapers& list, std::wstring_view devicePath)
{
    if (devicePath.empty())
    {
        return {};
    }

    for (const auto& entry : list)
    {
        if (Monitor::IEquals(entry.devicePath, devicePath))
        {
            return entry.image;
        }
    }

    return {};
}

void SetMonitorImage(MonitorWallpapers& list, const std::wstring& devicePath,
    const std::wstring& image)
{
    if (devicePath.empty())
    {
        return;
    }

    const auto same = [&devicePath](const MonitorWallpaper& entry)
    {
        return Monitor::IEquals(entry.devicePath, devicePath);
    };

    if (image.empty())
    {
        std::erase_if(list, same);
        return;
    }

    if (const auto found = std::find_if(list.begin(), list.end(), same); found != list.end())
    {
        found->image = image;
        return;
    }

    list.push_back(MonitorWallpaper{ devicePath, image });
}

} // namespace wintools::core
