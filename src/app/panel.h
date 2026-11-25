#pragma once

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/canvas.h"
#include "core/composer.h"
#include "core/group.h"
#include "core/hotkeys.h"
#include "core/layout_math.h"
#include "core/note_archive.h"
#include "core/window_layout.h"

namespace wintools::app
{

/// Everything the user can ask the panel to do.
///
/// The panel itself performs no actions. It reports what was clicked and renders whatever state
/// it is handed, so the same operations can be driven from the tray menu, from the command line,
/// or from the display-change watcher without any of them knowing about each other.
enum class PanelAction
{
    /// Choose a picture for the active group, and re-render every group straight away.
    PickWallpaper,

    /// Re-render and re-apply every group using the framings currently in the model.
    RecropWallpaper,

    /// Rotate the active group's picture clockwise by one quarter turn, and re-render straight
    /// away. A short click on the arc handle is still the quick way to step 90 degrees.
    RotateWallpaper,

    /// The drag handle released at an arbitrary angle: request.view carries the final framing.
    /// Commits it to the group and re-renders the desktop slices once, on release.
    CommitWallpaperRotation,

    /// Open one more sticky note.
    NewNote,

    /// Bring every sticky note back to the top of the z-order, the way the tray menu does.
    ShowNotes,

    /// Put back whatever the desktop showed before the first apply.
    RestoreWallpaper,

    /// Re-read every window's position from the live desktop and rewrite the layout file.
    RefreshLayout,

    ApplyLayout,
    ToggleAutoLayout,
    ToggleAutostart,
    /// Whether sticky notes are recreated at the next startup. Takes effect on restart.
    ToggleNoteRestore,

    /// Tick or untick one remembered window. index is its row, value is the wanted state.
    ToggleWindow,

    /// The user panned or zoomed the picture inside the arrangement map; request.view is the new
    /// framing of the active group.
    SetWallpaperView,

    /// The active group changed. index is the new slot; no pixels move, but the choice is stored
    /// so reopening the panel comes back to the same group.
    SetActiveGroup,

    /// Move one display into another group. target is its device path, index the destination slot;
    /// -1 means "into a group of its own".
    MoveMonitorToGroup,

    /// Pick the UI theme from the config page. value is 0 = light, 1 = dark, 2 = follow the
    /// system. Stored, and applied immediately.
    SetTheme,

    /// Pick the UI language from the config page. value is 0 = follow the system,
    /// 1 = Simplified Chinese, 2 = English. Applied immediately; the panel is refreshed so
    /// every label is measured again in the new language.
    SetLanguage,

    /// Open the log file in whatever the system uses for text.
    OpenLog,

    /// Show or hide the desktop calendar widget.
    ToggleCanvas,

    /// Reopen one closed note from the archive. The id is in `target`.
    ReopenNote,

    /// Permanently delete one archived note. The id is in `target`.
    ForgetNote,

    /// Bind one shortcut to the combination in `index` (low word = modifiers, high = vk),
    /// or clear it when vk is 0. `value` is the HotkeyAction.
    SetHotkey,

    /// Put both shortcuts back to their defaults.
    ResetHotkeys,

    /// Background check for a newer release. Failure is silent apart from the status line.
    CheckUpdate,

    /// User turned the opt-in background check on or off.
    ToggleAutoCheck,

    /// Show the release notes of the version found by the last check, before downloading.
    ShowUpdateNotes,

    /// The user accepted the notes: download, verify, replace, restart.
    InstallUpdate,

    /// Temporarily give up the global hotkeys so their combinations can be recorded. Without this,
    /// pressing the old Ctrl+Alt+N while rebinding would fire the action instead of reaching the
    /// panel's key handler.
    SuspendHotkeys,
    ResumeHotkeys,

    /// The calendar was dragged to a new spot in the arrangement map. x/y are the new top-left in
    /// virtual-desktop pixels; the size is unchanged.
    MoveCanvas,

    /// Replace the note paper palette with `colors` (2-5 RRGGBB values). An empty list puts the
    /// built-in six back. Applied to the open notes at once, so the panel's swatches and the notes
    /// on screen never disagree.
    SetNotePalette,

    Quit,
};

/// A request from the panel.
///
/// Carries the row index even for actions that do not need one, because a single-argument
/// signature means a future per-row action cannot be wired up with its payload silently dropped.
struct PanelRequest
{
    PanelAction action{};
    int index{ -1 };
    int value{};
    core::WallpaperView view{};

    /// Screen position for actions that move something. Only MoveCanvas reads them today; they
    /// exist as a pair rather than being packed into `index`/`value` because a packed coordinate
    /// is the kind of encoding that later gets unpacked in the wrong order.
    int x{};
    int y{};

