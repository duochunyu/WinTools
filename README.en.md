# WinTools

**English** · [简体中文](README.md)

Span one wallpaper across multiple monitors, and put every window back where it was when a display
is unplugged or wakes up.

A single-file executable with a statically linked CRT and no runtime dependencies. It lives in the
system tray; double-click to open the settings panel. No installer, no writes to Program Files,
no network access. The panel, tray menu, status messages, CLI help and log ship in both
Simplified Chinese and English; the language can follow the system or be pinned from the config page
or the title-bar menu.

| | |
|---|---|
| OS | Windows 10 1809+ / Windows 11, x64 |
| Version | v0.1.0 |
| Binary | 1,207,296 bytes; imports only 9 system DLLs |
| Built with | C++20 / Win32 / WIC / custom GDI rendering; no third-party libraries |
| Tests | 183 unit tests, green in debug and release |

![WinTools settings panel](docs/panel-en.png)

> The real panel at 1025x934. Screenshots use neutral sample data: 3 displays, 2 groups,
> 7 placeholder windows, and wallpapers drawn specifically for this project. No real window
> titles, file paths or personal content appear.
>
> The Simplified Chinese panel is shown in [panel.png](docs/panel.png) and
> [panel-demo.gif](docs/panel-demo.gif).

---

## 1. Features

### 1.1 Overview

| Feature | Description |
|---|---|
| Span wallpaper across displays | One image across all monitors, with seams aligned to each monitor's real rectangle |
| **Groups** | Each display belongs to one group; each group has one image, one framing and one set of member displays. Two-by-two spanning and four independent displays are the same model |
| Framing | Drag to pan on the arrangement map, scroll to zoom around the cursor (1.0-8.0), double-click or press `0` to reset |
| Wallpaper preview | The preview uses the same cover-crop code as the real wallpaper, so what you see on each display is what gets applied |
| **Free-angle wallpaper turn** | A round turn button (Lucide icon) floats outside a corner of the active group on the wallpaper page - it picks a corner that covers no other display or the calendar switch. Drag to turn the picture to any angle, or click once for a quick clockwise 90 degrees; slices are regenerated on release and the angle is remembered with the group. The right angles snap, the hint row shows the live angle and offers a one-click way back to square, and while dragging or turning the map shows the whole picture with everything outside the displays dimmed by half - so "how far was it enlarged" and "how much room is left" are things you can see |
| Remember window layouts | Positions recorded per display, including maximized, snapped and split ratios; windows follow their display when its origin shifts |
| Automatic restore | 2-second debounce after a display reconnects or is removed; only windows that need moving are touched |
| Per-window exclusion | The checkbox at the start of each row decides whether a window is remembered; unchecking never deletes the row |
| Filter by display | Numeric chips on the layout page are multi-select; none selected means all |
| Themes | Light / dark / follow system; both the three-way control on the config page and the title-bar shortcut, saved to disk |
| UI languages | Simplified Chinese / English; three-way control on the config page and a `CN` / `EN` title-bar menu, including follow-the-system. The choice is saved |
| Full keyboard access | Tab focus ring, 1-9 to switch groups, `+ - 0` to zoom/reset, arrow keys to pan, Ctrl+Z to undo a checkbox, Esc to close popups |
| Desktop calendar | A yin-yang perpetual calendar above the wallpaper: lunar date, solar term and almanac rings, plus a 10 Hz sweeping clock |
| Sticky notes | Always-on-top notes: remembered position and size, five built-in muted papers, editable and extendable to 2-5 from the notes page's Colours button, a bare sheet with one line of save time and a page-edge rule at rest (five icons on hover: delete / paper colour / screenshot / close / star), lit stars stay visible in gold and follow the note into the archive, confirmed-only delete that keeps nothing, 0.7 s debounced autosave, region screenshots pasted into the text and draggable afterwards, closed notes recoverable from panel history, new-note and show-all buttons on the notes page (a new note opens on the display under the cursor), visual global hotkey configuration |
| Tray and autostart | Tray menu: Change Wallpaper / Restore Wallpaper / Refresh Layout / Restore Layout / Desktop Calendar / New Note / Show All Notes / Quit; optional run at logon |

### 1.2 Groups: which displays share one image

A group is the only grouping granularity in the wallpaper configuration. Every display belongs to
exactly one group, and each group has **one image, one framing and one set of member displays**.

