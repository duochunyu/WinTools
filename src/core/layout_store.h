#pragma once

#include <filesystem>
#include <optional>

#include "core/window_layout.h"

namespace wintools::core
{

/// Default location of the saved layout: %LOCALAPPDATA%\WinTools\layout.txt
[[nodiscard]] std::filesystem::path DefaultLayoutFile();

/// Writes a layout as tab-separated text.
///
/// Plain text rather than JSON: the only consumer is this process, the file is meant to be opened
/// in an editor when something looks wrong, and it removes a whole class of escaping bugs around
/// Windows paths full of backslashes.
[[nodiscard]] bool SaveLayout(const Layout& layout, const std::filesystem::path& file);

/// Reads a layout back. Returns nullopt when the file is missing or has no usable entries.
[[nodiscard]] std::optional<Layout> LoadLayout(const std::filesystem::path& file);

} // namespace wintools::core