    /// Device path of the display a per-display action is aimed at; empty for the rest.
    ///
    /// The device path, not the row index, because a display can be unplugged between the paint
    /// and the click - and then the row the panel still shows points at a different display.
    std::wstring target;

    /// The whole palette for SetNotePalette: 2-5 RRGGBB papers, or empty for the built-in six.
    /// A list rather than a diff, because the panel is the one that let the user edit it and the
    /// service should not have to replay edits to arrive at the same answer.
    std::vector<std::uint32_t> colors;
};

/// How a status line reads, which is also its colour.
/// One row of the window list.
struct WindowRow
{
    std::wstring title;
    std::wstring app;

    /// Device path of the display the window was captured on.
    std::wstring monitorPath;

    /// Read-only description of where it was: "最大化", "左半屏", "1044×1048".
    std::wstring where;

    bool enabled{ true };
};

/// The panel is a pure function of this struct: give it different values and it looks different.
struct PanelState
{
    /// Displays as they are attached right now, in virtual-desktop coordinates.
    std::vector<core::MonitorSnapshot> monitors;

    /// The arrangement: every display belongs to exactly one group, and every group has a
    /// picture, a framing and a dirty flag of its own.
    std::vector<core::Group> groups;

    /// Slot of the group the wallpaper buttons act on.
    int activeSlot{};

    /// The saved layout's windows, in capture order.
    std::vector<WindowRow> windows;

    /// Closed sticky notes that can be reopened, newest first.
    std::vector<core::ArchiveRecord> archivedNotes;

    /// Global shortcuts, indexed by HotkeyAction - 1.
    core::Hotkeys hotkeys;

    /// "11-07 16:26" - when the layout file was last written. Empty when there is none.
    std::wstring snapshotAt;

    bool autoLayout{ true };
    bool autostart{};
    bool noteRestore{ true };
    bool darkTheme{};

    /// settings.txt's own values, not the resolved result: 0 light / 1 dark / 2 follow the system,
    /// and 0 follow the system / 1 Chinese / 2 English. The config page shows the *choice* - "follow
    /// the system" is something a user can pick, and a two-state widget cannot show it.
    int theme{};
    int language{};

    /// The desktop calendar: up or down, and where its square is on the virtual desktop.
    bool canvasOn{};
    core::CanvasRect canvas{};

    /// A long action is running: buttons show a spinner, the status dot turns amber.
    bool busy{};
    std::wstring busyLabel;

    /// The most recent status line, shown in the shared footer on every page.
    std::wstring status;
    bool statusIsError{};

    /// Result of the last update check. version is empty when no newer version was found.
    bool autoCheckUpdate{};
    std::wstring updateAvailableVersion;
    std::wstring updateNotesZh;
    std::wstring updateNotesEn;
    std::uint64_t updateSize{};
    std::wstring updateSha256;
    std::wstring lastUpdateCheck;

    [[nodiscard]] bool HasRows() const
    {
        return !windows.empty();
    }

    [[nodiscard]] const core::Group* ActiveGroup() const
    {
        if (activeSlot < 0 || activeSlot >= static_cast<int>(groups.size()))
        {
            return nullptr;
        }

        return &groups[static_cast<size_t>(activeSlot)];
    }
};

/// The sheet as raw pixels.
///
/// Two things in this design sit on top of something whose colour is not known when the code is
/// written: the number badge sits on the wallpaper, and a modal dims the whole sheet. GDI cannot
/// blend a rounded, translucent shape, and AlphaBlend under a scaled mapping mode is not
/// something to bet a whole panel on - so the panel keeps a pointer to its own 32bpp buffer and
/// blends the pixels directly. It exists here rather than in the implementation because the
/// drawing helpers and the panel itself both need the type.
struct PixelSurface
{
    std::uint32_t* bits{};
    int width{};
    int height{};
    double scaleX{ 1.0 };
    double scaleY{ 1.0 };
    bool valid{};
};

/// The settings panel: one window, drawn with GDI, no child controls.
///
/// Custom-drawn rather than a dialog with child controls because the layout is a scaled map of
/// the virtual desktop with clickable numbers inside it, group outlines drawn outside screens,
/// and a list that has to line up column by column. Painting the whole client area also means
/// there is no z-order, no flicker and no per-control DPI behaviour to reason about.
class Panel
{
public:
    using ActionHandler = std::function<void(const PanelRequest&)>;
    using StateProvider = std::function<PanelState()>;

    /// The four pages of the tabbed sheet, in the order the tabs stand in.
    enum class Tab
    {
        Layout,
        Wallpaper,
        Notes,
        Config,
    };