- One image across four displays = one group with four members; two-by-two spanning = two groups
  with two displays each; four independent displays = four single-display groups.
- **Group names are derived from member screen numbers** (`1+2`, `3+4`); no ID is stored.
- **There is exactly one entry point for changing groups**: the blue-and-white number badge in the
  top-left corner of a display. Click it to pick "join X" or "make a group of one"; the reverse
  direction is the `+` on the current group chip, which pulls another display in. Both directions
  are the same atomic operation.
- Clicking the display body pans the wallpaper; only the badge opens the menu. The move cursor
  appears only inside displays that have a wallpaper.
- **Grouping is expressed by a single outline outside the displays**, one per group. The outline
  traces the true union of the member rectangles, so an L-shaped arrangement does not frame the
  missing corner. Its inner edge stays 2 px away from the display, and a selected group grows
  outward only, so two adjacent groups always leave the image visible between them instead of
  merging into one thick band.
- A group that is emptied disappears and the others are renumbered. When membership changes the
  framing resets and the group is marked with a "not applied" dot.

Merging display 3 into `1+2`: the outline follows the members and all three displays switch to
the same wallpaper.

![Group merge demo](docs/panel-demo-en.gif)

### 1.3 Arrangement map and framing

The map at the top draws the **real relative positions** of the currently attached displays:
negative coordinates, misaligned edges and gaps are preserved, and the bounding rectangle is scaled
uniformly into the card. Grid plus white gaps = desktop area not covered by any display.

- No translucent blocks, colour bars or inner strokes are drawn over the image; wallpaper colours
  stay true.
- **Drag**: the picture follows the cursor, scoped to the current group. Dragging a display in
  another group switches to that group first. A drag shows the whole picture with the margin
  dimmed, so how much picture is left in the direction you are pushing is on screen. A turned
  picture still moves in **any** direction: the limit is the picture's own edge, not the screen's
  axes.
- **Scroll wheel**: zoom anchored at the pixel under the cursor, 1.0-8.0x, 10% per notch. Small
  deltas from precision touchpads are handled as fractional notches.
- **Double-click the display / `0`**: reset the framing. Dragging does not decode, write to disk
  or re-apply the wallpaper.
- The "not applied" dot in the section header means the framing changed but was not applied yet;
  clicking "Apply Wallpaper" generates the slices.
- **Turn**: a round handle floats outside one corner of the active group (Lucide `rotate-cw`;
  the corner is picked so it covers no other display). Hold and drag to turn the picture to any
  angle, or click for a clockwise quarter turn. The gear ratio is the arc the cursor travels over
  the radius it was pressed at, times 0.32 - roughly three degrees of hand to one of picture. The
  right angles are magnetic: within 2.5 degrees it snaps to 0/90/180/270, and leaving one costs
  six more degrees.
- **The whole picture while dragging or turning**: during the gesture the map draws the *entire*
  picture, and whatever no display of that group will receive is dimmed by half. Release and it
  returns to per-display slices. An oblique turn has to enlarge the picture for the corners (up to
  1.41x at 45 degrees) - the dimmed margin is the answer to "enlarged where, cropped how much".
- The hint row ends with `90° Square 100%`: the live angle and a one-click way back to square
  (rotation only - zoom and framing stay).
- The calendar switch floats at the top-right of the map; future widgets join the map's edges.

### 1.4 Window layouts

There are only three things to do: **change the wallpaper** → **click "Capture Layout"** →
**leave "Auto Restore" on**. The panel has two primary buttons, one toggle and one secondary
"Restore Layout" button.

- Each row shows a checkbox / window title / process name / a position label such as
  `Display 1 - Maximized`. The title is the primary information; the process name only
  disambiguates duplicates. Clicking the title does not change the checkbox.
- The position label is **read-only** and shows what was captured: `Maximized` / `Left half` /
  `Right 33%` / `Fill`; floating windows show their size. Windows return exactly as they were.
- Clicking "Refresh" re-captures positions while inheriting the unchecked rows.
- `apply` only moves windows that are already running; it never launches anything.
- The tray menu, panel buttons and command line all call the same action functions.

### 1.5 Panel and interaction

The panel is a **four-page tab strip**: under the title bar a row of tabs (**Layout /
Wallpaper / Notes / Config**) shows one page at a time. The active tab has no bottom edge and
covers the rule under it, so the tab and the page read as one shape - a folder tab, not a pill
floating above a line. The window defaults to **820×615 (4:3)** and
its height no longer follows the data; the border can be dragged to resize freely (minimum
560×430), and the window list and the note history fit their page to the window height, with
the wheel paging through the rest. The footer holds only the current status and Quit; it is shared
by every page and anchored to the bottom.

