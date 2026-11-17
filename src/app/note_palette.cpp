#include "app/note_palette.h"

#include <algorithm>

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

COLORREF Mix(COLORREF from, COLORREF to, int percentOfTo)
{
    const auto channel = [percentOfTo](int a, int b)
    { return (a * (100 - percentOfTo) + b * percentOfTo + 50) / 100; };

    return RGB(channel(GetRValue(from), GetRValue(to)), channel(GetGValue(from), GetGValue(to)),
        channel(GetBValue(from), GetBValue(to)));
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

    const int width = Scale(2 * kPadDesign + core::kNoteThemeCount * kSwatchDesign
        + (core::kNoteThemeCount - 1) * kGapDesign);
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
    for (int index = 0; index < core::kNoteThemeCount; ++index)
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
    RECT client{};
    ::GetClientRect(m_window, &client);

    // The chip is the note's own header tone rather than white: it is sitting on paper, and a white
    // slab on a warm note reads as a dialog that lost its way onto the desktop.
    const HBRUSH backdrop = ::CreateSolidBrush(m_backdrop);
    ::FillRect(dc, &client, backdrop);
    ::DeleteObject(backdrop);

    const HPEN edge = ::CreatePen(PS_SOLID, std::max(1, Scale(1)), Mix(m_backdrop, kRing, 18));
    const HGDIOBJ oldPen = ::SelectObject(dc, edge);
    const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, 0, 0, client.right, client.bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(edge);

    for (int index = 0; index < core::kNoteThemeCount; ++index)
    {
        const RECT box = SwatchRect(index);

        // The swatch is drawn as a disc, one pixel smaller than its box, so the ring around the
        // current paper has somewhere to go.
        const HBRUSH fill = ::CreateSolidBrush(ToRef(core::NoteThemeAt(index).paper));
        const HPEN ring = ::CreatePen(PS_SOLID, 1,
            index == m_current ? kRing
                               : ToRef(core::BlendColor(core::NoteThemeAt(index).paper, 0x37352F, 22)));

        const HGDIOBJ previousPen = ::SelectObject(dc, ring);
        const HGDIOBJ previousBrush = ::SelectObject(dc, fill);
        ::Ellipse(dc, box.left, box.top, box.right, box.bottom);
        ::SelectObject(dc, previousBrush);
        ::SelectObject(dc, previousPen);
        ::DeleteObject(ring);
        ::DeleteObject(fill);

        if (index == m_current || index == m_hover)
        {
            const HBRUSH hollow = static_cast<HBRUSH>(::GetStockObject(NULL_BRUSH));
            const HPEN halo = ::CreatePen(PS_SOLID, index == m_current ? 2 : 1,
                index == m_current ? kRing : ToRef(core::BlendColor(m_backdrop, 0x37352F, 40)));

            const HGDIOBJ previousPen2 = ::SelectObject(dc, halo);
            const HGDIOBJ previousBrush2 = ::SelectObject(dc, hollow);
            ::Ellipse(dc, box.left - Scale(2), box.top - Scale(2), box.right + Scale(2),
                box.bottom + Scale(2));
            ::SelectObject(dc, previousBrush2);
            ::SelectObject(dc, previousPen2);
            ::DeleteObject(halo);
        }
    }
}

} // namespace wintools::app
