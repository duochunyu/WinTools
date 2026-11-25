#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace wintools::platform::win
{

/// Lower-case hex SHA-256 of a byte buffer. Returns false only when the OS crypto provider is
/// unavailable; an empty buffer hashes to the standard e3b0c442... digest.
[[nodiscard]] bool Sha256Bytes(std::string_view bytes, std::string& hex);

/// Streaming file variant, so a 100 MB installer is never held in memory just to hash it.
[[nodiscard]] bool Sha256File(const std::filesystem::path& file, std::string& hex);

} // namespace wintools::platform::win
