#include "app/note_palette.h"

#include <algorithm>

#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include "core/note_theme.h"
#include "util/log.h"

namespace wintools::app
{

namespace
{

constexpr wchar_t kClassName[] = L"WinTools.NotePalette.5b13c0aa";

constexpr int kSwatchDesign = 24;
constexpr int kGapDesign = 8;
constexpr int kPadDesign = 10;

/// How far the chip sits from the edge of the paper when the dot is too close to it.
constexpr int kEdgeDesign = 6;

constexpr COLORREF kRing = RGB(0x37, 0x35, 0x2F);

COLORREF ToRef(std::uint32_t rgb)
{
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

Gdiplus::Color ToGdiplus(COLORREF color)
{
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

/// Same one-time startup as the note window's copy: GDI+ is initialised process-wide and never
/// shut down, because the token has to outlive every window that draws with it.
bool GdiplusReady()
{
    static ULONG_PTR token = 0;
    static const bool ready = [] {
        Gdiplus::GdiplusStartupInput input;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return ready;
}

} // namespace

PalettePopup::~PalettePopup()
{
    Hide();
}

void PalettePopup::Show(
    HWND parent, int dpi, POINT anchor, COLORREF backdrop, int current, Picked picked)
{
    Hide();

    if (parent == nullptr)
    {
        return;
    }

    m_instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    m_dpi = dpi > 0 ? dpi : 96;
    m_current = core::ClampNoteTheme(current);
    m_hover = -1;
    m_backdrop = backdrop;
    m_picked = std::move(picked);

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &PalettePopup::WndProc;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;

        if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            util::LogError(L"note palette: cannot register the window class");
            return;
        }

        registered = true;
    }

    const int count = core::NoteThemeCount();
    const int width = Scale(2 * kPadDesign + count * kSwatchDesign + (count - 1) * kGapDesign);
    const int height = Scale(2 * kPadDesign + kSwatchDesign);

    RECT client{};
    ::GetClientRect(parent, &client);

    // Inside the paper, always: a child window that runs past its parent is not clipped cleanly on
    // every Windows version, and a chip half off the note looks like a bug even when it works.
    const int margin = Scale(kEdgeDesign);
    const int clientRight = static_cast<int>(client.right);
    const int clientBottom = static_cast<int>(client.bottom);

    int x = anchor.x;
    int y = anchor.y;

    x = std::clamp(x, margin, std::max(margin, clientRight - margin - width));
    if (y + height > clientBottom - margin)
    {
        // No room below the dot: the chip goes over it instead, hanging from the top edge.
        y = std::max(margin, static_cast<int>(anchor.y) - height - Scale(2 * kPadDesign));
    }

    m_window = ::CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, x, y, width,
        height, parent, nullptr, m_instance, this);

    if (m_window == nullptr)
    {
        util::LogError(L"note palette: cannot create the window");
        return;
    }

    m_dpi = ::GetDpiForWindow(m_window);
    if (m_dpi <= 0)
    {
        m_dpi = dpi > 0 ? dpi : 96;
    }

