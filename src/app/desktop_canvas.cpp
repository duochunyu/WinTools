#include "app/desktop_canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

#include "core/calendar_face.h"
#include "util/log.h"

namespace wintools::app
{

namespace
{

constexpr wchar_t kClassName[] = L"WinTools.Canvas.5f3c1b7e";

constexpr UINT_PTR kTickTimer = 1;

/// A sweeping second hand wants about ten frames a second, which is also roughly what a mechanical
/// watch beats at - the tip of a 50 px hand only travels five pixels a second, so anything faster
/// would be drawing sub-pixel movement nobody can see. A stepping hand only has to look at the
/// clock often enough not to be noticeably late.
constexpr UINT kSweepMs = 100;
constexpr UINT kStepMs = 200;

/// How often the z-order and obstruction state are re-evaluated. Both change at human speed, and
/// walking the window list is the expensive part of a tick, so it is not done per frame.
constexpr ULONGLONG kProbeMs = 400;

/// One instance, because a WinEvent hook carries no user data. The service owns exactly one
/// canvas, so this is a documented invariant rather than a shortcut.
DesktopCanvas* g_instance = nullptr;

/// Warm ink on a near-black disc. Deliberately not themed: a desktop medallion that changed colour
/// with the panel's theme would be a different product.
namespace palette
{
constexpr COLORREF kDisc = RGB(0x17, 0x18, 0x1C);
constexpr COLORREF kEdge = RGB(0x3A, 0x3B, 0x44);
constexpr COLORREF kTick = RGB(0x63, 0x60, 0x5A);
constexpr COLORREF kTickMajor = RGB(0xB4, 0x9C, 0x6C);
constexpr COLORREF kGold = RGB(0xC8, 0xA9, 0x6B);
constexpr COLORREF kDayInk = RGB(0xF7, 0xF3, 0xEA);
constexpr COLORREF kLunarInk = RGB(0xE6, 0xD6, 0xB0);
constexpr COLORREF kFootInk = RGB(0x8E, 0x89, 0x80);
} // namespace palette

/// Where the dial sits and how big it is, as fractions of the widget's side. The text bands
/// below are chosen against these numbers, so they live together.
namespace dial
{
constexpr double kCentreY = 0.262;
constexpr double kRadius = 0.240;

/// Hands start outside the yin-yang hub rather than at the centre, so the mark stays whole.
constexpr double kHub = 0.072;
} // namespace dial

BITMAPINFO SurfaceInfo(int size)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return info;
}

/// Blends one line segment into the surface, with analytic coverage over the last pixel.
///
/// GDI does not antialias lines, and on a dial this small a two-pixel staircase is the first
/// thing the eye catches. The disc's own edge is already computed from a distance field, so the
/// hands and the tick marks use the same idea: distance from the pixel centre to the segment,
/// turned into coverage. It also writes only RGB, which matters - GDI fills zero the alpha byte,
/// and by the time a hand is drawn that byte is what keeps the dial opaque.
void BlendSegment(std::uint32_t* pixels, int size, double x0, double y0, double x1, double y1,
    double halfWidth, COLORREF color)
{
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double lengthSq = dx * dx + dy * dy;
    const double reach = halfWidth + 1.0;

    const int left = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - reach)));
    const int right = std::min(size - 1, static_cast<int>(std::ceil(std::max(x0, x1) + reach)));
    const int top = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - reach)));
    const int bottom = std::min(size - 1, static_cast<int>(std::ceil(std::max(y0, y1) + reach)));

    const double red = GetRValue(color);
    const double green = GetGValue(color);
    const double blue = GetBValue(color);

    for (int y = top; y <= bottom; ++y)
    {
        for (int x = left; x <= right; ++x)
        {
            const double px = x + 0.5 - x0;
            const double py = y + 0.5 - y0;

            double t = 0.0;
            if (lengthSq > 0.0)
            {
                t = std::clamp((px * dx + py * dy) / lengthSq, 0.0, 1.0);
            }

            const double ex = px - dx * t;
            const double ey = py - dy * t;
            const double coverage =
                std::clamp(halfWidth + 0.5 - std::sqrt(ex * ex + ey * ey), 0.0, 1.0);
            if (coverage <= 0.0)
            {
                continue;
            }

            std::uint32_t& pixel = pixels[static_cast<size_t>(y) * size + x];
            const double keep = 1.0 - coverage;

            const auto channel = [&](std::uint32_t shift, double ink)
            {
                const auto from = static_cast<double>((pixel >> shift) & 0xFFu);
                return static_cast<std::uint32_t>(std::lround(ink * coverage + from * keep));
            };

            pixel = (pixel & 0xFF000000u) | (channel(16, red) << 16) | (channel(8, green) << 8)
                | channel(0, blue);
        }
    }
}

