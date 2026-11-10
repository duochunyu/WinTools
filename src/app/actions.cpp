#include "app/actions.h"

#include <Windows.h>
#include <shobjidl.h>

#include <format>
#include <system_error>
#include <vector>

#include "core/composer.h"
#include "core/desktop_wallpaper.h"
#include "core/group.h"
#include "core/layout_store.h"
#include "core/state.h"
#include "core/wallpaper_store.h"
#include "platform/win/com.h"
#include "util/log.h"
#include "util/path.h"
#include "util/text.h"

namespace wintools::app
{

using util::LogError;
using util::LogInfo;
using util::LogWarn;

namespace
{

/// Opens the shell wallpaper interface, reporting the failure in the caller's own words.
bool OpenWallpaper(core::DesktopWallpaper& wallpaper, ActionResult& result)
{
    if (wallpaper.IsValid())
    {
        return true;
    }

    result.ok = false;
    result.message = L"无法访问壁纸接口";
    LogError(L"无法创建桌面壁纸 COM 对象");
    return false;
}

std::vector<core::Monitor> CollectMonitors(const core::DesktopWallpaper& wallpaper, ActionResult& result)
{
    std::vector<core::Monitor> monitors = wallpaper.Enumerate();
    if (monitors.empty())
    {
        result.ok = false;
        result.message = L"没有检测到任何显示器";
        LogError(L"没有枚举到任何显示器：当前会话可能不是交互式桌面，或者 shell 接口不可用");
    }
    else
    {
        result.ok = true;
    }

    return monitors;
}

/// "11-06 22:31" in local time, or empty when the timestamp cannot be read.
std::wstring LastWriteLabel(const std::filesystem::path& file)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data))
    {
        return {};
    }

    FILETIME local{};
    if (!::FileTimeToLocalFileTime(&data.ftLastWriteTime, &local))
    {
        return {};
    }

    SYSTEMTIME time{};
    if (!::FileTimeToSystemTime(&local, &time))
    {
        return {};
    }

    return std::format(L"{:02}-{:02} {:02}:{:02}", time.wMonth, time.wDay, time.wHour, time.wMinute);
}

/// Read-modify-write of the saved layout, used by every panel edit.
/// Always re-reads from disk instead of trusting a caller-held copy: the panel repaints from the
/// file anyway, and a stale copy would silently undo an edit that happened in between.
template <typename Edit>
ActionResult UpdateSavedWindow(int index, Edit&& edit)
{
    ActionResult result;

    const std::filesystem::path file = core::DefaultLayoutFile();
    auto layout = core::LoadLayout(file);
    if (!layout.has_value())
    {
        result.message = L"还没有保存";
        return result;
    }

    if (index < 0 || index >= static_cast<int>(layout->windows.size()))
    {
        result.message = L"窗口已不在布局里";
        return result;
    }

    edit(layout->windows[index]);

    if (!core::SaveLayout(*layout, file))
    {
        result.message = L"布局文件写入失败";
        return result;
    }

    result.ok = true;
    return result;
}

/// snapshots carry the geometry the user framed against; the shell is asked for the device path
/// separately because SetWallpaper only accepts the string the shell itself produced.
const core::Monitor* FindLiveMonitor(
    const std::vector<core::Monitor>& live, std::wstring_view devicePath)
{
    for (const auto& monitor : live)
    {
        if (monitor.MatchesKey(devicePath))
        {
            return &monitor;
        }
    }

    return nullptr;
}

core::Monitor CanvasMonitor(const core::MonitorSnapshot& snapshot)
{
    core::Monitor monitor;
    monitor.devicePath = snapshot.devicePath;
    monitor.monitorKey = core::Monitor::BuildKey(snapshot.devicePath);
    monitor.left = snapshot.rect.left;
    monitor.top = snapshot.rect.top;
    monitor.width = snapshot.rect.width;
    monitor.height = snapshot.rect.height;
    return monitor;
}

