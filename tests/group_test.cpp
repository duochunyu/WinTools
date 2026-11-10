#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/group.h"

using namespace wintools::core;

namespace
{

/// A file that cleans up after itself; the persistence format is the thing under test.
class TempFile
{
public:
    explicit TempFile(const std::wstring& name)
        : m_path(std::filesystem::temp_directory_path() / name)
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

/// Four displays in the arrangement the design was drawn against: two on top, two below.
/// Named so the device path already matches the screen number - D1/D2 are the top row, D3/D4 the
/// bottom one - because a fixture whose names disagree with its reading order makes every
/// assertion below harder to read than the code it checks.
std::vector<MonitorSnapshot> FourScreens()
{
    return {
        MonitorSnapshot{ L"D1", Rect{ 0, -1200, 1920, 1200 } },
        MonitorSnapshot{ L"D2", Rect{ 1920, -1200, 1920, 1200 } },
        MonitorSnapshot{ L"D3", Rect{ 0, 0, 1920, 1200 } },
        MonitorSnapshot{ L"D4", Rect{ 1920, 0, 1920, 1200 } },
    };
}

Group MakeGroup(std::vector<std::wstring> members)
{
    Group group;
    group.members = std::move(members);
    return group;
}

std::wstring LabelOf(const std::vector<Group>& groups, size_t index,
    const std::vector<MonitorSnapshot>& monitors)
{
    return GroupLabel(groups[index], monitors);
}

} // namespace

WINTOOLS_TEST(Group_ReadingOrderIsLeftToRightThenTopToBottom)
{
    const auto monitors = FourScreens();
    const auto order = ReadingOrder(monitors);

    CHECK_EQ(order.size(), size_t{ 4 });
    CHECK_EQ(monitors[order[0]].devicePath, std::wstring{ L"D1" });
    CHECK_EQ(monitors[order[1]].devicePath, std::wstring{ L"D2" });
    CHECK_EQ(monitors[order[2]].devicePath, std::wstring{ L"D3" });
    CHECK_EQ(monitors[order[3]].devicePath, std::wstring{ L"D4" });

    CHECK_EQ(ScreenNumber(L"D1", monitors), 1);
    CHECK_EQ(ScreenNumber(L"D3", monitors), 3);
    CHECK_EQ(ScreenNumber(L"D4", monitors), 4);
    CHECK_EQ(ScreenNumber(L"gone", monitors), 0);
    CHECK_EQ(ScreenNumber(L"", monitors), 0);
}

WINTOOLS_TEST(Group_ReadingOrderIsRowsNotColumns)
{
    // The same 2x2 desk with the displays handed over shuffled: reading order must still be
    // 1 2 / 3 4 and not 1 3 / 2 4, which is what sorting by x alone produces.
    const std::vector<MonitorSnapshot> shuffled{
        MonitorSnapshot{ L"bottom-right", Rect{ 1920, 0, 1920, 1200 } },
        MonitorSnapshot{ L"top-right", Rect{ 1920, -1200, 1920, 1200 } },
        MonitorSnapshot{ L"top-left", Rect{ 0, -1200, 1920, 1200 } },
        MonitorSnapshot{ L"bottom-left", Rect{ 0, 0, 1920, 1200 } },
    };

    const auto order = ReadingOrder(shuffled);

    CHECK_EQ(shuffled[order[0]].devicePath, std::wstring{ L"top-left" });
    CHECK_EQ(shuffled[order[1]].devicePath, std::wstring{ L"top-right" });
    CHECK_EQ(shuffled[order[2]].devicePath, std::wstring{ L"bottom-left" });
    CHECK_EQ(shuffled[order[3]].devicePath, std::wstring{ L"bottom-right" });
}

WINTOOLS_TEST(Group_LabelUsesAscendingScreenNumbers)
{
    const auto monitors = FourScreens();

    // Members stored out of order, and named the way the design writes them.
    const auto groups = std::vector<Group>{ MakeGroup({ L"D4", L"D3" }) };
    CHECK_EQ(LabelOf(groups, 0, monitors), std::wstring{ L"3+4" });

    const auto first = std::vector<Group>{ MakeGroup({ L"D2", L"D1" }) };
    CHECK_EQ(LabelOf(first, 0, monitors), std::wstring{ L"1+2" });

    const auto all = std::vector<Group>{ MakeGroup({ L"D1", L"D2", L"D3", L"D4" }) };
    CHECK_EQ(LabelOf(all, 0, monitors), std::wstring{ L"1+2+3+4" });
}

WINTOOLS_TEST(Group_LabelIsEmptyWhenNoMemberIsAttached)
{
    const auto monitors = FourScreens();
    const auto groups = std::vector<Group>{ MakeGroup({ L"unplugged-a", L"unplugged-b" }) };
    CHECK_EQ(LabelOf(groups, 0, monitors), std::wstring{});
}

WINTOOLS_TEST(Group_NormaliseAssignsNewDisplayToTheFirstGroup)
{
    auto monitors = FourScreens();
    std::vector<Group> groups{ MakeGroup({ L"D1", L"D2" }), MakeGroup({ L"D3" }) };

    // D4 was plugged in after the groups were written.
    NormaliseGroups(groups, monitors);

    CHECK_EQ(groups.size(), size_t{ 2 });
    CHECK_EQ(groups[0].members.size(), size_t{ 3 });
    CHECK_EQ(GroupOf(groups, L"D4"), 0);
    CHECK_EQ(groups[0].slot, 0);
    CHECK_EQ(groups[1].slot, 1);
}

WINTOOLS_TEST(Group_NormaliseDropsEmptyGroupsAndRenumbersSlots)
{
    auto monitors = FourScreens();
    std::vector<Group> groups{ MakeGroup({}), MakeGroup({ L"D1", L"D2" }), MakeGroup({}) };

    NormaliseGroups(groups, monitors);

    CHECK_EQ(groups.size(), size_t{ 1 });
    CHECK_EQ(groups[0].slot, 0);

    // The two displays that were in no group followed the survivors into the first one.
    CHECK_EQ(GroupOf(groups, L"D3"), 0);
    CHECK_EQ(GroupOf(groups, L"D4"), 0);
}

WINTOOLS_TEST(Group_NormaliseKeepsADisplayInOnlyOneGroup)
{
    auto monitors = FourScreens();
    std::vector<Group> groups{ MakeGroup({ L"D1", L"D2" }), MakeGroup({ L"D2", L"D3" }) };

    NormaliseGroups(groups, monitors);

    CHECK_EQ(GroupOf(groups, L"D2"), 0);
    CHECK_EQ(groups[1].members.size(), size_t{ 1 });
    CHECK_EQ(groups[1].members[0], std::wstring{ L"D3" });
}

WINTOOLS_TEST(Group_NormaliseKeepsUnpluggedMembersInTheirPlace)
{
    // A display that is away keeps its membership, so plugging it back in puts it where it was
    // instead of dropping it into the first group.
    const std::vector<MonitorSnapshot> attached{ MonitorSnapshot{ L"D1", Rect{ 0, 0, 1920, 1200 } } };
    std::vector<Group> groups{ MakeGroup({ L"D1" }), MakeGroup({ L"D9" }) };

    NormaliseGroups(groups, attached);

    CHECK_EQ(groups.size(), size_t{ 2 });
    CHECK_EQ(groups[1].members.size(), size_t{ 1 });
    CHECK_EQ(groups[1].members[0], std::wstring{ L"D9" });
    CHECK_EQ(LabelOf(groups, 1, attached), std::wstring{});
}

WINTOOLS_TEST(Group_SplitMovesOneDisplayIntoAGroupOfItsOwn)
{
    const auto monitors = FourScreens();
    std::vector<Group> groups{ MakeGroup({ L"D3", L"D4" }) };

    const int slot = MoveMonitorToGroup(groups, L"D4", -1);

    CHECK_EQ(slot, 1);
    CHECK_EQ(groups.size(), size_t{ 2 });
    CHECK_EQ(LabelOf(groups, 0, monitors), std::wstring{ L"3" });
    CHECK_EQ(LabelOf(groups, 1, monitors), std::wstring{ L"4" });
    CHECK_EQ(groups[1].slot, 1);
}

WINTOOLS_TEST(Group_SplitIsANoOpWhenTheDisplayIsAlreadyAlone)
{
    std::vector<Group> groups{ MakeGroup({ L"D1", L"D2" }), MakeGroup({ L"D3" }) };

    const int slot = MoveMonitorToGroup(groups, L"D3", -1);

    CHECK_EQ(slot, 1);
    CHECK_EQ(groups.size(), size_t{ 2 });
}

WINTOOLS_TEST(Group_MergeEmptiesTheSourceGroupAndRenumbers)
{
    const auto monitors = FourScreens();
    std::vector<Group> groups{ MakeGroup({ L"D1" }), MakeGroup({ L"D3" }) };

    // Move 1 into the second group; the first group has no members left and disappears, so the
    // merged group moves from slot 1 to slot 0.
    const int slot = MoveMonitorToGroup(groups, L"D1", 1);

    CHECK_EQ(slot, 0);
    CHECK_EQ(groups.size(), size_t{ 1 });
    CHECK_EQ(LabelOf(groups, 0, monitors), std::wstring{ L"1+3" });
    CHECK_EQ(groups[0].slot, 0);
}

WINTOOLS_TEST(Group_MergeIntoALaterGroupKeepsThatGroupsIdentity)
{
    const auto monitors = FourScreens();
    std::vector<Group> groups{ MakeGroup({ L"D1", L"D2" }), MakeGroup({ L"D3", L"D4" }) };

    const int slot = MoveMonitorToGroup(groups, L"D2", 1);

    CHECK_EQ(slot, 1);
    CHECK_EQ(groups.size(), size_t{ 2 });
    CHECK_EQ(LabelOf(groups, 0, monitors), std::wstring{ L"1" });
    CHECK_EQ(LabelOf(groups, 1, monitors), std::wstring{ L"2+3+4" });
}

WINTOOLS_TEST(Group_MoveRejectsAnUnknownGroup)
{
    std::vector<Group> groups{ MakeGroup({ L"D1", L"D2" }) };

    CHECK_EQ(MoveMonitorToGroup(groups, L"D1", 7), -1);
    CHECK_EQ(MoveMonitorToGroup(groups, L"missing", 0), -1);
    CHECK_EQ(groups[0].members.size(), size_t{ 2 });
}

WINTOOLS_TEST(Group_SaveLoadRoundTrip)
{
    const TempFile file(L"wintools_groups_test.tsv");

    Group first;
    first.image = L"D:\\pics\\wide.png";
    first.view = WallpaperView{ 2.5, 0.25, 0.75 };
    first.members = { L"\\\\?\\DISPLAY#A", L"\\\\?\\DISPLAY#B" };
    first.dirty = true;

    Group second;
    second.members = { L"\\\\?\\DISPLAY#C" };

    std::vector<Group> written{ first, second };
    CHECK(SaveGroups(written, file.Path()));

    const auto read = LoadGroups(file.Path());
    CHECK(read.has_value());
    if (!read.has_value())
    {
        return;
    }

    CHECK_EQ(read->size(), size_t{ 2 });
    CHECK_EQ((*read)[0].image, std::wstring{ L"D:\\pics\\wide.png" });
    CHECK((*read)[0].view.zoom > 2.49 && (*read)[0].view.zoom < 2.51);
    CHECK((*read)[0].view.focusX > 0.249 && (*read)[0].view.focusX < 0.251);
    CHECK((*read)[0].view.focusY > 0.749 && (*read)[0].view.focusY < 0.751);
    CHECK((*read)[0].dirty);
    CHECK_EQ((*read)[0].members.size(), size_t{ 2 });
    CHECK_EQ((*read)[0].members[1], std::wstring{ L"\\\\?\\DISPLAY#B" });
    CHECK_EQ((*read)[1].image, std::wstring{});
    CHECK(!(*read)[1].dirty);
}

WINTOOLS_TEST(Group_LoadRejectsAFileWithNoGroups)
{
    const TempFile file(L"wintools_groups_empty.tsv");

    {
        std::ofstream out(file.Path(), std::ios::binary | std::ios::trunc);
        out << "# WinTools groups v1\n\n";
    }

    CHECK(!LoadGroups(file.Path()).has_value());
    CHECK(!LoadGroups(std::filesystem::temp_directory_path() / L"nope-not-here.tsv").has_value());
}

WINTOOLS_TEST(Group_LegacyMigrationPutsOverridesInTheirOwnGroups)
{
    const auto monitors = FourScreens();

    MonitorWallpapers overrides;
    overrides.push_back(MonitorWallpaper{ L"D4", L"C:\\own.jpg" });

    const auto groups = GroupsFromLegacy(monitors, L"C:\\wide.jpg",
        WallpaperView{ 1.5, 0.2, 0.5 }, overrides);

    CHECK_EQ(groups.size(), size_t{ 2 });
    CHECK_EQ(LabelOf(groups, 0, monitors), std::wstring{ L"1+2+3" });
    CHECK_EQ(groups[0].image, std::wstring{ L"C:\\wide.jpg" });
    CHECK(groups[0].view.zoom > 1.49);
    CHECK_EQ(LabelOf(groups, 1, monitors), std::wstring{ L"4" });
    CHECK_EQ(groups[1].image, std::wstring{ L"C:\\own.jpg" });
}

WINTOOLS_TEST(Group_LegacyMigrationWithNothingStoredStillGroupsEveryDisplay)
{
    const auto monitors = FourScreens();
    const auto groups = GroupsFromLegacy(monitors, {}, WallpaperView{}, {});

    CHECK_EQ(groups.size(), size_t{ 1 });
    CHECK_EQ(LabelOf(groups, 0, monitors), std::wstring{ L"1+2+3+4" });
    CHECK_EQ(groups[0].image, std::wstring{});
}