/// The dial's hour marks. Twelve of them, not sixty: at this size minute marks would be a grey
/// haze, and the minute hand already says what they would.
void DrawDialMarks(std::uint32_t* pixels, int size)
{
    const double centreX = size / 2.0;
    const double centreY = size * dial::kCentreY;
    const double radius = size * dial::kRadius;
    const double outer = radius - std::max(1.0, size * 0.008);

    for (int hour = 0; hour < 12; ++hour)
    {
        const double radians = hour * 6.283185307179586 / 12.0;
        const bool quarter = hour % 3 == 0;
        const double inner = outer - size * (quarter ? 0.040 : 0.024);
        const double halfWidth = std::max(0.55, size * (quarter ? 0.0042 : 0.0026));

        BlendSegment(pixels, size, centreX + std::sin(radians) * inner,
            centreY - std::cos(radians) * inner, centreX + std::sin(radians) * outer,
            centreY - std::cos(radians) * outer, halfWidth,
            quarter ? palette::kTickMajor : palette::kTick);
    }
}

/// Windows the shell puts above everything and that are not "something covering the widget":
/// the desktop host, its icon view, the taskbar, and the Windows 11 shell's own core window.
bool IsShellSurface(HWND window)
{
    wchar_t name[64]{};
    if (::GetClassNameW(window, name, static_cast<int>(std::size(name))) == 0)
    {
        return false;
    }

    static const wchar_t* const kNames[] = { L"Progman", L"WorkerW", L"SHELLDLL_DefView",
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow", kClassName };

    for (const wchar_t* candidate : kNames)
    {
        if (std::wcscmp(name, candidate) == 0)
        {
            return true;
        }
    }

    return false;
}

/// Re-asserts the widget's place whenever another window is activated.
///
/// The shell does not promise that a window which was just above the desktop stays there:
/// activating a window, an Alt+Tab, a new top-level window - any of them can end with the widget
/// pushed below the desktop, where it is simply gone. This hook is what turns "placed once" into
/// "always where it belongs".
void CALLBACK OnForegroundHook(
    HWINEVENTHOOK hook, DWORD event, HWND window, LONG object, LONG child, DWORD thread, DWORD time)
{
    (void)hook;
    (void)event;
    (void)window;
    (void)object;
    (void)child;
    (void)thread;
    (void)time;

    if (g_instance == nullptr)
    {
        return;
    }

    g_instance->Restack();
}

/// The window that hosts the desktop icons.
///
/// Windows 11 24H2 moved SHELLDLL_DefView from a WorkerW straight under Progman; before that the
/// icons live in a WorkerW sibling. Both are handled by looking for the view rather than by
/// testing a version number - the structure is what matters, and it is the only thing that is
/// actually observable.
HWND FindDesktopHost()
{
    const HWND shell = ::GetShellWindow();
    if (shell == nullptr)
    {
        return nullptr;
    }

    if (::FindWindowExW(shell, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr)
    {
        return shell;
    }

    HWND worker = nullptr;
    while ((worker = ::FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr)
    {
        if (::FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr)
        {
            return worker;
        }
    }

    return shell;
}

COLORREF Mix(COLORREF base, COLORREF tint, double amount)
{
    const auto blend = [amount](int from, int to)
    {
        return static_cast<int>(std::lround(from + (to - from) * amount));
    };

    return RGB(blend(GetRValue(base), GetRValue(tint)), blend(GetGValue(base), GetGValue(tint)),
        blend(GetBValue(base), GetBValue(tint)));
}

void FillCircle(HDC dc, int left, int top, int right, int bottom, COLORREF color)
{
    const HBRUSH brush = ::CreateSolidBrush(color);
    const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    const HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));

    ::Ellipse(dc, left, top, right, bottom);

    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

/// The yin-yang mark: a light disc, its left half dark, and the two half-size circles that turn
/// the straight split into an S.
void DrawTaiji(HDC dc, int centreX, int centreY, int radius, COLORREF light, COLORREF dark,
    COLORREF edge)
{
    const int left = centreX - radius;
    const int top = centreY - radius;
    const int right = centreX + radius;
    const int bottom = centreY + radius;

    FillCircle(dc, left, top, right, bottom, light);

    // Left half: the full disc again, clipped to the left half of its box.
    const HRGN half = ::CreateRectRgn(left, top, centreX, bottom);
    const int saved = ::SaveDC(dc);
    ::SelectClipRgn(dc, half);
    FillCircle(dc, left, top, right, bottom, dark);
    ::RestoreDC(dc, saved);
    ::DeleteObject(half);

    const int small = radius / 2;
    FillCircle(dc, centreX - small, centreY - radius, centreX + small, centreY, dark);
    FillCircle(dc, centreX - small, centreY, centreX + small, centreY + radius, light);

    const int dot = std::max(1, radius / 6);
    FillCircle(dc, centreX - dot, centreY - radius / 2 - dot, centreX + dot,
        centreY - radius / 2 + dot, light);
    FillCircle(dc, centreX - dot, centreY + radius / 2 - dot, centreX + dot,
        centreY + radius / 2 + dot, dark);

    const HPEN pen = ::CreatePen(PS_SOLID, 1, edge);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Ellipse(dc, left, top, right, bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void DrawCentred(HDC dc, HFONT font, COLORREF color, const RECT& box, const std::wstring& text)
{
    if (font == nullptr || text.empty())
    {
        return;
    }

    const HGDIOBJ oldFont = ::SelectObject(dc, font);
    ::SetTextColor(dc, color);
    ::SetBkMode(dc, TRANSPARENT);

    RECT target = box;
    ::DrawTextW(dc, text.c_str(), -1, &target,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    ::SelectObject(dc, oldFont);
}

} // namespace

DesktopCanvas::~DesktopCanvas()
{
    Stop();
}

bool DesktopCanvas::Start(HINSTANCE instance, const core::CanvasRect& rect)
{
    if (m_window != nullptr)
    {
        SetRect(rect);
        return true;
    }

    m_instance = instance;
    m_rect = rect;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &DesktopCanvas::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;

    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        util::LogError(L"cannot register the canvas window class");
        return false;
    }

    // LAYERED for per-pixel alpha, TOOLWINDOW so it never shows up in Alt+Tab or the taskbar,
    // NOACTIVATE so it cannot take focus, TRANSPARENT so clicks fall through to the desktop.
    constexpr DWORD kExStyle =
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;

    m_window = ::CreateWindowExW(kExStyle, kClassName, L"WinTools", WS_POPUP, rect.x, rect.y,
        rect.size, rect.size, nullptr, nullptr, instance, this);

    if (m_window == nullptr)
    {
        util::LogError(L"cannot create the canvas window");
        return false;
    }

    g_instance = this;

    m_hook = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        &OnForegroundHook, 0, 0, WINEVENT_OUTOFCONTEXT);

    ::SetTimer(m_window, kTickTimer, m_sweep ? kSweepMs : kStepMs, nullptr);

    // The service only creates the widget on request, so the caller usually has the desktop open.
    Restack();
    RenderAll();
    ::ShowWindow(m_window, SW_SHOWNOACTIVATE);

    util::LogInfo(L"desktop canvas started at ({}, {}) {}px", rect.x, rect.y, rect.size);
    return true;
}

void DesktopCanvas::Stop()
{
    if (m_window == nullptr)
    {
        return;
    }

    ::KillTimer(m_window, kTickTimer);

    if (m_hook != nullptr)
    {
        ::UnhookWinEvent(m_hook);
        m_hook = nullptr;
    }

    g_instance = nullptr;

    ::DestroyWindow(m_window);
    m_window = nullptr;

    DestroySurface();
    DestroyBase();
    DestroyFonts();

    util::LogInfo(L"desktop canvas stopped");
}

void DesktopCanvas::SetRect(const core::CanvasRect& rect)
{
    if (rect == m_rect)
    {
        return;
    }

    const bool resized = rect.size != m_rect.size;
    m_rect = rect;

    if (m_window == nullptr)
    {
        return;
    }

    ApplyBounds();

    if (resized)
    {
        // The surface, the cache and the fonts are all sized in pixels, so a resize invalidates
        // all three; RenderAll rebuilds whatever is missing.
        DestroySurface();
        DestroyBase();
        DestroyFonts();
        RenderAll();
        return;
    }

    // A move only changes where the frame lands, so the cached base is still good.
    RenderFrame();
}

void DesktopCanvas::SetSweep(bool sweep)
{
    if (m_sweep == sweep)
    {
        return;
    }

    m_sweep = sweep;

    if (m_window == nullptr)
    {
        return;
    }

    ::SetTimer(m_window, kTickTimer, m_sweep && !m_obstructed ? kSweepMs : kStepMs, nullptr);
    RenderFrame();
}

void DesktopCanvas::ApplyBounds()
{
    ::SetWindowPos(m_window, nullptr, m_rect.x, m_rect.y, m_rect.size, m_rect.size,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

void DesktopCanvas::Restack()
{
    if (m_window == nullptr)
    {
        return;
    }

    constexpr UINT kFlags =
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;

    // One rule, no state: sit directly above the desktop host. Everything the user opened is above
    // that, so the widget is covered by real windows and visible whenever the desktop is; and
    // "show desktop" needs no special case at all, because it takes the windows out of the way
    // rather than moving the desktop.
    //
    // The earlier version kept a "is the desktop raised" flag instead. It could not work: the flag
    // was re-derived from where the widget itself had just been placed, so the two readings
    // disagreed and the widget alternated between visible and hidden. Anything that has to be
    // idempotent has to be a position, not a memory.
    const HWND host = FindDesktopHost();
    const HWND above = host != nullptr ? ::GetWindow(host, GW_HWNDPREV) : nullptr;

    ::SetWindowPos(m_window, above != nullptr ? above : HWND_BOTTOM, 0, 0, 0, 0, kFlags);
}

void DesktopCanvas::CreateSurface()
{
    const int size = m_rect.size;
    if (size <= 0)
    {
        return;
    }

    BITMAPINFO info = SurfaceInfo(size);

    void* bits = nullptr;
    m_bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (m_bitmap == nullptr)
    {
        util::LogError(L"cannot create the canvas surface");
        return;
    }

    m_memory = ::CreateCompatibleDC(nullptr);
    m_oldBitmap = ::SelectObject(m_memory, m_bitmap);
    m_bits = bits;
    m_surface = size;
}

void DesktopCanvas::DestroySurface()
{
    if (m_memory != nullptr)
    {
        if (m_oldBitmap != nullptr)
        {
            ::SelectObject(m_memory, m_oldBitmap);
            m_oldBitmap = nullptr;
        }

        ::DeleteDC(m_memory);
        m_memory = nullptr;
    }

    if (m_bitmap != nullptr)
    {
        ::DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }

    m_bits = nullptr;
    m_surface = 0;
}

void DesktopCanvas::DestroyBase()
{
    if (m_baseBitmap != nullptr)
    {
        ::DeleteObject(m_baseBitmap);
        m_baseBitmap = nullptr;
    }

    m_baseBits = nullptr;
    m_base = 0;
}

void DesktopCanvas::RecreateFonts()
{
    const int size = m_surface;
    if (size <= 0 || m_fontSize == size)
    {
        return;
    }

    DestroyFonts();

    const auto make = [](int pixels, int weight)
    {
        return ::CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
            L"Microsoft YaHei UI");
    };

    m_fontHeadline = make(std::max(10, static_cast<int>(std::lround(size * 0.050))), FW_NORMAL);
    m_fontDay = make(std::max(18, static_cast<int>(std::lround(size * 0.182))), FW_SEMIBOLD);
    m_fontLunar = make(std::max(11, static_cast<int>(std::lround(size * 0.078))), FW_NORMAL);
    m_fontFoot = make(std::max(9, static_cast<int>(std::lround(size * 0.046))), FW_NORMAL);

    m_fontSize = size;
}

void DesktopCanvas::DestroyFonts()
{
    for (HFONT* font : { &m_fontHeadline, &m_fontDay, &m_fontLunar, &m_fontFoot })
    {
        if (*font != nullptr)
        {
            ::DeleteObject(*font);
            *font = nullptr;
        }
    }

    m_fontSize = 0;
}

void DesktopCanvas::CreateBase(const core::CalendarFace& face)
{
    DestroyBase();

    const int size = m_surface;
    if (m_bits == nullptr || size <= 0)
    {
        return;
    }

    std::memset(m_bits, 0, static_cast<size_t>(size) * size * 4);

    const int centre = size / 2;
    const auto hub = std::max(6, static_cast<int>(std::lround(size * dial::kHub * 0.80)));

    FillCircle(m_memory, 0, 0, size, size, palette::kDisc);

    {
        const HPEN pen = ::CreatePen(PS_SOLID, 1, palette::kEdge);
        const HGDIOBJ oldPen = ::SelectObject(m_memory, pen);
        const HGDIOBJ oldBrush = ::SelectObject(m_memory, ::GetStockObject(NULL_BRUSH));
        ::Ellipse(m_memory, 1, 1, size - 1, size - 1);
        ::SelectObject(m_memory, oldBrush);
        ::SelectObject(m_memory, oldPen);
        ::DeleteObject(pen);
    }

    // The mark is the dial's hub. Hands start outside it rather than at the centre, so the symbol
    // stays whole instead of being crossed twice a minute.
    DrawTaiji(m_memory, centre, static_cast<int>(std::lround(size * dial::kCentreY)), hub,
        Mix(palette::kDisc, RGB(0xFF, 0xFF, 0xFF), 0.90),
        Mix(palette::kDisc, RGB(0xFF, 0xFF, 0xFF), 0.13),
        Mix(palette::kDisc, RGB(0xFF, 0xFF, 0xFF), 0.30));

    DrawDialMarks(static_cast<std::uint32_t*>(m_bits), size);

    RecreateFonts();

    const auto band = [size](double from, double to)
    {
        return RECT{ 0, static_cast<LONG>(std::lround(size * from)),
            static_cast<LONG>(size), static_cast<LONG>(std::lround(size * to)) };
    };

    DrawCentred(m_memory, m_fontHeadline, palette::kGold, band(0.512, 0.588),
        core::FaceHeadline(face));
    DrawCentred(m_memory, m_fontDay, palette::kDayInk, band(0.554, 0.762),
        std::to_wstring(face.day));
    DrawCentred(m_memory, m_fontLunar, palette::kLunarInk, band(0.748, 0.834),
        core::FaceLunarLine(face));
    DrawCentred(m_memory, m_fontFoot, palette::kFootInk, band(0.834, 0.906),
        core::FaceFootline(face));

    // The disc is the only opaque thing here; everything outside its radius has to become
    // transparent, and GDI never writes an alpha channel, so it is computed afterwards from the
    // circle's own distance field. RGB is premultiplied on the way out because that is what
    // UpdateLayeredWindow expects - and because it means a frame is a memcpy, not another pass.
    auto* pixels = static_cast<std::uint32_t*>(m_bits);
    const double centreF = size / 2.0;
    const double radiusF = size / 2.0 - 0.5;

    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const double dx = x + 0.5 - centreF;
            const double dy = y + 0.5 - centreF;
            const double distance = std::sqrt(dx * dx + dy * dy);

            std::uint32_t& pixel = pixels[static_cast<size_t>(y) * size + x];

            const double coverage = std::clamp(radiusF - distance + 0.5, 0.0, 1.0);
            if (coverage <= 0.0)
            {
                pixel = 0;
                continue;
            }

            const auto alpha = static_cast<std::uint32_t>(coverage * 255.0 + 0.5);
            const std::uint32_t rb = ((pixel & 0x00FF00FFu) * alpha + 0x00800080u) >> 8;
            const std::uint32_t g = ((pixel & 0x0000FF00u) * alpha + 0x00008000u) >> 8;
            pixel = (alpha << 24) | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
        }
    }

    BITMAPINFO info = SurfaceInfo(size);
    m_baseBitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &m_baseBits, nullptr, 0);
    if (m_baseBitmap == nullptr)
    {
        util::LogError(L"cannot cache the canvas base");
        return;
    }

    std::memcpy(m_baseBits, m_bits, static_cast<size_t>(size) * size * 4);
    m_base = size;
}