/// Captures the wallpaper that was there before the first apply.
///
/// Once, and never again: re-capturing after every apply would overwrite the user's real
/// wallpaper with a generated piece, and 还原 would then restore our own output.
void CaptureOriginalOnce(const core::DesktopWallpaper& wallpaper)
{
    const std::filesystem::path stateFile = util::StateFile();
    std::error_code ec;
    if (std::filesystem::exists(stateFile, ec))
    {
        return;
    }

    const core::WallpaperState state = core::CaptureState(wallpaper);
    if (state.Save(stateFile))
    {
        LogInfo(L"原始壁纸状态已保存: {}", stateFile.wstring());
    }
    else
    {
        LogWarn(L"原始壁纸状态保存失败，restore 将不可用");
    }
}

} // namespace

ActionResult ApplySpanningWallpaper(
    const std::filesystem::path& image, bool dryRun, const core::WallpaperView& view)
{
    ActionResult result;

    core::DesktopWallpaper wallpaper;
    if (!OpenWallpaper(wallpaper, result))
    {
        return result;
    }

    const std::vector<core::Monitor> monitors = CollectMonitors(wallpaper, result);
    if (monitors.empty())
    {
        return result;
    }

    // Read once per apply and hand the renderer a lookup, not a container: whether a display has
    // its own picture is a property of one display, and the renderer is the one that walks them.
    const core::MonitorWallpapers assigned = ListDisplayPictures();
    const auto ownPicture = [&assigned](const std::wstring& devicePath) {
        return std::filesystem::path{ core::FindMonitorImage(assigned, devicePath) };
    };

    const std::filesystem::path folder = util::PieceFolder();
    std::vector<core::RenderedPiece> pieces;
    const bool allRendered = core::RenderSpan(image, monitors, folder, pieces, view, ownPicture);

    if (pieces.empty())
    {
        result.ok = false;
        result.message = L"没有生成任何切片";
        LogError(L"没有生成任何切片，放弃应用");
        return result;
    }

    LogInfo(L"已生成 {} / {} 张切片 -> {}", pieces.size(), monitors.size(), folder.wstring());

    if (dryRun)
    {
        LogInfo(L"--dry-run：未修改壁纸");
        result.ok = allRendered;
        result.message = std::format(L"演练完成，生成 {} 张切片", pieces.size());
        return result;
    }

    // Capture the original state once, before the first application. Keeping the very first
    // snapshot is deliberate: re-capturing after every apply would overwrite the user's real
    // wallpaper with the previous generated piece and restore would become a no-op.
    CaptureOriginalOnce(wallpaper);

    // Center, not Fill: the pieces are already exactly one monitor in size, so any other mode
    // would resample them a second time.
    if (!wallpaper.SetPosition(core::WallpaperPosition::Center))
    {
        result.ok = false;
        result.message = L"无法设为居中";
        LogError(L"无法把壁纸位置设为 Center");
        return result;
    }

    int applied = 0;
    for (const auto& piece : pieces)
    {
        if (piece.monitorIndex >= monitors.size())
        {
            continue;
        }

        const auto& monitor = monitors[piece.monitorIndex];
        if (wallpaper.SetWallpaper(monitor.devicePath, piece.path))
        {
            ++applied;
        }
    }

    LogInfo(L"拼接壁纸已应用");

    if (applied != static_cast<int>(pieces.size()) || !allRendered)
    {
        result.ok = false;
        result.message = std::format(L"只成功贴上 {} / {} 台显示器", applied, monitors.size());
        LogError(L"部分显示器贴图失败，可用 restore 回滚");
        return result;
    }

    // Only once everything landed: a slice that failed to apply may still be the one the desktop
    // is showing, and deleting it would leave that display pointing at nothing.
    const int removed = core::RemoveStalePieces(folder, pieces);
    if (removed > 0)
    {
        LogInfo(L"清掉 {} 张过期切片", removed);
    }

    result.ok = true;
    result.message = std::format(L"已铺满 {} 台显示器", monitors.size());
    return result;
}

