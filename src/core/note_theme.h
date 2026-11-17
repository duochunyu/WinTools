#pragma once

#include <cstdint>

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

/// Six is the number the eye can tell apart at a glance while all six still read as paper rather
/// than as sticky plastic. Adding a seventh starts to make the list feel like a colour picker.
constexpr int kNoteThemeCount = 6;

/// Warm yellow, the colour a note is expected to be before the user says otherwise.
constexpr int kNoteThemeDefault = 0;

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
