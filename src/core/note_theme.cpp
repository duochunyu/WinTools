#include "core/note_theme.h"

#include <algorithm>
#include <array>

namespace wintools::core
{

namespace
{

/// Ordered warm to cool: the palette is also the order new notes cycle through, so the first few
/// notes on a fresh desktop sit in colours that go together instead of jumping around the wheel.
///
/// Five, not six: five is the most a palette may hold, so the default is a set the editor can
/// reproduce. The warm grey that used to end the row was the least used of the six, and it is the
/// one paper a note could not be given once the palette had been customised.
constexpr std::array<NoteTheme, kNoteThemeCount> kThemes{ {
    // paper,    header,   rule,     muted
    { 0xFFF8E2, 0xFBEFC8, 0xEFE2B9, 0x9A8F76 }, // warm yellow
    { 0xFCEFE6, 0xF7E1D1, 0xEBD3C0, 0x9C8879 }, // peach
    { 0xE9F0E4, 0xDCE7D5, 0xCBD9C2, 0x7D8C75 }, // mint
    { 0xE7EFF7, 0xD8E5F2, 0xC6D7E8, 0x77889A }, // misty blue
    { 0xEFE9F4, 0xE3DAEC, 0xD3C7E0, 0x857A96 }, // lavender
} };

/// The live palette. Empty means "use the built-in six"; otherwise it holds 2-5 custom themes.
std::vector<NoteTheme> g_customThemes;

/// The built-in palette as a vector, so NotePalette can always hand back a reference that outlives
/// the call. Built lazily; kThemes itself stays an array so the compile-time size survives.
const std::vector<NoteTheme>& BuiltinPalette()
{
    static const std::vector<NoteTheme> themes(kThemes.begin(), kThemes.end());
    return themes;
}

/// The ink every tone is derived from, the same near-black the notes draw their text with.
constexpr std::uint32_t kInk = 0x37352F;

/// Custom papers get their three supporting tones by blending towards the ink. The percentages
/// follow the direction the built-in papers step in - header barely darker, rule a touch more,
/// muted about halfway to the ink - so a custom paper never reads as a different kind of object.
constexpr int kHeaderBlend = 8;
constexpr int kRuleBlend = 16;
constexpr int kMutedBlend = 48;

NoteTheme DeriveTheme(std::uint32_t paper)
{
    return NoteTheme{ paper, BlendColor(paper, kInk, kHeaderBlend),
        BlendColor(paper, kInk, kRuleBlend), BlendColor(paper, kInk, kMutedBlend) };
}

int HexDigit(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9')
    {
        return ch - L'0';
    }
    if (ch >= L'a' && ch <= L'f')
    {
        return ch - L'a' + 10;
    }
    if (ch >= L'A' && ch <= L'F')
    {
        return ch - L'A' + 10;
    }
    return -1;
}

int Channel(std::uint32_t color, int shift)
{
    return static_cast<int>((color >> shift) & 0xFFu);
}

} // namespace

void SetNotePalette(std::vector<std::uint32_t> papers)
{
    // Two to five: two is the smallest set worth configuring, five the largest that still reads as
    // papers rather than as a colour picker. Anything else keeps the built-in palette.
    if (papers.size() < 2 || papers.size() > 5)
    {
        g_customThemes.clear();
        return;
    }

    g_customThemes.clear();
    g_customThemes.reserve(papers.size());
    for (const std::uint32_t paper : papers)
    {
        g_customThemes.push_back(DeriveTheme(paper & 0xFFFFFFu));
    }
}

const std::vector<NoteTheme>& NotePalette()
{
    return g_customThemes.empty() ? BuiltinPalette() : g_customThemes;
}

int NoteThemeCount()
{
    return g_customThemes.empty() ? kNoteThemeCount : static_cast<int>(g_customThemes.size());
}

bool NotePaletteIsCustom()
{
    return !g_customThemes.empty();
}

int ClampNoteTheme(int index)
{
    if (index < 0 || index >= NoteThemeCount())
    {
        return kNoteThemeDefault;
    }

    return index;
}

NoteTheme NoteThemeAt(int index)
{
    if (!g_customThemes.empty())
    {
        return g_customThemes[static_cast<std::size_t>(ClampNoteTheme(index))];
    }

    return kThemes[static_cast<std::size_t>(ClampNoteTheme(index))];
}

std::vector<std::uint32_t> ParseNotePalette(std::wstring_view text)
{
    std::vector<std::uint32_t> papers;

    std::size_t offset = 0;
    while (offset <= text.size())
    {
        const std::size_t end = text.find_first_of(L",;", offset);
        const std::wstring_view field = text.substr(offset,
            end == std::wstring_view::npos ? std::wstring_view::npos : end - offset);
        offset = (end == std::wstring_view::npos) ? text.size() + 1 : end + 1;

        std::size_t first = 0;
        while (first < field.size()
            && (field[first] == L' ' || field[first] == L'\t' || field[first] == L'#'))
        {
            ++first;
        }
        std::size_t last = field.size();
        while (last > first && (field[last - 1] == L' ' || field[last - 1] == L'\t'))
        {
            --last;
        }
        const std::wstring_view digits = field.substr(first, last - first);
        if (digits.size() != 6)
        {
            continue;
        }

        std::uint32_t paper = 0;
        bool usable = true;
        for (const wchar_t ch : digits)
        {
            const int value = HexDigit(ch);
            if (value < 0)
            {
                usable = false;
                break;
            }
            paper = (paper << 4) | static_cast<std::uint32_t>(value);
        }

        if (usable)
        {
            papers.push_back(paper);
        }
    }

    return papers;
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
