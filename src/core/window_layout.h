#pragma once

#include <Windows.h>

#include <string>
#include <vector>

#include "core/monitor.h"
#include "core/slice.h"

namespace wintools::core
{

/// How a remembered window should be placed when its display comes back.
///
/// Absolute coordinates alone are fragile: the same window on a monitor that came back with a
/// different resolution, or a different arrangement around it, can land somewhere useless. A dock
/// mode says what the window is *for* ("the left half of this screen") and survives resolutions
/// and rearrangements.
///
/// Position is the default and keeps the exact previous geometry, shifted along with the display.
enum class DockMode : unsigned char
{
    Position = 0,
    Left = 1,
    Right = 2,
    Top = 3,
    Bottom = 4,
    Maximize = 5,
    Fill = 6,

    Count = 7,
};

/// True when this mode ignores the saved rectangle and derives everything from the work area.
[[nodiscard]] bool IsDockMode(const DockMode mode);

/// How much of the work area a split covers, in thousandths. 500 is the ordinary half; Windows 11's
/// snap layouts also produce thirds and quarters, and a window that is exactly a third of its
/// display should come back as a third of whatever display it lands on, not as a frozen rectangle.
///
/// Thousandths rather than percent because percent loses the thing it is trying to preserve: a
/// third of 1920 px is 640, which is 33% - and 33% of 1920 is 633, a seven-pixel gap between two
/// windows that used to touch. 333 thousandths restores 639, one pixel off at worst.
constexpr int kDefaultSplitPermille = 500;
constexpr int kMinSplitPermille = 50;
constexpr int kMaxSplitPermille = 950;

/// Human name of a mode including its split, e.g. "左半屏" or "右侧 33%".
[[nodiscard]] std::wstring DescribeDock(DockMode mode, int splitPermille = kDefaultSplitPermille);

/// The rectangle a docked window should occupy, in screen coordinates.
///
/// Splits are taken from the work area rather than the full monitor rectangle, so a docked window
/// does not slide under the taskbar. The two sides of a 50% split always add up to the whole area
/// - a 1921-wide work area gives 960 and 961, never 960 and 960.
[[nodiscard]] Rect ComputeDockRect(
    DockMode mode, const Rect& workArea, int splitPermille = kDefaultSplitPermille);

/// Reads a dock mode back out of a captured window rectangle.
///
/// A window counts as split only when three of its edges are flush with the work area and the
/// fourth cuts across it - the shape every snap layout produces. Anything else stays Position,
/// so an ordinary floating window keeps its exact rectangle instead of being turned into a
/// ratio that happens to fit today's resolution.
[[nodiscard]] DockMode DetectDock(const RECT& windowRect, const Rect& workArea, int& splitPermille);

/// One display as it looked when the layout was captured.
struct MonitorSnapshot
{
    std::wstring devicePath;
    Rect rect{};
};

/// One window as it looked when the layout was captured.
///
/// The rectangle is in screen coordinates, i.e. physical pixels, so it can be compared and shifted
/// arithmetically without any conversion - and so it lands exactly where it was, at any mix of
/// display scale factors.
struct WindowSnapshot
{
    std::wstring exePath;
    std::wstring appName;
    std::wstring title;
    std::wstring monitorPath;
    Rect rect{};

    /// Ignored when dock is not Position: the plan derives its target from the work area instead.
    DockMode dock{ DockMode::Position };

    /// Share of the work area taken by a split dock, in percent. Meaningless for the other modes.
    int splitPermille{ kDefaultSplitPermille };

    bool minimized{};
    bool maximized{};

    /// False when the user unticked this window in the panel.
    ///
    /// The row stays in the file rather than being deleted, because deleting it would make the
    /// choice irreversible: the window would vanish from the list and there would be no way to
    /// put it back short of capturing the whole layout again.
    bool enabled{ true };
};

struct Layout
{
    std::vector<MonitorSnapshot> monitors;
    std::vector<WindowSnapshot> windows;

