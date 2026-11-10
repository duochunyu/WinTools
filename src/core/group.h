#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/layout_math.h"
#include "core/monitor.h"
#include "core/wallpaper_store.h"
#include "core/window_layout.h"

namespace wintools::core
{

/// One combination: a picture, a framing, and the displays that share them.
///
/// The panel's whole model is this struct. "One picture spanning four screens" is one group with
/// four members; "two pairs, each spliced" is two groups of two; "four independent pictures" is
/// four groups of one. There is no mode switch, because those are the same thing three times.
///
/// The name shown to the user is *not* stored. It is derived from the member screen numbers
/// (see GroupLabel), because a stored number - "group B" - has to be renumbered the moment a
/// member moves, and then it stops matching whatever the user remembered it as.
struct Group
{
    /// Position in the list, which decides both the border colour (--gN) and the order of the
    /// chips. Always equal to the group's index after NormaliseGroups, stored anyway so a colour
    /// is a property of the group rather than of a loop counter.
    int slot{};

    /// Full path of the picture this group is showing; empty until one is picked.
    std::wstring image;

    /// How the picture is framed across this group's canvas. Per group, not per app: two groups
    /// showing the same file can be framed differently, and moving a screen between groups must
    /// not silently reframe the destination.
    WallpaperView view{};

    /// Display device paths. May contain displays that are not attached right now - a monitor
    /// that was unplugged keeps its place in its group and rejoins it on the way back - so this
    /// is not the same list as the group's *visible* members. See AttachedMembers.
    std::vector<std::wstring> members;

    /// The framing or the picture changed and has not been rendered onto the desktop yet.
    bool dirty{};
};

/// Displays in the order a person reads them off the desk: left to right, then top to bottom.
///
/// Indices into monitors. "Screen 2" everywhere in the panel means the second entry here, which
/// is why the sorting lives in one function instead of being re-derived at every call site.
[[nodiscard]] std::vector<size_t> ReadingOrder(std::span<const MonitorSnapshot> monitors);

/// 1-based screen number of a display in reading order; 0 when it is not attached.
[[nodiscard]] int ScreenNumber(std::wstring_view devicePath,
    std::span<const MonitorSnapshot> monitors);

/// The members of this group that are attached right now, in reading order.
[[nodiscard]] std::vector<size_t> AttachedMembers(
    const Group& group, std::span<const MonitorSnapshot> monitors);

/// "3+4": member screen numbers, ascending, joined with a plus. No "screen" prefix, no spaces.
///
/// Empty when none of the members is attached - a group whose screens are all unplugged has no
/// name to show, and the panel hides it until one comes back.
[[nodiscard]] std::wstring GroupLabel(
    const Group& group, std::span<const MonitorSnapshot> monitors);

/// The slot of the group a display belongs to, or -1 when it is in none.
[[nodiscard]] int GroupOf(const std::vector<Group>& groups, std::wstring_view devicePath);

/// Makes the list obey the three invariants, in this order:
///
///   1. a display belongs to exactly one group (a duplicate membership keeps the first one);
///   2. every attached display belongs to some group (newcomers join the first group);
///   3. no group is empty, and slots are 0..n-1 in list order.
///
/// Idempotent, and deliberately total: it is called on every state read, so a display plugged in
/// between two paints is already home by the time the panel draws it.
void NormaliseGroups(std::vector<Group>& groups, std::span<const MonitorSnapshot> monitors);

/// Moves one display into another group; targetSlot -1 means "into a group of its own".
///
/// The only membership operation there is: a split and a merge are the same edit seen from two
/// ends, so both the screen-number menu and the chip's plus button land here. A group that loses
/// its last member disappears, which renumbers the others - callers must not hold on to slots.
///
/// Returns the slot the moved display ended up in, or -1 when nothing changed (unknown display,
/// or it is already there).
int MoveMonitorToGroup(
    std::vector<Group>& groups, std::wstring_view devicePath, int targetSlot);

/// The state a fresh install migrates into, or an upgrade from the per-display format.
///
/// Before groups existed the app stored one spanning picture plus optional per-display overrides.
/// That maps onto the new model exactly: the spanning picture becomes one group holding every
/// display that was not overridden, and each overridden display becomes a group of one. Nothing
/// is lost, and the arrangement the user already had is reproduced pixel for pixel.
[[nodiscard]] std::vector<Group> GroupsFromLegacy(std::span<const MonitorSnapshot> monitors,
    const std::wstring& spanImage, const WallpaperView& spanView,
    const MonitorWallpapers& overrides);

/// Default location: %LOCALAPPDATA%\WinTools\groups.tsv
///
/// Separate from settings.txt because it is a list rather than a set of scalars, and separate
/// from layout.txt because the two change at completely different moments.
[[nodiscard]] std::filesystem::path DefaultGroupFile();

[[nodiscard]] bool SaveGroups(const std::vector<Group>& groups, const std::filesystem::path& file);

/// Reads the groups back. nullopt when the file is missing or holds no usable row - the caller
/// then migrates instead of starting empty, so an upgrade never loses the current arrangement.
[[nodiscard]] std::optional<std::vector<Group>> LoadGroups(const std::filesystem::path& file);

} // namespace wintools::core
