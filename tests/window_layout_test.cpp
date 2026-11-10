#include "test_framework.h"

#include "core/window_layout.h"

using namespace wintools::core;

namespace
{

WindowSnapshot MakeWindow(int left, int top, int width, int height)
{
    WindowSnapshot window;
    window.exePath = L"C:\\Apps\\Code.exe";
    window.appName = L"Code.exe";
    window.title = L"main.cpp";
    window.monitorPath = L"\\\\?\\DISPLAY#AAA#{guid}";
    window.rect = Rect{ left, top, width, height };
    return window;
}

WindowSnapshot MakeDockedWindow(DockMode mode)
{
    WindowSnapshot window = MakeWindow(100, 80, 900, 700);
    window.dock = mode;
    return window;
}

MonitorSnapshot MakeMonitor(int left, int top, int width, int height)
{
    MonitorSnapshot monitor;
    monitor.devicePath = L"\\\\?\\DISPLAY#AAA#{guid}";
    monitor.rect = Rect{ left, top, width, height };
    return monitor;
}

RECT MakeRect(int left, int top, int width, int height)
{
    return RECT{ left, top, left + width, top + height };
}

/// Calls the six-argument planner with the work area equal to the display, which is the common
/// case on a desktop without a taskbar-side dock.
PlacementPlan Plan(const WindowSnapshot& window, const MonitorSnapshot& savedMonitor,
    const Rect& currentMonitor, const RECT& actual)
{
    return PlanPlacement(window, savedMonitor, currentMonitor, currentMonitor, actual, false);
}

} // namespace

// Same display, same position: the window should not be touched.
WINTOOLS_TEST(Placement_NoShift_WhenDisplayUnmoved)
{
    const auto window = MakeWindow(100, 80, 900, 700);
    const auto monitor = MakeMonitor(0, 0, 1920, 1080);
    const RECT actual = MakeRect(100, 80, 900, 700);

    const PlacementPlan plan = Plan(window, monitor, monitor.rect, actual);

    CHECK_EQ(plan.target.left, 100);
    CHECK_EQ(plan.target.top, 80);
    CHECK_EQ(plan.target.right, 1000);
    CHECK_EQ(plan.target.bottom, 780);
    CHECK(!plan.move);
}

// The display moved right by 300 (common when only some panels came back): the window must
// follow it by exactly that delta, keeping its position on the panel.
WINTOOLS_TEST(Placement_ShiftsWithDisplayOrigin)
{
    const auto window = MakeWindow(100, 80, 900, 700);
    const auto savedMonitor = MakeMonitor(0, 0, 1920, 1080);
    const Rect currentMonitor{ 300, 0, 1920, 1080 };
    const RECT actual = MakeRect(100, 80, 900, 700);

    const PlacementPlan plan = Plan(window, savedMonitor, currentMonitor, actual);

    CHECK_EQ(plan.target.left, 400);
    CHECK_EQ(plan.target.right, 1300);
    CHECK_EQ(plan.target.top, 80);
    CHECK_EQ(plan.target.bottom, 780);
    CHECK(plan.move);
}

// Negative origins happen to any monitor placed left of or above the primary one.
WINTOOLS_TEST(Placement_HandlesNegativeDisplayOrigin)
{
    const auto window = MakeWindow(-1500, -800, 800, 600);
    const auto savedMonitor = MakeMonitor(-1920, -1080, 1920, 1080);
    const Rect currentMonitor{ -1920, -1080, 1920, 1080 };
    const RECT actual = MakeRect(-1500, -800, 800, 600);

    const PlacementPlan plan = Plan(window, savedMonitor, currentMonitor, actual);

    CHECK_EQ(plan.target.left, -1500);
    CHECK_EQ(plan.target.top, -800);
    CHECK(!plan.move);
}

// Sub-pixel drift from DWM rounding should not trigger a placement call; a real displacement
// should.
WINTOOLS_TEST(Placement_ToleratesSmallDrift)
{
    const auto window = MakeWindow(100, 80, 900, 700);
    const auto monitor = MakeMonitor(0, 0, 1920, 1080);

    const RECT onePixelOff = MakeRect(101, 81, 900, 700);
    CHECK(!Plan(window, monitor, monitor.rect, onePixelOff).move);

    const RECT barelyInside = MakeRect(102, 82, 900, 700);
    CHECK(!Plan(window, monitor, monitor.rect, barelyInside).move);

    const RECT clearlyOff = MakeRect(110, 80, 900, 700);
    CHECK(Plan(window, monitor, monitor.rect, clearlyOff).move);
}

