#include "core/layout_math.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <numbers>

namespace wintools::core
{

namespace
{

int Clamp(int value, int low, int high)
{
    if (value < low)
    {
        return low;
    }
    return value > high ? high : value;
}

/// Matches C#'s Math.Round(double), which defaults to MidpointRounding.ToEven.
///
/// std::round would NOT match: it rounds half away from zero, so 0.5 becomes 1 here and stays 0
/// there. std::nearbyint uses the FPU's current rounding mode, which is round-to-nearest-even by
/// default - the same rule. Getting this wrong shifts every slice by one pixel on half the
/// layouts, and the whole point of this port is pixel-for-pixel equality with the C# original.
int RoundToEven(double value)
{
    return static_cast<int>(std::nearbyint(value));
}

} // namespace

Rect GetCanvas(std::span<const Monitor> monitors)
{
    if (monitors.empty())
    {
        return {};
    }

    int left = INT_MAX;
    int top = INT_MAX;
    int right = INT_MIN;
    int bottom = INT_MIN;

    for (const auto& monitor : monitors)
    {
        left = std::min(left, monitor.left);
        top = std::min(top, monitor.top);
        right = std::max(right, monitor.Right());
        bottom = std::max(bottom, monitor.Bottom());
    }

    return Rect{ left, top, right - left, bottom - top };
}

WallpaperView ClampView(WallpaperView view)
{
    // std::clamp is useless against NaN: every comparison with it is false, so it returns the NaN
    // unchanged and it then poisons every slice derived from it. Infinity is caught the same way.
    const auto finite = [](double value) { return std::isfinite(value); };

    view.zoom = finite(view.zoom) ? std::clamp(view.zoom, kMinZoom, kMaxZoom) : kMinZoom;
    view.focusX = finite(view.focusX) ? std::clamp(view.focusX, 0.0, 1.0) : 0.5;
    view.focusY = finite(view.focusY) ? std::clamp(view.focusY, 0.0, 1.0) : 0.5;

    // Any angle is legal, but keeping it in [0, 360) means the preview cache sees one key per
    // orientation instead of an unbounded family of equivalent ones. NaN/inf fold to zero.
    view.rotation = finite(view.rotation) ? std::fmod(view.rotation, 360.0) : 0.0;
    if (view.rotation < 0.0)
    {
        view.rotation += 360.0;
    }
    return view;
}

RotatedBox RotatedBounds(int width, int height, double degrees)
{
    if (width <= 0 || height <= 0 || !std::isfinite(degrees))
    {
        return {};
    }

    const double radians = std::fmod(degrees, 360.0) * std::numbers::pi / 180.0;
    const double cosine = std::abs(std::cos(radians));
    const double sine = std::abs(std::sin(radians));

    // ceil: a fraction of a pixel of the turned picture still has to live somewhere, and the
    // framing math works in whole pixels. The small epsilon keeps cos(90) from turning an exact
    // 600-pixel swap into 601 pixels through floating-point noise.
    const auto whole = [](double value)
    {
        return std::max(1, static_cast<int>(std::ceil(value - 1e-9)));
    };
    RotatedBox box;
    box.width = whole(width * cosine + height * sine);
    box.height = whole(width * sine + height * cosine);
    return box;
}

ViewSlack ComputeSlack(
    const Rect& canvas, int sourceWidth, int sourceHeight, const WallpaperView& view)
{
    ViewSlack slack;
    if (sourceWidth <= 0 || sourceHeight <= 0 || !canvas.IsValid())
    {
        return slack;
    }

    const WallpaperView clamped = ClampView(view);

    const double radians = clamped.rotation * std::numbers::pi / 180.0;
    const double cosine = std::abs(std::cos(radians));
    const double sine = std::abs(std::sin(radians));

    // How much picture the canvas needs along the picture's own axes: rotate the canvas back by
    // the turn and take its extent. At 0 and 90 degrees this is the canvas size, so an axis-aligned
    // turn keeps the old numbers exactly.
    const double across = canvas.width * cosine + canvas.height * sine;
    const double down = canvas.width * sine + canvas.height * cosine;

    // Cover with the picture, not with its bounding box. The box is bigger than the picture by
    // four corner triangles, and covering with it is what used to leave a wedge of matte on every
    // display once the picture was turned off-axis.
    const double cover =
        std::max(across / sourceWidth, down / sourceHeight);
    const double scale = cover * clamped.zoom;

    const RotatedBox turned = RotatedBounds(sourceWidth, sourceHeight, clamped.rotation);

    // Ceiling keeps the scaled image at or above the canvas so no monitor slice can fall off the
    // edge; the extra fraction of a pixel is lost in the framing offset.
    slack.scale = scale;
    slack.scaledWidth = static_cast<int>(std::ceil(turned.width * scale));
    slack.scaledHeight = static_cast<int>(std::ceil(turned.height * scale));
    slack.x = std::max(0, slack.scaledWidth - canvas.width);
    slack.y = std::max(0, slack.scaledHeight - canvas.height);
    slack.unscaled = (scale == 1.0);

    // An axis-aligned turn leaves no empty corner, so the whole box is usable and the framing
    // matches the slack to the pixel - which is what keeps the unturned output byte-identical.
    // The comparison needs a tolerance: cos(90 degrees) in double is 6e-17, not zero, and feeding
    // that into the room maths below would collapse it to nothing.
    constexpr double kAxisEpsilon = 1e-9;
    slack.cosine = std::cos(radians);
    slack.sine = std::sin(radians);
    slack.rotation = clamped.rotation;

    if (sine < kAxisEpsilon || cosine < kAxisEpsilon)
    {
        slack.halfX = slack.x / 2.0;
        slack.halfY = slack.y / 2.0;
        return slack;
    }

    // The canvas needs `across` by `down` of picture, measured along the picture's own axes, so
    // that is what erodes the picture - the same two numbers the cover above was computed from.
    //
    // A whole pixel comes off each. The offset is rounded to an integer, and that rounding lands
    // half a pixel out on each canvas axis - up to 0.71 of a picture pixel - which is enough to
    // show a hairline of matte along an edge the constraint had just declared safe.
    constexpr double kRoundMargin = 1.0;
    slack.halfX = std::max(0.0, (sourceWidth * scale - across) / 2.0 - kRoundMargin);
    slack.halfY = std::max(0.0, (sourceHeight * scale - down) / 2.0 - kRoundMargin);
    slack.turned = true;
    return slack;
}

namespace
{

/// One axis of an axis-aligned framing: where the window starts along `slack` pixels of travel.
///
/// The untouched default (zoom 1.0, centred) keeps the original truncating division instead of a
/// round, because that is what produced the pixel-for-pixel match against the C# original. Once
/// the user has actually moved or enlarged the picture the exact pixel no longer carries that
/// meaning, and rounding is the honest choice.
int FrameOffsetOne(int slack, double focus, bool untouchedDefault)
{
    if (slack <= 0)
    {
        return 0;
    }

    if (untouchedDefault)
    {
        return slack / 2;
    }

    return std::clamp(static_cast<int>(std::lround(slack * focus)), 0, slack);
}

} // namespace

FrameOffset FrameOffsetFor(const ViewSlack& fit, const WallpaperView& view)
{
    const WallpaperView clamped = ClampView(view);
    const bool untouched =
        clamped.zoom == kMinZoom && clamped.focusX == 0.5 && clamped.focusY == 0.5;

    // Unturned or quarter-turned: one axis at a time, exactly as the original pipeline did it.
    if (!fit.turned)
    {
        return FrameOffset{ FrameOffsetOne(fit.x, clamped.focusX, untouched),
            FrameOffsetOne(fit.y, clamped.focusY, untouched) };
    }

    // Turned: the focus pair addresses a point in the picture's own box, and the offset is that
    // point carried back into canvas axes. Centred is the same point at every angle, so an
    // untouched view stays exactly where the cover put it.
    const double u = (2.0 * clamped.focusX - 1.0) * fit.halfX;
    const double v = (2.0 * clamped.focusY - 1.0) * fit.halfY;
    const double centreX = untouched ? static_cast<double>(fit.x / 2) : fit.x / 2.0;
    const double centreY = untouched ? static_cast<double>(fit.y / 2) : fit.y / 2.0;

    return FrameOffset{
        static_cast<int>(std::lround(centreX + u * fit.cosine - v * fit.sine)),
        static_cast<int>(std::lround(centreY + u * fit.sine + v * fit.cosine)) };
}

WallpaperView FocusForOffset(const ViewSlack& fit, const WallpaperView& view, double x, double y)
{
    WallpaperView result = ClampView(view);

    if (!fit.turned)
    {
        result.focusX = (fit.x > 0) ? std::clamp(x / fit.x, 0.0, 1.0) : 0.5;
        result.focusY = (fit.y > 0) ? std::clamp(y / fit.y, 0.0, 1.0) : 0.5;
        return result;
    }

    const double dx = x - fit.x / 2.0;
    const double dy = y - fit.y / 2.0;
    const double u = dx * fit.cosine + dy * fit.sine;
    const double v = -dx * fit.sine + dy * fit.cosine;

    result.focusX = (fit.halfX > 0.0) ? std::clamp(0.5 + u / (2.0 * fit.halfX), 0.0, 1.0) : 0.5;
    result.focusY = (fit.halfY > 0.0) ? std::clamp(0.5 + v / (2.0 * fit.halfY), 0.0, 1.0) : 0.5;
    return result;
}

std::optional<Slice> ComputeSlice(
    const Rect& canvas,
    int sourceWidth,
    int sourceHeight,
    const Monitor& monitor,
    const WallpaperView& view)
{
    if (sourceWidth <= 0 || sourceHeight <= 0)
    {
        return std::nullopt;
    }

    if (!canvas.IsValid())
    {
        return std::nullopt;
    }

    const WallpaperView clamped = ClampView(view);
    const ViewSlack fit = ComputeSlack(canvas, sourceWidth, sourceHeight, clamped);
    const double scale = fit.scale;

    const int scaledWidth = fit.scaledWidth;
    const int scaledHeight = fit.scaledHeight;

    // The bitmap the renderer holds is the turned one, so its pixel size is the bounding box.
    const RotatedBox bitmap = RotatedBounds(sourceWidth, sourceHeight, clamped.rotation);

    // Where the window starts inside that bitmap. One function answers this for the slicer and for
    // the panel's preview; letting the two drift apart is a preview that disagrees with the
    // wallpaper it claims to predict.
    const FrameOffset origin = FrameOffsetFor(fit, clamped);
    const int offsetX = origin.x;
    const int offsetY = origin.y;

    const int canvasX = monitor.left - canvas.left + offsetX;
    const int canvasY = monitor.top - canvas.top + offsetY;

    // Back to source pixels, then clamp so rounding can never ask for pixels the source does not
    // have. Source coordinates are kept for logging and assertions only; rendering uses the
    // canvas ones.
    const int sourceX = Clamp(RoundToEven(canvasX / scale), 0, bitmap.width - 1);
    const int sourceY = Clamp(RoundToEven(canvasY / scale), 0, bitmap.height - 1);

    int sliceWidth = std::max(1, RoundToEven(monitor.width / scale));
    int sliceHeight = std::max(1, RoundToEven(monitor.height / scale));

    if (sourceX + sliceWidth > bitmap.width)
    {
        sliceWidth = bitmap.width - sourceX;
    }

    if (sourceY + sliceHeight > bitmap.height)
    {
        sliceHeight = bitmap.height - sourceY;
    }

    Slice slice;
    slice.sourceX = sourceX;
    slice.sourceY = sourceY;
    slice.sourceWidth = sliceWidth;
    slice.sourceHeight = sliceHeight;
    slice.outputWidth = monitor.width;
    slice.outputHeight = monitor.height;
    slice.canvasX = canvasX;
    slice.canvasY = canvasY;
    slice.scaledWidth = scaledWidth;
    slice.scaledHeight = scaledHeight;
    slice.unscaled = fit.unscaled;
    return slice;
}

} // namespace wintools::core
