#include "test_framework.h"

#include <cstdint>
#include <set>

#include "core/note_theme.h"

using namespace wintools::core;

namespace
{

/// Rough perceived brightness. Only ever used to assert an ordering, so the exact weights do not
/// matter as long as green weighs the most.
int Luminance(std::uint32_t color)
{
    const int red = static_cast<int>((color >> 16) & 0xFFu);
    const int green = static_cast<int>((color >> 8) & 0xFFu);
    const int blue = static_cast<int>(color & 0xFFu);
    return red * 299 + green * 587 + blue * 114;
}

} // namespace

WINTOOLS_TEST(NoteTheme_OffersSixPapersThatAreAllDifferent)
{
    std::set<std::uint32_t> papers;

    for (int index = 0; index < kNoteThemeCount; ++index)
    {
        papers.insert(NoteThemeAt(index).paper);
    }

    // Two identical papers in the palette would be a colour the user can pick but cannot see.
    CHECK_EQ(papers.size(), static_cast<std::size_t>(kNoteThemeCount));
    CHECK_EQ(kNoteThemeCount, 6);
}

WINTOOLS_TEST(NoteTheme_HeaderSitsOneStepDarkerThanThePaper)
{
    for (int index = 0; index < kNoteThemeCount; ++index)
    {
        const NoteTheme theme = NoteThemeAt(index);

        // The bar has to read as the same sheet, not as a second surface, so it is darker but
        // close; the rule is darker again or it would disappear against the bar.
        CHECK(Luminance(theme.header) < Luminance(theme.paper));
        CHECK(Luminance(theme.rule) < Luminance(theme.header));
        CHECK(Luminance(theme.muted) < Luminance(theme.rule));
    }
}

WINTOOLS_TEST(NoteTheme_ClampsAnUnknownIndexToTheDefault)
{
    CHECK_EQ(ClampNoteTheme(-1), kNoteThemeDefault);
    CHECK_EQ(ClampNoteTheme(kNoteThemeCount), kNoteThemeDefault);
    CHECK_EQ(ClampNoteTheme(9999), kNoteThemeDefault);
    CHECK_EQ(NoteThemeAt(42).paper, NoteThemeAt(kNoteThemeDefault).paper);
}

WINTOOLS_TEST(NoteTheme_BlendReachesBothEnds)
{
    constexpr std::uint32_t from = 0x112233;
    constexpr std::uint32_t to = 0xAABBCC;

    CHECK_EQ(BlendColor(from, to, 0), from);
    CHECK_EQ(BlendColor(from, to, 100), to);

    // Out-of-range percentages are a caller bug, but clamping is cheaper than a wrong colour.
    CHECK_EQ(BlendColor(from, to, -40), from);
    CHECK_EQ(BlendColor(from, to, 400), to);
}

WINTOOLS_TEST(NoteTheme_BlendRoundsToNearest)
{
    CHECK_EQ(BlendColor(0x000000, 0xFFFFFF, 50), 0x808080u);
    CHECK_EQ(BlendColor(0xFF0000, 0x000000, 50), 0x800000u);
}

WINTOOLS_TEST(NoteTheme_BlendKeepsChannelsApart)
{
    CHECK_EQ(BlendColor(0x102030, 0x102030, 70), 0x102030u);
    CHECK_EQ(BlendColor(0x00FF00, 0x0000FF, 100), 0x0000FFu);
}