    [[nodiscard]] bool IsEmpty() const
    {
        return windows.empty();
    }
};

struct RestoreStats
{
    int moved{};
    int alreadyInPlace{};
    int skippedMissingMonitor{};
    /// Unticked in the panel, so deliberately left alone.
    int skippedDisabled{};
    /// The layout remembers this window but its program is not running right now.
    int missingWindow{};
    /// A placement call was made and rejected.
    int failed{};

    [[nodiscard]] bool IsClean() const
    {
        return failed == 0;
    }
};

/// Decision for a single window, computed as a pure function so it can be tested without a
/// desktop full of real windows.
struct PlacementPlan
{
    /// The display this window was captured on is not attached right now: leave the window alone.
    bool skip{};
    /// The window needs to be moved. False means it is already close enough to where it belongs.
    bool move{};
    /// Where the window should end up, in screen coordinates.
    RECT target{};

    /// The state the window should end up in. Carried here rather than read from the snapshot at
    /// the call site so a dock mode can override what was captured ("maximise" wins over a window
    /// that happened to be restored when the layout was taken).
    bool minimized{};
    bool maximized{};
};

/// Windows move by whole pixels; a couple of pixels of drift is not worth another placement call
/// (which visibly flickers for some apps).
constexpr int kPlacementTolerancePx = 2;

/// How strictly a restore is allowed to rearrange the desktop.
enum class RestoreScope
{
    /// Every window ends up exactly where the layout says: rectangle, dock and all. What the panel's
    /// 恢复 button does, because there the user is asking for the layout, not for damage control.
    Exact,

    /// Only bring windows home. A window that is already on its own display - a display that has not
    /// changed size since the capture - is left exactly where the user put it.
    ///
    /// This is what an automatic restore after a display change runs. Applying the layout verbatim
    /// there is destructive in a way the feature does not need to be: waking a monitor back up is
    /// not a request to undo the split the user arranged since the last capture. Windows that were
    /// squeezed onto another screen still come back, at their captured dock and split - which is the
    /// half of the feature that actually earns its keep.
    Homecoming,
};

/// Works out where a captured window should go, given where its display is now.
///
/// For DockMode::Position the display's own origin is subtracted out and re-added at its current
/// position, which is what makes a partly restored arrangement work: when only some panels are
/// back, the others usually sit at different virtual offsets, and raw coordinates would land
/// off-screen.
///
/// currentWorkArea is only used by the dock modes. actualMaximized is needed because a maximized
/// window's normal rectangle is its restore-down rectangle, not the area it covers: without it a
/// docked-and-maximized window would look "misplaced" on every display change and be re-placed
/// every time, which flickers.
[[nodiscard]] PlacementPlan PlanPlacement(
    const WindowSnapshot& window,
    const MonitorSnapshot& savedMonitor,
    const Rect& currentMonitorRect,
    const Rect& currentWorkArea,
    const RECT& actualNormalRect,
    bool actualMaximized,
    RestoreScope scope = RestoreScope::Exact);

/// Displays attached right now, with their virtual-desktop rectangles.
[[nodiscard]] std::vector<MonitorSnapshot> CaptureMonitors();

/// The work area of the display that contains rect, in screen coordinates.
///
/// Falls back to rect itself when the monitor cannot be resolved, which degrades docking to "use
/// the full screen" rather than failing the placement.
[[nodiscard]] Rect WorkAreaFor(const Rect& monitorRect);

/// Snapshot of every display plus every manageable window right now.
[[nodiscard]] Layout CaptureLayout();

/// Puts every captured window back, per window.
///
/// Windows whose display is missing are left exactly where they are. Displays come back one at a
/// time, and the windows that are already home must not be disturbed by the ones that are not.
RestoreStats RestoreLayout(const Layout& layout, RestoreScope scope = RestoreScope::Exact);

/// Applies one placement. Exposed for the restore loop and for manual "move this window" actions.
bool ApplyPlacement(HWND window, const RECT& targetScreenRect, bool minimized, bool maximized);

} // namespace wintools::core
