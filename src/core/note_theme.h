#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace wintools::core
{

/// The colours of one note paper.
///
/// Every colour is a plain 0xRRGGBB value rather than a COLORREF, because this header is compiled
/// into the tests, which have no business including windows.h to describe a colour. The window
/// translates once, when it hands the values to GDI.
///
/// A theme owns four tones and nothing else: the ink is the same near-black charcoal on all six
/// papers, so switching colour never has to touch the text runs the user pasted in.
struct NoteTheme
{
    std::uint32_t paper{};   ///< The sheet itself, and the whole window while the bar is hidden.
    std::uint32_t header{};  ///< The bar, one step darker than the paper.
    std::uint32_t rule{};    ///< The hairline under the bar.
    std::uint32_t muted{};   ///< The save time, and the resting colour of the header icons.
};

/// The built-in palette's size, and also the most papers a palette may hold. The default is the
/// maximum on purpose: the editor offers at most five rows, so a default of six would be a palette
/// the user could not reproduce after touching it - the sixth paper would have to be dropped
/// silently the first time any colour changed.
///
/// This is the built-in size; the live palette can be narrower - see NoteThemeCount.
constexpr int kNoteThemeCount = 5;

/// Warm yellow, the colour a note is expected to be before the user says otherwise.
constexpr int kNoteThemeDefault = 0;

/// Replaces the live palette with user-chosen papers.
///
/// Two to five papers are accepted; anything else - an empty list, six papers, a nonsense colour -
/// keeps the built-in six. Each paper's header, rule and muted tones are derived from it by
/// blending towards the ink colour, in the same direction the built-in papers step, so a custom
/// paper behaves like a native one everywhere the four tones are drawn.
void SetNotePalette(std::vector<std::uint32_t> papers);

/// The papers currently in force, built-in or custom.
[[nodiscard]] const std::vector<NoteTheme>& NotePalette();

/// How many papers the live palette holds: 2-5 when a custom palette is set, kNoteThemeCount
/// otherwise. Palette indexes everywhere (the note window, the swatch popup, the index file) are
/// clamped against this, not against the constant.
[[nodiscard]] int NoteThemeCount();

/// True when the palette in force came from the user rather than from the built-in list. The
/// service needs the distinction to know whether to write a `note.themes` value at all.
[[nodiscard]] bool NotePaletteIsCustom();

/// Parses a `note.themes=` settings value: papers as RRGGBB hex, separated by commas or
/// semicolons, `#` prefixes tolerated. Junk fields are skipped; applying the two-to-five rule is
/// SetNotePalette's business, so this returns every usable paper it found.
[[nodiscard]] std::vector<std::uint32_t> ParseNotePalette(std::wstring_view text);

/// Index outside the palette is a corrupt or hand-edited file, not a reason to lose a note, so it
/// falls back to the default instead of failing.
[[nodiscard]] int ClampNoteTheme(int index);

[[nodiscard]] NoteTheme NoteThemeAt(int index);
/// Mixes two colours, `percentOfTo` percent of the way from `from` towards `to`.
///
/// GDI has no alpha channel, so every soft tint in the note - the hover fill behind a button, the
/// scrollbar thumb - has to be pre-mixed against a known background instead. Rounding is to
/// nearest, so a 50% mix of black and white is 0x808080 rather than the 0x7F a truncating divide
/// would give, and a light tint stays light instead of accumulating downward.
[[nodiscard]] std::uint32_t BlendColor(std::uint32_t from, std::uint32_t to, int percentOfTo);

} // namespace wintools::core
