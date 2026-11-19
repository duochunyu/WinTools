#pragma once

#include <span>

#include "core/slice.h"
#include "core/window_layout.h"

namespace wintools::core
{

/// Where a sticky note sits, in virtual-desktop pixels.
///
/// Reuses Rect rather than a type of its own because a note is an ordinary rectangle: unlike the
/// disc-shaped desktop canvas, width and height are genuinely independent here, and inventing
/// another struct would only add a conversion at every boundary.
inline constexpr int kNoteMinWidth = 220;
inline constexpr int kNoteMinHeight = 150;
inline constexpr int kNoteMaxWidth = 1600;
inline constexpr int kNoteMaxHeight = 1200;
inline constexpr int kNoteDefaultWidth = 360;
inline constexpr int kNoteDefaultHeight = 300;

/// True when the point is inside the note.
[[nodiscard]] bool NoteContains(const Rect& rect, int x, int y);

/// The display the note belongs to: the one under its centre, else the one it overlaps most,
/// else the nearest one. -1 only when there is no display at all.
[[nodiscard]] int NoteHomeMonitor(const Rect& rect, std::span<const MonitorSnapshot> monitors);

/// Width and height clamped to the allowed range, leaving the position alone.
[[nodiscard]] Rect ClampNoteSize(const Rect& rect);

/// Puts the whole note on one display, shrinking it only when it cannot fit.
///
/// Called when the user drops it and whenever displays change underneath it: a note half off the
/// edge is a note whose text the user can no longer reach, and a note that is bigger than every
/// screen is a note that cannot be grabbed anywhere.
[[nodiscard]] Rect ClampNote(const Rect& rect, std::span<const MonitorSnapshot> monitors);

/// The display containing the virtual-desktop point, or -1 when no snapshot covers it.
[[nodiscard]] int MonitorAtPoint(std::span<const MonitorSnapshot> monitors, int x, int y);

/// A sensible first position: near the top-left of `preferredMonitor` (the primary when it is -1
/// or out of range), stepped down and right by cascade so successive notes do not land exactly on
/// top of each other.
[[nodiscard]] Rect DefaultNoteRect(std::span<const MonitorSnapshot> monitors, int cascade,
    int preferredMonitor = -1);

} // namespace wintools::core
