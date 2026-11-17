#include "test_framework.h"

#include <string>
#include <vector>

#include "core/canvas.h"

using namespace wintools::core;

namespace
{

MonitorSnapshot MakeMonitor(const wchar_t* path, int left, int top, int width, int height)
{
    MonitorSnapshot snapshot;
    snapshot.devicePath = path;
    snapshot.rect = Rect{ left, top, width, height };
    return snapshot;
}

/// The layout this whole feature is aimed at: a wide display with a portrait one beside it.
std::vector<MonitorSnapshot> TwoDisplays()
{
    return {
        MakeMonitor(L"\\\\?\\DISPLAY#AAA", 0, 0, 1920, 1080),
        MakeMonitor(L"\\\\?\\DISPLAY#BBB", 1920, 0, 1200, 1920),
    };
}

} // namespace

WINTOOLS_TEST(Canvas_ContainsUsesTheHalfOpenSquare)
{
    const CanvasRect rect{ 100, 100, 240 };

    CHECK(CanvasContains(rect, 100, 100));
    CHECK(CanvasContains(rect, 339, 339));
    CHECK(!CanvasContains(rect, 340, 200));
    CHECK(!CanvasContains(rect, 200, 340));
    CHECK(!CanvasContains(rect, 99, 200));
}

WINTOOLS_TEST(Canvas_SizeIsClampedToTheAllowedRange)
{
    CHECK_EQ(ClampCanvasSize(10), kCanvasMinSize);
    CHECK_EQ(ClampCanvasSize(2000), kCanvasMaxSize);
    CHECK_EQ(ClampCanvasSize(240), 240);
}

WINTOOLS_TEST(Canvas_HomeIsTheDisplayUnderItsCentre)
{
    const std::vector<MonitorSnapshot> monitors = TwoDisplays();

    CHECK_EQ(CanvasHomeMonitor(CanvasRect{ 100, 100, 240 }, monitors), 0);

    // Centre at 2100: inside the portrait display even though most of the canvas is not.
    CHECK_EQ(CanvasHomeMonitor(CanvasRect{ 1980, 100, 240 }, monitors), 1);
}

WINTOOLS_TEST(Canvas_ClampsInsideTheDisplayItIsOn)
{
    const std::vector<MonitorSnapshot> monitors = TwoDisplays();

    const CanvasRect offLeft = ClampCanvas(CanvasRect{ -80, -40, 240 }, monitors);
    CHECK_EQ(offLeft.x, 0);
    CHECK_EQ(offLeft.y, 0);
    CHECK_EQ(offLeft.size, 240);

    // Centre inside the wide display, bottom-right corner past its edge.
    const CanvasRect offRight = ClampCanvas(CanvasRect{ 1700, 900, 240 }, monitors);
    CHECK_EQ(offRight.x, 1920 - 240);
    CHECK_EQ(offRight.y, 1080 - 240);
}

WINTOOLS_TEST(Canvas_DraggedIntoTheVoidSnapsToTheNearestDisplay)
{
    // A 200 pixel gap between two displays, exactly where the shell draws a seam.
    std::vector<MonitorSnapshot> monitors = {
        MakeMonitor(L"\\\\?\\DISPLAY#AAA", 0, 0, 1920, 1080),
        MakeMonitor(L"\\\\?\\DISPLAY#BBB", 2120, 0, 1920, 1080),
    };

    // Small enough to sit entirely in the gap: no display overlaps it, so the nearest one wins
    // and the square is pulled back onto it instead of staying half visible.
    const CanvasRect dropped = ClampCanvas(CanvasRect{ 1930, 40, 120 }, monitors);
    CHECK_EQ(dropped.x, 1920 - 120);

    // Straddling the edge of a display is not a gap: the display it overlaps most keeps it.
    const CanvasRect droppedRight = ClampCanvas(CanvasRect{ 2050, 40, 120 }, monitors);
    CHECK_EQ(droppedRight.x, 2120);
}

WINTOOLS_TEST(Canvas_NoDisplaysLeavesTheRectAlone)
{
    const std::vector<MonitorSnapshot> monitors;
    const CanvasRect rect{ 300, 200, 240 };

    CHECK_EQ(CanvasHomeMonitor(rect, monitors), -1);
    CHECK(ClampCanvas(rect, monitors) == rect);
}

WINTOOLS_TEST(Canvas_OversizedSquareKeepsItsCornerOnScreen)
{
    // 480 is the maximum size and a 400 pixel wide display exists, so the clamp range would be
    // inverted if it were taken at face value.
    const std::vector<MonitorSnapshot> monitors = { MakeMonitor(L"\\\\?\\DISPLAY#TINY", 0, 0, 400, 300) };

    const CanvasRect rect = ClampCanvas(CanvasRect{ -50, -50, 480 }, monitors);
    CHECK_EQ(rect.x, 0);
    CHECK_EQ(rect.y, 0);
    CHECK_EQ(rect.size, 480);
}