- **Layout page**: display filter, auto-restore toggle, capture / restore layout, window list.
- **Wallpaper page**: group chips, change / apply wallpaper, hint line, arrangement map; the
  turn handle lives inside the map at the active group's corner and the calendar switch floats at
  its top-right, so widgets do not crowd the hint line.
- **Notes page**: colours (edit the paper palette), new note and show-all buttons; `Note history ·
  N` and `2 / 3` with two chevrons sit in a meta row **above** the card, at the same level as the
  layout page's window count. The card itself has one line per row: paper dot, star (filled gold or
  hollow, always present), close time, body preview (ellipsised to the column width), restore /
  delete; starred rows come first; when it is empty it says so.
- **Config page**: the settings that used to live behind title-bar icons are sorted into two
  columns of two blocks - **Appearance** (theme and language, each three choices laid out at
  once), **Startup** (run at logon), **Information** (open log) and **Shortcuts** (each action on
  its own row with the combination it holds; click a row to record). The title bar also keeps two
  quick switches: `CN` / `EN` opens the language menu and the theme button cycles light / dark /
  follow system. Both write the same settings the config page edits; Help and Close stay at the
  far right.

All three content page headers share the same structure: title + object selector + flexible space + one
primary button + one secondary button. The whole panel uses one button size and one row height.

| Key | Action |
|---|---|
| `Tab` / `Shift+Tab` | Move between controls; double-ring focus indicator |
| `Enter` / `Space` | Open the group menu / toggle a checkbox |
| `1` - `9` | Switch the active group |
| `+` `-` `0` | Zoom / reset the active group |
| Arrow keys (Shift to accelerate) | Pan the active group |
| `Ctrl+Z` | Undo the last checkbox change |
| `Esc` | Close a popup; press again to hide to tray |

### 1.6 Command line

No arguments = stay in the tray and open the panel; `--tray` = tray only (used by autostart).

```bat
WinTools.exe monitors                  :: list displays and virtual-desktop rectangles
WinTools.exe windows                   :: list manageable windows

WinTools.exe span "D:\pics\wide.png"   :: span one image across all displays
WinTools.exe span "D:\pics\wide.png" --dry-run
WinTools.exe dump "D:\pics\wide.png"   :: compute slice parameters without writing or applying
WinTools.exe restore                   :: restore the wallpaper from before the change

WinTools.exe capture                   :: save the current window layout
WinTools.exe show                      :: show the saved layout
WinTools.exe apply                     :: put the windows back
```

### 1.7 Desktop calendar

A round widget that sits **above** the wallpaper: a yin-yang hub, a ring of tick marks and four
text bands (date, lunar date, solar term, almanac), with an analog clock over the hub.

- **It does not take over the desktop.** A real dynamic wallpaper would mean owning WorkerW or
  `IDesktopWallpaper` and betting on the shell's behaviour. This is an ordinary top-level window
  that is re-stacked directly above the desktop host window - wallpaper, icons and taskbar are
  left alone.
- Position and diameter are set by dragging in the panel preview and persisted; after a display
  change the widget is clamped back onto a screen.
- The second hand moves at 10 Hz and stops when another window covers the desktop, leaving the
  minute hand to carry on (`canvas.sweep=0` disables the sweep for good).
- Only the hands are drawn per frame: disc, ticks and text bands are rendered once into a cached
  bitmap that each frame copies wholesale before the three hands go on top.

### 1.8 Sticky notes

A note that stays on top of everything except a fullscreen application.

- Drag the header to move, drag the corner to resize; both are persisted immediately and come back
  after a restart.
- **At rest it is a sheet of paper with one line of text and a page-edge rule**: with the pointer
  outside and the caret elsewhere there is no bar and no buttons. When the pointer arrives or typing
  starts, the icons (delete / paper colour / screenshot / close / star) appear straight on the paper
  in the top-right corner, with no plate behind them - except that **a lit star stays visible in
  gold**, quiet but always readable. Unsaved edits turn both the time and the rule terracotta.
  Both states share the same geometry, so nothing jumps.
