#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace wintools::core
{

/// Copies the files an older build would need to read into `appData/backup/<fromVersion>/`.
/// Deliberately copy-only: an update must never move or delete the user's live notes, layout or
/// settings. Existing files in the backup folder are overwritten, but nothing else is touched.
[[nodiscard]] bool SnapshotBeforeUpdate(const std::filesystem::path& appData,
    std::wstring_view fromVersion, std::wstring& error);

} // namespace wintools::core
