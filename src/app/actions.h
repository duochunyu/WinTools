#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/group.h"
#include "core/layout_math.h"
#include "core/wallpaper_store.h"
#include "core/window_layout.h"

namespace wintools::app
{

/// Result of a user-triggered action.
///
/// Carries a short human sentence rather than an error code, because there are exactly two
/// consumers - the console and the panel's status line - and both want to show that sentence.
/// The detailed, per-monitor lines still go through util::Log while the action runs.
struct ActionResult
{
    bool ok{};
    std::wstring message;
};

/// Renders per-monitor pieces from image and applies them. The whole spanning pipeline.
///
/// view is the framing the user adjusted in the panel; the default is the centred cover crop.
ActionResult ApplySpanningWallpaper(const std::filesystem::path& image, bool dryRun = false,
    const core::WallpaperView& view = {});

/// Renders and applies every group of the arrangement, each onto its own members.
///
/// One pass for the whole set, not one call per group, and that is a correctness requirement
/// rather than tidiness: the slice folder is cleaned by "delete everything this round did not
/// produce", so applying group 2 after group 1 would delete group 1's slices - which the desktop
/// is already showing. Every group's pieces are collected first and the folder is swept once.
///
/// Groups with no picture are skipped. Their displays keep whatever the shell has, which is the
/// only honest outcome: "no picture" is a state the user created by splitting a group before
/// choosing one.
ActionResult ApplyGroups(const std::vector<core::Group>& groups,
    const std::vector<core::MonitorSnapshot>& monitors, bool dryRun = false);

/// Gives one display its own picture, or drops it back to the spanning slice when image is empty.
///
/// Only the stored assignment is touched. Re-rendering is the caller's job, because only the
/// service knows the spanning picture and the crop that are currently in force; doing it here
/// would need those two as parameters and would duplicate the apply path.
ActionResult SetDisplayPicture(const std::wstring& devicePath, const std::filesystem::path& image);

/// Every display that has a picture of its own, as stored. Empty when nothing has been assigned.
[[nodiscard]] std::vector<core::MonitorWallpaper> ListDisplayPictures();

/// Puts back whatever wallpaper state was captured before the first apply.
ActionResult RestoreSavedWallpaper();

/// Re-captures the positions of every window on every display.
///
/// Windows that were already in the saved layout keep their "remember this one" tick, so pressing
/// refresh after moving a couple of windows around does not silently re-enable the ones the user
/// had deliberately excluded.
ActionResult CaptureCurrentLayout(int& windowCount, int& monitorCount);

/// Puts the saved layout back. stats is filled in even when the action fails part-way through.
/// defaultScope is Exact for the buttons and the command line, Homecoming for the automatic
/// restore that runs when displays settle - see core::RestoreScope.
ActionResult RestoreSavedLayout(core::RestoreStats& stats,
    core::RestoreScope scope = core::RestoreScope::Exact);

/// What the panel needs to describe the saved layout without walking every window.
struct LayoutSummary
{
    bool exists{};
    int windows{};
    int monitors{};
    std::wstring savedAt;
};

LayoutSummary DescribeSavedLayout();

/// "11-06 22:31" when the layout file was last written, or empty when it cannot be read.
[[nodiscard]] std::wstring SavedLayoutTimestamp();

/// The saved layout itself, for the panel's overlay and window list. nullopt when there is none.
[[nodiscard]] std::optional<core::Layout> LoadSavedLayout();

/// Untick or re-tick one remembered window.
///
/// Only that one field is rewritten; the file is re-read first, so an edit from one window can
/// never clobber an edit that landed a moment earlier from another.
ActionResult SetWindowRemembered(int index, bool remembered);

/// Native "open file" dialog restricted to images WIC can decode.
///
/// Returns nullopt when the user cancels. An empty optional does not distinguish cancel from
/// failure on purpose: both mean "do nothing", and a modal error box for a cancelled dialog is
/// the kind of thing that makes small utilities annoying.
std::optional<std::filesystem::path> PickImageFile(
    HWND owner, std::wstring_view title = L"选择图片");

} // namespace wintools::app
