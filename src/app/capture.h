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

    /// The same picture as top-down 32-bit BGRA. Kept because the clipboard takes pixels, not a
    /// file: copying would otherwise mean decoding the PNG that the capture just encoded.
    std::vector<std::uint8_t> bgra;
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

/// What a capture ended as. Cancelling is not a failure: the user pressed Esc, and saying "failed"
/// to someone who changed their mind is its own small bug.
enum class CaptureResult
{
    Cancelled,
    Copied,
    Failed,
};

/// Lets the user drag a rectangle and puts it on the clipboard, so that Ctrl+V anywhere can paste
/// it.
///
/// Nothing is written to disk: a screenshot the user is about to paste has no reason to become a
/// file, and the files it used to become were invisible to them anyway.
[[nodiscard]] CaptureResult CopyRegionToClipboard(HINSTANCE instance);

/// Encodes a rectangle of 32-bit BGRA pixels as a PNG.
///
/// Shared with the note's paste path: RichEdit takes a picture as RTF and nothing else, so anything
/// that arrives on the clipboard as pixels has to be encoded first.
[[nodiscard]] bool EncodePng(
    const void* bits, int width, int height, std::vector<std::uint8_t>& out);

/// The registered "PNG" clipboard format, which screenshot tools publish next to the DIB.
///
/// Registered once: the id is process-wide and never changes. Zero means registration failed.
[[nodiscard]] UINT PngClipboardFormat();

} // namespace wintools::app
