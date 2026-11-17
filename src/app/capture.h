#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wintools::app
{

/// A screenshot on its way into a note: PNG bytes plus the pixel size they decode to.
struct CapturedImage
{
    std::vector<std::uint8_t> png;
    int width{};
    int height{};
};

/// Largest edge, in pixels, of a screenshot as it is stored. A full 4K grab is megabytes of PNG
/// and tens of megabytes once hex-encoded into RTF, which would be rewritten to disk on every
/// autosave; a note is a few hundred pixels wide, so nothing is lost by shrinking.
inline constexpr int kCaptureMaxWidth = 960;

/// Lets the user drag a rectangle and returns it as a PNG.
///
/// The overlay covers the whole virtual desktop with a frozen screenshot, dimmed everywhere and
/// left untouched inside the rubber band, so the user selects from exactly the pixels they will
/// keep. Returns false when they cancel with Esc or the right mouse button, or when the drag is too
/// small to be anything but a stray click.
[[nodiscard]] bool PickRegion(HINSTANCE instance, CapturedImage& image);

/// Lets the user drag a rectangle and writes it as a PNG into the Pictures folder. Returns the
/// written path, or an empty string when the user cancelled or the file could not be written.
/// Used by the global capture hotkey, which has no note to insert into.
[[nodiscard]] std::wstring SaveRegionToPictures(HINSTANCE instance);

} // namespace wintools::app