void DesktopCanvas::RenderAll()
{
    if (m_window == nullptr)
    {
        return;
    }

    // Both bitmaps follow the widget's pixel size; they are built lazily so a resize does not have
    // to be tracked in two places.
    if (m_surface != m_rect.size)
    {
        DestroySurface();
        DestroyBase();
        CreateSurface();
    }

    if (m_bits == nullptr)
    {
        return;
    }

    SYSTEMTIME now{};
    ::GetLocalTime(&now);

    m_drawnYear = now.wYear;
    m_drawnMonth = now.wMonth;
    m_drawnDay = now.wDay;
    m_drawnHour = now.wHour;
    m_drawnMinute = now.wMinute;

    CreateBase(
        core::MakeFace(now.wYear, now.wMonth, now.wDay, now.wDayOfWeek, now.wHour, now.wMinute));

    RenderFrame();
}

void DesktopCanvas::RenderFrame()
{
    if (m_window == nullptr || m_bits == nullptr || m_base != m_surface)
    {
        return;
    }

    const int size = m_surface;
    std::memcpy(m_bits, m_baseBits, static_cast<size_t>(size) * size * 4);

    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    m_drawnSecond = now.wSecond;

    // A sweeping hand carries the fraction of a second; a stepping one drops it and lands on the
    // tick. Hours and minutes always advance continuously - a hand that only moved once a minute
    // would look frozen beside a sweeping second hand.
    const double second = m_sweep ? now.wSecond + now.wMilliseconds / 1000.0 : now.wSecond;
    const double minute = now.wMinute + second / 60.0;
    const double hour = now.wHour % 12 + minute / 60.0;

    auto* pixels = static_cast<std::uint32_t*>(m_bits);
    const double centreX = size / 2.0;
    const double centreY = size * dial::kCentreY;
    const double radius = size * dial::kRadius;
    const double hub = size * dial::kHub;

    const auto hand = [&](double turns, double length, double halfWidth, COLORREF color)
    {
        const double radians = turns * 6.283185307179586;
        const double sin = std::sin(radians);
        const double cos = std::cos(radians);

        BlendSegment(pixels, size, centreX + sin * hub, centreY - cos * hub,
            centreX + sin * length, centreY - cos * length, halfWidth, color);
    };

    hand(hour / 12.0, radius * 0.56, std::max(0.9, size * 0.0058), palette::kDayInk);
    hand(minute / 60.0, radius * 0.82, std::max(0.8, size * 0.0042), palette::kDayInk);

    // Thicker than the other two on purpose: it is the only hand that moves on its own, and a
    // hairline disappears into the antialiasing at this size.
    hand(second / 60.0, radius * 0.90, std::max(0.75, size * 0.0036), palette::kGold);

    POINT source{ 0, 0 };
    POINT target{ m_rect.x, m_rect.y };
    SIZE extent{ size, size };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    ::UpdateLayeredWindow(m_window, nullptr, &target, &extent, m_memory, &source, 0, &blend,
        ULW_ALPHA);
}