// A window that a program resized itself must be restored to the captured size, not just moved.
WINTOOLS_TEST(Placement_RestoresSizeNotOnlyPosition)
{
    const auto window = MakeWindow(100, 80, 900, 700);
    const auto monitor = MakeMonitor(0, 0, 1920, 1080);
    const RECT wrongSize = MakeRect(100, 80, 1200, 400);

    const PlacementPlan plan = Plan(window, monitor, monitor.rect, wrongSize);

    CHECK(plan.move);
    CHECK_EQ(plan.target.right - plan.target.left, 900);
    CHECK_EQ(plan.target.bottom - plan.target.top, 700);
}

// Both displays moved relative to the capture, which is the actual scenario this feature exists
// for: power-cycled panels come back in a different order and therefore at different origins.
WINTOOLS_TEST(Placement_EachWindowFollowsItsOwnDisplay)
{
    const auto ultrawide = MakeMonitor(0, 0, 3440, 1440);
    const auto small = MakeMonitor(3440, 0, 1920, 1200);

    const auto windowOnUltrawide = MakeWindow(500, 300, 1200, 900);
    const auto windowOnSmall = MakeWindow(3800, 100, 1000, 800);

    // After the power cycle the small panel is now the primary and sits at the origin.
    const Rect ultrawideNow{ 1920, 0, 3440, 1440 };
    const Rect smallNow{ 0, 0, 1920, 1200 };

    const auto planA = Plan(windowOnUltrawide, ultrawide, ultrawideNow, MakeRect(500, 300, 1200, 900));
    const auto planB = Plan(windowOnSmall, small, smallNow, MakeRect(3800, 100, 1000, 800));

    CHECK_EQ(planA.target.left, 2420);
    CHECK_EQ(planB.target.left, 360);

    // Both are still inside their own display's new rectangle.
    CHECK(planA.target.left >= ultrawideNow.left);
    CHECK(planA.target.right <= ultrawideNow.left + ultrawideNow.width);
    CHECK(planB.target.left >= smallNow.left);
    CHECK(planB.target.right <= smallNow.left + smallNow.width);
}

// --- Dock modes -----------------------------------------------------------------------------
//
// A dock mode is the answer to "the saved coordinates are useless now". It has to ignore the
// saved rectangle completely and derive everything from the display as it is at restore time.

WINTOOLS_TEST(Dock_LeftHalfIgnoresTheSavedRectangle)
{
    // A window that was almost full-screen on the old arrangement must still land in exactly the
    // left half: no inheritance from the captured geometry at all.
    const auto window = MakeDockedWindow(DockMode::Left);
    const auto savedMonitor = MakeMonitor(0, 0, 1920, 1080);
    const Rect currentMonitor{ 2560, 0, 1920, 1200 };
    const RECT actual = MakeRect(100, 80, 900, 700);

    const PlacementPlan plan = Plan(window, savedMonitor, currentMonitor, actual);

    CHECK_EQ(plan.target.left, 2560);
    CHECK_EQ(plan.target.top, 0);
    CHECK_EQ(plan.target.right, 2560 + 960);
    CHECK_EQ(plan.target.bottom, 1200);
    CHECK(plan.move);
    CHECK(!plan.maximized);
    CHECK(!plan.minimized);
}

WINTOOLS_TEST(Dock_OddWidthHalvesTileExactly)
{
    // 1921 splits into 960 and 961, not 960 and 960: a one-pixel seam between snapped windows is
    // the classic off-by-one of half-screen docking.
    const Rect workArea{ 100, 50, 1921, 1081 };

    const Rect left = ComputeDockRect(DockMode::Left, workArea);
    const Rect right = ComputeDockRect(DockMode::Right, workArea);
    const Rect top = ComputeDockRect(DockMode::Top, workArea);
    const Rect bottom = ComputeDockRect(DockMode::Bottom, workArea);

    CHECK_EQ(left.width, 960);
    CHECK_EQ(right.width, 961);
    CHECK_EQ(left.left, workArea.left);
    CHECK_EQ(right.Right(), workArea.Right());
    CHECK_EQ(left.Right(), right.left);

    CHECK_EQ(top.height, 540);
    CHECK_EQ(bottom.height, 541);
    CHECK_EQ(top.top, workArea.top);
    CHECK_EQ(bottom.Bottom(), workArea.Bottom());
    CHECK_EQ(top.Bottom(), bottom.top);
}

