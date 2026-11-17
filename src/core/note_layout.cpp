#include "core/note_layout.h"

#include <algorithm>
#include <cstdlib>

namespace wintools::core
{

namespace
{

/// Area of the intersection, or 0 when they do not touch.
long long OverlapArea(const Rect& a, const Rect& b)
{
    const int width = std::min(a.Right(), b.Right()) - std::max(a.left, b.left);
    const int height = std::min(a.Bottom(), b.Bottom()) - std::max(a.top, b.top);

    if (width <= 0 || height <= 0)
    {
        return 0;
    }

    return static_cast<long long>(width) * height;
}

/// Distance between the two centres, used only to break ties between displays the note misses.
long long CentreDistance(const Rect& a, const Rect& b)
{
    const long long dx = (a.left + a.width / 2) - (b.left + b.width / 2);
    const long long dy = (a.top + a.height / 2) - (b.top + b.height / 2);

    return dx * dx + dy * dy;
}

/// Slides a single axis so [start, start + length) fits inside [low, high).
int FitAxis(int start, int length, int low, int high)
{
    const int span = high - low;
    if (length >= span)
    {
        // Bigger than the display: pin it to the edge the user can actually grab, rather than
        // leaving an arbitrary overhang on whichever side it happened to be.
        return low;
    }

    return std::clamp(start, low, high - length);
}

} // namespace

bool NoteContains(const Rect& rect, int x, int y)
{
    return x >= rect.left && x < rect.Right() && y >= rect.top && y < rect.Bottom();
}

int NoteHomeMonitor(const Rect& rect, std::span<const MonitorSnapshot> monitors)
{
    if (monitors.empty())
    {
        return -1;
    }

    const int centreX = rect.left + rect.width / 2;
    const int centreY = rect.top + rect.height / 2;

    for (std::size_t index = 0; index < monitors.size(); ++index)
    {
        if (NoteContains(monitors[index].rect, centreX, centreY))
        {
            return static_cast<int>(index);
        }
    }

    int best = -1;
    long long bestArea = 0;
    for (std::size_t index = 0; index < monitors.size(); ++index)
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

    best = 0;
    long long bestDistance = CentreDistance(rect, monitors[0].rect);
    for (std::size_t index = 1; index < monitors.size(); ++index)
    {
        const long long distance = CentreDistance(rect, monitors[index].rect);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = static_cast<int>(index);
        }
    }

    return best;
}

Rect ClampNoteSize(const Rect& rect)
{
    Rect result = rect;
    result.width = std::clamp(result.width, kNoteMinWidth, kNoteMaxWidth);
    result.height = std::clamp(result.height, kNoteMinHeight, kNoteMaxHeight);
    return result;
}

Rect ClampNote(const Rect& rect, std::span<const MonitorSnapshot> monitors)
{
    Rect result = ClampNoteSize(rect);

    const int home = NoteHomeMonitor(result, monitors);
    if (home < 0)
    {
        return result;
    }

    const Rect& monitor = monitors[static_cast<std::size_t>(home)].rect;

    // Never wider than the display it lives on: a note that is asked for a size the screen cannot
    // show is a bug in the caller, and pinning the size keeps it usable instead of clipping text
    // the user cannot scroll to.
    result.width = std::min(result.width, monitor.width);
    result.height = std::min(result.height, monitor.height);

    result.left = FitAxis(result.left, result.width, monitor.left, monitor.Right());
    result.top = FitAxis(result.top, result.height, monitor.top, monitor.Bottom());

    return result;
}

Rect DefaultNoteRect(std::span<const MonitorSnapshot> monitors, int cascade)
{
    Rect rect{};
    rect.width = kNoteDefaultWidth;
    rect.height = kNoteDefaultHeight;

    if (monitors.empty())
    {
        return rect;
    }

    // The primary display is the one whose origin is (0, 0) - that is what "primary" means in
    // virtual-desktop coordinates, and it avoids a separate display-config query for it.
    std::size_t primary = 0;
    for (std::size_t index = 0; index < monitors.size(); ++index)
    {
        if (monitors[index].rect.left == 0 && monitors[index].rect.top == 0)
        {
            primary = index;
            break;
        }
    }

    const Rect& monitor = monitors[primary].rect;
    const int step = 36 * std::max(0, cascade % 6);

    rect.left = monitor.left + 96 + step;
    rect.top = monitor.top + 96 + step;

    return ClampNote(rect, monitors);
}

} // namespace wintools::core
