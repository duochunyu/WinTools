#include "test_framework.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
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

/// Writes a plain 24-bit BMP of one solid colour.
///
/// BMP on purpose: the writer is twenty lines and needs no encoder, which keeps the test focused
/// on the preview pipeline instead of on generating a fixture. WIC decodes it like any other file,
/// so it exercises exactly the same code the panel uses.
void WriteSolidBmp(const std::filesystem::path& file, int width, int height, COLORREF color)
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

    const char blue = static_cast<char>(GetBValue(color));
    const char green = static_cast<char>(GetGValue(color));
    const char red = static_cast<char>(GetRValue(color));

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            out.put(blue);
            out.put(green);
            out.put(red);
        }

        for (int pad = width * 3; pad < rowBytes; ++pad)
        {
            out.put(0);
        }
    }
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

    const auto preview = RenderPreview(file, 64, 48);
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

// Cover, not fit: a wide source must be cropped to the requested aspect instead of being
// letterboxed, because that is what the real wallpaper pipeline does.
WINTOOLS_TEST(Preview_CoversInsteadOfLetterboxing)
{
    const std::filesystem::path file = TempPath(L"preview-wide.bmp");
    WriteSolidBmp(file, 400, 40, RGB(240, 240, 240));

    const auto preview = RenderPreview(file, 40, 40);
    CHECK(preview.has_value());
    if (!preview.has_value())
    {
        return;
    }

    CHECK_EQ(preview->width, 40);
    CHECK_EQ(preview->height, 40);

    // Every pixel must be image, never a padding bar: a letterboxed preview would leave the top
    // or bottom row untouched by the source.
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(preview->pixels.data());
    const int lastRow = (preview->height - 1) * preview->width * 4;
    CHECK(bytes[lastRow + 0] > 200);
    CHECK(bytes[lastRow + 1] > 200);
    CHECK(bytes[lastRow + 2] > 200);
}

WINTOOLS_TEST(Preview_ReturnsNothingForBadInput)
{
    CHECK(!RenderPreview(TempPath(L"no-such-image.png"), 32, 32).has_value());
    CHECK(!RenderPreview(TempPath(L"preview-wide.bmp"), 0, 32).has_value());
}