WINTOOLS_TEST(Dock_FillCoversTheWorkAreaNotTheDisplay)
{
    // Docking uses the work area so a docked window never slides under the taskbar.
    const auto window = MakeDockedWindow(DockMode::Fill);
    const auto savedMonitor = MakeMonitor(0, 0, 1920, 1080);
    const Rect currentMonitor{ 0, 0, 1920, 1200 };
    const Rect workArea{ 0, 0, 1920, 1160 };

    const PlacementPlan plan = PlanPlacement(
        window, savedMonitor, currentMonitor, workArea, MakeRect(0, 0, 10, 10), false);

    CHECK_EQ(plan.target.left, 0);
    CHECK_EQ(plan.target.top, 0);
    CHECK_EQ(plan.target.right, 1920);
    CHECK_EQ(plan.target.bottom, 1160);
    CHECK(plan.move);
}

WINTOOLS_TEST(Dock_MaximizeRestoresThenMaximizes)
{
    const auto window = MakeDockedWindow(DockMode::Maximize);
    const auto savedMonitor = MakeMonitor(0, 0, 1920, 1080);
    const Rect currentMonitor{ 0, 0, 3440, 1440 };

    // Not maximized yet: it has to be moved and told to maximize.
    const PlacementPlan fresh = Plan(window, savedMonitor, currentMonitor, MakeRect(0, 0, 800, 600));
    CHECK(fresh.move);
    CHECK(fresh.maximized);
    CHECK(!fresh.minimized);

    // Already maximized on the right display: touching it again would make it flash. A maximized
    // window reports its restore-down rectangle, which never equals the target, so this can only
    // be decided from the actual maximized state.
    const PlacementPlan settled = PlanPlacement(window, savedMonitor, currentMonitor,
        currentMonitor, MakeRect(200, 150, 900, 700), true);
    CHECK(!settled.move);
}

WINTOOLS_TEST(Dock_MaximizeOnAnotherDisplayIsMoved)
{
    const auto window = MakeDockedWindow(DockMode::Maximize);
    const auto savedMonitor = MakeMonitor(0, 0, 1920, 1080);
    const Rect currentMonitor{ 1920, 0, 1920, 1080 };

    // Maximized, but on the wrong display: the normal rectangle sits outside the target.
    const PlacementPlan plan = PlanPlacement(window, savedMonitor, currentMonitor, currentMonitor,
        MakeRect(0, 0, 1920, 1080), true);

    CHECK(plan.move);
    CHECK(plan.maximized);
}

// A minimized window that the user docked must come back visible: the dock mode is the more
// recent instruction, and restoring a window into a hidden state looks like the restore failed.
WINTOOLS_TEST(Dock_OverridesCapturedMinimized)
{
    auto window = MakeDockedWindow(DockMode::Right);
    window.minimized = true;

    const auto savedMonitor = MakeMonitor(0, 0, 1920, 1080);

    const PlacementPlan plan = Plan(window, savedMonitor, savedMonitor.rect, MakeRect(0, 0, 10, 10));

    CHECK(!plan.minimized);
    CHECK(plan.move);
}

// A docked window that is already exactly where it belongs must not be moved again: restore runs
// on every display change, and re-placing an already-correct window makes the screen flicker.
WINTOOLS_TEST(Dock_AlreadyInPlaceIsLeftAlone)
{
    const auto window = MakeDockedWindow(DockMode::Bottom);
    const auto monitor = MakeMonitor(0, 0, 1920, 1080);

    const RECT alreadyThere = MakeRect(0, 540, 1920, 540);
    CHECK(!Plan(window, monitor, monitor.rect, alreadyThere).move);

    const RECT onePixelOff = MakeRect(0, 541, 1920, 540);
    CHECK(!Plan(window, monitor, monitor.rect, onePixelOff).move);

    const RECT halfway = MakeRect(0, 200, 1920, 540);
    CHECK(Plan(window, monitor, monitor.rect, halfway).move);
}

// Every mode has to have a label: the panel draws it and the tray menu repeats it, and a null
// would be an empty button with no way to tell what it does.
WINTOOLS_TEST(Dock_EveryModeHasALabel)
{
    for (int mode = 0; mode < static_cast<int>(DockMode::Count); ++mode)
    {
        const std::wstring label = DescribeDock(static_cast<DockMode>(mode));
        CHECK(!label.empty());
    }

    CHECK(!IsDockMode(DockMode::Position));
    CHECK(IsDockMode(DockMode::Left));
    CHECK(IsDockMode(DockMode::Maximize));
}

