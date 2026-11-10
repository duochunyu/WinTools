#pragma once

#include <cwctype>
#include <string>
#include <string_view>

namespace wintools::core
{

/// One monitor as the shell reports it through IDesktopWallpaper.
///
/// The rectangle comes from IDesktopWallpaper rather than from DisplayConfig on purpose: it is
/// already in the coordinate space the wallpaper is mapped against, so the cropping math never
/// has to reconcile two different sources.
struct Monitor
{
    /// Full shell device path, including the trailing "#{interface-class-guid}" segment.
    std::wstring devicePath;

    /// devicePath with the trailing "#{...}" removed.
    ///
    /// This is the form DisplayConfig reports as monitorDevicePath and the form the settings UI
    /// writes. Keeping both lets a lookup accept either convention without the caller caring.
    std::wstring monitorKey;

    int left{};
    int top{};
    int width{};
    int height{};

    [[nodiscard]] int Right() const
    {
        return left + width;
    }

    [[nodiscard]] int Bottom() const
    {
        return top + height;
    }

    /// Strips the "#{interface-class-guid}" suffix the shell appends to every monitor id.
    ///
    /// DisplayConfig and the Settings UI both work with the prefix, so the cut is from the last
    /// "#{" to the end. Returns the input unchanged when there is no such suffix.
    [[nodiscard]] static std::wstring BuildKey(std::wstring_view devicePath)
    {
        const auto brace = devicePath.rfind(L"#{");
        if (brace == std::wstring_view::npos)
        {
            return std::wstring{ devicePath };
        }

        return std::wstring{ devicePath.substr(0, brace) };
    }

    /// True when key identifies this monitor in either the full or the stripped form.
    [[nodiscard]] bool MatchesKey(std::wstring_view key) const
    {
        if (key.empty())
        {
            return false;
        }

        return IEquals(key, devicePath) || IEquals(key, monitorKey);
    }

    /// Case-insensitive comparison, hand-rolled so this header stays free of Windows.h.
    [[nodiscard]] static bool IEquals(std::wstring_view lhs, std::wstring_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            if (std::towlower(lhs[i]) != std::towlower(rhs[i]))
            {
                return false;
            }
        }

        return true;
    }
};

} // namespace wintools::core