ActionResult ApplyGroups(const std::vector<core::Group>& groups,
    const std::vector<core::MonitorSnapshot>& monitors, bool dryRun)
{
    ActionResult result;

    core::DesktopWallpaper wallpaper;
    if (!OpenWallpaper(wallpaper, result))
    {
        return result;
    }

    const std::vector<core::Monitor> live = CollectMonitors(wallpaper, result);
    if (live.empty())
    {
        return result;
    }

    const std::filesystem::path folder = util::PieceFolder();

    // Pieces are kept per group until every group has been rendered, because the sweep at the end
    // deletes whatever this round did not produce - and a group rendered first would be swept by
    // the second group's cleanup. See the note on ApplyGroups.
    struct Applied
    {
        core::RenderedPiece piece;
        std::wstring devicePath;
    };

    std::vector<Applied> applied;
    std::vector<core::RenderedPiece> keep;
    int screens = 0;
    int groupsRendered = 0;
    bool everyGroupRendered = true;

    for (const auto& group : groups)
    {
        if (group.image.empty())
        {
            continue;
        }

        // The canvas is the group's own bounding box: exactly what the panel previewed, since the
        // preview calls the same GetCanvas with the same members.
        std::vector<core::Monitor> canvas;
        for (const size_t index : core::AttachedMembers(group, monitors))
        {
            const core::Monitor* monitor = FindLiveMonitor(live, monitors[index].devicePath);
            if (monitor == nullptr)
            {
                // Unplugged between the panel reading the state and this call. Skipping it is
                // better than writing a piece for a display that is not there.
                continue;
            }

            core::Monitor entry = CanvasMonitor(monitors[index]);
            entry.devicePath = monitor->devicePath;
            entry.monitorKey = monitor->monitorKey;
            canvas.push_back(std::move(entry));
        }

        if (canvas.empty())
        {
            continue;
        }

        std::vector<core::RenderedPiece> pieces;
        if (!core::RenderSpan(group.image, canvas, folder, pieces, group.view))
        {
            everyGroupRendered = false;
        }

        if (pieces.empty())
        {
            everyGroupRendered = false;
            LogWarn(L"组合没有生成任何切片: {}", group.image);
            continue;
        }

        ++groupsRendered;
        screens += static_cast<int>(pieces.size());
        LogInfo(L"组合 {}: {} 张切片", core::GroupLabel(group, monitors), pieces.size());

        for (const auto& piece : pieces)
        {
            if (piece.monitorIndex >= canvas.size())
            {
                continue;
            }

            applied.push_back(Applied{ piece, canvas[piece.monitorIndex].devicePath });
            keep.push_back(piece);
        }
    }

    if (applied.empty())
    {
        result.ok = false;
        result.message = L"还没有组合选过图片";
        LogWarn(L"没有任何组合带图片，未修改壁纸");
        return result;
    }

    if (dryRun)
    {
        result.ok = everyGroupRendered;
        result.message = std::format(L"演练完成，生成 {} 张切片", applied.size());
        return result;
    }

    CaptureOriginalOnce(wallpaper);

    // Center, not Fill: the pieces are already exactly one monitor in size, so any other mode
    // would resample them a second time.
    if (!wallpaper.SetPosition(core::WallpaperPosition::Center))
    {
        result.ok = false;
        result.message = L"无法设为居中";
        LogError(L"无法把壁纸位置设为 Center");
        return result;
    }

    int pasted = 0;
    for (const auto& entry : applied)
    {
        if (wallpaper.SetWallpaper(entry.devicePath, entry.piece.path))
        {
            ++pasted;
        }
    }

    if (pasted != static_cast<int>(applied.size()) || !everyGroupRendered)
    {
        result.ok = false;
        result.message = std::format(L"只成功贴上 {} / {} 台显示器", pasted, screens);
        LogError(L"部分显示器贴图失败，可用 restore 回滚");
        return result;
    }

    // Once, for every group at the same time: the sweep cannot tell which group a slice belongs
    // to, so it has to see the whole round's output before it decides what is stale.
    const int removed = core::RemoveStalePieces(folder, keep);
    if (removed > 0)
    {
        LogInfo(L"清掉 {} 张过期切片", removed);
    }

    result.ok = true;
    result.message = groupsRendered > 1
        ? std::format(L"已贴 {} 张切片 · {} 组合 {} 台屏", pasted, groupsRendered, screens)
        : std::format(L"已贴 {} 张切片 · {} 台屏", pasted, screens);
    return result;
}