    // Above the body: the body is a sibling child window and would otherwise draw over the chip.
    ::SetWindowPos(m_window, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
    ::ShowWindow(m_window, SW_SHOWNA);
    ::InvalidateRect(m_window, nullptr, FALSE);
}

void PalettePopup::Hide()
{
    if (m_window == nullptr)
    {
        return;
    }

    HWND window = m_window;
    m_window = nullptr;

    // A child of a note that is being destroyed has already gone with it.
    if (::IsWindow(window) != FALSE)
    {
        ::DestroyWindow(window);
    }

    if (closed)
    {
        closed();
    }
}

bool PalettePopup::Contains(POINT parentClient) const
{
    if (m_window == nullptr)
    {
        return false;
    }

    RECT box{};
    ::GetWindowRect(m_window, &box);
    ::MapWindowPoints(HWND_DESKTOP, ::GetParent(m_window), reinterpret_cast<POINT*>(&box), 2);

    return parentClient.x >= box.left && parentClient.x < box.right && parentClient.y >= box.top
        && parentClient.y < box.bottom;
}

LRESULT CALLBACK PalettePopup::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<PalettePopup*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    auto* self = reinterpret_cast<PalettePopup*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr)
    {
        return self->HandleMessage(window, message, wparam, lparam);
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT PalettePopup::HandleMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_MOUSEACTIVATE:
            // Never steal the caret from the note. Picking a colour is not worth losing the place
            // the user was typing at.
            return MA_NOACTIVATE;

        case WM_LBUTTONDOWN:
            {
                const POINT here{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                const int picked = SwatchAt(here);

                // The callback runs before the window goes, because hiding it may destroy the note
                // that owns it.
                Picked action = m_picked;
                Hide();

                if (picked >= 0 && action)
                {
                    action(picked);
                }
            }
            return 0;

        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            Hide();
            return 0;

        case WM_MOUSEMOVE:
            {
                const POINT here{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                const int over = SwatchAt(here);
                if (over != m_hover)
                {
                    m_hover = over;
                    ::InvalidateRect(window, nullptr, FALSE);
                }
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            {
                PAINTSTRUCT paint{};
                const HDC dc = ::BeginPaint(window, &paint);
                Paint(dc);
                ::EndPaint(window, &paint);
            }
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

int PalettePopup::Scale(int value) const
{
    return ::MulDiv(value, m_dpi, 96);
}

RECT PalettePopup::SwatchRect(int index) const
{
    const int size = Scale(kSwatchDesign);
    const int left = Scale(kPadDesign) + index * (size + Scale(kGapDesign));

    RECT box{ left, Scale(kPadDesign), left + size, Scale(kPadDesign) + size };
    return box;
}

int PalettePopup::SwatchAt(POINT client) const
{
    for (int index = 0; index < core::NoteThemeCount(); ++index)
    {
        const RECT box = SwatchRect(index);
        if (client.x >= box.left && client.x < box.right && client.y >= box.top
            && client.y < box.bottom)
        {
            return index;
        }
    }

    return -1;
}

void PalettePopup::Paint(HDC dc) const
{
    if (!GdiplusReady())
    {
        return;
    }

    RECT client{};
    ::GetClientRect(m_window, &client);

    // The chip is the note's own paper and nothing else: no plate, no border. A framed slab on a
    // warm note reads as a dialog that lost its way onto the desktop; an unframed strip of paper
    // reads as part of the sheet, which is what swatches are.
    const HBRUSH backdrop = ::CreateSolidBrush(m_backdrop);
    ::FillRect(dc, &client, backdrop);
    ::DeleteObject(backdrop);

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    for (int index = 0; index < core::NoteThemeCount(); ++index)
    {
        const RECT box = SwatchRect(index);
        const float left = static_cast<float>(box.left);
        const float top = static_cast<float>(box.top);
        const float size = static_cast<float>(box.right - box.left);

        // The disc itself. Anti-aliasing is the whole point of drawing these through GDI+ rather
        // than ::Ellipse: a hard-edged circle at 24 px carries a visible staircase on every curve.
        Gdiplus::SolidBrush fill(ToGdiplus(ToRef(core::NoteThemeAt(index).paper)));
        graphics.FillEllipse(&fill, left, top, size, size);

        // The ring separates a swatch from papers close to its own tone. A 2px band on a 24px disc
        // read as a tyre around the colour rather than as a selection, so the current paper now
        // takes a 1.5px hairline drawn *inside* the disc - it marks the choice without eating the
        // swatch - and the rest keep a one-pixel edge that only separates neighbours.
        const bool current = index == m_current;
        const float ringWidth = current ? std::max(1.5F, static_cast<float>(Scale(1)) * 1.5F) : 1.0F;
        const float inset = current ? ringWidth / 2.0F : 0.0F;
        Gdiplus::Pen ring(ToGdiplus(current ? kRing
                                : ToRef(core::BlendColor(
                                      core::NoteThemeAt(index).paper, 0x37352F, 22))),
            ringWidth);
        graphics.DrawEllipse(&ring, left + inset, top + inset, size - inset * 2.0F,
            size - inset * 2.0F);

        // Hover keeps the old halo idea but drawn anti-aliased, one ring outside the disc.
        if (index == m_hover && !current)
        {
            const Gdiplus::Pen halo(
                ToGdiplus(ToRef(core::BlendColor(ToRef(m_backdrop), 0x37352F, 40))), 1.0F);
            const float outset = static_cast<float>(Scale(2));
            graphics.DrawEllipse(&halo, left - outset, top - outset, size + outset * 2.0F,
                size + outset * 2.0F);
        }
    }
}

} // namespace wintools::app