- **Five built-in muted papers** (warm yellow, peach, mint, misty blue, lavender). The notes
  page's Colours button lists one paper per row: click a row for the system colour picker, the
  trailing `×` drops that paper (two is the floor), "add a colour" appears below five, and "reset
  to the five defaults" puts everything back. The same set can be written straight into
  `note.themes=RRGGBB,...` in settings.txt (2-5 entries). New notes walk the palette in order and
  the dot in the note's header changes this note's paper (the swatch chip has no frame; the discs
  are drawn straight on the paper). The colour lives in the index; switching it never touches a
  byte of the body.
- **The star** is stored in the index, follows the note into the archive, survives a reopen, and
  brings its history rows to the front of the panel list.
- **Delete** (the flame icon) asks for confirmation first; a confirmed delete keeps nothing - no
  archive row, no body file - unlike Close, which leaves a copy in the history.
- Closing a note that was never typed into leaves nothing behind either (a starred blank note
  excepted): opening a note and changing your mind should not print a "(blank note)" row.
- Paper all round: the body starts below the time line and stops half a header height above the
  bottom edge, so the last line never runs into the paper's edge.
- The body is rich text: whatever formatting arrives with a paste is kept, and pictures are stored
  with it. Line spacing is 1.5, and the side margins are wider than the text needs.
- A picture is one character in that text: drag it to move it, the text around the drop point
  reflows, and Ctrl+Z takes the move back (two steps).
- Autosave 0.7 s after the last edit, plus one more save when focus leaves and one on close. The
  corner always shows `MM-DD HH:MM:SS`, taken from the body file's real write time rather than the
  moment the note was opened; **unsaved is that same line in terracotta**, with no extra word.
- "Shot" starts a region selection: the screen freezes, everything outside the region darkens, and
  releasing pastes that region at the caret, scaled down to the note width. A picture always
  starts a fresh line: RichEdit reserves the picture's size where it lands and then draws it on
  the following line, so without a soft break a blank band is left behind.
