#include "test_framework.h"

#include "core/hotkeys.h"

using namespace wintools::core;

WINTOOLS_TEST(Hotkeys_DefaultsAreCtrlAltNAndS)
{
    const Hotkeys hotkeys = DefaultHotkeys();

    const Hotkey& note = hotkeys.At(HotkeyAction::NewNote);
    const Hotkey& capture = hotkeys.At(HotkeyAction::Capture);

    CHECK(note.Bound());
    CHECK_EQ(note.modifiers, 0x0003u); // MOD_CONTROL | MOD_ALT
    CHECK_EQ(note.vk, static_cast<unsigned int>('N'));
    CHECK_EQ(capture.modifiers, 0x0003u);
    CHECK_EQ(capture.vk, static_cast<unsigned int>('S'));
}

WINTOOLS_TEST(Hotkeys_RoundTripThroughText)
{
    Hotkeys hotkeys;
    hotkeys.At(HotkeyAction::NewNote) = Hotkey{ 0x0007, 'K' };
    hotkeys.At(HotkeyAction::Capture) = Hotkey{};

    const Hotkey note = ParseHotkey(FormatHotkey(hotkeys.At(HotkeyAction::NewNote)));
    const Hotkey capture = ParseHotkey(FormatHotkey(hotkeys.At(HotkeyAction::Capture)));

    CHECK(note == hotkeys.At(HotkeyAction::NewNote));
    CHECK(!capture.Bound());
}

WINTOOLS_TEST(Hotkeys_RejectsGarbageAndUnknownModifiers)
{
    CHECK(!ParseHotkey(L"nonsense").Bound());
    CHECK(!ParseHotkey(L"3").Bound());
    CHECK(!ParseHotkey(L"abc,78").Bound());

    // Bits outside the four RegisterHotKey accepts are dropped, not remembered.
    const Hotkey odd = ParseHotkey(L"255,78");
    CHECK_EQ(odd.modifiers, 0x000Fu);
    CHECK_EQ(odd.vk, static_cast<unsigned int>('N'));

    // Zero and values past the virtual-key range are not usable for a global hotkey.
    CHECK(!ParseHotkey(L"3,0").Bound());
    CHECK(!ParseHotkey(L"3,300").Bound());
}

WINTOOLS_TEST(Hotkeys_DescribeShowsEveryModifier)
{
    const Hotkey all{ 0x000F, 'Z' };
    CHECK(DescribeHotkey(all) == L"Ctrl + Alt + Shift + Win + Z");
    CHECK(DescribeHotkey(Hotkey{}) == L"未设置");
    CHECK(DescribeHotkey(Hotkey{ 0x0002, '0' }) == L"Ctrl + 0");
    CHECK(DescribeHotkey(Hotkey{ 0x0002, 112 }) == L"Ctrl + VK112"); // 112 = VK_F1
}

WINTOOLS_TEST(Hotkeys_FindMatchesOnlyBoundCombination)
{
    const Hotkeys hotkeys = DefaultHotkeys();

    CHECK(FindHotkey(hotkeys, 0x0003, 'N') == &hotkeys.At(HotkeyAction::NewNote));
    CHECK(FindHotkey(hotkeys, 0x0003, 'S') == &hotkeys.At(HotkeyAction::Capture));
    CHECK(FindHotkey(hotkeys, 0x0002, 'N') == nullptr); // missing Alt
    CHECK(FindHotkey(hotkeys, 0x0003, 'X') == nullptr); // unbound key
}

WINTOOLS_TEST(Hotkeys_ActionLabelsAreStable)
{
    CHECK(HotkeyActionLabel(HotkeyAction::NewNote) == L"新建便签");
    CHECK(HotkeyActionLabel(HotkeyAction::Capture) == L"框选截图");
}
