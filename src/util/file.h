#pragma once

#include <filesystem>
#include <string_view>

namespace wintools::util
{

/// Writes UTF-8 text to `file` atomically: same-directory temp file, then a replacing rename.
///
/// Every settings/store file used to be written with ofstream(trunc), which leaves a truncated
/// or empty file behind if the process dies mid-write. That is not a theoretical concern here:
/// auto-update closes and restarts the process, so a write landing at the wrong moment would
/// destroy the user's notes or layout. A rename within one directory is atomic on NTFS, so a
/// reader sees either the whole old file or the whole new one - never a half-written one.
///
/// The temp file is created in the target's own directory on purpose: MoveFileEx with
/// MOVEFILE_REPLACE_EXISTING cannot cross volumes, and a rename on the same volume is a
/// directory entry change rather than a copy.
///
/// Returns false and logs the reason on failure. On failure the target file is left untouched
/// and the temp file is removed.
[[nodiscard]] bool AtomicWrite(const std::filesystem::path& file, std::wstring_view text);

/// Byte-preserving form, for RTF note bodies. Do not run those through ToUtf8: the bytes come
/// straight out of the rich-edit control and must survive unchanged.
[[nodiscard]] bool AtomicWriteBytes(const std::filesystem::path& file, std::string_view bytes);

} // namespace wintools::util
