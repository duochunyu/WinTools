#pragma once

#include <filesystem>
#include <string>

#include "core/monitor.h"
#include "core/slice.h"

namespace wintools::util
{

/// %LOCALAPPDATA%\WinTools
[[nodiscard]] std::filesystem::path AppDataFolder();

/// %LOCALAPPDATA%\WinTools\wallpaper\pieces - where generated per-monitor pieces land.
[[nodiscard]] std::filesystem::path PieceFolder();

/// %LOCALAPPDATA%\WinTools\wallpaper\state.txt - snapshot taken before the wallpaper is changed.
[[nodiscard]] std::filesystem::path StateFile();

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