std::vector<core::MonitorWallpaper> ListDisplayPictures()
{
    const auto stored = core::LoadMonitorWallpapers(core::DefaultWallpaperFile());
    return stored.value_or(core::MonitorWallpapers{});
}

ActionResult SetDisplayPicture(const std::wstring& devicePath, const std::filesystem::path& image)
{
    ActionResult result;

    if (devicePath.empty())
    {
        result.message = L"没有选中显示器";
        return result;
    }

    // Read-modify-write, like the layout edits: two windows cannot race here, but a file edited
    // by hand while the panel is open would otherwise be silently overwritten from a stale copy.
    const std::filesystem::path file = core::DefaultWallpaperFile();
    core::MonitorWallpapers stored = ListDisplayPictures();
    core::SetMonitorImage(stored, devicePath, image.wstring());

    if (!core::SaveMonitorWallpapers(stored, file))
    {
        result.message = L"逐屏壁纸写入失败";
        LogError(L"无法保存逐屏壁纸: {}", file.wstring());
        return result;
    }

    result.ok = true;
    return result;
}

ActionResult RestoreSavedWallpaper()
{
    ActionResult result;

    core::DesktopWallpaper wallpaper;
    if (!OpenWallpaper(wallpaper, result))
    {
        return result;
    }

    const std::filesystem::path stateFile = util::StateFile();
    const auto state = core::WallpaperState::Load(stateFile);
    if (!state.has_value())
    {
        result.ok = false;
        result.message = L"没有保存过原壁纸，无法还原";
        LogError(L"没有找到状态文件，无可恢复: {}", stateFile.wstring());
        return result;
    }

    if (!core::RestoreState(wallpaper, *state))
    {
        result.ok = false;
        result.message = L"还原未完成，详情见日志";
        LogError(L"恢复未完全成功，可检查上面几行的具体失败项");
        return result;
    }

    LogInfo(L"壁纸已恢复");
    result.ok = true;
    result.message = L"已还原";
    return result;
}

ActionResult CaptureCurrentLayout(int& windowCount, int& monitorCount)
{
    ActionResult result;

    core::Layout layout = core::CaptureLayout();
    windowCount = static_cast<int>(layout.windows.size());
    monitorCount = static_cast<int>(layout.monitors.size());

    if (layout.windows.empty())
    {
        result.ok = false;
        result.message = L"没有找到可记录的窗口";
        LogWarn(L"没有枚举到任何可管理的窗口");
        return result;
    }

    // Refresh re-enumerates from scratch, which would otherwise re-tick every window the user had
    // deliberately unticked. Only the tick is carried over: everything else - rectangle, dock,
    // split - must come from the capture that just happened, or the point of refreshing is lost.
    if (const auto previous = core::LoadLayout(core::DefaultLayoutFile()); previous.has_value())
    {
        for (auto& window : layout.windows)
        {
            for (const auto& old : previous->windows)
            {
                if (old.exePath == window.exePath && old.title == window.title
                    && old.monitorPath == window.monitorPath)
                {
                    window.enabled = old.enabled;
                    break;
                }
            }
        }
    }

    const std::filesystem::path file = core::DefaultLayoutFile();
    if (!core::SaveLayout(layout, file))
    {
        result.ok = false;
        result.message = L"布局文件写入失败";
        return result;
    }

    LogInfo(L"已保存布局: {} 个窗口 / {} 台显示器 -> {}", windowCount, monitorCount, file.wstring());
    result.ok = true;
    result.message = std::format(L"已记录 {} 个窗口", windowCount);
    return result;
}

