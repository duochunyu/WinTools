#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace wintools::core
{

/// The actions a global shortcut can trigger. The order is the order shown in the settings popup,
/// and the values are the hotkey ids registered with RegisterHotKey - keep them stable.
enum class HotkeyAction : int
{
    NewNote = 1,
    Capture = 2,
};

inline constexpr std::array<HotkeyAction, 2> kHotkeyActions{ HotkeyAction::NewNote,
    HotkeyAction::Capture };

/// One shortcut: a set of Win32 MOD_* flags and a virtual key. A zero vk means "not bound", which
/// is how a user clears a shortcut.
struct Hotkey
{
    unsigned int modifiers{};
    unsigned int vk{};

    [[nodiscard]] bool Bound() const
    {
        return vk != 0;
    }

    [[nodiscard]] bool operator==(const Hotkey& other) const
    {
        return modifiers == other.modifiers && vk == other.vk;
    }
};

/// All configurable shortcuts, indexed by HotkeyAction - 1.
struct Hotkeys
{
    std::array<Hotkey, kHotkeyActions.size()> bindings{};

    [[nodiscard]] Hotkey& At(HotkeyAction action)
    {
        return bindings[static_cast<std::size_t>(action) - 1];
    }

    [[nodiscard]] const Hotkey& At(HotkeyAction action) const
    {
        return bindings[static_cast<std::size_t>(action) - 1];
    }
};

/// Ctrl+Alt+N and Ctrl+Alt+S. Both include Alt so they are unlikely to collide with an app's own
/// Ctrl+N / Ctrl+S; RegisterHotKey reports a collision, which the settings popup shows rather than
/// silently doing nothing.
[[nodiscard]] Hotkeys DefaultHotkeys();

/// "mods,vk" - numeric, because these are Win32 bit flags and virtual-key codes, not words a user
/// is expected to type. An unparsable value leaves that binding unbound.
[[nodiscard]] Hotkey ParseHotkey(std::wstring_view text);

[[nodiscard]] std::wstring FormatHotkey(const Hotkey& hotkey);

/// "Ctrl + Alt + N", or "未设置" when unbound. Display only; the language table translates the
/// modifier names and the unbound label at the edge.
[[nodiscard]] std::wstring DescribeHotkey(const Hotkey& hotkey);

/// Which action a key combination belongs to. Returns nullptr when it matches nothing.
[[nodiscard]] const Hotkey* FindHotkey(const Hotkeys& hotkeys, unsigned int modifiers, unsigned int vk);

/// Name of one action, for the settings rows.
[[nodiscard]] std::wstring HotkeyActionLabel(HotkeyAction action);

} // namespace wintools::core