    Panel(ActionHandler onAction, StateProvider readState);
    ~Panel();

    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;
    Panel(Panel&&) = delete;
    Panel& operator=(Panel&&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance, HICON icon);

    void ShowPanel();
    void HidePanel();
    void Toggle();

    /// Re-reads state from the provider, re-fits the window and repaints.
    void Refresh();

    /// Opens the release-notes modal from outside the panel (the tray menu does this).
    /// No-op when no newer version is known, so the notes can never describe nothing.
    void ShowUpdateNotes();

    [[nodiscard]] HWND Handle() const
    {
        return m_window;
    }

    /// How the virtual desktop is scaled into the map, exposed for tests.
    struct Projection
    {
        core::Rect canvas{};
        RECT area{};      // client-space rectangle the canvas is fitted into
        RECT drawn{};     // client-space rectangle the canvas actually occupies
        double scale{ 1.0 };

        /// Maps a virtual-desktop rectangle into client space.
        ///
        /// Both edges are rounded independently rather than computing a size and adding it, so
        /// two displays that touch in virtual space still touch on screen instead of leaving a
        /// one-pixel seam that reads as a gap in the arrangement.
        [[nodiscard]] RECT ToClient(const core::Rect& rect) const;
    };

    [[nodiscard]] static Projection Fit(const std::vector<core::MonitorSnapshot>& monitors,
        const RECT& area);

    /// Everything the framing arithmetic needs, in one value.
    ///
    /// `canvas` is the *group's* canvas, not the virtual desktop's: two groups spliced side by
    /// side each frame their own picture across their own bounding box, which is exactly what
    /// RenderSpan does with the same member list.
    struct MapContext
    {
        core::Rect canvas{};
        /// The picture's size **before** the group's turn. The framing maths turns it itself; a
        /// turned bounding box is bigger than the picture and would leave the corners empty.
        core::ImageSize source{};
        /// Map pixels per canvas pixel.
        double scale{ 1.0 };

        /// The clockwise angle this context was built with; the preview cache key needs it.
        double rotation{ 0.0 };

        [[nodiscard]] bool IsValid() const
        {
            return canvas.IsValid() && source.width > 0 && source.height > 0 && scale > 0.0;
        }
    };

    /// The cover fit for one view, plus what it takes to convert between map and canvas pixels.
    struct MapFrame
    {
        /// Cover fit of the whole picture at the given view's zoom, in canvas pixels.
        /// `x`/`y` are the slack: how much wider/taller than the canvas the picture is.
        core::ViewSlack surface{};
        double scale{ 1.0 };
        double zoom{ 1.0 };
    };

    [[nodiscard]] static MapFrame FrameFor(
        const MapContext& context, const core::WallpaperView& view);

    /// Size of the cached surface, in pixels: the whole picture cover-fitted at zoom 1, before the
    /// canvas crops anything out of it.
    [[nodiscard]] static SIZE PreviewSize(const MapContext& context);

    /// The angle a turn gesture should show: `start` plus `delta`, quantised and magnetised.
    ///
    /// A turn is only exact at a multiple of 90 - everywhere else the renderer resamples - so the
    /// four angles that matter are the four that hold. Capture is wide enough to catch what the eye
    /// calls square and release is wider still, which makes arriving easy and leaving deliberate;
    /// `snap` carries the held angle between calls, because hysteresis is state by definition. It
    /// is reset to -1 on press.
    [[nodiscard]] static double TurnAngle(double start, double delta, int& snap);

    /// Moves the framing by a mouse delta in map pixels, with the picture following the cursor.
    [[nodiscard]] static core::WallpaperView PanView(const core::WallpaperView& view,
        const MapFrame& frame, int dx, int dy);

    /// Changes the zoom while keeping the picture pixel under `anchor` under the cursor.
    [[nodiscard]] static core::WallpaperView ZoomView(const core::WallpaperView& view,
        const MapContext& context, POINT anchor, double factor);

    /// The zoom factor for one wheel event, 10% per notch with fractional notches in between.
    [[nodiscard]] static double ZoomFactor(int wheelDelta);

    /// The part of the cached preview to sample for one display, in preview-bitmap pixels.
    ///
    /// monitorRect is relative to the group's canvas, like a core::Monitor is to the virtual
    /// desktop. tests/panel_view_test.cpp checks it against core::ComputeSlice.
    struct MapSample
    {
        int x{};
        int y{};
        int width{};
        int height{};
    };

    [[nodiscard]] static MapSample SampleFor(const MapFrame& frame,
        const core::Rect& monitorRect, const core::WallpaperView& view);

    /// A design measurement in device pixels at the given scale factor.
    [[nodiscard]] static int ForDpi(int value, UINT dpi);

