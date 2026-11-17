#include "core/hotkeys.h"

#include <format>
#include <sstream>

namespace wintools::core
{

namespace
{

constexpr unsigned int kCtrl = 0x0002; // MOD_CONTROL
constexpr unsigned int kAlt = 0x0001;  // MOD_ALT
constexpr unsigned int kShift = 0x0004;
constexpr unsigned int kWin = 0x0008;

bool ToUint(std::wstring_view text, unsigned int& out)
{
    try
    {
        const unsigned long value = std::stoul(std::wstring{ text });
        out = static_cast<unsigned int>(value);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace

Hotkeys DefaultHotkeys()
{
    Hotkeys hotkeys;
    hotkeys.At(HotkeyAction::NewNote) = Hotkey{ kCtrl | kAlt, 'N' };
    hotkeys.At(HotkeyAction::Capture) = Hotkey{ kCtrl | kAlt, 'S' };
    return hotkeys;
}

Hotkey ParseHotkey(std::wstring_view text)
{
    Hotkey hotkey;
    const std::size_t comma = text.find(L',');
    if (comma == std::wstring_view::npos)
    {
        return hotkey;
    }

    unsigned int modifiers = 0;
    unsigned int vk = 0;
    if (!ToUint(text.substr(0, comma), modifiers)
        || !ToUint(text.substr(comma + 1), vk))
    {
        return hotkey;
    }

    // Only the four modifiers RegisterHotKey accepts. Anything else came from a corrupt file and
    // would make registration fail, so it is dropped rather than remembered.
    hotkey.modifiers = modifiers & (kCtrl | kAlt | kShift | kWin);
    // Virtual keys are 1..255; letter codes ('N'=78) live there too. Zero and anything larger are
    // not real keys, so an out-of-range value falls back to "unbound".
    hotkey.vk = (vk > 0 && vk < 256) ? vk : 0;
    return hotkey;
}

std::wstring FormatHotkey(const Hotkey& hotkey)
{
    if (!hotkey.Bound())
    {
        return L"0,0";
    }

    return std::format(L"{},{}", hotkey.modifiers, hotkey.vk);
}

std::wstring DescribeHotkey(const Hotkey& hotkey)
{
    if (!hotkey.Bound())
    {
        return L"未设置";
    }

    std::wstring text;
    if (hotkey.modifiers & kCtrl)
    {
        text += L"Ctrl + ";
    }
    if (hotkey.modifiers & kAlt)
    {
        text += L"Alt + ";
    }
    if (hotkey.modifiers & kShift)
    {
        text += L"Shift + ";
    }
    if (hotkey.modifiers & kWin)
    {
        text += L"Win + ";
    }

    // Letters and digits are the useful hotkeys; for everything else the raw VK number is still
    // honest, if less friendly, than an invented name.
    if (hotkey.vk >= 'A' && hotkey.vk <= 'Z')
    {
        text += static_cast<wchar_t>(hotkey.vk);
    }
    else if (hotkey.vk >= '0' && hotkey.vk <= '9')
    {
        text += static_cast<wchar_t>(hotkey.vk);
    }
    else
    {
        text += std::format(L"VK{}", hotkey.vk);
    }

    return text;
}

const Hotkey* FindHotkey(const Hotkeys& hotkeys, unsigned int modifiers, unsigned int vk)
{
    for (const Hotkey& hotkey : hotkeys.bindings)
    {
        if (hotkey.Bound() && hotkey.modifiers == modifiers && hotkey.vk == vk)
        {
            return &hotkey;
        }
    }

    return nullptr;
}

std::wstring HotkeyActionLabel(HotkeyAction action)
{
    switch (action)
    {
        case HotkeyAction::NewNote:
            return L"新建便签";
        case HotkeyAction::Capture:
            return L"框选截图";
    }

    return {};
}

} // namespace wintools::core
