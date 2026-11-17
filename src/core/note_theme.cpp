#include "core/note_theme.h"

#include <algorithm>
#include <array>

namespace wintools::core
{

namespace
{

/// Ordered warm to cool: the palette is also the order new notes cycle through, so the first few
/// notes on a fresh desktop sit in colours that go together instead of jumping around the wheel.
constexpr std::array<NoteTheme, kNoteThemeCount> kThemes{ {
    // paper,    header,   rule,     muted
    { 0xFFF8E2, 0xFBEFC8, 0xEFE2B9, 0x9A8F76 }, // warm yellow
    { 0xFCEFE6, 0xF7E1D1, 0xEBD3C0, 0x9C8879 }, // peach
    { 0xE9F0E4, 0xDCE7D5, 0xCBD9C2, 0x7D8C75 }, // mint
    { 0xE7EFF7, 0xD8E5F2, 0xC6D7E8, 0x77889A }, // misty blue
    { 0xEFE9F4, 0xE3DAEC, 0xD3C7E0, 0x857A96 }, // lavender
    { 0xEFEDE8, 0xE4E1DA, 0xD5D1C8, 0x8B877E }, // warm grey
} };

int Channel(std::uint32_t color, int shift)
{
    return static_cast<int>((color >> shift) & 0xFFu);
}

} // namespace

int ClampNoteTheme(int index)
{
    if (index < 0 || index >= kNoteThemeCount)
    {
        return kNoteThemeDefault;
    }

    return index;
}

NoteTheme NoteThemeAt(int index)
{
    return kThemes[static_cast<std::size_t>(ClampNoteTheme(index))];
}

std::uint32_t BlendColor(std::uint32_t from, std::uint32_t to, int percentOfTo)
{
    const int percent = std::clamp(percentOfTo, 0, 100);

    const auto mix = [percent](int a, int b)
    {
        return (a * (100 - percent) + b * percent + 50) / 100;
    };

    const std::uint32_t red = static_cast<std::uint32_t>(mix(Channel(from, 16), Channel(to, 16)));
    const std::uint32_t green = static_cast<std::uint32_t>(mix(Channel(from, 8), Channel(to, 8)));
    const std::uint32_t blue = static_cast<std::uint32_t>(mix(Channel(from, 0), Channel(to, 0)));

    return (red << 16) | (green << 8) | blue;
}

} // namespace wintools::core