    /// The rectangle a group's outline stands off from: the screen box grown by 2 design units.
    ///
    /// The line's *inner* edge is this rectangle and it thickens outward from here, which is why
    /// selecting a group never moves the line toward the picture - and why two groups that touch
    /// keep a strip of the neighbour's picture between their two lines instead of collapsing
    /// into one thick band.
    [[nodiscard]] static RECT FrameBounds(const RECT& groupBox);

    /// 1 when the group is not the one being edited, 2 when it is. Drawn from the outer edge in.
    [[nodiscard]] static int FrameThickness(bool selected);

    /// The one button height the panel has. Exposed so a test can hold every button to it.
    [[nodiscard]] static int ButtonHeight();

    /// List row height, likewise.
    [[nodiscard]] static int ListRowHeight();

private:
    /// Every hit-testable thing. The index means whatever the hot value says it means.
    enum class Hot
    {
        None,
        IconLanguage,
        IconTheme,
        IconHelp,
        IconClose,
        Tab,          // index = 0 layout, 1 wallpaper, 2 notes, 3 config
        GroupChip,   // index = group slot
        GroupAdd,    // index = group slot
        Pick,
        ApplyWallpaper,
        RotateWallpaper,
        ResetRotation, // puts the picture back to square, the turn readout's own button
        ScreenChip,  // index = monitor index
        AutoLayout,
        Refresh,
        ApplyLayout,
        RowCheck,    // index = index into m_state.windows
        RowBody,
        ScreenBadge, // index = monitor index
        MapBody,
        CanvasSwitch,
        CanvasBody,
        NewNote,
        ShowNotes,
        NoteColors, // the paper palette editor, first button on the notes page
        ArchiveRestore, // index = row in m_state.archivedNotes (absolute, not the page row)
        ArchiveForget,  // index = row in m_state.archivedNotes (absolute, not the page row)
        ArchivePrev,    // one page back in the closed-note history
        ArchiveNext,    // one page on
        /// The config page's rows. The switches and the segmented controls carry an index: the
        /// segment chosen for the first two, the HotkeyAction for a shortcut row.
        ConfigTheme,
        ConfigLanguage,
        ConfigAutostart,
        ConfigNoteRestore,
        ConfigHotkey,
        ConfigResetHotkeys,
        ConfigOpenLog,
        ConfigAutoCheck,
        ConfigCheckUpdate,
        ConfigUpdateNotes,
        Quit,
        PopupItem,   // index = item within the open popup
        PopupRemove, // index = the popup row whose trailing "x" was hit
        ModalButton, // index = button within the open modal
    };

    struct Target
    {
        Hot hot{ Hot::None };
        int index{ -1 };
    };

    /// One entry of whichever popup is open. The hit test and the paint both walk the same list,
    /// so a click cannot land on an item that was never drawn.
    struct PopupEntry
    {
        std::wstring text;
        std::wstring trailing;
        /// What the item means to its menu.
        int id{ -1 };
        /// Group slot for the colour pip, -1 for none, -2 for a neutral dot.
        int pipIndex{ -1 };
        /// A literal colour for the pip, used by the note-palette rows. Takes precedence over
        /// pipIndex; `hasSwatch` is what says "draw this one" rather than "draw a group colour".
        std::uint32_t swatch{};
        bool hasSwatch{};
        /// The row can be deleted through a small "×" in its trailing column. Only rows whose menu
        /// is a list the user owns - today just the note papers - carry one.
        bool removable{};
        bool dashed{};
        bool separator{};
        bool checked{};
    };

    /// What a config row does when clicked. Painting and hit testing both read this, so a row
    /// cannot be drawn as a switch and behave like a button.
    enum class ConfigControl
    {
        Switch,   ///< a track and a knob on the right; the state is `on`
        Segments, ///< two or three choices, the current one in `selected`
        Hotkey,   ///< the whole row starts recording; `value` is the combination or the hint
        Link,     ///< the whole row runs something, with a trailing chevron
        Text,     ///< read-only label and value; no hot target at all
        Button,   ///< only the button on the right is clickable
    };

    /// Geometry for one frame, computed once per paint and once per hit-test so the two can
    /// never disagree.
    struct Geometry
    {
        RECT titleBar{};
        RECT iconLanguage{};
        RECT iconTheme{};
        RECT iconHelp{};
        RECT iconClose{};

        /// The four page tabs, left to right, and the bar they stand in.
        RECT tabBar{};
        RECT tabLayout{};
        RECT tabWallpaper{};
        RECT tabNotes{};
        RECT tabConfig{};

        RECT wallHead{};
        RECT pick{};
        RECT applyWallpaper{};

