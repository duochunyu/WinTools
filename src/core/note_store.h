#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/slice.h"

namespace wintools::core
{

/// One note as the index remembers it. The text itself lives next to it, in the body file.
struct NoteRecord
{
    std::wstring id;
    Rect rect{};

    /// Which paper the note is written on. An index written before the palette existed has no such
    /// field at all, so this defaults to the yellow every note used to be.
    int theme{ 0 };
};

/// The notes on disk: a small index plus one RTF file per note.
///
/// The index is a hand-editable text file for the same reason settings.txt is - it is a handful
/// of numbers, and a binary or JSON format would buy nothing except a parser. The bodies are
/// stored as raw RTF bytes with no conversion on the way in or out: RichEdit writes them in the
/// control's own code page, and reinterpreting those bytes as UTF-8 would corrupt every non-ASCII
/// character in the note.
class NoteStore
{
public:
    /// %LOCALAPPDATA%\WinTools\notes
    [[nodiscard]] static std::filesystem::path Folder();

    /// Redirects Folder() somewhere else. Only the tests call this: they write real files, and a
    /// test that ran against the real folder would delete the user's notes on a failed assertion.
    /// Passing an empty path restores the default.
    static void SetFolder(std::filesystem::path folder);

    /// %LOCALAPPDATA%\WinTools\notes\index.txt
    [[nodiscard]] static std::filesystem::path IndexFile();

    /// Reads the index. A missing or unreadable file yields an empty store, not an error: the
    /// first run has no notes, and a corrupt index should cost the user their layout, not the app.
    [[nodiscard]] static NoteStore Load();

    /// Writes the index, creating the folder if needed.
    [[nodiscard]] bool Save() const;

    /// %LOCALAPPDATA%\WinTools\notes\<id>.rtf
    [[nodiscard]] std::filesystem::path BodyPath(std::wstring_view id) const;

    /// Next free id ("n1", "n2", ...). Advances the counter; the caller saves when it is ready.
    std::wstring AllocateId();

    /// Drops one note and deletes its body. Saving is the caller's job, as everywhere else here.
    void Remove(std::wstring_view id);

    /// Oldest first, which is also the order the notes are re-created in.
    std::vector<NoteRecord> notes;

    int nextId{ 1 };
};

/// Parses index text. Split out of Load() so the tolerant-parsing rules can be asserted without a
/// file, and so a future migration can run over bytes that came from somewhere else.
///
/// Records that cannot be trusted are skipped individually - a bad id or unreadable geometry costs
/// one note, never the whole list.
[[nodiscard]] NoteStore ParseNoteIndex(std::wstring_view text);

/// Renders the index in the format ParseNoteIndex expects.
[[nodiscard]] std::wstring FormatNoteIndex(const NoteStore& store);

} // namespace wintools::core
