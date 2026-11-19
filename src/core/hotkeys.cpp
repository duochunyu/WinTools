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

/// The virtual keys worth naming, as (vk, label). A-Z and 0-9 are handled by their own characters
/// and are not in here; everything else that a keyboard actually has is, because "VK113" is what a
/// user gets otherwise - their own F2 staring back at them as a number they did not press.
struct KeyName
{
    unsigned int vk;
    const wchar_t* label;
};

constexpr KeyName kKeyNames[] = {
    // Function keys, 0x70..0x87. VK_F1 is 0x70, so the label is the offset plus one.
    { 0x70, L"F1" }, { 0x71, L"F2" }, { 0x72, L"F3" }, { 0x73, L"F4" }, { 0x74, L"F5" },
    { 0x75, L"F6" }, { 0x76, L"F7" }, { 0x77, L"F8" }, { 0x78, L"F9" }, { 0x79, L"F10" },
    { 0x7A, L"F11" }, { 0x7B, L"F12" }, { 0x7C, L"F13" }, { 0x7D, L"F14" }, { 0x7E, L"F15" },
    { 0x7F, L"F16" }, { 0x80, L"F17" }, { 0x81, L"F18" }, { 0x82, L"F19" }, { 0x83, L"F20" },
    { 0x84, L"F21" }, { 0x85, L"F22" }, { 0x86, L"F23" }, { 0x87, L"F24" },

    // Editing and navigation.
    { 0x08, L"Backspace" }, { 0x09, L"Tab" }, { 0x0D, L"Enter" }, { 0x1B, L"Esc" },
    { 0x20, L"Space" }, { 0x2D, L"Insert" }, { 0x2E, L"Delete" }, { 0x24, L"Home" },
    { 0x23, L"End" }, { 0x21, L"PgUp" }, { 0x22, L"PgDn" },
    // Arrows as glyphs rather than words: they read the same in every language table.
    { 0x25, L"\u2190" }, { 0x26, L"\u2191" }, { 0x27, L"\u2192" }, { 0x28, L"\u2193" },

    // Locks, system keys and the numeric keypad.
    { 0x14, L"CapsLock" }, { 0x90, L"NumLock" }, { 0x91, L"ScrLk" }, { 0x2C, L"PrtSc" },
    { 0x13, L"Pause" }, { 0x5B, L"Win" }, { 0x5C, L"Win" }, { 0x5D, L"Menu" },
    { 0x60, L"Num0" }, { 0x61, L"Num1" }, { 0x62, L"Num2" }, { 0x63, L"Num3" },
    { 0x64, L"Num4" }, { 0x65, L"Num5" }, { 0x66, L"Num6" }, { 0x67, L"Num7" },
    { 0x68, L"Num8" }, { 0x69, L"Num9" }, { 0x6A, L"Num*" }, { 0x6B, L"Num+" },
    { 0x6D, L"Num-" }, { 0x6E, L"Num." }, { 0x6F, L"Num/" },

    // Punctuation on a US layout. The labels are what the caps actually show.
    { 0xBB, L"=" }, { 0xBD, L"-" }, { 0xBC, L"," }, { 0xBE, L"." }, { 0xBF, L"/" },
    { 0xC0, L"`" }, { 0xDB, L"[" }, { 0xDC, L"\\" }, { 0xDD, L"]" }, { 0xDE, L"'" },
    { 0xBA, L";" },
};

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

std::wstring KeyLabel(unsigned int vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
    {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }

    for (const KeyName& entry : kKeyNames)
    {
        if (entry.vk == vk)
        {
            return entry.label;
        }
    }

    // Nothing friendly for this one, and a wrong name would be worse than the number.
    return std::format(L"VK{}", vk);
}


Hotkeys DefaultHotkeys()
{
    Hotkeys hotkeys;
    hotkeys.At(HotkeyAction::NewNote) = Hotkey{ kCtrl | kAlt, 'N' };
    hotkeys.At(HotkeyAction::Capture) = Hotkey{ kCtrl | kAlt, 'S' };
    // L for "layout", R for "restore". Both stay inside Ctrl+Alt so they cannot collide with the
    // plain Ctrl+L / Ctrl+R every app already owns.
    hotkeys.At(HotkeyAction::CaptureLayout) = Hotkey{ kCtrl | kAlt, 'L' };
    hotkeys.At(HotkeyAction::ApplyLayout) = Hotkey{ kCtrl | kAlt, 'R' };
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

    text += KeyLabel(hotkey.vk);
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
        case HotkeyAction::CaptureLayout:
            return L"获取布局";
        case HotkeyAction::ApplyLayout:
            return L"恢复布局";
    }

    return {};
}

} // namespace wintools::core