        /// The rotate handle: a Lucide refresh-cw glyph inside a small chip, floating just
        /// outside one corner of the selected group's box. `rotateWallpaper` is its hit square,
        /// `rotateCentre` its centre. Hit and paint both use these, so they cannot disagree.
        RECT rotateWallpaper{};
        POINT rotateCentre{};
        int rotateRadius{};

        RECT wallHint{};
        RECT zoomBadge{};
        /// The angle the picture is turned to, and the button that puts it back to zero. Both sit
        /// in the hint row, because the readout has to be visible at the same moment the hand is
        /// on the handle, and the handle moves with the group.
        RECT rotateBadge{};
        RECT resetRotation{};
        RECT dirtyBadge{};
        RECT groupMeta{};
        RECT canvasSwitch{};

        RECT mapCard{};
        RECT mapArea{};

        RECT layoutHead{};
        RECT autoSwitch{};
        RECT refresh{};
        RECT applyLayout{};

        RECT layoutHint{};
        RECT snapshotMeta{};
        RECT windowMeta{};

        RECT listCard{};
        RECT listArea{};
        int listRows{};
        int firstRow{};

        RECT footer{};
        RECT statusDot{};
        RECT statusText{};
        RECT quit{};

        /// The notes page's head row: title and its two buttons.
        RECT notesHead{};
        RECT newNote{};
        RECT showNotes{};
        RECT noteColors{};

        /// Closed-note history block. Empty when there is nothing archived. Rows carry the same
        /// index as m_state.archivedNotes; the two buttons hit-test separately from the row.
        RECT archiveCard{};
        /// The list's meta row above the card: title on the left, paging on the right.
        RECT archiveMeta{};
        /// The slice of the archive the card is showing: first row index into
        /// m_state.archivedNotes and how many rows the card could hold at this window height.
        /// The head line reads the range; the wheel scrolls the first index.
        int archiveFirst{};
        int archiveRowsCapacity{};
        std::vector<RECT> archiveRows;
        std::vector<RECT> archiveRestore;
        std::vector<RECT> archiveForget;
        /// Page arrows at the right end of the history head, and the "2 / 5" label between them
        /// and the title. Paging moves by a whole cardful, which is what the wheel does too.
        RECT archivePrev{};
        RECT archiveNext{};
        RECT archivePage{};

        /// The config page, top to bottom, in one list. A heading is an entry with `section` set
        /// and no control; every other entry is a row. Paint and hit test walk the same list, so a
        /// heading and its rows cannot drift apart.
        struct ConfigRow
        {
            RECT rect{};
            /// The clickable control: the switch track, or the button. Empty for the row kinds
            /// where the whole row is the target.
            RECT control{};
            /// One rect per choice, for a segmented row.
            std::vector<RECT> segments;
            /// The names of those choices, in the same order as `segments`.
            std::vector<std::wstring> choices;
            std::wstring label;
            std::wstring value;
            Hot hot{ Hot::None };
            ConfigControl kind{ ConfigControl::Link };
            int index{ -1 };
            int selected{ -1 };
            bool on{};
            bool section{};
        };
        std::vector<ConfigRow> configRows;

        /// One entry per group that has an attached member, in chip order.
        std::vector<RECT> groupChips;
        std::vector<RECT> groupAdds;
        std::vector<int> chipOrder;

        /// One entry per attached display, in reading order.
        std::vector<RECT> screenChips;
        std::vector<int> chipMonitors;

        int clientHeight{};
    };

    struct MapLayout
    {
        Projection projection{};
        bool valid{};
    };

    struct ModalLayout
    {
        RECT card{};
        /// Buttons, left to right.
        std::vector<RECT> buttons;
    };

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    [[nodiscard]] Geometry ComputeGeometry() const;

    /// Client coordinates of a mouse position, in design units.
    [[nodiscard]] POINT ToDesign(POINT device) const;

    [[nodiscard]] Target HitTest(POINT point) const;

    /// Rows of the window list after the screen filter, as indices into m_state.windows.
    [[nodiscard]] std::vector<int> VisibleRows() const;

    void CreateFonts();
    void DestroyFonts();

    /// Puts the measuring DC under the same horizontal scale the paint pass uses.
    ///
    /// A nine-pixel run hinted at 120 dpi is wider than the same run measured in a 1:1 DC: GDI
    /// re-hints every glyph at the device size, so the advance it reports at 96 dpi is not the
    /// advance it will use when drawing through a scaled mapping. Measuring through the same
    /// scale is the only way the layout and the pixels agree; when they do not, the symptom is a
    /// label that runs into its own plate.
    void ScaleMeasureDc();

