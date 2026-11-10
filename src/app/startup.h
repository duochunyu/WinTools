#pragma once

#include <string>

namespace wintools::app
{

/// Takes the process-wide instance lock.
///
/// Returns false when another copy already holds it, after asking that copy to show its panel.
/// The lock is a named mutex, so it is released by the kernel even if the process is killed -
/// a lock file would survive a crash and leave the app permanently unable to start.
[[nodiscard]] bool ClaimSingleInstance();

/// True when the Run key currently starts WinTools at logon.
[[nodiscard]] bool IsAutostartEnabled();

/// Writes or removes the HKCU Run entry.
///
/// HKCU rather than HKLM on purpose: no elevation, and the setting belongs to the user who
/// turned it on. Returns false when the registry refused, so the panel can show the real state
/// instead of the state that was requested.
[[nodiscard]] bool SetAutostart(bool enabled);

} // namespace wintools::app