// The half-screen snap everyone actually uses. Three edges flush, the fourth cut at the middle.
WINTOOLS_TEST(DetectDock_RecognisesTheHalves)
{
    const Rect work{ 0, 0, 1920, 1040 };

    int leftSplit = 0;
    CHECK(DetectDock(MakeRect(0, 0, 960, 1040), work, leftSplit) == DockMode::Left);
    CHECK(leftSplit == 500);

    int rightSplit = 0;
    CHECK(DetectDock(MakeRect(960, 0, 960, 1040), work, rightSplit) == DockMode::Right);
    CHECK(rightSplit == 500);

    int topSplit = 0;
    CHECK(DetectDock(MakeRect(0, 0, 1920, 520), work, topSplit) == DockMode::Top);
    CHECK(topSplit == 500);

    int bottomSplit = 0;
    CHECK(DetectDock(MakeRect(0, 520, 1920, 520), work, bottomSplit) == DockMode::Bottom);
    CHECK(bottomSplit == 500);
}

// A snapped window is measured by its frame, and the frame keeps an invisible resize border of a
// pixel or two on the side that faces the other half. A true half therefore measures 501‰ - and
// storing that restores a pair overlapping by those two pixels, permanently, because the next
// capture measures the overlap and hands back 501 again. Anything within a frame edge of half is
// half; the round trip then tiles exactly and the stored value settles on a plain 500.
WINTOOLS_TEST(DetectDock_SnapsAHalfThatTheWindowFrameWidened)
{
    const Rect work{ 181, -1440, 3440, 1390 };

    int split = 0;
    CHECK(DetectDock(MakeRect(180, -1440, 1722, 1391), work, split) == DockMode::Left);
    CHECK(split == 500);

    // 181 + 1720: the left half ends exactly where the right half starts.
    const Rect restored = ComputeDockRect(DockMode::Left, work, split);
    CHECK(restored.Right() == 1901);

    // Only near-half collapses; a genuinely narrower split keeps its own ratio.
    CHECK(DetectDock(MakeRect(181, -1440, 1500, 1390), work, split) == DockMode::Left);
    CHECK(split == 436);
}

// Snapping to a third is the case that a percentage cannot hold: 1920 / 3 is 640 - a clean
// number - but 1040 / 3 is 346.67, and 33% of 1040 rounds back to 343 and drifts 4px per restore.
WINTOOLS_TEST(DetectDock_KeepsThirdsExact)
{
    const Rect work{ 0, 0, 1920, 1040 };

    int leftSplit = 0;
    CHECK(DetectDock(MakeRect(0, 0, 640, 1040), work, leftSplit) == DockMode::Left);
    CHECK(leftSplit == 333);

    // The right-hand third keeps the far side, so its ratio is what the near side gave up.
    int rightSplit = 0;
    CHECK(DetectDock(MakeRect(1280, 0, 640, 1040), work, rightSplit) == DockMode::Right);
    CHECK(rightSplit == 667);

    // One permille of 1920 is 1.92px, so a third cannot round-trip exactly: 333 permille puts the
    // cut at 639px instead of 640. What matters is that the error is a single pixel and that
    // re-capturing the restored window yields 333 again, so the drift never accumulates.
    Rect restored = ComputeDockRect(DockMode::Left, work, leftSplit);
    CHECK(restored.left == 0);
    CHECK(restored.width >= 639 && restored.width <= 640);

    int resplit = 0;
    static_cast<void>(DetectDock(MakeRect(0, 0, restored.width, 1040), work, resplit));
    CHECK(resplit == leftSplit);

    restored = ComputeDockRect(DockMode::Right, work, rightSplit);
    CHECK(restored.left == 1280);
    CHECK(restored.Right() == 1920);
}

// A window that merely happens to be near an edge is not snapped. It has to be flush on the two
// opposite edges and cut on the third, otherwise the app was never docked by the user.
WINTOOLS_TEST(DetectDock_LeavesFloatingWindowsAlone)
{
    const Rect work{ 0, 0, 1920, 1040 };

    int split = 0;
    CHECK(DetectDock(MakeRect(100, 80, 900, 700), work, split) == DockMode::Position);
    CHECK(split == 500);

    // Flush on all four edges is a full-screen window, not half of anything.
    CHECK(DetectDock(MakeRect(0, 0, 1920, 1040), work, split) == DockMode::Fill);

    // Two pixels of slop is tolerated; three is a real gap.
    CHECK(DetectDock(MakeRect(2, 0, 958, 1040), work, split) == DockMode::Left);
    CHECK(DetectDock(MakeRect(3, 0, 957, 1040), work, split) == DockMode::Position);
}

