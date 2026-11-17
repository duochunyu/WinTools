#pragma once

namespace wintools::core
{

/// Axis-aligned rectangle in virtual-desktop pixels.
struct Rect
{
    int left{};
    int top{};
    int width{};
    int height{};

    [[nodiscard]] int Right() const
    {
        return left + width;
    }

    [[nodiscard]] int Bottom() const
    {
        return top + height;
    }

    [[nodiscard]] bool IsValid() const
    {
        return width > 0 && height > 0;
    }

    [[nodiscard]] bool operator==(const Rect& other) const
    {
        return left == other.left && top == other.top && width == other.width
            && height == other.height;
    }
};

/// One monitor's piece of a spanning image: which rectangle of the source image it comes from,
/// and what size it must end up at.
///
/// Two coordinate systems are carried on purpose:
///
///   source*  - pixels in the original image. Only used for logging and test assertions.
///   canvas*  - pixels in the cover-scaled image, i.e. after the whole picture has been scaled to
///              scaledWidth x scaledHeight. This is what the imaging pipeline actually consumes.
///
/// The split exists because WIC's clipper operates on the scaler's output, so passing source
/// coordinates to it silently truncates. The PowerToys C# original hit the same wall with
/// BitmapTransform.Bounds, which returns E_INVALIDARG for any slice not starting at the origin.
struct Slice
{
    int sourceX{};
    int sourceY{};
    int sourceWidth{};
    int sourceHeight{};

    int outputWidth{};
    int outputHeight{};

    int canvasX{};
    int canvasY{};
    int scaledWidth{};
    int scaledHeight{};

    /// True when the source needed no scaling at all (cover scale == 1.0).
    ///
    /// This is what decides whether the renderer may clip straight out of the decoded frame and
    /// skip the scaler entirely. Note it is stronger than IsExactCopy(): a source can happen to
    /// produce a piece the same size as the monitor after scaling down, and in that case the
    /// canvas coordinates differ from the source coordinates even though the sizes match.
    bool unscaled{};

    /// True when the piece can be copied without any resampling.
    ///
    /// Meaningful for logging and assertions. Renderers must branch on `unscaled`, not on this:
    /// a 1:1 scale through a resampler can still shift flat colour areas by +/-1, and
    /// pixel-for-pixel equality with the C# implementation is the acceptance criterion here.
    [[nodiscard]] bool IsExactCopy() const
    {
        return sourceWidth == outputWidth && sourceHeight == outputHeight;
    }
};

} // namespace wintools::core