void DesktopCanvas::Tick()
{
    if (m_window == nullptr)
    {
        return;
    }

    const ULONGLONG stamp = ::GetTickCount64();
    if (stamp - m_probedAt >= kProbeMs)
    {
        m_probedAt = stamp;

        // A safety net under the foreground hook: activation is the only thing that can bury the
        // widget, but the hook is out-of-context and a missed event would leave it in the wrong
        // place. Re-asserting the same position costs nothing when it is already right.
        Restack();

        const bool obstructed = IsObstructed();
        if (obstructed != m_obstructed)
        {
            m_obstructed = obstructed;

            // Nobody is looking at a covered widget, so it steps instead of sweeping - and the
            // timer is re-armed rather than left waking ten times a second to do nothing.
            ::SetTimer(m_window, kTickTimer, m_sweep && !m_obstructed ? kSweepMs : kStepMs,
                nullptr);

            util::LogInfo(L"desktop canvas {}", m_obstructed ? L"steps (covered)" : L"sweeps");
        }
    }

    SYSTEMTIME now{};
    ::GetLocalTime(&now);

    const bool faceChanged = now.wMinute != m_drawnMinute || now.wHour != m_drawnHour
        || now.wDay != m_drawnDay || now.wMonth != m_drawnMonth || now.wYear != m_drawnYear;

    if (faceChanged)
    {
        RenderAll();
        return;
    }

    if ((m_sweep && !m_obstructed) || now.wSecond != m_drawnSecond)
    {
        RenderFrame();
    }
}