    /// Brings the window back inside the monitor's work area, no size changes. The window is
    /// user-resizable, so nothing about the model changes its size any more.
    void ConstrainToWorkArea();
    void CentreOnCursorMonitor();

    void Paint();

    /// Draws the whole panel into dc, without needing the window to be visible.
    void PaintContent(HDC dc, int width, int height);

    void PaintSheet(HDC dc, const Geometry& geometry);
    void PaintTitleBar(HDC dc, const Geometry& geometry);
    void PaintTabBar(HDC dc, const Geometry& geometry);
    void PaintWallpaperBlock(HDC dc, const Geometry& geometry);
    void PaintNotesHead(HDC dc, const Geometry& geometry);
    void PaintConfigBlock(HDC dc, const Geometry& geometry);
    void PaintMap(HDC dc, const Geometry& geometry);

    /// The whole picture of the group being turned or moved, dimmed wherever no display of that
    /// group will receive it.
    ///
    /// Painted before the display loop, which then draws the screens themselves at full strength
    /// over it: the spill is context, the screens are the answer. A turn enlarges the cover on its
    /// own, a move shifts the window into a picture that no longer fits the screens - both leave
    /// the user looking at slices, which is why both ask for this.
    void PaintWholePicture(
        HDC dc, const Geometry& geometry, const MapLayout& map, int slot, bool coarse);

    void DrawScreenBadge(HDC dc, const RECT& box, int index);

    /// One outline around the union of a group's screens.
    ///
    /// Not the bounding box: an L-shaped group would have its box's free corner drawn straight
    /// across whatever sits beside it, and a group of one screen above another would get a line
    /// running over the second screen. Segments that another member of the same group continues
    /// are dropped, so the outline hugs the actual silhouette.
    void DrawGroupOutline(HDC dc, const MapLayout& map, const core::Group& group,
        COLORREF color, int offset, int thickness, double alpha = 1.0) const;
    void PaintLayoutBlock(HDC dc, const Geometry& geometry);
    void PaintWindowList(HDC dc, const Geometry& geometry);
    void PaintScrollbar(HDC dc, const Geometry& geometry);
    void PaintFooter(HDC dc, const Geometry& geometry);
    void PaintArchive(HDC dc, const Geometry& geometry);
    void PaintOverlays(HDC dc, const Geometry& geometry);
    void PaintPopup(HDC dc, const Geometry& geometry);
    void PaintModal(HDC dc, const Geometry& geometry);

    [[nodiscard]] MapLayout LayoutMap(const Geometry& geometry) const;

    [[nodiscard]] RECT MonitorRectOnMap(const MapLayout& map,
        const std::vector<core::MonitorSnapshot>& monitors, size_t index) const;

    [[nodiscard]] RECT GroupBoxOnMap(const MapLayout& map, const core::Group& group) const;

    /// The calendar square as it should be drawn right now: the dragged position while a drag is
    /// in progress, the model's position otherwise.
    [[nodiscard]] core::CanvasRect ShownCanvas() const;

    [[nodiscard]] RECT CanvasRectOnMap(const MapLayout& map) const;

    /// The number badge of a screen: 4 units in from the top-left corner.
    ///
    /// Shrinks to the number alone, and then disappears, rather than growing past the screen it
    /// belongs to - a label that spills onto the neighbour reads as that neighbour's label.
    struct BadgeLayout
    {
        RECT rect{};
        bool withResolution{};
    };

    [[nodiscard]] BadgeLayout LayoutBadge(const RECT& box, int index) const;
    [[nodiscard]] RECT BadgeRect(const RECT& box, int index) const;

    /// Monitor under a point in the map, as an index into m_state.monitors; -1 outside.
    [[nodiscard]] int MonitorAt(POINT point) const;

    /// True when dragging this screen would move a picture, i.e. its group has one.
    ///
    /// The move cursor is shown only where that is true: over the empty board between screens a
    /// four-arrow cursor promises a drag that does nothing.
    [[nodiscard]] bool MonitorTakesDrag(int index) const;

    void DrawCheckbox(HDC dc, const RECT& box, bool checked, bool hot) const;

    /// Fonts, one per type step. Five sizes, no intermediates.
    HFONT m_fontNum{};     // 14/700  screen numbers, group names, chip numbers
    HFONT m_fontSmall{};   // 9       resolutions, counts
    HFONT m_fontTitle{};   // 12.5/600 section titles, window titles
    HFONT m_fontBody{};    // 12      buttons, menu items
    HFONT m_fontAux{};     // 11      hints, status, process names
    HFONT m_fontAuxBold{}; // 11/600  "屏 1" in the list
    HFONT m_fontIcon{};    // Segoe UI Symbol, for the four title-bar glyphs


