#include "test_framework.h"

#include <vector>

#include "core/note_layout.h"

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

/// The arrangement the notes were designed against: a 1920x1080 primary with a portrait display to
/// its right, so "off the edge of one screen" is also "on top of another".
std::vector<MonitorSnapshot> TwoDisplays()
{
    return {
        MakeMonitor(L"\\\\?\\DISPLAY#AAA", 0, 0, 1920, 1080),
        MakeMonitor(L"\\\\?\\DISPLAY#BBB", 1920, 0, 1200, 1920),
    };
}

Rect MakeRect(int left, int top, int width, int height)
{
    return Rect{ left, top, width, height };
}

} // namespace

WINTOOLS_TEST(NoteLayout_ContainsIsHalfOpen)
{
    const Rect rect = MakeRect(100, 100, 200, 150);

    CHECK(NoteContains(rect, 100, 100));
    CHECK(NoteContains(rect, 299, 249));
    CHECK(!NoteContains(rect, 300, 200));
    CHECK(!NoteContains(rect, 200, 250));
    CHECK(!NoteContains(rect, 99, 120));
}

WINTOOLS_TEST(NoteLayout_HomeIsTheDisplayUnderItsCentre)
{
    const std::vector<MonitorSnapshot> monitors = TwoDisplays();

    CHECK_EQ(NoteHomeMonitor(MakeRect(100, 100, 360, 300), monitors), 0);

    // Centre at 2100, well inside the portrait display, even though the note straddles both.
    CHECK_EQ(NoteHomeMonitor(MakeRect(1900, 100, 400, 300), monitors), 1);
}

WINTOOLS_TEST(NoteLayout_HomeFallsBackToOverlapThenDistance)
{
    const std::vector<MonitorSnapshot> monitors = TwoDisplays();

    // Centre lands in the gap-free space but outside both rects: the whole thing is right of 1920
    // and below 1920, so it overlaps neither and the nearest centre decides.
    CHECK_EQ(NoteHomeMonitor(MakeRect(3300, 2000, 200, 200), monitors), 1);
    CHECK_EQ(NoteHomeMonitor(MakeRect(-2000, -2000, 200, 200), monitors), 0);

    CHECK_EQ(NoteHomeMonitor(MakeRect(0, 0, 200, 200), std::span<const MonitorSnapshot>{}), -1);
}

WINTOOLS_TEST(NoteLayout_SizeIsClampedButPositionIsNot)
{
    const Rect tiny = ClampNoteSize(MakeRect(40, 50, 10, 10));
    CHECK_EQ(tiny.left, 40);
    CHECK_EQ(tiny.top, 50);
    CHECK_EQ(tiny.width, kNoteMinWidth);
    CHECK_EQ(tiny.height, kNoteMinHeight);

    const Rect huge = ClampNoteSize(MakeRect(0, 0, 9000, 9000));
    CHECK_EQ(huge.width, kNoteMaxWidth);
    CHECK_EQ(huge.height, kNoteMaxHeight);
}

WINTOOLS_TEST(NoteLayout_ClampPullsItBackOntoItsDisplay)
{
    const std::vector<MonitorSnapshot> monitors = TwoDisplays();

    // Hanging off the right edge of the primary, with its centre still on the primary.
    const Rect right = ClampNote(MakeRect(1700, 100, 360, 300), monitors);
    CHECK_EQ(right.Right(), 1920);
    CHECK_EQ(right.width, 360);

    // Above the top edge: pinned to the top, not left where it was.
    const Rect above = ClampNote(MakeRect(200, -120, 360, 300), monitors);
    CHECK_EQ(above.top, 0);
    CHECK_EQ(above.left, 200);
}

WINTOOLS_TEST(NoteLayout_ClampShrinksWhatCannotFit)
{
    const std::vector<MonitorSnapshot> monitors = TwoDisplays();

    // Wider than the portrait display it sits on, and taller than the landscape one.
    const Rect wide = ClampNote(MakeRect(1920, 100, 1600, 300), monitors);
    CHECK_EQ(wide.width, 1200);
    CHECK_EQ(wide.Right(), 3120);

    const Rect tall = ClampNote(MakeRect(100, 100, 360, 1200), monitors);
    CHECK_EQ(tall.height, 1080);
    CHECK_EQ(tall.top, 0);
}

WINTOOLS_TEST(NoteLayout_DefaultCascadesFromThePrimary)
{
    const std::vector<MonitorSnapshot> monitors = TwoDisplays();

    const Rect first = DefaultNoteRect(monitors, 0);
    CHECK_EQ(first.width, kNoteDefaultWidth);
    CHECK_EQ(first.height, kNoteDefaultHeight);
    CHECK_EQ(first.left, 96);
    CHECK_EQ(first.top, 96);

    // Each note steps down and right, and the step wraps rather than marching off the screen.
    const Rect second = DefaultNoteRect(monitors, 1);
    CHECK_EQ(second.left, 96 + 36);
    CHECK_EQ(second.top, 96 + 36);

    const Rect wrapped = DefaultNoteRect(monitors, 6);
    CHECK_EQ(wrapped.left, first.left);
    CHECK_EQ(wrapped.top, first.top);
}

WINTOOLS_TEST(NoteLayout_MonitorAtPointUsesSnapshotsNotTheOrigin)
{
    const std::vector<MonitorSnapshot> monitors = TwoDisplays();

    CHECK_EQ(MonitorAtPoint(monitors, 100, 100), 0);
    CHECK_EQ(MonitorAtPoint(monitors, 2200, 700), 1);

    // Half-open rects: the second display starts exactly at x = 1920.
    CHECK_EQ(MonitorAtPoint(monitors, 1920, 0), 1);
    CHECK_EQ(MonitorAtPoint(monitors, 1919, 0), 0);

    // With negative coordinates and gaps, an uncovered point must not silently become primary.
    CHECK_EQ(MonitorAtPoint(monitors, -10, -10), -1);
    CHECK_EQ(MonitorAtPoint(std::span<const MonitorSnapshot>{}, 100, 100), -1);
}

WINTOOLS_TEST(NoteLayout_DefaultLandsOnThePreferredDisplay)
{
    const std::vector<MonitorSnapshot> monitors = TwoDisplays();

    const Rect first = DefaultNoteRect(monitors, 0, 1);
    CHECK_EQ(first.left, 1920 + 96);
    CHECK_EQ(first.top, 96);
    CHECK_EQ(NoteHomeMonitor(first, monitors), 1);

    const Rect second = DefaultNoteRect(monitors, 1, 1);
    CHECK_EQ(second.left, first.left + 36);
    CHECK_EQ(second.top, first.top + 36);

    // -1 means the caller does not know: fall back to the primary, as before.
    const Rect fallback = DefaultNoteRect(monitors, 0, -1);
    CHECK_EQ(fallback.left, 96);
    CHECK_EQ(fallback.top, 96);

    // A stale index must not crash or wander: it falls back to the primary too.
    const Rect stale = DefaultNoteRect(monitors, 0, 42);
    CHECK_EQ(stale.left, 96);
    CHECK_EQ(stale.top, 96);
}

WINTOOLS_TEST(NoteLayout_DefaultCopesWithNoDisplays)
{
    const Rect rect = DefaultNoteRect(std::span<const MonitorSnapshot>{}, 0);

    CHECK_EQ(rect.left, 0);
    CHECK_EQ(rect.top, 0);
    CHECK_EQ(rect.width, kNoteDefaultWidth);
    CHECK_EQ(rect.height, kNoteDefaultHeight);
}