ActionResult RestoreSavedLayout(core::RestoreStats& stats, core::RestoreScope scope)
{
    ActionResult result;
    stats = {};

    const std::filesystem::path file = core::DefaultLayoutFile();
    const auto layout = core::LoadLayout(file);
    if (!layout.has_value())
    {
        result.ok = false;
        result.message = L"还没有保存";
        return result;
    }

    stats = core::RestoreLayout(*layout, scope);

    result.ok = stats.failed == 0 && (stats.moved + stats.alreadyInPlace > 0);

    // One sentence, built from whatever actually happened. A fixed template would either hide a
    // partial failure or invent one.
    std::wstring message;
    if (stats.moved > 0)
    {
        message = std::format(L"已摆好 {} 个窗口", stats.moved);
    }
    else if (stats.alreadyInPlace > 0)
    {
        message = L"窗口都还在原位";
    }
    else if (stats.skippedMissingMonitor > 0)
    {
        message = L"布局里涉及的显示器现在都不在";
    }
    else if (stats.skippedDisabled > 0)
    {
        message = L"布局里的窗口都被取消勾选了";
    }
    else
    {
        message = L"布局里的程序都没有在运行";
    }

    std::vector<std::wstring> notes;
    if (stats.moved > 0 && stats.alreadyInPlace > 0)
    {
        notes.push_back(std::format(L"{} 个本来就在位", stats.alreadyInPlace));
    }
    if (stats.missingWindow > 0)
    {
        notes.push_back(std::format(L"{} 个程序没运行", stats.missingWindow));
    }
    if (stats.skippedMissingMonitor > 0)
    {
        notes.push_back(std::format(L"{} 个显示器不在", stats.skippedMissingMonitor));
    }
    if (stats.skippedDisabled > 0)
    {
        notes.push_back(std::format(L"{} 个已取消勾选", stats.skippedDisabled));
    }
    if (stats.failed > 0)
    {
        notes.push_back(std::format(L"{} 个摆放失败", stats.failed));
    }

    if (!notes.empty())
    {
        std::wstring joined;
        for (const auto& note : notes)
        {
            if (!joined.empty())
            {
                joined += L"，";
            }
            joined += note;
        }

        message += L"（" + joined + L"）";
    }

    result.message = std::move(message);
    return result;
}

LayoutSummary DescribeSavedLayout()
{
    LayoutSummary summary;

    const std::filesystem::path file = core::DefaultLayoutFile();
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
    {
        return summary;
    }

    const auto layout = core::LoadLayout(file);
    if (!layout.has_value())
    {
        return summary;
    }

    summary.exists = true;
    summary.windows = static_cast<int>(layout->windows.size());
    summary.monitors = static_cast<int>(layout->monitors.size());
    summary.savedAt = LastWriteLabel(file);
    return summary;
}

std::wstring SavedLayoutTimestamp()
{
    return LastWriteLabel(core::DefaultLayoutFile());
}

std::optional<core::Layout> LoadSavedLayout()
{
    // No DPI conversion here, and that is a measured fact rather than an assumption: this desktop
    // runs its displays at 125%, and capturing the same windows before and after the switch to
    // per-monitor awareness produced identical rectangles. A DPI-unaware process is only put in a
    // scaled coordinate space when the *system* DPI is above 96, which needs the primary display
    // to be scaled - so what the file holds is already real pixels.
    return core::LoadLayout(core::DefaultLayoutFile());
}

ActionResult SetWindowRemembered(int index, bool remembered)
{
    ActionResult result = UpdateSavedWindow(
        index, [remembered](core::WindowSnapshot& window) { window.enabled = remembered; });

    if (result.ok)
    {
        result.message = remembered ? L"跟随恢复" : L"不跟随恢复";
    }

    return result;
}

std::optional<std::filesystem::path> PickImageFile(HWND owner, std::wstring_view title)
{
    platform::ComPtr<IFileOpenDialog> dialog;
    const HRESULT created = ::CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(created))
    {
        LogError(L"无法创建文件选择对话框: {}", util::FormatHresult(created));
        return std::nullopt;
    }

    const COMDLG_FILTERSPEC filters[] = {
        { L"图片", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff" },
        { L"所有文件", L"*.*" },
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    const std::wstring caption{ title };
    dialog->SetTitle(caption.c_str());

    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    }

    // Modal to the panel: without an owner the dialog can end up behind the panel and the app
    // looks frozen.
    if (dialog->Show(owner) != S_OK)
    {
        return std::nullopt;
    }

    platform::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)) || item == nullptr)
    {
        return std::nullopt;
    }

    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr)
    {
        return std::nullopt;
    }

    std::filesystem::path chosen{ raw };
    ::CoTaskMemFree(raw);
    return chosen;
}

} // namespace wintools::app