    /// A memory DC that exists only so text can be measured without a paint in progress. The
    /// geometry depends on how wide a label is, and the geometry is needed before anything is
    /// drawn - including by the hit test.
    HDC m_measure{};

    /// Decoded preview of one group's picture, kept between frames. The angle and the quality
    /// tier are both part of the cache key: turning changes the pixels, and a coarse drag frame
    /// must not be reused once the handle is released.
    struct GroupPreview
    {
        int slot{ -1 };
        std::wstring path;
        double rotation{ 0.0 };
        bool coarse{};
        int width{};
        int height{};
        HBITMAP bitmap{};

        [[nodiscard]] bool Matches(int candidateSlot, const std::wstring& candidate, double r,
            bool c, int w, int h) const
        {
            return bitmap != nullptr && slot == candidateSlot && path == candidate
                && std::abs(rotation - r) < 0.01 && coarse == c && width == w && height == h;
        }
    };

    [[nodiscard]] HBITMAP PreviewFor(
        int slot, const std::wstring& path, double rotation, int width, int height, bool coarse);
    void ReleasePreviews();

    struct SourceSizeEntry
    {
        std::wstring path;
        core::ImageSize size;
    };

    /// Pixel size of an image, read from its header once per file.
    [[nodiscard]] core::ImageSize SourceSizeFor(const std::wstring& path);

    [[nodiscard]] MapContext ContextForGroup(
        const Projection& projection, const core::Group& group, const core::WallpaperView& view);

    /// Picks a corner of the selected group's box whose outward quadrant covers no other
    /// display, then pins the two-headed arc handle there. Computed inside ComputeGeometry so
    /// paint and hit-test always see the same placement.
    void PlaceRotateArc(Geometry& geometry) const;
    void PaintRotateArc(HDC dc, const Geometry& geometry) const;
    [[nodiscard]] bool HitRotateArc(POINT point, const Geometry& geometry) const;

    /// Current framing of the active group. Kept here rather than read from the state on every
    /// paint so a drag shows its result immediately instead of waiting for the round trip.
    core::WallpaperView m_view{};
    int m_viewSlot{ -1 };

    /// Pan gesture state.
    bool m_panning{};
    POINT m_panOrigin{};
    core::WallpaperView m_panStart{};

    /// Continuous turn gesture on the turn chip. Steering is by arc length, not by pointer angle:
    /// the cursor travels along the circle it started on, and the picture turns by that distance
    /// over the starting radius - slower than 1:1, and it cannot accelerate when the drag wanders
    /// towards the centre. A short click with no movement still steps a quarter turn.
    bool m_rotating{};
    bool m_rotationMoved{};
    POINT m_rotationLast{};
    /// Where the press landed. A gesture that travels is a drag even when the angle has not moved
    /// yet - the right-angle deadband swallows the first few degrees, and a drag that ends inside
    /// it must not be mistaken for a click and step a whole quarter turn.
    POINT m_rotationOrigin{};
    /// The centre of the group's box at press: the circle the hand is steering around, and the
    /// pivot the picture turns about. Not the chip, which is only a grip.
    POINT m_rotationCentre{};
    double m_rotationRadius{ 120.0 };
    double m_rotationDelta{};
    core::WallpaperView m_rotationStart{};
    /// The right angle the gesture is currently held on, or a negative value when it is free.
    /// Capture and release use different distances, which is what makes the angle stick: 2.5
    /// degrees are enough to arrive, 6 are needed to leave.
    int m_rotationSnap{ -1 };

    /// Calendar drag: a move gesture, not a resize. The new position is committed on release so
    /// the settings file is not rewritten once per mouse move.
    bool m_draggingCanvas{};
    POINT m_canvasGrab{};
    core::CanvasRect m_canvasStart{};
    bool m_canvasLive{};
    core::CanvasRect m_canvasDrag{};

    /// A wheel zoom is committed to the model on a short idle instead of on every event.
    static constexpr UINT_PTR kZoomCommitTimer = 1;
    static constexpr UINT kZoomCommitDelayMs = 150;
    static constexpr UINT_PTR kSpinTimer = 2;
    static constexpr UINT kSpinDelayMs = 60;
    bool m_zoomPending{};
    int m_wheelResidual{};
    int m_spinAngle{};

    void CommitView();

    /// The open popup, if any. One at a time, and a click anywhere else closes it first.
    enum class Popup
    {
        None,
        LanguageMenu, // anchored at the title-bar language button
        GroupJoin,  // anchored at a screen number
        GroupMerge, // anchored at the active chip's plus
        NoteColors, // anchored at the notes page's palette button
    };

    Popup m_popup{ Popup::None };

