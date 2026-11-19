#include "test_framework.h"

#include <cstdint>
#include <set>
#include <vector>

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

WINTOOLS_TEST(NoteTheme_OffersFivePapersThatAreAllDifferent)
{
    std::set<std::uint32_t> papers;

    for (int index = 0; index < kNoteThemeCount; ++index)
    {
        papers.insert(NoteThemeAt(index).paper);
    }

    // Two identical papers in the palette would be a colour the user can pick but cannot see. The
    // count is also the most a palette may hold, so the default is a set the editor can reproduce.
    CHECK_EQ(papers.size(), static_cast<std::size_t>(kNoteThemeCount));
    CHECK_EQ(kNoteThemeCount, 5);
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

WINTOOLS_TEST(NoteTheme_CustomPaletteTakesOverAndDerivesItsTones)
{
    SetNotePalette({ 0xFFEEDD, 0xDDEEFF, 0xFFDDDD });

    CHECK_EQ(NoteThemeCount(), 3);
    CHECK_EQ(NoteThemeAt(0).paper, 0xFFEEDDu);
    CHECK_EQ(NoteThemeAt(2).paper, 0xFFDDDDu);

    // The supporting tones are derived, but they keep the same ordering the built-in papers
    // follow: bar darker than the sheet, rule darker than the bar, metadata darker still.
    for (int index = 0; index < NoteThemeCount(); ++index)
    {
        const NoteTheme theme = NoteThemeAt(index);
        CHECK(Luminance(theme.header) < Luminance(theme.paper));
        CHECK(Luminance(theme.rule) < Luminance(theme.header));
        CHECK(Luminance(theme.muted) < Luminance(theme.rule));
    }

    // An index past a shorter palette falls back instead of reaching into nothing.
    CHECK_EQ(ClampNoteTheme(3), kNoteThemeDefault);

    // Clearing restores the built-in palette, so other tests are not affected by this one.
    SetNotePalette({});
    CHECK_EQ(NoteThemeCount(), kNoteThemeCount);
    CHECK_EQ(NoteThemeAt(4).paper, 0xEFE9F4u);

    // The palette is exactly as long as the most a custom one may be, so the last paper is also
    // the first index past the end.
    CHECK_EQ(ClampNoteTheme(5), kNoteThemeDefault);
}

WINTOOLS_TEST(NoteTheme_RejectsPalettesOutsideTwoToFive)
{
    SetNotePalette({ 0xFFEEDD });
    CHECK_EQ(NoteThemeCount(), kNoteThemeCount);

    SetNotePalette({ 0xFFEEDD, 0xDDEEFF, 0xFFDDDD, 0xEEDDFF, 0xDDFFEE, 0xFFEEEE });
    CHECK_EQ(NoteThemeCount(), kNoteThemeCount);

    // Two is the smallest set worth configuring, and it is accepted.
    SetNotePalette({ 0xFFEEDD, 0xDDEEFF });
    CHECK_EQ(NoteThemeCount(), 2);

    SetNotePalette({});
    CHECK_EQ(NoteThemeCount(), kNoteThemeCount);
}

WINTOOLS_TEST(NoteTheme_ParsesTheSettingsValue)
{
    // '#' prefixes, spaces and semicolons are all tolerated; junk fields are skipped.
    const std::vector<std::uint32_t> parsed =
        ParseNotePalette(L"#FFEEDD, DDEEFF ; nope112233;112233");

    CHECK_EQ(parsed.size(), static_cast<std::size_t>(3));
    if (parsed.size() == 3)
    {
        CHECK_EQ(parsed[0], 0xFFEEDDu);
        CHECK_EQ(parsed[1], 0xDDEEFFu);
        CHECK_EQ(parsed[2], 0x112233u);
    }

    // A single paper parses fine - the two-to-five rule is SetNotePalette's business - but junk
    // fields never turn into colours.
    CHECK_EQ(ParseNotePalette(L"FFEEDD").size(), static_cast<std::size_t>(1));
    CHECK(ParseNotePalette(L"").empty());
    CHECK(ParseNotePalette(L"xyz").empty());
}
