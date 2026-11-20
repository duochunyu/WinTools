#pragma once

namespace wintools::app
{

/// Desktop widget plugins - the calendar today, more later.
///
/// Shelved for this release: the widget works, but the product around it (which widgets, how they
/// are placed, how they are configured) still needs design work. Every entry point is compiled out
/// of the published build while the code stays in place: the panel switch, the tray item, the
/// arrangement-map drag target and the startup restore. Flip this to true to bring them back.
inline constexpr bool kDesktopWidgetsEnabled = false;

} // namespace wintools::app