bool DesktopCanvas::IsObstructed() const
{
    if (m_window == nullptr)
    {
        return false;
    }

    const RECT self{ m_rect.x, m_rect.y, m_rect.x + m_rect.size, m_rect.y + m_rect.size };

    for (HWND above = ::GetWindow(m_window, GW_HWNDPREV); above != nullptr;
        above = ::GetWindow(above, GW_HWNDPREV))
    {
        if (!::IsWindowVisible(above) || IsShellSurface(above))
        {
            continue;
        }

        RECT other{};
        RECT overlap{};
        if (::GetWindowRect(above, &other) != FALSE
            && ::IntersectRect(&overlap, &self, &other) != FALSE)
        {
            return true;
        }
    }

    return false;
}

LRESULT CALLBACK DesktopCanvas::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<DesktopCanvas*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_window = window;
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    auto* self = reinterpret_cast<DesktopCanvas*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr)
    {
        return self->HandleMessage(message, wparam, lparam);
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT DesktopCanvas::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_TIMER:
            if (wparam == kTickTimer)
            {
                Tick();
                return 0;
            }
            break;

        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            ApplyBounds();
            Restack();
            RenderAll();
            return 0;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_NCHITTEST:
            // Belt and braces next to WS_EX_TRANSPARENT: a widget must never eat a desktop click.
            return HTTRANSPARENT;

        case WM_ERASEBKGND:
            return 1;

        case WM_DESTROY:
            m_window = nullptr;
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(m_window, message, wparam, lparam);
}

} // namespace wintools::app