- The scrollbar is a hand-drawn 3 px thumb (RichEdit's own bar is moved outside the client area),
  accurate to the pixel; an empty note shows one line of muted placeholder text.
- Closed notes move to the panel history: one line per row with the star (if any), paper colour,
  close time and preview, and a note can be reopened with its body intact or forgotten for good.
- Global hotkeys live on the config page: New Note / Region Capture / Capture Layout /
  Restore Layout, each row recording a combination on click. Defaults are `Ctrl+Alt+N`, +S, +L and
  +R; a binding can be cleared or reset to defaults. Keys are named the way they are printed on
  the keyboard (`F2`, `Num5`, `←`); only a key with no familiar name falls back to `VK<n>`.
  The capture hotkey has no note context, so its selection is saved as a PNG in `Pictures\WinTools`.
- The tray menu offers New Note and Show All Notes.

---

## 2. Architecture

### 2.1 Layers

```
main.cpp                      wWinMain; borrows the parent console, then dispatches subcommands
    |
    +-- app/                  process boundary
    |   +-- cli / commands    argument parsing and subcommands
    |   +-- actions           user actions (span / restore / capture / apply / regroup)
    |   +-- service           resident service: tray, display watcher, state machine, persistence
    |   +-- panel             owner-drawn settings panel (no child controls)
    |   +-- desktop_canvas    calendar widget (layered window, one blit per frame)
    |   +-- note / note_palette / note_manager / capture   note window, note collection, region capture
    |   +-- tray / settings / startup
    |
    +-- core/                 pure business logic, no UI dependency
    |   +-- group             group model and groups.tsv
    |   +-- layout_math / composer / desktop_wallpaper / state / slice
    |   +-- window_enum / window_layout / layout_store
    |   +-- canvas / calendar_face          widget geometry and calendar text
    |   +-- note_layout / note_store / note_theme        note placement, index and body files
    |   +-- display_watcher
    |
    +-- platform/win/         COM RAII, WIC factory
    +-- util/                 path / text / log / i18n
```

Three constraints run through the whole project:

- **One implementation per action**: the tray menu, panel buttons and CLI all call `actions.h`;
  `Service` is the only place that knows what an action means.
- **`Panel` performs no IO**: every click becomes a `PanelAction` handed to `Service`, and
  `PanelState` is a pure input delivered by one `ReadState()` call. The panel reads neither the
  registry nor files.
- **Drawing and hit testing share one source**: `ComputeGeometry()` feeds both, so a click can
  never land on a control that was not drawn.

### 2.2 Architectural traits

| Trait | Approach |
|---|---|
| Zero-dependency single file | Statically linked CRT; imports only `KERNEL32 / USER32 / GDI32 / ole32 / SHELL32 / ADVAPI32 / dwmapi / gdiplus` |
| Pure COM graphics stack | `IDesktopWallpaper` + `IWICImagingFactory`, both built into Windows; no WinRT / .NET / Windows App SDK |
| Testable geometry | Cover cropping, framing, placement plans and panel geometry are static pure functions; the test binary compiles `panel.cpp` directly |
| Group-based rendering | One `RenderSpan` per group over its own member canvas; all `pieces` are collected before a single cleanup pass |
| Preview/render parity | Both sides share the same cover geometry; a unit test aligns `SampleFor` with `ComputeSlice` group by group |
| High DPI | Per-Monitor V2 process; the panel lays out in 96 dpi design units and maps to device pixels with `MM_ANISOTROPIC` |
| Custom rendering | Borderless `WS_POPUP`; the entire client area is drawn into a 32bpp DIB; rounded corners are antialiased with a per-pixel distance field |
| Asynchronous feedback | Long tasks paint a busy frame first, then run on a worker thread and post `WM_APP+4` back to the UI thread |
| Fault tolerance | COM failures are logged and return false rather than throwing; the log has millisecond timestamps and rotates at 1 MB |
| Bilingual UI | The Chinese source string is the lookup key; drawing, text measurement, logging, menus and the CLI all resolve through one resource layer. Missing entries fall back to Chinese |
| Live language switch | Picking a language on the config page refreshes immediately; buttons, chips and labels are re-measured in the new language without a restart |
| The widget is just a window | The calendar never owns the desktop; it re-asserts one idempotent rule - sit directly above the desktop host - every 400 ms |
| Bodies stored untouched | Note bodies go in and out as the exact RTF bytes RichEdit produced, never reinterpreted as UTF-8, so Chinese text and pictures survive |

### 2.3 Group model

`core::Group = { slot, image, view, members, dirty }`, one line per group in `groups.tsv`:

```
group  zoom  focusX  focusY  dirty  image  m1;m2;...
```

- `NormaliseGroups()` is idempotent and runs on every `ReadState()`: one display per group, a
  newly attached display joins the first group, emptied groups disappear, `slot` values are
  renumbered, and temporarily detached members keep their place.
- `MoveMonitorToGroup()` is the only atomic regrouping operation; `targetSlot == -1` means
  "make a group of one".
- Group names are derived: `GroupLabel()` concatenates ascending screen numbers. Screen numbers
  themselves are read row by row and then sorted by `left`, so a 2x2 arrangement is not numbered
  by column.
- The legacy `wallpaper.source` + `wallpapers.txt` format is migrated by `GroupsFromLegacy()` on
  first launch: uncovered displays go into one main group, each per-display override becomes its
  own single-display group. The old files are kept.

### 2.4 Wallpaper rendering pipeline

```
source -- WIC decode -- cover scale -- crop per display rect -- PNG slices -- IDesktopWallpaper
                            ^                                        |
              WallpaperView{zoom,focusX,focusY}               position = Center
```

- The system's Span mode is not used: it maps the image onto the virtual-desktop bounding
  rectangle and is only correct when the source dimensions match that rectangle exactly. Here each
  slice is computed per display, so gaps in the bounding rectangle never enter any image.
- When source dimensions equal the canvas, a 1:1 fast path copies pixels directly.
- `WallpaperView` reaches all the way to the renderer; centered cover is the default, and panel
  dragging / scrolling modifies it.
- `ApplyGroups()` renders group by group, then calls `RemoveStalePieces(folder, keep)` once at
  the end. Cleanup is driven by "what was generated this round" and only recognises the slice
  filename shape.
- The map calls `RenderSurface` with each group's own canvas context - the whole turned picture,
  uncropped - and then takes each display's window out of it with the same geometry
  `ComputeSlice` uses, so what is drawn is what gets applied.

### 2.5 Window layout restoration

- **Record display ownership, not absolute coordinates**: when a display origin moves, its windows
  shift by the same amount; windows on a display that is offline are skipped until it returns.
- Maximized windows store `rcNormalPosition`; snapping is recognised by shape (three edges flush,
  one cut), split ratios are stored in permille, and half-screen targets derive from `rcWork`.
- "Already in place" has two rules: ordinary windows compare all four edges (2 px tolerance);
  maximized windows check whether they are still maximized and whether their restore rectangle
  lands on the target display.
- Automatic restore uses `Homecoming` semantics (it does not override a manual split); the manual
  "Restore Layout" button uses `Exact`.
- `layout.txt` supports v1 / v2 / v3: v3 starts with a version marker and adds a `splitPermille`
  column. Older files are parsed by column count; unchecking a window writes `-` into the flags
  column instead of deleting the row.

### 2.6 Panel rendering

- Everything is drawn into a 32bpp DIB with no child controls. `PaintContent` maps design units
  to device pixels with `MM_ANISOTROPIC`, and text is rasterised at the device font size.
- The measurement DC and the paint DC use the same horizontal and vertical scaling, which keeps
  every text-width-based layout (buttons, chips, badge plates) aligned.
- Rounded corners use a per-pixel distance field while straight edges remain block fills;
  strokes compute coverage from the centre line.
- `DrawGroupOutline()` takes the four edges of every member display, subtracts internal seams
  covered by other members of the same group, and draws only the true union outline. The selected
  group's halo is a real 16% alpha blend.
- Screen badges use a three-tier layout based on display width: number + resolution, number only,
  or nothing. A badge never overflows its own display.

### 2.7 Data and configuration

All runtime files live in one directory:

```
%LOCALAPPDATA%\WinTools\
├── settings.txt        theme, UI language, auto restore, autostart, active group, debounce seconds
├── groups.tsv          group definitions (primary wallpaper configuration file)
├── layout.txt          window layout snapshot
├── wallpapers.txt      legacy migration source
├── WinTools.log        diagnostic log
├── wallpaper\
│   ├── state.txt       wallpaper state from before the change
│   └── pieces\         one slice per display; the filename carries the geometry
└── notes\
    ├── index.txt       note index: id, position, size, paper colour and star
    ├── archive.txt     closed notes: id, position, paper colour, preview, close time and star
    ├── <id>.rtf        body
    └── <id>.deleted.rtf  retired body; reopening from history renames it back
```

Autostart writes the `WinTools` value under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`; a single-instance mutex is named
`Local\WinTools.SingleInstance.*`. Configuration files are plain TSV / key=value text, validated
by column count and per-field rules when read.

---

## 3. Possible Next Steps

- **Dynamic desktop**: let the desktop surface refresh continuously so a personal workbench
  (calendar, tasks, live metrics) can be embedded directly into the desktop.
- **Window interaction and desktop customization**: build on the existing layout restoration with
  more proactive window operations, plus other customization of the Windows desktop layer.

---

## 4. Build and Test

Prerequisites: Visual Studio Build Tools with the "Desktop development with C++" workload, plus a
Windows 10/11 SDK. CMake and Ninja come with Visual Studio; `build.bat` enters `vcvars64` and
wires up the SDK environment itself. No .NET / Windows App SDK / vcpkg.

```bat
scripts\build.bat                          :: debug
scripts\build.bat windows-x64-release      :: release
build\debug\bin\wintools_tests.exe         :: 183 tests
```

Output lands in `build\<config>\bin\`; the release `WinTools.exe` can be copied out on its own.
Test groups: layout maths and monitors 20, window placement and snapping 24, layout file 8,
groups 20, panel 17, note index 15, note placement 10, note archive 9, paper palette 9,
note body 5, wallpaper store 8, UI languages 7, slice cache 5, preview 4, hotkeys 8,
lunar calendar 7, canvas 7.

---

## 5. Repository Layout

```
WinTools/
├── CMakeLists.txt / CMakePresets.json   single build entry point, Ninja + cl.exe
├── README.md / README.en.md             Simplified Chinese / English
├── LICENSE        MIT license
├── assets/        wintools.ico, app.rc (version info), app.manifest (Per-Monitor V2)
├── scripts/       build, test images, pixel verification, panel capture, click automation
├── docs/          panel.png / panel-en.png screenshots, panel-demo.gif / panel-demo-en.gif animations
├── src/           main / app / core / platform/win / util
├── tests/         unit tests
└── build/         generated output, gitignored
```

---

## 6. Version and License

- The binary version resource is `0.1.0.0` (`assets/app.rc`); keep it in sync with the release tag.
- The source is released under the [MIT License](LICENSE): use, modify and redistribute it
  freely, provided the copyright and permission notices are kept.
