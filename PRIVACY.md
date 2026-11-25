# Privacy

WinTools does not collect anything. It has no telemetry, no analytics, no crash reporting, no
account system and no advertising. There is no server to send data to.

## What is stored, and where

Everything WinTools remembers lives on your own machine, under `%LOCALAPPDATA%\WinTools\`:

| File | Contents |
|---|---|
| `settings.txt` | your preferences: theme, language, hotkeys, whether the update check is on |
| `layout.txt`, `groups.txt` | remembered window positions, per monitor |
| `wallpaper\` | the desktop wallpaper state and the copy needed to restore it |
| `notes\` | sticky notes, including pasted screenshots |
| `backup\` | a read-only copy of the above, taken before an update is applied |

Nothing here leaves the machine. Uninstalling means deleting the program and, if you want the data
gone too, that folder.

The registry is touched in exactly one place, and only if you use it: turning on "start with
Windows" writes a single value, `WinTools`, under
`HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`, pointing at the executable with
`--tray`. Turning it off deletes that value again. Nothing is written to `HKEY_LOCAL_MACHINE`, so no
administrator rights are ever requested and the change affects only your own account.

## The only network access

WinTools makes exactly one kind of request, and only when **you** turn it on: the update check.
It is off by default.

- **When it runs:** at most every six hours while the app is running, only after you enable
  "check for updates" in the settings panel, or when you click check now.
- **What it fetches:** a small text file describing the latest release, and, if you confirm an
  update, the installer itself.
- **Where from:** `gitee.com` (primary) or `github.com` (mirror). Nothing else, ever.
- **What is sent:** an ordinary HTTPS download request, no different from opening those pages in a
  browser. No identifiers, no usage data, no machine information. The request does carry your IP
  address and user agent, as every web request does, and those are handled by Gitee and GitHub under
  their own privacy policies. WinTools operates no server and receives nothing.
- **Integrity:** the downloaded file is accepted only if its SHA-256 matches the value in the
  release description, so a tampered download is discarded instead of installed.

If you never enable the update check, WinTools never touches the network at all.

## Selling and sharing

There is nothing to sell or share. No data is collected, so none is transferred to third parties.