// Full height and flush left at an odd width is what a half-snapped window looks like after the
// user drags its inner edge, so it is a dock, not a floating window - and the ratio it carries
// has to rebuild the very same rectangle. This is the case the old "half or nothing" model lost.
WINTOOLS_TEST(DetectDock_KeepsAResizedSnapAtItsOwnRatio)
{
    const Rect work{ 0, 0, 1920, 1040 };

    int split = 0;
    const RECT resized = MakeRect(0, 0, 900, 1040);
    CHECK(DetectDock(resized, work, split) == DockMode::Left);
    CHECK(split == 469);

    const Rect restored = ComputeDockRect(DockMode::Left, work, split);
    CHECK(restored.left == resized.left);
    CHECK(restored.width == resized.right - resized.left);
    CHECK(restored.height == resized.bottom - resized.top);
}

// A sliver too thin to be a snap layout would round-trip to a nonsense ratio and be applied as
// something wider than the user left it, so it is downgraded to a plain position.
WINTOOLS_TEST(DetectDock_RejectsSlivers)
{
    const Rect work{ 0, 0, 1920, 1040 };

    int split = 0;
    CHECK(DetectDock(MakeRect(0, 0, 80, 1040), work, split) == DockMode::Position);
    CHECK(split == 500);
}

// --- Automatic restore: bring windows home, do not re-align the ones already there -------------
//
// The case that motivated this: a monitor went to sleep and came back on the same port, Windows
// left the windows alone, and the restore applied the captured layout over the split the user had
// arranged since. Every wake undid the work, and re-capturing could not help because the capture
// happened after the flattening.

// A window on its own display, on a display that has not changed size, is the user's business.
WINTOOLS_TEST(Homecoming_LeavesAHandArrangedWindowWhereItIs)
{
    // Captured as "covers the display"; now the user has it snapped to the left 38%.
    auto window = MakeDockedWindow(DockMode::Fill);
    const auto monitor = MakeMonitor(0, 0, 3440, 1440);

    const RECT rearranged = MakeRect(0, 0, 1300, 1400);
    const PlacementPlan plan = PlanPlacement(window, monitor, monitor.rect, monitor.rect,
        rearranged, false, RestoreScope::Homecoming);

    CHECK(!plan.move);
    CHECK(!plan.maximized);
}

// The same window under the exact scope - the panel's 恢复 button - is put back where it was.
WINTOOLS_TEST(Homecoming_ExactScopeStillAppliesTheLayout)
{
    auto window = MakeDockedWindow(DockMode::Fill);
    const auto monitor = MakeMonitor(0, 0, 3440, 1440);

    const RECT rearranged = MakeRect(0, 0, 1300, 1400);
    const PlacementPlan plan = PlanPlacement(window, monitor, monitor.rect, monitor.rect,
        rearranged, false, RestoreScope::Exact);

    CHECK(plan.move);
    CHECK_EQ(plan.target.right, 3440);
}

// A window that was squeezed onto another display still has to come home - that is the half of
// the feature that earns its keep.
WINTOOLS_TEST(Homecoming_BringsBackAWindowOnTheWrongDisplay)
{
    auto window = MakeDockedWindow(DockMode::Left);
    window.splitPermille = 380;
    const auto savedMonitor = MakeMonitor(181, -1440, 3440, 1440);

    // Its display is back, and the window is sitting on the primary one in between.
    const Rect currentMonitor{ 181, -1440, 3440, 1440 };
    const Rect workArea{ 181, -1440, 3440, 1390 };
    const RECT stranded = MakeRect(200, 100, 900, 700);

    const PlacementPlan plan = PlanPlacement(window, savedMonitor, currentMonitor, workArea,
        stranded, false, RestoreScope::Homecoming);

    CHECK(plan.move);
    CHECK_EQ(plan.target.left, 181);
    CHECK_EQ(plan.target.right, 181 + 1307);
}

// A display that came back at a different resolution has moved everything inside it, so the
// captured dock is applied even though the window never left that screen.
WINTOOLS_TEST(Homecoming_AppliesTheLayoutWhenTheDisplayChangedSize)
{
    auto window = MakeDockedWindow(DockMode::Fill);
    const auto savedMonitor = MakeMonitor(0, 0, 3440, 1440);
    const Rect currentMonitor{ 0, 0, 2560, 1440 };

    const RECT stale = MakeRect(0, 0, 2000, 1400);
    const PlacementPlan plan = PlanPlacement(window, savedMonitor, currentMonitor,
        currentMonitor, stale, false, RestoreScope::Homecoming);

    CHECK(plan.move);
    CHECK_EQ(plan.target.right, 2560);
}
