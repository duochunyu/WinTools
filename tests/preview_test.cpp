#include "test_framework.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>

#include "core/composer.h"

using namespace wintools::core;

namespace
{

std::filesystem::path TempPath(const wchar_t* name)
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / L"wintools-tests";
    std::filesystem::create_directories(folder);
    return folder / name;
}

void WriteLittleEndian(std::ofstream& out, std::uint32_t value, int bytes)
{
    for (int index = 0; index < bytes; ++index)
    {
        out.put(static_cast<char>((value >> (8 * index)) & 0xFF));
    }
}

/// Writes a plain 24-bit BMP whose colour is decided per pixel.
///
/// BMP on purpose: the writer is twenty lines and needs no encoder, which keeps the test focused
/// on the preview pipeline instead of on generating a fixture. WIC decodes it like any other file,
/// so it exercises exactly the same code the panel uses.
void WriteBmp(const std::filesystem::path& file, int width, int height,
    const std::function<COLORREF(int, int)>& colourAt)
{
    const int rowBytes = ((width * 3) + 3) & ~3;
    const std::uint32_t pixelBytes = static_cast<std::uint32_t>(rowBytes * height);

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.put('B');
    out.put('M');
    WriteLittleEndian(out, 14 + 40 + pixelBytes, 4);
    WriteLittleEndian(out, 0, 4);
    WriteLittleEndian(out, 54, 4);

    WriteLittleEndian(out, 40, 4);
    WriteLittleEndian(out, static_cast<std::uint32_t>(width), 4);
    WriteLittleEndian(out, static_cast<std::uint32_t>(height), 4);
    WriteLittleEndian(out, 1, 2);
    WriteLittleEndian(out, 24, 2);
    WriteLittleEndian(out, 0, 4);
    WriteLittleEndian(out, pixelBytes, 4);
    WriteLittleEndian(out, 2835, 4);
    WriteLittleEndian(out, 2835, 4);
    WriteLittleEndian(out, 0, 4);
    WriteLittleEndian(out, 0, 4);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const COLORREF color = colourAt(x, y);
            out.put(static_cast<char>(GetBValue(color)));
            out.put(static_cast<char>(GetGValue(color)));
            out.put(static_cast<char>(GetRValue(color)));
        }

        for (int pad = width * 3; pad < rowBytes; ++pad)
        {
            out.put(0);
        }
    }
}

void WriteSolidBmp(const std::filesystem::path& file, int width, int height, COLORREF color)
{
    WriteBmp(file, width, height, [color](int, int) { return color; });
}

/// Left half one colour, right half the other. Anything that crops instead of scaling loses one
/// of the two, which is what makes this the fixture for "the whole picture is still there".
void WriteSplitBmp(
    const std::filesystem::path& file, int width, int height, COLORREF left, COLORREF right)
{
    WriteBmp(file, width, height, [width, left, right](int x, int) {
        return x < width / 2 ? left : right;
    });
}

/// Brightness of one preview pixel, 0..255 - the largest channel, so a check never has to care
/// which corner of RGB it is looking at. COLORREF would pack them and compare as one number.
int Brightness(const PreviewBitmap& preview, int x, int y)
{
    const std::uint32_t pixel = preview.pixels[static_cast<size_t>(y) * preview.width + x];
    return static_cast<int>(std::max({ (pixel >> 16) & 0xFFu, (pixel >> 8) & 0xFFu, pixel & 0xFFu }));
}

} // namespace

// The arrangement map shrinks the wallpaper into a thumbnail that is a couple of hundred pixels
// wide. If the channel order or the stride is wrong there, the map shows plausible-looking but
// wrong colours - which is exactly what it did before this test existed - so the colour is
// asserted, not just the size.
WINTOOLS_TEST(Preview_KeepsColourAndSize)
{
    const std::filesystem::path file = TempPath(L"preview-blue.bmp");
    WriteSolidBmp(file, 32, 32, RGB(20, 90, 240));

    const auto preview = RenderSurface(file, 64, 48);
    CHECK(preview.has_value());
    if (!preview.has_value())
    {
        return;
    }

    CHECK_EQ(preview->width, 64);
    CHECK_EQ(preview->height, 48);
    CHECK(preview->IsValid());

    // 32bppBGRA in memory: byte 0 is blue.
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(preview->pixels.data());
    CHECK(bytes[0] > 200);
    CHECK(bytes[1] > 60 && bytes[1] < 130);
    CHECK(bytes[2] < 60);

    // And it has to hold across the whole surface, not just the first pixel.
    for (int index = 0; index < preview->width * preview->height; ++index)
    {
        const std::uint8_t* pixel = bytes + index * 4;
        if (pixel[0] < 200 || pixel[2] > 60)
        {
            CHECK(false);
            break;
        }
    }
}

// The surface is what the renderer slices, so it must carry the picture *whole*: the map shows the
// part outside the displays while a turn is in progress, and a preview that cropped to the canvas
// would make that margin invisible. Both halves of a split source therefore have to survive a
// scale that changes its aspect completely.
WINTOOLS_TEST(Preview_KeepsTheWholePicture)
{
    const std::filesystem::path file = TempPath(L"preview-split.bmp");
    WriteSplitBmp(file, 400, 40, RGB(20, 20, 20), RGB(240, 240, 240));

    const auto preview = RenderSurface(file, 40, 40);
    CHECK(preview.has_value());
    if (!preview.has_value())
    {
        return;
    }

    CHECK_EQ(preview->width, 40);
    CHECK_EQ(preview->height, 40);

    // Left half dark, right half bright, top to bottom: no crop, no letterbox bar.
    for (const int y : { 0, 20, 39 })
    {
        CHECK(Brightness(*preview, 1, y) < 60);
        CHECK(Brightness(*preview, 38, y) > 200);
    }
}

// A quarter turn is the only rotation the map can be checked against without guessing at
// resampling: the picture's dimensions swap and its left edge becomes its top edge. Getting the
// direction wrong here would show the user a picture turned the opposite way to the wallpaper.
WINTOOLS_TEST(Preview_TurnsClockwise)
{
    const std::filesystem::path file = TempPath(L"preview-turn.bmp");
    WriteSplitBmp(file, 16, 8, RGB(20, 20, 20), RGB(240, 240, 240));

    const auto preview = RenderSurface(file, 8, 16, 90.0);
    CHECK(preview.has_value());
    if (!preview.has_value())
    {
        return;
    }

    CHECK_EQ(preview->width, 8);
    CHECK_EQ(preview->height, 16);

    // Clockwise: the left half of the source becomes the top half.
    for (const int x : { 0, 4, 7 })
    {
        CHECK(Brightness(*preview, x, 1) < 60);
        CHECK(Brightness(*preview, x, 14) > 200);
    }
}

WINTOOLS_TEST(Preview_ReturnsNothingForBadInput)
{
    CHECK(!RenderSurface(TempPath(L"no-such-image.png"), 32, 32).has_value());
    CHECK(!RenderSurface(TempPath(L"preview-split.bmp"), 0, 32).has_value());
}
