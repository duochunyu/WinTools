#pragma once

#include <span>

#include "core/layout_math.h"
#include "core/window_layout.h"

namespace wintools::core
{

/// The desktop canvas is a square, so its position is one point and one edge length.
///
/// Kept as a square rather than a rectangle because the widget it hosts is a disc: a rectangle
/// would need two sizes that can disagree, and a disc that is not round is a bug, not a setting.
struct CanvasRect
{
    /// Top-left corner in virtual-desktop pixels: the same space monitors report their rect in,
    /// which is what makes "put it on the second screen" a comparison instead of a conversion.
    int x{};
    int y{};
    int size{ 240 };

    [[nodiscard]] int Right() const
    {
        return x + size;
    }

    [[nodiscard]] int Bottom() const
    {
        return y + size;
    }

    [[nodiscard]] bool operator==(const CanvasRect& other) const
    {
        return x == other.x && y == other.y && size == other.size;
    }
};

inline constexpr int kCanvasMinSize = 120;
inline constexpr int kCanvasMaxSize = 480;
inline constexpr int kCanvasDefaultSize = 240;

/// True when the point is inside the square.
[[nodiscard]] bool CanvasContains(const CanvasRect& rect, int x, int y);

/// The display the canvas belongs to: the one under its centre, else the one it overlaps most,
/// else the nearest one. -1 only when there is no display at all.
[[nodiscard]] int CanvasHomeMonitor(
    const CanvasRect& rect, std::span<const MonitorSnapshot> monitors);

/// Puts the square fully on one display, keeping its size.
///
/// Called both when the user drops it in the panel and when displays change underneath it: a
/// canvas half off the edge of a screen is a widget the user cannot see and cannot grab again.
[[nodiscard]] CanvasRect ClampCanvas(
    const CanvasRect& rect, std::span<const MonitorSnapshot> monitors);

/// Size clamped to kCanvasMinSize..kCanvasMaxSize.
[[nodiscard]] int ClampCanvasSize(int size);

} // namespace wintools::core
