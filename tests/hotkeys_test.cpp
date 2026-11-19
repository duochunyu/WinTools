#include "test_framework.h"

#include "core/hotkeys.h"

using namespace wintools::core;

WINTOOLS_TEST(Hotkeys_DefaultsAreCtrlAltMnemonic)
{
    const Hotkeys hotkeys = DefaultHotkeys();

    const Hotkey& note = hotkeys.At(HotkeyAction::NewNote);
    const Hotkey& capture = hotkeys.At(HotkeyAction::Capture);
    const Hotkey& save = hotkeys.At(HotkeyAction::CaptureLayout);
    const Hotkey& restore = hotkeys.At(HotkeyAction::ApplyLayout);

    CHECK(note.Bound());
    CHECK_EQ(note.modifiers, 0x0003u); // MOD_CONTROL | MOD_ALT
    CHECK_EQ(note.vk, static_cast<unsigned int>('N'));
    CHECK_EQ(capture.modifiers, 0x0003u);
    CHECK_EQ(capture.vk, static_cast<unsigned int>('S'));
    CHECK_EQ(save.modifiers, 0x0003u);
    CHECK_EQ(save.vk, static_cast<unsigned int>('L'));
    CHECK_EQ(restore.modifiers, 0x0003u);
    CHECK_EQ(restore.vk, static_cast<unsigned int>('R'));
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
    CHECK(DescribeHotkey(Hotkey{ 0x0002, 112 }) == L"Ctrl + F1"); // 112 = VK_F1
}

WINTOOLS_TEST(Hotkeys_KeyLabelNamesTheKeysPeoplePress)
{
    // The whole point of the table: F2 must read as F2, not as the number 113 it is under the
    // hood. Terry reported exactly that after binding the capture shortcut.
    CHECK(KeyLabel(0x71) == L"F2");
    CHECK(KeyLabel(0x70) == L"F1");
    CHECK(KeyLabel(0x87) == L"F24");

    // Letters and digits keep their own character rather than going through a table.
    CHECK(KeyLabel('N') == L"N");
    CHECK(KeyLabel('0') == L"0");

    // Raw codes, not VK_* names: the test harness deliberately includes no Windows header.
    CHECK(KeyLabel(0x20) == L"Space");       // VK_SPACE
    CHECK(KeyLabel(0xC0) == L"`");           // VK_OEM_3
    CHECK(KeyLabel(0x65) == L"Num5");        // VK_NUMPAD5

    // Arrows are glyphs: no language table can make them clearer.
    CHECK(KeyLabel(0x25) == L"\u2190");      // VK_LEFT

    // A key with no familiar name still has to show something, and a wrong name would be worse
    // than the number the platform uses.
    CHECK(KeyLabel(0xFE) == L"VK254");
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
    CHECK(HotkeyActionLabel(HotkeyAction::CaptureLayout) == L"获取布局");
    CHECK(HotkeyActionLabel(HotkeyAction::ApplyLayout) == L"恢复布局");

    // A blank row in the settings popup is indistinguishable from a bug, so every action in the
    // list has to answer. This also fails the day someone adds an action and forgets the label.
    for (const HotkeyAction action : kHotkeyActions)
    {
        CHECK(!HotkeyActionLabel(action).empty());
    }
}

WINTOOLS_TEST(Hotkeys_IdsAreContiguousAndStable)
{
    // RegisterHotKey ids and the ids baked into settings.txt are these numbers - a gap or a
    // renumbering silently rebinds somebody's shortcut.
    CHECK_EQ(static_cast<int>(kHotkeyActions.size()), 4);
    for (std::size_t i = 0; i < kHotkeyActions.size(); ++i)
    {
        CHECK_EQ(static_cast<int>(kHotkeyActions[i]), static_cast<int>(i) + 1);
    }
}
