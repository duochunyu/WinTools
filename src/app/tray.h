#pragma once

#include <Windows.h>
#include <shellapi.h>

#include <string>
#include <string_view>

namespace wintools::app
{

/// Thin wrapper over Shell_NotifyIcon.
///
/// Owns nothing but the icon itself: the menu, the commands and what they mean live in the
/// service, so the tray can be reasoned about (and restarted after an Explorer crash) without
/// knowing anything about wallpapers or window layouts.
class TrayIcon
{
public:
    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;
    TrayIcon(TrayIcon&&) = delete;
    TrayIcon& operator=(TrayIcon&&) = delete;

    /// callbackMessage is delivered to owner with the raw mouse message in lParam.
    bool Add(HWND owner, HICON icon, std::wstring_view tooltip, UINT callbackMessage);

    /// Re-adds after Explorer restarts. Every tray icon in the system disappears when the
    /// taskbar is rebuilt; the shell broadcasts "TaskbarCreated" and expects apps to notice.
    bool ReAdd();

    void Remove();
    void SetTooltip(std::wstring_view tooltip);
    void ShowInfo(std::wstring_view title, std::wstring_view text);

    [[nodiscard]] bool IsVisible() const
    {
        return m_added;
    }

private:
    NOTIFYICONDATAW m_data{};
    std::wstring m_tooltip;
    bool m_added{};
};

} // namespace wintools::app
