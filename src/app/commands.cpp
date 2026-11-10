#include "app/commands.h"

#include <Windows.h>

#include <format>
#include <system_error>
#include <vector>

#include "app/actions.h"
#include "app/service.h"
#include "app/startup.h"
#include "core/composer.h"
#include "core/desktop_wallpaper.h"
#include "core/layout_math.h"
#include "core/layout_store.h"
#include "core/window_enum.h"
#include "util/log.h"
#include "util/path.h"

namespace wintools::app
{

using util::LogError;
using util::LogInfo;
using util::LogWarn;

namespace
{

/// IDI_WINTOOLS from assets/app.rc. Kept as a number rather than a generated header: there is
/// exactly one resource in the project and a header for it would be more moving parts than it
/// saves.
constexpr int kIconResource = 1;

const wchar_t* PositionName(core::WallpaperPosition position)
{
    switch (position)
    {
        case core::WallpaperPosition::Center:
            return L"Center(居中)";
        case core::WallpaperPosition::Tile:
            return L"Tile(平铺)";
        case core::WallpaperPosition::Stretch:
            return L"Stretch(拉伸)";
        case core::WallpaperPosition::Fit:
            return L"Fit(适应)";
        case core::WallpaperPosition::Fill:
            return L"Fill(填充)";
        case core::WallpaperPosition::Span:
            return L"Span(跨屏)";
    }

    return L"unknown";
}

bool CollectMonitors(const core::DesktopWallpaper& wallpaper, std::vector<core::Monitor>& monitors)
{
    monitors = wallpaper.Enumerate();
    if (monitors.empty())
    {
        LogError(L"没有枚举到任何显示器：当前会话可能不是交互式桌面，或者 shell 接口不可用");
        return false;
    }

    return true;
}

/// The window title shortened to something that fits on one console line.
std::wstring Shorten(const std::wstring& text, size_t limit)
{
    if (text.size() <= limit)
    {
        return text;
    }

    return text.substr(0, limit - 1) + L"…";
}

} // namespace

int RunMonitors()
{
    core::DesktopWallpaper wallpaper;
    if (!wallpaper.IsValid())
    {
        LogError(L"无法创建桌面壁纸 COM 对象");
        return kEnvironmentProblem;
    }

    std::vector<core::Monitor> monitors;
    if (!CollectMonitors(wallpaper, monitors))
    {
        return kEnvironmentProblem;
    }

    core::WallpaperPosition position{};
    if (wallpaper.GetPosition(position))
    {
        LogInfo(L"壁纸位置模式: {}", PositionName(position));
    }

    LogInfo(L"共 {} 台显示器:", monitors.size());
    for (const auto& monitor : monitors)
    {
        LogInfo(L"  ({:>5},{:>5})  {}x{}  {}", monitor.left, monitor.top, monitor.width,
            monitor.height, monitor.devicePath);
    }

    const core::Rect canvas = core::GetCanvas(monitors);
    LogInfo(L"虚拟桌面外接矩形: ({},{}) {}x{}", canvas.left, canvas.top, canvas.width, canvas.height);

    return kSuccess;
}

int RunDump(const std::filesystem::path& image)
{
    core::DesktopWallpaper wallpaper;
    if (!wallpaper.IsValid())
    {
        LogError(L"无法创建桌面壁纸 COM 对象");
        return kEnvironmentProblem;
    }

    std::vector<core::Monitor> monitors;
    if (!CollectMonitors(wallpaper, monitors))
    {
        return kEnvironmentProblem;
    }

    const auto size = core::QueryImageSize(image);
    if (!size.has_value())
    {
        LogError(L"无法读取图片尺寸: {}", image.wstring());
        return kRuntimeFailure;
    }

    const core::Rect canvas = core::GetCanvas(monitors);

    LogInfo(L"源图: {} ({}x{})", image.filename().wstring(), size->width, size->height);
    LogInfo(L"外接矩形: {}x{} @ ({},{})", canvas.width, canvas.height, canvas.left, canvas.top);

    const double scale = std::max(
        static_cast<double>(canvas.width) / size->width,
        static_cast<double>(canvas.height) / size->height);
    LogInfo(L"覆盖缩放比例: {:.4f}  ->  缩放后 {}x{}", scale,
        static_cast<int>(std::ceil(size->width * scale)),
        static_cast<int>(std::ceil(size->height * scale)));

    if (scale > 1.0)
    {
        LogWarn(L"源图小于外接矩形，将被放大 {:.1f}%，边缘可能变糊", (scale - 1.0) * 100.0);
    }

    bool allOk = true;
    for (const auto& monitor : monitors)
    {
        const auto slice = core::ComputeSlice(canvas, size->width, size->height, monitor);
        if (!slice.has_value())
        {
            LogError(L"  ({},{}) {}x{}  无法计算切片", monitor.left, monitor.top, monitor.width,
                monitor.height);
            allOk = false;
            continue;
        }

        LogInfo(
            L"  ({:>5},{:>5}) {:>5}x{:<5}  src ({},{}) {}x{}  canvas ({},{})  {}",
            monitor.left, monitor.top, monitor.width, monitor.height, slice->sourceX, slice->sourceY,
            slice->sourceWidth, slice->sourceHeight, slice->canvasX, slice->canvasY,
            slice->unscaled ? L"1:1 直拷" : L"需重采样");
    }

    return allOk ? kSuccess : kRuntimeFailure;
}

int RunSpan(const std::filesystem::path& image, bool dryRun)
{
    const ActionResult result = ApplySpanningWallpaper(image, dryRun);

    if (dryRun)
    {
        return result.ok ? kSuccess : kRuntimeFailure;
    }

    LogInfo(L"{}", result.message);
    return result.ok ? kSuccess : kRuntimeFailure;
}

int RunRestore()
{
    const ActionResult result = RestoreSavedWallpaper();
    LogInfo(L"{}", result.message);
    return result.ok ? kSuccess : kRuntimeFailure;
}

int RunWindows()
{
    const auto windows = core::EnumerateWindows();
    if (windows.empty())
    {
        LogWarn(L"没有找到任何可管理的窗口");
        return kSuccess;
    }

    LogInfo(L"共 {} 个可管理的窗口（按 z 序）:", windows.size());

    for (const auto& entry : windows)
    {
        const std::wstring monitor = core::GetMonitorPathForWindow(entry.handle);

        LogInfo(L"  {:>5},{:>5} {:>5}x{:<5}  {:<24} {}", entry.rect.left, entry.rect.top,
            entry.rect.right - entry.rect.left, entry.rect.bottom - entry.rect.top,
            Shorten(entry.appName, 24), Shorten(entry.title, 48));

        // The display each window belongs to is what makes per-display restore possible, so it
        // is printed on its own indented line rather than squeezed into the row above.
        LogInfo(L"        显示器: {}", monitor.empty() ? L"(未知)" : monitor);
    }

    return kSuccess;
}

int RunCapture()
{
    int windows = 0;
    int monitors = 0;
    const ActionResult result = CaptureCurrentLayout(windows, monitors);

    LogInfo(L"{}", result.message);
    if (result.ok)
    {
        LogInfo(L"布局文件: {}", core::DefaultLayoutFile().wstring());
    }

    return result.ok ? kSuccess : kRuntimeFailure;
}

int RunApply()
{
    core::RestoreStats stats{};
    const ActionResult result = RestoreSavedLayout(stats);

    LogInfo(L"{}", result.message);
    return result.ok ? kSuccess : kRuntimeFailure;
}

int RunShow()
{
    const std::filesystem::path file = core::DefaultLayoutFile();
    const auto layout = core::LoadLayout(file);
    if (!layout.has_value())
    {
        LogError(L"还没有保存过布局: {}", file.wstring());
        return kRuntimeFailure;
    }

    const LayoutSummary summary = DescribeSavedLayout();

    LogInfo(L"布局文件: {}", file.wstring());
    LogInfo(L"保存时间: {}", summary.savedAt.empty() ? L"(未知)" : summary.savedAt);
    LogInfo(L"{} 台显示器 / {} 个窗口", layout->monitors.size(), layout->windows.size());

    LogInfo(L"");
    LogInfo(L"显示器:");
    for (const auto& monitor : layout->monitors)
    {
        LogInfo(L"  ({:>5},{:>5}) {:>5}x{:<5}  {}", monitor.rect.left, monitor.rect.top,
            monitor.rect.width, monitor.rect.height, monitor.devicePath);
    }

    LogInfo(L"");
    LogInfo(L"窗口:");
    for (const auto& window : layout->windows)
    {
        LogInfo(L"  ({:>5},{:>5}) {:>5}x{:<5}  {:<24} {}", window.rect.left, window.rect.top,
            window.rect.width, window.rect.height, Shorten(window.appName, 24),
            Shorten(window.title, 48));
    }

    return kSuccess;
}

int RunTray(bool showPanel)
{
    if (!ClaimSingleInstance())
    {
        LogInfo(L"已有一个实例在运行，已把它叫到前台");
        return kSuccess;
    }

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    HICON icon = ::LoadIconW(instance, MAKEINTRESOURCEW(kIconResource));
    if (icon == nullptr)
    {
        LogWarn(L"应用图标资源缺失，回退到系统默认图标");
        icon = ::LoadIconW(nullptr, IDI_APPLICATION);
    }

    Service service(instance, icon);
    if (!service.Start(showPanel))
    {
        LogError(L"常驻服务启动失败");
        ::MessageBoxW(nullptr, L"托盘或设置面板创建失败，请查看日志。", L"WinTools",
            MB_ICONERROR | MB_OK);
        return kRuntimeFailure;
    }

    return service.Run();
}

} // namespace wintools::app
