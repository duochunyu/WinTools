#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace wintools::app
{

/// Copies the running exe into %TEMP% and starts it with the hidden --apply-update command.
/// Must be called while the main process still holds its own exe path; the updater works from the
/// copy so Windows lets it replace the original once this process exits.
[[nodiscard]] bool StartUpdaterProcess(const std::filesystem::path& target,
    const std::filesystem::path& package, unsigned long parentPid, std::wstring& error);

/// The hidden mode itself. Waits for `pidText` to exit, backs up the old exe, moves the verified
/// package into place and starts the new build in tray-only mode. Returns a process exit code.
int RunUpdater(
    std::wstring_view pidText, std::wstring_view target, std::wstring_view package);

} // namespace wintools::app
