#include "core/canvas.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace wintools::core
{

namespace
{

/// Overlapping area of two rectangles; 0 when they do not touch.
long long OverlapArea(const CanvasRect& rect, const Rect& monitor)
{
    const int left = std::max(rect.x, monitor.left);
    const int top = std::max(rect.y, monitor.top);
    const int right = std::min(rect.Right(), monitor.left + monitor.width);
    const int bottom = std::min(rect.Bottom(), monitor.top + monitor.height);

    if (right <= left || bottom <= top)
    {
        return 0;
    }

    return static_cast<long long>(right - left) * (bottom - top);
}

/// Squared distance from the canvas centre to the nearest point of a monitor.
long long DistanceTo(const CanvasRect& rect, const Rect& monitor)
{
    const long long centreX = rect.x + rect.size / 2;
    const long long centreY = rect.y + rect.size / 2;

    const long long nearestX = std::clamp(centreX, static_cast<long long>(monitor.left),
        static_cast<long long>(monitor.left + monitor.width));
    const long long nearestY = std::clamp(centreY, static_cast<long long>(monitor.top),
        static_cast<long long>(monitor.top + monitor.height));

    const long long dx = centreX - nearestX;
    const long long dy = centreY - nearestY;
    return dx * dx + dy * dy;
}

} // namespace

bool CanvasContains(const CanvasRect& rect, int x, int y)
{
    return x >= rect.x && x < rect.Right() && y >= rect.y && y < rect.Bottom();
}

int ClampCanvasSize(int size)
{
    return std::clamp(size, kCanvasMinSize, kCanvasMaxSize);
}

int CanvasHomeMonitor(const CanvasRect& rect, std::span<const MonitorSnapshot> monitors)
{
    if (monitors.empty())
    {
        return -1;
    }

    const long long centreX = rect.x + rect.size / 2;
    const long long centreY = rect.y + rect.size / 2;

    for (size_t index = 0; index < monitors.size(); ++index)
    {
        const Rect& monitor = monitors[index].rect;
        if (centreX >= monitor.left && centreX < monitor.left + monitor.width
            && centreY >= monitor.top && centreY < monitor.top + monitor.height)
        {
            return static_cast<int>(index);
        }
    }

    // No centre hit: the canvas may be dragged half across a seam, or into the void between two
    // screens. The display it covers most is the one the user means.
    int best = -1;
    long long bestArea = 0;
    for (size_t index = 0; index < monitors.size(); ++index)
    {
        const long long area = OverlapArea(rect, monitors[index].rect);
        if (area > bestArea)
        {
            bestArea = area;
            best = static_cast<int>(index);
        }
    }

    if (best >= 0)
    {
        return best;
    }

    // Dragged entirely off every screen: snap to the nearest, so it cannot be lost.
    long long bestDistance = std::numeric_limits<long long>::max();
    for (size_t index = 0; index < monitors.size(); ++index)
    {
        const long long distance = DistanceTo(rect, monitors[index].rect);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = static_cast<int>(index);
        }
    }

    return best;
}

CanvasRect ClampCanvas(const CanvasRect& rect, std::span<const MonitorSnapshot> monitors)
{
    CanvasRect result = rect;
    result.size = ClampCanvasSize(result.size);

    const int home = CanvasHomeMonitor(result, monitors);
    if (home < 0)
    {
        return result;
    }

    const Rect& monitor = monitors[static_cast<size_t>(home)].rect;

    // A canvas larger than the screen it is on cannot be fully visible; keep its top-left corner
    // on the display rather than letting std::clamp see an inverted range.
    const int maxX = std::max(monitor.left, monitor.left + monitor.width - result.size);
    const int maxY = std::max(monitor.top, monitor.top + monitor.height - result.size);

    result.x = std::clamp(result.x, monitor.left, maxX);
    result.y = std::clamp(result.y, monitor.top, maxY);
    return result;
}

} // namespace wintools::core
