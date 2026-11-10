#include "app/tray.h"

#include <algorithm>
#include <cwchar>

#include "util/log.h"

namespace wintools::app
{

namespace
{

constexpr UINT kIconId = 1;

void CopyInto(wchar_t* destination, size_t capacity, std::wstring_view source)
{
    // NOTIFYICONDATAW's string fields are fixed-size arrays; an over-long tooltip is silently
    // truncated by the shell, but truncating it ourselves keeps the terminator in place.
    const size_t count = std::min(source.size(), capacity - 1);
    std::wmemcpy(destination, source.data(), count);
    destination[count] = L'\0';
}

} // namespace

TrayIcon::~TrayIcon()
{
    Remove();
}

bool TrayIcon::Add(HWND owner, HICON icon, std::wstring_view tooltip, UINT callbackMessage)
{
    if (m_added)
    {
        return true;
    }

    m_tooltip.assign(tooltip);

    m_data = {};
    m_data.cbSize = sizeof(NOTIFYICONDATAW);
    m_data.hWnd = owner;
    m_data.uID = kIconId;
    m_data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    m_data.uCallbackMessage = callbackMessage;
    m_data.hIcon = icon;
    CopyInto(m_data.szTip, ARRAYSIZE(m_data.szTip), m_tooltip);

    m_added = ::Shell_NotifyIconW(NIM_ADD, &m_data) != FALSE;
    if (!m_added)
    {
        util::LogError(L"Shell_NotifyIcon(NIM_ADD) failed; the tray icon will not appear");
    }

    return m_added;
}

bool TrayIcon::ReAdd()
{
    if (m_data.hWnd == nullptr)
    {
        return false;
    }

    m_added = false;
    m_data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    CopyInto(m_data.szTip, ARRAYSIZE(m_data.szTip), m_tooltip);

    m_added = ::Shell_NotifyIconW(NIM_ADD, &m_data) != FALSE;
    return m_added;
}

void TrayIcon::Remove()
{
    if (!m_added)
    {
        return;
    }

    ::Shell_NotifyIconW(NIM_DELETE, &m_data);
    m_added = false;
}

void TrayIcon::SetTooltip(std::wstring_view tooltip)
{
    if (m_tooltip == tooltip)
    {
        return;
    }

    m_tooltip.assign(tooltip);
    CopyInto(m_data.szTip, ARRAYSIZE(m_data.szTip), m_tooltip);

    if (!m_added)
    {
        return;
    }

    m_data.uFlags = NIF_TIP | NIF_SHOWTIP;
    ::Shell_NotifyIconW(NIM_MODIFY, &m_data);
}

void TrayIcon::ShowInfo(std::wstring_view title, std::wstring_view text)
{
    if (!m_added)
    {
        return;
    }

    m_data.uFlags = NIF_INFO;
    m_data.dwInfoFlags = NIIF_NOSOUND | NIIF_RESPECT_QUIET_TIME;
    CopyInto(m_data.szInfoTitle, ARRAYSIZE(m_data.szInfoTitle), title);
    CopyInto(m_data.szInfo, ARRAYSIZE(m_data.szInfo), text);

    ::Shell_NotifyIconW(NIM_MODIFY, &m_data);

    // NIF_INFO is a one-shot: leaving the flag set would re-show the balloon on the next
    // tooltip update.
    m_data.uFlags = 0;
}

} // namespace wintools::app
