#include "app/settings.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "util/log.h"
#include "util/path.h"
#include "util/text.h"

namespace wintools::app
{

namespace
{

constexpr wchar_t kKeyWallpaperSource[] = L"wallpaper.source=";
constexpr wchar_t kKeyZoom[] = L"wallpaper.zoom=";
constexpr wchar_t kKeyFocusX[] = L"wallpaper.focusX=";
constexpr wchar_t kKeyFocusY[] = L"wallpaper.focusY=";
constexpr wchar_t kKeyAutoRestore[] = L"layout.autoRestore=";
constexpr wchar_t kKeyStartWithWindows[] = L"startWithWindows=";
constexpr wchar_t kKeyDebounce[] = L"layout.debounceSeconds=";
constexpr wchar_t kKeyLastAutoRestore[] = L"layout.lastAutoRestore=";
constexpr wchar_t kKeyTheme[] = L"theme=";
constexpr wchar_t kKeyActiveGroup[] = L"wallpaper.activeGroup=";
constexpr wchar_t kKeyLanguage[] = L"language=";
constexpr wchar_t kKeyCanvas[] = L"canvas.enabled=";
constexpr wchar_t kKeyCanvasX[] = L"canvas.x=";
constexpr wchar_t kKeyCanvasY[] = L"canvas.y=";
constexpr wchar_t kKeyCanvasSize[] = L"canvas.size=";
constexpr wchar_t kKeyCanvasSweep[] = L"canvas.sweep=";
constexpr wchar_t kKeyHotkeyNewNote[] = L"hotkey.newNote=";
constexpr wchar_t kKeyHotkeyCapture[] = L"hotkey.capture=";

std::wstring Trim(const std::wstring& text)
{
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
    {
        return {};
    }

    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool ToBool(const std::wstring& value)
{
    return value == L"1" || value == L"true" || value == L"yes";
}

/// Parses one floating-point field, leaving the destination alone when the text is not a number.
///
/// std::stod accepts "1e400" and returns an infinity rather than throwing, and it accepts leading
/// garbage-free prefixes like "3abc". Both are clamped away by ClampView afterwards, so only the
/// exception path needs handling here.
void ToDouble(const std::wstring& value, double& target)
{
    try
    {
        target = std::stod(value);
    }
    catch (...)
    {
        // Keep the default.
    }
}

} // namespace

std::filesystem::path Settings::FilePath()
{
    return util::AppDataFolder() / L"settings.txt";
}

Settings Settings::Load()
{
    Settings settings;

    const std::filesystem::path file = FilePath();
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        // First run: defaults are already in place.
        return settings;
    }

