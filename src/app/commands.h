#pragma once

#include <filesystem>

namespace wintools::app
{

/// Exit codes, shared by every subcommand.
enum ExitCode : int
{
    kSuccess = 0,
    kBadArguments = 1,
    kRuntimeFailure = 2,
    kEnvironmentProblem = 3,
};

int RunMonitors();
int RunDump(const std::filesystem::path& image);
int RunSpan(const std::filesystem::path& image, bool dryRun);
int RunRestore();

/// Lists every window the layout engine would remember, with the display it sits on.
int RunWindows();

/// Snapshots the current arrangement.
int RunCapture();

/// Puts the saved arrangement back.
int RunApply();

/// Prints the saved arrangement without touching anything.
int RunShow();

/// Residency mode: tray icon, panel, display watcher. Blocks until the user quits.
///
/// showPanel is false for the autostart path, where stealing focus at login would be rude.
int RunTray(bool showPanel);

} // namespace wintools::app
