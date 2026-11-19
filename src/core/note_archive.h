#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/slice.h"

namespace wintools::core
{

/// One closed note, kept so it can be opened again. The body itself lives in a `.deleted.rtf`
/// file beside the index; this record only remembers where it sat and what it started with, so the
/// panel can show something more useful than a bare id.
struct ArchiveRecord
{
    std::wstring id;
    Rect rect{};
    int theme{ 0 };

    /// Whether the star came with the note into the archive, so reopening it does not lose it.
    bool starred{ false };

    /// First line or two of the body, with tabs and newlines folded to spaces.
    std::wstring preview;

    /// "11-17 02:31" in local time, for the panel. Display text, not a timestamp: nothing does
    /// arithmetic with it and a readable field is the whole point.
    std::wstring closedAt;
};

/// Closed notes, newest first. Stored as a tab-separated text file next to index.txt for the same
/// reason the index is: a handful of fields that a person may need to repair by hand.
class NoteArchive
{
public:
    /// %LOCALAPPDATA%\WinTools\notes\archive.txt
    [[nodiscard]] static std::filesystem::path FilePath();

    /// Redirects FilePath for tests that must not touch the user's notes. Empty restores default.
    static void SetFolder(std::filesystem::path folder);

    [[nodiscard]] static NoteArchive Load();

    [[nodiscard]] bool Save() const;

    /// Puts one record at the front. An id already in the archive is moved, never duplicated.
    void Add(ArchiveRecord record);

    /// Removes and returns the record with this id, or nullptr when it is not archived.
    [[nodiscard]] const ArchiveRecord* Find(std::wstring_view id) const;

    /// Removes one record by id. Returns true when something was removed.
    bool Remove(std::wstring_view id);

    std::vector<ArchiveRecord> records;
};

/// Tolerant parser, split out so a bad row can be asserted to cost one note and not the archive.
[[nodiscard]] NoteArchive ParseNoteArchive(std::wstring_view text);

[[nodiscard]] std::wstring FormatNoteArchive(const NoteArchive& archive);

} // namespace wintools::core
