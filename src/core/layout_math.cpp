#include "core/layout_math.h"

#include <algorithm>
#include <climits>
#include <cmath>

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
    return view;
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

    // Cover scale: the smaller ratio would fit, the larger covers. Covering is what keeps every
    // monitor filled and every slice inside the source.
    const double cover = std::max(
        static_cast<double>(canvas.width) / sourceWidth,
        static_cast<double>(canvas.height) / sourceHeight);
    const double scale = cover * clamped.zoom;

    // Ceiling keeps the scaled image at or above the canvas so no monitor slice can fall off the
    // edge; the extra fraction of a pixel is lost in the framing offset.
    slack.scale = scale;
    slack.scaledWidth = static_cast<int>(std::ceil(sourceWidth * scale));
    slack.scaledHeight = static_cast<int>(std::ceil(sourceHeight * scale));
    slack.x = std::max(0, slack.scaledWidth - canvas.width);
    slack.y = std::max(0, slack.scaledHeight - canvas.height);
    slack.unscaled = (scale == 1.0);
    return slack;
}

namespace
{

/// Where the enlarged picture starts, relative to the canvas origin.
///
/// The untouched default (zoom 1.0, centred) keeps the original truncating division instead of a
/// round, because that is what produced the pixel-for-pixel match against the C# original. Once
/// the user has actually moved or enlarged the picture the exact pixel no longer carries that
/// meaning, and rounding is the honest choice.
int FrameOffset(int slack, double focus, bool untouchedDefault)
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

    const bool untouchedDefault =
        clamped.zoom == kMinZoom && clamped.focusX == 0.5 && clamped.focusY == 0.5;

    const int scaledWidth = fit.scaledWidth;
    const int scaledHeight = fit.scaledHeight;

    const int offsetX = FrameOffset(fit.x, clamped.focusX, untouchedDefault);
    const int offsetY = FrameOffset(fit.y, clamped.focusY, untouchedDefault);

    const int canvasX = monitor.left - canvas.left + offsetX;
    const int canvasY = monitor.top - canvas.top + offsetY;

    // Back to source pixels, then clamp so rounding can never ask for pixels the source does not
    // have. Source coordinates are kept for logging and assertions only; rendering uses the
    // canvas ones.
    const int sourceX = Clamp(RoundToEven(canvasX / scale), 0, sourceWidth - 1);
    const int sourceY = Clamp(RoundToEven(canvasY / scale), 0, sourceHeight - 1);

    int sliceWidth = std::max(1, RoundToEven(monitor.width / scale));
    int sliceHeight = std::max(1, RoundToEven(monitor.height / scale));

    if (sourceX + sliceWidth > sourceWidth)
    {
        sliceWidth = sourceWidth - sourceX;
    }

    if (sourceY + sliceHeight > sourceHeight)
    {
        sliceHeight = sourceHeight - sourceY;
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
