#pragma once

#include <optional>
#include <span>

#include "core/monitor.h"
#include "core/slice.h"

namespace wintools::core
{

/// Bounding box of every attached monitor, in virtual-desktop pixels.
///
/// Returns a zero-size rectangle for an empty input rather than throwing: the caller decides
/// whether "no monitors" is an error, and a box of 0x0 makes that decision obvious.
[[nodiscard]] Rect GetCanvas(std::span<const Monitor> monitors);

/// How the picture is framed inside the virtual-desktop box.
///
/// Cover-and-cut is a sensible default, but it is only one possible crop of the picture: a photo
/// of three people has to be shifted to keep all three inside a 3840x2640 box that is mostly two
/// 1920x1200 rectangles with a 3440x1440 slab above them. Letting the user move and enlarge the
/// picture is the difference between "the tool guessed wrong" and "I fixed it in two seconds".
struct WallpaperView
{
    /// 1.0 makes the picture cover the box exactly. Above that it is enlarged and cropped more.
    ///
    /// Never below 1.0: a smaller zoom would leave part of some display with no image at all,
    /// and a wallpaper with bare desktop showing through is not a wallpaper.
    double zoom{ 1.0 };

    /// Which part of the slack is shown: 0 is the left/top edge of the picture, 1 the right/bottom
    /// edge, 0.5 centred. Only meaningful on the axis where the picture is larger than the box;
    /// on the other axis there is no slack and the value is ignored.
    ///
    /// On a turned picture the two axes are the picture's own, which is why a drag can run in any
    /// direction: they are split along the turn rather than along the screen.
    double focusX{ 0.5 };
    double focusY{ 0.5 };

    /// Degrees clockwise applied to the picture before any framing math runs. Any angle is
    /// legal - the drag handle turns the picture continuously, not in quarter steps. Zoom and
    /// focus keep their meaning: they are relative to the rotated picture's bounding box.
    double rotation{ 0.0 };

    /// Compared field by field so the panel can tell "the user moved the picture" from "nothing
    /// happened" and skip a settings write plus a repaint on a drag that ended where it started.
    [[nodiscard]] bool operator==(const WallpaperView&) const = default;
};

constexpr double kMinZoom = 1.0;
constexpr double kMaxZoom = 8.0;

/// The axis-aligned box that fully contains a `width x height` picture turned clockwise by
/// `degrees`. A 90-degree turn swaps the dimensions; an oblique turn grows both, because the
/// corners stick out. Both the composer and the panel use this so the preview's framing math
/// never disagrees with the bitmap it is framing.
struct RotatedBox
{
    int width{};
    int height{};
};

[[nodiscard]] RotatedBox RotatedBounds(int width, int height, double degrees);

/// Brings a view back into the range the renderer accepts. Anything read from a file or computed
/// from mouse deltas goes through here before it can reach the slicing code.
[[nodiscard]] WallpaperView ClampView(WallpaperView view);

/// How much room the enlarged picture has to move inside the box, plus the scale that produced it.
///
/// The panel needs this to turn a mouse delta into a change of focus: dragging by N pixels moves
/// the picture by N pixels, so the focus moves by N / slack. At zoom 1.0 one axis has no slack at
/// all (the picture covers it exactly) and dragging along that axis must do nothing.
struct ViewSlack
{
    int scaledWidth{};
    int scaledHeight{};
    /// Slack in canvas pixels along each canvas axis; zero where the picture covers it exactly.
    int x{};
    int y{};
    /// How far the window may slide along each of the *picture's* axes, as a half-extent in canvas
    /// pixels.
    ///
    /// The window has to stay inside the turned picture, and a rectangle inside a turned rectangle
    /// is a turned rectangle: the usable region is a box in the picture's own frame, not in the
    /// canvas's. Half-extents, because the focus runs -1..1 about the centre.
    ///
    /// This is what makes a moved picture travel in any direction: clamping the two canvas axes to
    /// the box *around* that region would stop a diagonal drag long before the picture ran out.
    double halfX{};
    double halfY{};
    /// True for every angle that is not a multiple of 90 degrees. The axis-aligned path is kept
    /// separate so a quarter turn stays pixel-identical to the unturned framing.
    bool turned{};
    /// cos/sin of the turn, published so callers split a mouse delta the same way the offset is
    /// composed and cannot disagree on the sign.
    double cosine{ 1.0 };
    double sine{ 0.0 };
    double rotation{};
    /// The scale actually used, before rounding the size up. Kept because the slice maths divides
    /// by it, and dividing by the rounded size instead would shift slices by a pixel on some
    /// layouts.
    double scale{ 1.0 };
    bool unscaled{};
};

/// Top-left corner of the window inside the turned bitmap, in canvas pixels.
///
/// This is the single answer both the slicer and the panel need: the panel shifts its preview by
/// it, `ComputeSlice` offsets each monitor by it. Two implementations of this arithmetic drift,
/// and a drifting one shows up as a preview that does not match the applied wallpaper.
struct FrameOffset
{
    int x{};
    int y{};
};

[[nodiscard]] FrameOffset FrameOffsetFor(const ViewSlack& fit, const WallpaperView& view);

/// The view that puts the window's top-left at (`x`, `y`) canvas pixels - the inverse of
/// `FrameOffsetFor`, used by the zoom that has to keep one pixel under the cursor.
[[nodiscard]] WallpaperView FocusForOffset(
    const ViewSlack& fit, const WallpaperView& view, double x, double y);


/// `sourceWidth`/`sourceHeight` are the picture's own pixel size, **before** the view's rotation:
/// turning is this function's business, not the caller's. It covers the box with the turned
/// picture - not with its bounding box, whose empty corners would leave a matte wedge on every
/// display - and reports how far that picture may still be panned.
[[nodiscard]] ViewSlack ComputeSlack(
    const Rect& canvas, int sourceWidth, int sourceHeight, const WallpaperView& view);

/// Computes the source rectangle and output size for one monitor.
///
/// The algorithm is deliberately "cover, then cut":
///
///   1. scale the whole source image by max(canvasW/srcW, canvasH/srcH) so it covers the virtual
///      desktop bounding box completely;
///   2. place that scaled image over the box, centred unless the view was panned;
///   3. give each monitor the slice that falls under its own rectangle.
///
/// The shell's own DESKTOP_WALLPAPER_POSITION.Span maps the image onto the bounding box instead,
/// which is only correct when the source is exactly the size of that box and the monitors tile
/// it completely. Real layouts rarely do - two 1920x1200 screens below a 3440x1440 ultrawide
/// leave 5.7% of the box uncovered - so covering is what keeps every monitor filled and every
/// slice inside the source.
///
/// `sourceWidth`/`sourceHeight` are, like ComputeSlack's, the picture before the turn; the slice
/// it returns indexes the turned bitmap the renderer actually holds.
///
/// Returns std::nullopt for non-positive source or canvas sizes.
[[nodiscard]] std::optional<Slice> ComputeSlice(
    const Rect& canvas,
    int sourceWidth,
    int sourceHeight,
    const Monitor& monitor,
    const WallpaperView& view = {});

} // namespace wintools::core