    /// Which page of the tabbed sheet is on show. Defaults to the layout, the page the panel
    /// exists for; the choice is session-local, not a setting.
    Tab m_tab{ Tab::Layout };
    int m_popupIndex{ -1 }; // monitor index, for the screen the group menu belongs to
    POINT m_popupAt{};
    int m_popupItem{ -1 };

    /// Which shortcut row on the config page is waiting for a key press. -1 means nothing is being
    /// recorded; otherwise it is a HotkeyAction value. While it is set the global combinations are
    /// unregistered, so the key being recorded reaches this window instead of firing the old one.
    int m_recordingAction{ -1 };

    enum class Modal
    {
        None,
        Quit,
        Help,
        UpdateNotes,
    };

    Modal m_modal{ Modal::None };

    [[nodiscard]] std::vector<PopupEntry> PopupItems() const;
    [[nodiscard]] std::wstring PopupHead() const;
    [[nodiscard]] RECT PopupRect(const Geometry& geometry) const;

    /// One rectangle per entry, empty for separators. Shared by the paint and the hit test.
    [[nodiscard]] std::vector<RECT> PopupItemRects(
        const Geometry& geometry, const RECT& card) const;

    [[nodiscard]] ModalLayout LayoutModal(const Geometry& geometry) const;

    /// Undo for the last tick change.
    int m_undoRow{ -1 };
    bool m_undoValue{};
    bool m_undoValid{};

    /// Keyboard focus, as an index into the focus ring built for the current geometry.
    int m_focus{ -1 };
    [[nodiscard]] std::vector<Target> FocusRing(const Geometry& geometry) const;
    [[nodiscard]] RECT RectForTarget(const Geometry& geometry, const Target& target) const;

    void Activate(const Target& target);
    void ActivatePopup(int item);
    /// Drops one row from the note palette. Separate from ActivatePopup because it has to put the
    /// menu back afterwards: the user is usually removing one paper among several, not finishing.
    void ActivatePopupRemove(int item);

    /// Hands a paper palette to the service and reopens the editor on the result. Empty means the
    /// built-in set. `removed` is the index that went away, or -1 for an edit, an addition or a
    /// reset: the notes' paper indexes shift when one is dropped, and only the caller knows which.
    void ApplyPapers(std::vector<std::uint32_t> papers, int removed = -1);

    void ActivateModal(int button);

    void OpenPopup(Popup kind, POINT anchor, int index);
    // A hotkey row is activated by closing the menu first. That must not re-arm the global
    // combinations: the key about to be recorded may be one of them.
    void ClosePopup();

    /// Turns one screen number on or off in the filter. An empty filter means "all".
    void ToggleFilter(int monitorIndex);

    void OnKeyDown(WPARAM key);
    void ZoomActiveGroup(double factor, POINT anchor);
    void PanActiveGroup(int dx, int dy);
    void ResetActiveView();

    ActionHandler m_onAction;
    StateProvider m_readState;
    PanelState m_state;

    HWND m_window{};
    HINSTANCE m_instance{};

    /// Screen filter. Session state: it describes what the user is looking at right now, not a
    /// preference, so it is deliberately not written to disk.
    std::vector<std::wstring> m_filter;

    /// Client size in design units, re-derived from the window on every geometry pass. The
    /// window opens at 4:3 and is user-resizable, so these are inputs the layout adapts to, not
    /// consequences of the model. Mutable because every const hit-test member recomputes them.
    mutable int m_clientW{ 820 };
    mutable int m_clientH{ 615 };

    /// First visible row of the window list.
    int m_scroll{};

    /// First visible row of the notes page's history card. The card holds as many rows as the
    /// window's height allows and wheels through the rest. Mutable for the same reason as the
    /// client size: the const geometry pass clamps it to the slice it can actually show.
    mutable int m_archiveScroll{ 0 };

    UINT m_dpi{ 96 };

    std::vector<GroupPreview> m_previews;
    std::vector<SourceSizeEntry> m_sources;

    PixelSurface m_surface{};

    Hot m_hot{ Hot::None };
    int m_hotIndex{ -1 };
    Hot m_pressed{ Hot::None };
    int m_pressedIndex{ -1 };
    POINT m_pressedAt{};
    bool m_trackingLeave{};
};

/// The map's preview bitmap is decoded at this multiple of the drawn canvas size and then sampled
/// with zoom applied, so panning and zooming never touch WIC.
inline constexpr int kMapSupersample = 2;

/// Class name of the panel window, exposed so a second process can find the first one.
inline constexpr wchar_t kPanelClassName[] = L"WinTools.Panel.6d21f0a4";

/// Brings an already running instance's panel to the front. No-op when none is running.
void ActivateExistingPanel();

} // namespace wintools::app
