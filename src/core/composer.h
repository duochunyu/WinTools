#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/layout_math.h"
#include "core/monitor.h"
#include "core/slice.h"

namespace wintools::core
{

struct ImageSize
{
    int width{};
    int height{};
};

/// Reads the pixel size of an image without decoding its pixels.
///
/// Exists so `dump` can report slice geometry without writing files, and so the caller can warn
/// about a source that is smaller than the virtual desktop before touching the wallpaper.
[[nodiscard]] std::optional<ImageSize> QueryImageSize(const std::filesystem::path& sourceImage);


struct RenderedPiece
{
    std::filesystem::path path;
    Slice slice;

    /// Index into the monitor span this piece belongs to. Kept explicitly because a monitor can
    /// fail to render, and the caller still needs to know which display each surviving piece is
    /// for; pairing by position in the vector would silently shift assignments after a failure.
    size_t monitorIndex{};
};

/// Resolves a display's own picture. Returns an empty path when that display should keep using the
/// spanning one.
///
/// A callback rather than a list type so composer stays unaware of how assignments are stored -
/// the file format belongs to core/wallpaper_store, and the panel never sees either.
using MonitorImageLookup = std::function<std::filesystem::path(const std::wstring& devicePath)>;

/// Decodes `sourceImage` once, computes one slice per monitor and writes a PNG per monitor into
/// `outputFolder`.
///
/// The image is decoded once and reused across monitors, which matters for large wallpapers:
/// decoding a 8K JPEG takes long enough to notice when it happens three times.
///
/// A monitor with its own picture is handled as its own canvas - cover, centred - and the others
/// still share the spanning slice, so mixing the two is one pass. `monitorImage` is consulted per
/// monitor and its files are decoded at most once each.
///
/// Returns false when the image cannot be decoded or any piece cannot be written. Pieces already
/// written are kept, because a partially applied wallpaper is still better than none and the
/// caller can decide what to report.
[[nodiscard]] bool RenderSpan(
    const std::filesystem::path& sourceImage,
    std::span<const Monitor> monitors,
    const std::filesystem::path& outputFolder,
    std::vector<RenderedPiece>& pieces,
    const WallpaperView& view = {},
    const MonitorImageLookup& monitorImage = {});

/// Per-monitor variant: this monitor is its own canvas, so the slice covers the whole image and
/// only the output is resized to the monitor.
[[nodiscard]] bool RenderForMonitor(
    const std::filesystem::path& sourceImage,
    const Monitor& monitor,
    const std::filesystem::path& outputFolder,
    RenderedPiece& piece);

/// Deletes slice files in `outputFolder` that this round did not produce. Returns how many went.
///
/// Piece names carry the geometry (`m<hash8>_<left>_<top>_<w>x<h>.png`), so a display that changed
/// resolution or moved leaves its old file behind forever - a few megabytes each, never referenced
/// again. Only names matching that exact shape are considered: the folder lives under the user's
/// profile and a wrong argument must not be able to delete anything that is not ours.
///
/// Fails quietly. A slice that cannot be deleted is a wasted megabyte, not an error worth telling
/// the user about.
int RemoveStalePieces(
    const std::filesystem::path& outputFolder, std::span<const RenderedPiece> keep);

/// A small, cover-cropped copy of an image laid out the way the shell will lay the wallpaper
/// out on the virtual desktop.
///
/// Used by the panel to draw the wallpaper inside the arrangement map. The crop is computed by
/// the same ComputeSlice the real pipeline uses - with the whole canvas standing in for a single
/// monitor - so the preview cannot drift away from what actually gets applied: if the source is
/// letterboxed or off-centre here, it will be in the real thing too.
struct PreviewBitmap
{
    int width{};
    int height{};

    /// 32bpp BGRA, top-down, row-major, exactly width * height entries.
    std::vector<std::uint32_t> pixels;

    [[nodiscard]] bool IsValid() const
    {
        return width > 0 && height > 0 && pixels.size() == static_cast<size_t>(width) * height;
    }
};

/// Returns nullopt when the image cannot be decoded or the requested size is not positive.
[[nodiscard]] std::optional<PreviewBitmap> RenderPreview(
    const std::filesystem::path& sourceImage, int width, int height, const WallpaperView& view = {});

} // namespace wintools::core
