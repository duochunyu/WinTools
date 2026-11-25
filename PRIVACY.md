# Privacy

WinTools does not collect anything. It has no telemetry, no analytics, no crash reporting, no
account system and no advertising. There is no server to send data to.

## What is stored, and where

Everything WinTools remembers lives on your own machine, under `%LOCALAPPDATA%\WinTools\`:

| File | Contents |
|---|---|
| `settings.txt` | your preferences: theme, language, hotkeys, whether the update check is on |
| `layout.txt` | the saved window arrangement, one entry per window |
| `groups.tsv` | which windows belong to the same group |
| `wallpapers.txt` | the images you have used as wallpaper |
| `wallpaper\state.txt`, `wallpaper\pieces\` | the desktop wallpaper as it was before WinTools changed it, and the rendered slices |
| `notes\index.txt`, `notes\archive.txt`, `notes\*.rtf` | sticky notes: their text, position and pasted pictures |
| `WinTools.log`, `WinTools.log.1` | a diagnostic log of what the program did and what failed, including local file paths. It never leaves the machine and you can delete it at any time |
| `backup\<version>\` | a read-only copy of the files above, taken before an update was applied |

Nothing here leaves the machine. Uninstalling means deleting the program and, if you want the data
gone too, that folder.

The registry is touched in exactly one place, and only if you use it: turning on **Run at logon**
writes a single value, `WinTools`, under
`HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`, pointing at the executable with
`--tray`. Turning it off deletes that value again. Nothing is written to `HKEY_LOCAL_MACHINE`, so no
administrator rights are ever requested and the change affects only your own account.

## The only network access

WinTools makes exactly one kind of request, and only when **you** turn it on: the update check.
It is off by default.

- **When it runs:** at most every six hours while the app is running, only after you enable
  "check for updates" in the settings panel, or when you click check now.
- **What it fetches:** a small text file describing the latest release, and, if you confirm an
  update, the new executable itself.
- **Where from:** `gitee.com` (primary) or `github.com` (mirror), including the file-delivery hosts
  those two use. Nothing else, ever.
- **What is sent:** an ordinary HTTPS download request, no different from opening those pages in a
  browser. No identifiers, no usage data, no machine information. The request does carry your IP
  address and user agent, as every web request does, and those are handled by Gitee and GitHub under
  their own privacy policies. WinTools operates no server and receives nothing.
- **Integrity:** the downloaded file is accepted only if its SHA-256 matches the value published with
  the release, so a tampered download is discarded instead of installed.

If you never enable the update check, WinTools never touches the network at all.

## Selling and sharing

There is nothing to sell or share. No data is collected, so none is transferred to third parties.
