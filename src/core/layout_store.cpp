#include "core/layout_store.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>
#include <vector>

#include "util/log.h"
#include "util/path.h"
#include "util/text.h"

namespace wintools::core
{

namespace
{

/// The version marker lives in the first line; a file without it is version 1 and simply has no
/// dock column. Added rather than required: a file written by an older build must keep loading,
/// because the layout is the one thing the user cannot easily recreate from memory after an
/// update.
///
/// v3 inserts the split percentage after the dock mode. A v2 file has no split column, and its
/// dock values were all 50% halves by construction, so the default fills the gap exactly.
constexpr wchar_t kHeaderV2[] = L"# WinTools layout v2";
constexpr wchar_t kHeaderV3[] = L"# WinTools layout v3";

/// Tabs and newlines would break the record format, and neither belongs in a window title.
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

std::vector<std::wstring> SplitTabs(const std::wstring& line)
{
    std::vector<std::wstring> parts;
    size_t start = 0;

    while (true)
    {
        const size_t tab = line.find(L'\t', start);
        if (tab == std::wstring::npos)
        {
            parts.push_back(line.substr(start));
            break;
        }

        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }

    return parts;
}

bool TryParseInt(const std::wstring& text, int& value)
{
    try
    {
        value = std::stoi(text);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace

std::filesystem::path DefaultLayoutFile()
{
    return util::AppDataFolder() / L"layout.txt";
}

bool SaveLayout(const Layout& layout, const std::filesystem::path& file)
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

    std::wstring text;
    text += kHeaderV3;
    text += L'\n';

    for (const auto& monitor : layout.monitors)
    {
        text += std::format(
            L"monitor\t{}\t{}\t{}\t{}\t{}\n",
            monitor.rect.left,
            monitor.rect.top,
            monitor.rect.width,
            monitor.rect.height,
            monitor.devicePath);
    }

    for (const auto& window : layout.windows)
    {
        std::wstring flags;
        if (window.minimized)
        {
            flags += L'm';
        }
        if (window.maximized)
        {
            flags += L'x';
        }
        if (!window.enabled)
        {
            flags += L'-';
        }
        if (flags.empty())
        {
            flags = L"0";
        }

        text += std::format(
            L"window\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
            window.rect.left,
            window.rect.top,
            window.rect.width,
            window.rect.height,
            flags,
            static_cast<int>(window.dock),
            std::clamp(window.splitPermille, kMinSplitPermille, kMaxSplitPermille),
            window.monitorPath,
            window.exePath,
            Sanitize(window.title));
    }

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        util::LogError(L"cannot open '{}' for writing", file.wstring());
        return false;
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

std::optional<Layout> LoadLayout(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }

    const std::string utf8{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    const std::wstring text = util::FromUtf8(utf8);

    Layout layout;
    size_t offset = 0;
    int version = 1;

    while (offset < text.size())
    {
        const size_t end = text.find(L'\n', offset);
        std::wstring line =
            text.substr(offset, end == std::wstring::npos ? std::wstring::npos : end - offset);
        offset = (end == std::wstring::npos) ? text.size() : end + 1;

        // A file written by us has bare LF, but anything that has been through Notepad, an editor
        // or a git checkout will be CRLF. Without this the carriage return sticks to the last
        // field of every record and the monitor device paths stop matching, which then looks like
        // "every display is missing" rather than like a parse error.
        if (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }

        if (line.empty())
        {
            continue;
        }

        if (line.starts_with(L"#"))
        {
            if (line.starts_with(kHeaderV3))
            {
                version = 3;
            }
            else if (line.starts_with(kHeaderV2))
            {
                version = 2;
            }
            continue;
        }

        const auto parts = SplitTabs(line);
        if (parts.empty())
        {
            continue;
        }

        if (parts[0] == L"monitor" && parts.size() >= 6)
        {
            MonitorSnapshot monitor;
            if (!TryParseInt(parts[1], monitor.rect.left) || !TryParseInt(parts[2], monitor.rect.top)
                || !TryParseInt(parts[3], monitor.rect.width) || !TryParseInt(parts[4], monitor.rect.height))
            {
                continue;
            }

            monitor.devicePath = parts[5];
            layout.monitors.push_back(std::move(monitor));
        }
        else if (parts[0] != L"window")
        {
            continue;
        }
        else
        {
            // Version 1 records run `... flags monitor exe title`; version 2 inserts the dock mode
            // between flags and monitor, version 3 the split percentage after it. The version is
            // what decides where each column starts, not the column count: a title can contain
            // tabs, so counting would be ambiguous exactly when it matters.
            const size_t dockIndex = 6;
            const size_t splitIndex = 7;
            const size_t monitorIndex = (version >= 3) ? 8 : (version >= 2 ? 7 : 6);
            const size_t exeIndex = monitorIndex + 1;
            const size_t titleIndex = exeIndex + 1;

            if (parts.size() <= titleIndex)
            {
                continue;
            }

            WindowSnapshot window;
            if (!TryParseInt(parts[1], window.rect.left) || !TryParseInt(parts[2], window.rect.top)
                || !TryParseInt(parts[3], window.rect.width) || !TryParseInt(parts[4], window.rect.height))
            {
                continue;
            }

            const std::wstring& flags = parts[5];
            window.minimized = flags.find(L'm') != std::wstring::npos;
            window.maximized = flags.find(L'x') != std::wstring::npos;
            window.enabled = flags.find(L'-') == std::wstring::npos;

            if (version >= 2)
            {
                int dock = 0;
                if (!TryParseInt(parts[dockIndex], dock) || dock < 0
                    || dock >= static_cast<int>(DockMode::Count))
                {
                    dock = 0;
                }
                window.dock = static_cast<DockMode>(dock);
            }

            if (version >= 3 && IsDockMode(window.dock))
            {
                int split = kDefaultSplitPermille;
                if (!TryParseInt(parts[splitIndex], split) || split < kMinSplitPermille
                    || split > kMaxSplitPermille)
                {
                    split = kDefaultSplitPermille;
                }
                window.splitPermille = split;
            }

            window.monitorPath = parts[monitorIndex];
            window.exePath = parts[exeIndex];

            // Everything past the exe is the title; a title containing a tab (rare but possible)
            // would otherwise be silently truncated.
            std::wstring title;
            for (size_t index = titleIndex; index < parts.size(); ++index)
            {
                if (!title.empty())
                {
                    title += L'\t';
                }
                title += parts[index];
            }
            window.title = title;

            const size_t slash = window.exePath.find_last_of(L"\\/");
            window.appName = (slash == std::wstring::npos) ? window.exePath : window.exePath.substr(slash + 1);

            layout.windows.push_back(std::move(window));
        }
    }

    if (layout.monitors.empty() && layout.windows.empty())
    {
        return std::nullopt;
    }

    return layout;
}

} // namespace wintools::core
