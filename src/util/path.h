#pragma once

#include <filesystem>
#include <string>

#include "core/monitor.h"
#include "core/slice.h"

namespace wintools::util
{

/// %LOCALAPPDATA%\WinTools
[[nodiscard]] std::filesystem::path AppDataFolder();

/// The user's Pictures library, for screenshots taken with the global hotkey. Empty when the
/// known-folder lookup fails, so callers can fall back rather than writing somewhere surprising.
[[nodiscard]] std::filesystem::path PicturesFolder();

/// %LOCALAPPDATA%\WinTools\wallpaper\pieces - where generated per-monitor pieces land.
[[nodiscard]] std::filesystem::path PieceFolder();

/// %LOCALAPPDATA%\WinTools\wallpaper\state.txt - snapshot taken before the wallpaper is changed.
[[nodiscard]] std::filesystem::path StateFile();

/// %LOCALAPPDATA%\WinTools\update - downloaded installers, pending update markers and the
/// pre-upgrade snapshot. Kept separate from the config and notes so an update never has to
/// walk through user data to find its own files.
[[nodiscard]] std::filesystem::path UpdateFolder();

/// %LOCALAPPDATA%\WinTools\WinTools.log - every log line, appended.
///
/// A tray app normally has no console at all, so without this file a failure is completely
/// invisible: the process starts, nothing appears, and there is nothing to look at.
[[nodiscard]] std::filesystem::path LogFile();

/// Stable, collision-free file name for one monitor's piece.
///
/// The position and size are part of the name so a stale piece from a previous arrangement is
/// never picked up by mistake: after the layout changes, the name simply stops matching.
///
/// The hash is FNV-1a over the device path, deliberately identical to the PowerToys C#
/// implementation so piece folders from both builds can be compared file-by-file.
[[nodiscard]] std::wstring BuildPieceName(const core::Monitor& monitor, const core::Slice& slice);

} // namespace wintools::util