    const std::string utf8{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    const std::wstring text = util::FromUtf8(utf8);

    size_t offset = 0;
    while (offset < text.size())
    {
        const size_t end = text.find(L'\n', offset);
        const std::wstring line =
            Trim(text.substr(offset, end == std::wstring::npos ? std::wstring::npos : end - offset));
        offset = (end == std::wstring::npos) ? text.size() : end + 1;

        if (line.empty() || line.starts_with(L"#"))
        {
            continue;
        }

        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos)
        {
            continue;
        }

        const std::wstring key = line.substr(0, equals + 1);
        const std::wstring value = Trim(line.substr(equals + 1));

        if (key == kKeyWallpaperSource)
        {
            settings.wallpaperSource = value;
        }
        else if (key == kKeyZoom)
        {
            ToDouble(value, settings.wallpaperView.zoom);
        }
        else if (key == kKeyFocusX)
        {
            ToDouble(value, settings.wallpaperView.focusX);
        }
        else if (key == kKeyFocusY)
        {
            ToDouble(value, settings.wallpaperView.focusY);
        }
        else if (key == kKeyAutoRestore)
        {
            settings.autoRestoreLayout = ToBool(value);
        }
        else if (key == kKeyStartWithWindows)
        {
            settings.startWithWindows = ToBool(value);
        }
        else if (key == kKeyLastAutoRestore)
        {
            settings.lastAutoRestore = value;
        }
        else if (key == kKeyDebounce)
        {
            try
            {
                settings.debounceSeconds = std::clamp(std::stoi(value), 1, 30);
            }
            catch (...)
            {
                // Keep the default; a corrupt number is not worth failing a load over.
            }
        }
        else if (key == kKeyTheme)
        {
            try
            {
                settings.theme = std::clamp(std::stoi(value), 0, 2);
            }
            catch (...)
            {
            }
        }
        else if (key == kKeyActiveGroup)
        {
            try
            {
                settings.activeGroup = std::max(0, std::stoi(value));
            }
            catch (...)
            {
            }
        }
        else if (key == kKeyLanguage)
        {
            try
            {
                settings.language = std::clamp(std::stoi(value), 0, 2);
            }
            catch (...)
            {
            }
        }
        else if (key == kKeyCanvas)
        {
            settings.canvasEnabled = ToBool(value);
        }
        else if (key == kKeyCanvasX)
        {
            try
            {
                settings.canvas.x = std::stoi(value);
            }
            catch (...)
            {
            }
        }
        else if (key == kKeyCanvasY)
        {
            try
            {
                settings.canvas.y = std::stoi(value);
            }
            catch (...)
            {
            }
        }
        else if (key == kKeyCanvasSize)
        {
            try
            {
                settings.canvas.size = core::ClampCanvasSize(std::stoi(value));
            }
            catch (...)
            {
            }
        }
        else if (key == kKeyCanvasSweep)
        {
            settings.canvasSweep = ToBool(value);
        }
        else if (key == kKeyHotkeyNewNote)
        {
            // An absent or unparsable value keeps the default, so settings written before
            // shortcuts existed do not silently lose both bindings on upgrade.
            const core::Hotkey parsed = core::ParseHotkey(value);
            if (parsed.Bound() || value == L"0,0")
            {
                settings.hotkeys.At(core::HotkeyAction::NewNote) = parsed;
            }
        }
        else if (key == kKeyHotkeyCapture)
        {
            const core::Hotkey parsed = core::ParseHotkey(value);
            if (parsed.Bound() || value == L"0,0")
            {
                settings.hotkeys.At(core::HotkeyAction::Capture) = parsed;
            }
        }
    }

    // A hand-edited file can put anything in the three framing fields; the renderer refuses
    // nonsense but the panel draws with these values directly, so they are made sane once here.
    settings.wallpaperView = core::ClampView(settings.wallpaperView);
    return settings;
}

bool Settings::Save() const
{
    const std::filesystem::path file = FilePath();

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

    // Written in full every time. The file is six lines; a merge-in-place implementation would
    // only add a way to be wrong.
    std::wstring text;
    text += L"# WinTools settings\n";
    text += std::format(L"{}{}\n", kKeyWallpaperSource, wallpaperSource);
    text += std::format(L"{}{:.4f}\n", kKeyZoom, wallpaperView.zoom);
    text += std::format(L"{}{:.4f}\n", kKeyFocusX, wallpaperView.focusX);
    text += std::format(L"{}{:.4f}\n", kKeyFocusY, wallpaperView.focusY);
    text += std::format(L"{}{}\n", kKeyAutoRestore, autoRestoreLayout ? 1 : 0);
    text += std::format(L"{}{}\n", kKeyStartWithWindows, startWithWindows ? 1 : 0);
    text += std::format(L"{}{}\n", kKeyDebounce, debounceSeconds);
    text += std::format(L"{}{}\n", kKeyLastAutoRestore, lastAutoRestore);
    text += std::format(L"{}{}\n", kKeyTheme, theme);
    text += std::format(L"{}{}\n", kKeyActiveGroup, activeGroup);
    text += std::format(L"{}{}\n", kKeyLanguage, language);
    text += std::format(L"{}{}\n", kKeyCanvas, canvasEnabled ? 1 : 0);
    text += std::format(L"{}{}\n", kKeyCanvasX, canvas.x);
    text += std::format(L"{}{}\n", kKeyCanvasY, canvas.y);
    text += std::format(L"{}{}\n", kKeyCanvasSize, canvas.size);
    text += std::format(L"{}{}\n", kKeyCanvasSweep, canvasSweep ? 1 : 0);
    text += std::format(L"{}{}\n", kKeyHotkeyNewNote,
        core::FormatHotkey(hotkeys.At(core::HotkeyAction::NewNote)));
    text += std::format(L"{}{}\n", kKeyHotkeyCapture,
        core::FormatHotkey(hotkeys.At(core::HotkeyAction::Capture)));

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

} // namespace wintools::app
