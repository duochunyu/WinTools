#include "app/panel.h"

#include <Windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <objidl.h>   // IUnknown before gdiplus.h, or GdiplusImaging.h loses its base class
#include <gdiplus.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

#include "core/monitor.h"
#include "core/note_theme.h"
#include "util/i18n.h"
#include "util/log.h"

#include "app/features.h"

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

namespace wintools::app
{

using util::LogError;
using util::LogInfo;

namespace
{

// --- Geometry -------------------------------------------------------------------------------
//
// The sheet is a tabbed column, top to bottom:
//
//   title bar -> tab strip -> one page (layout | wallpaper | notes) -> footer
//
// Only the current page's geometry is filled; the other pages' rects stay empty, which is what
// keeps their controls out of both the paint and the hit test - an empty rectangle contains no
// point. The window's height follows the page, because the pages hold different amounts.
//
// Every number is a design unit - 96 dpi, the scale Windows draws at when nothing is enlarged.
// They are never adjusted for DPI: PaintContent scales the mapping mode once, so one number
// describes one distance at every scale factor.
//
// Heights are chained from a single cursor in ComputeGeometry rather than written as absolute
// positions, because a hand-numbered layout drifts apart the moment one band changes size - and
// then the paint code and the hit-test disagree in ways that are invisible until a click lands on
// the wrong control. SheetHeight is the same chain written as arithmetic, and a test holds the
// two against each other.

constexpr int kDesignDpi = 96;

/// Vertical extent of the measuring DC. Any value works - only the horizontal scale matters, and
/// only .cx is ever read - but a plain 1:1 vertical scale keeps the font metrics familiar.
constexpr int kMeasureHeight = 1000;

constexpr int kWidth = 820;

/// The panel opens at a 4:3 sheet and the user resizes it from there; the layout adapts to
/// whatever client size it is given instead of the window following the model.
constexpr int kDefaultHeight = 615;
constexpr int kMinWidth = 560;
constexpr int kMinHeight = 430;

constexpr int kTitleBarHeight = 36;
constexpr int kSheetPaddingX = 18;
constexpr int kSheetPaddingTop = 4;
constexpr int kSheetPaddingBottom = 12;

/// A block's first row: title, object picker, one primary, then the secondary actions.
constexpr int kHeadHeight = 32;

/// The hint row: the shortcut sentence on the left, read-only numbers on the right.
constexpr int kHintHeight = 18;
constexpr int kHintGapTop = 4;
constexpr int kHintGapBottom = 7;

constexpr int kMapHeight = 264;
constexpr int kMapHeightCandidates[] = { 264, 220, 176 };

constexpr int kRowHeight = 32;
constexpr int kListBorder = 1;

constexpr int kFooterGap = 11;
constexpr int kFooterPad = 10;
constexpr int kFooterHeight = 30;

constexpr int kButtonHeight = 30;
constexpr int kButtonPadX = 10;

/// The tab strip under the title bar: one page per tab, and only that page's geometry is filled.
/// Left-aligned, sized to their labels, the way a row of tabs normally reads - not a full-width
/// segmented control.
constexpr int kTabBarHeight = 26;

/// The gap between two tabs, the tabs' side padding, and between the strip and the page content.
constexpr int kTabGap = 8;
constexpr int kTabPadX = 14;
constexpr int kContentGapTop = 6;

/// The wallpaper page's right column, beside the map: the rotate button on top, then the
/// widgets - the calendar today, room for more later.
constexpr int kWallFloatInset = 10;

/// Blank band kept around the map projection so the two-headed turn handle can sit outside a
/// group corner without being clipped by the card edge.
constexpr int kArcMargin = 40;

/// The notes page: the head row's gap into the history card, and the card's own furniture.
constexpr int kNotesHeadGap = 2;
constexpr int kNotesCardGapTop = 3;
constexpr int kNotesHeadHeight = 22;
/// The list's meta row: same small type as the layout page's "N 个窗口" line above its list.
constexpr int kArchiveMetaHeight = 18;
constexpr int kNotesCardPadBottom = 8;

/// Closed notes shown per screenful in the panel. The card holds as many rows as the window
/// height allows; older notes wheel in from the archive file either way.
constexpr int kArchiveRowHeight = 30;

/// The config page. Two columns of sections, because ten rows in one column do not fit a sheet
/// that is 4:3 by design. Rows are 30 high with their control on the column's right edge, so the
/// labels form one line of text down each column and the controls another.
constexpr int kConfigRowHeight = 30;
constexpr int kConfigSectionHeight = 26;
constexpr int kConfigSectionGap = 12;
constexpr int kConfigColumnGap = 26;
constexpr int kConfigSegmentHeight = 22;
constexpr int kConfigSegmentPadX = 9;
constexpr int kConfigSwitchWidth = 30;
constexpr int kConfigSwitchHeight = 17;

/// Room for the widest combination anyone would bind - four modifiers and a long key name - so a
/// shortcut never reads as "Ctrl + Alt + Sh…".
constexpr int kConfigHotkeyValueWidth = 132;

/// The star in the closed-note list, the same amber the note's own header uses. Listed next to the
/// time rather than drawn over the paper dot, so the two flags never read as one.
constexpr COLORREF kStarGold = RGB(0xE8, 0xA3, 0x1C);

constexpr int kWindowChrome = 40; // caption plus borders, when the sheet is measured against a work area

/// Group outline geometry. The gap is outside the screen box, which is what keeps the outline off
/// the wallpaper; the two thicknesses share the outer edge so a selection change moves nothing.
constexpr int kFrameGap = 2;
constexpr int kFrameThin = 1;
constexpr int kFrameThick = 2;
constexpr int kFrameGlow = 2;

/// The number badge inside a screen's top-left corner.
constexpr int kBadgeMargin = 4;
constexpr int kBadgeRadius = 5;
constexpr int kBadgePaddingX = 5;
constexpr int kBadgeHeight = 16;
constexpr int kBadgeResolutionGap = 5;

/// The plate behind a screen number. A fixed brand blue, not the theme accent: the dark theme's
/// accent is a light blue that white text cannot sit on.
constexpr COLORREF kBadgeBlue = RGB(43, 108, 246);

/// Grid behind the screens: 20 units. Painted as a solid blended colour - a translucent line over
/// a solid background has exactly one visible result, and computing it once keeps the map free of
/// alpha.
constexpr int kGridStep = 20;

constexpr UINT kMsgActivate = WM_APP + 41;

// --- Palette --------------------------------------------------------------------------------

struct Theme
{
    COLORREF bg{};
    COLORREF card{};
    COLORREF card2{};
    COLORREF line{};
    COLORREF line2{};
    COLORREF hover{};
    COLORREF hover2{};
    COLORREF text{};
    COLORREF text2{};
    COLORREF text3{};
    COLORREF text4{};
    COLORREF accent{};
    COLORREF accent2{};
    COLORREF accentWeak{};
    COLORREF accentLine{};
    COLORREF danger{};
    COLORREF dangerWeak{};
    COLORREF ok{};
    COLORREF warn{};
    COLORREF group[6]{};
    COLORREF mapBg{};
    COLORREF mapGrid{};
    COLORREF monitorFlat{};
    bool dark{};
};

constexpr Theme kLight{
    RGB(255, 255, 255), // bg
    RGB(251, 251, 253), // card
    RGB(255, 255, 255), // card2
    RGB(229, 232, 238), // line
    RGB(216, 218, 224), // line2
    RGB(238, 241, 247), // hover
    RGB(232, 237, 246), // hover2
    RGB(24, 24, 28),    // text
    RGB(58, 60, 66),    // text2
    RGB(107, 110, 120), // text3
    RGB(138, 141, 151), // text4
    RGB(43, 108, 246),  // accent
    RGB(30, 90, 220),   // accent2
    RGB(234, 241, 255), // accentWeak
    RGB(181, 204, 255), // accentLine
    RGB(197, 48, 48),   // danger
    RGB(253, 236, 236), // dangerWeak
    RGB(29, 158, 117),  // ok
    RGB(168, 104, 11),  // warn
    { RGB(91, 127, 176), RGB(176, 133, 84), RGB(138, 124, 180), RGB(79, 150, 140),
        RGB(174, 113, 140), RGB(119, 144, 90) },
    RGB(255, 255, 255), // mapBg
    RGB(247, 248, 250), // mapGrid
    RGB(43, 49, 59),    // monitorFlat
    false,
};

constexpr Theme kDark{
    RGB(28, 30, 34),    // bg
    RGB(36, 39, 45),    // card
    RGB(28, 30, 34),    // card2
    RGB(51, 55, 62),    // line
    RGB(65, 70, 79),    // line2
    RGB(44, 49, 58),    // hover
    RGB(51, 58, 70),    // hover2
    RGB(233, 235, 239), // text
    RGB(195, 199, 207), // text2
    RGB(154, 160, 171), // text3
    RGB(124, 130, 141), // text4
    RGB(107, 155, 255), // accent
    RGB(138, 176, 255), // accent2
    RGB(29, 43, 71),    // accentWeak
    RGB(58, 86, 134),   // accentLine
    RGB(240, 131, 127), // danger
    RGB(58, 34, 36),    // dangerWeak
    RGB(93, 202, 165),  // ok
    RGB(227, 180, 91),  // warn
    { RGB(137, 169, 214), RGB(213, 173, 120), RGB(180, 164, 220), RGB(124, 196, 185),
        RGB(211, 156, 180), RGB(166, 196, 127) },
    RGB(23, 26, 31),    // mapBg
    RGB(31, 35, 41),    // mapGrid
    RGB(32, 36, 44),    // monitorFlat
    true,
};

COLORREF Mix(COLORREF a, COLORREF b, double weightA)
{
    const double t = std::clamp(weightA, 0.0, 1.0);
    const auto channel = [t](int from, int to) {
        return static_cast<int>(std::lround(from * t + to * (1.0 - t)));
    };

    return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)),
        channel(GetBValue(a), GetBValue(b)));
}

// --- Direct pixel work: alpha and antialiased corners ---------------------------------------
//
// GDI does not antialias: RoundRect walks its arc in whole pixels, and at 125% a seven-unit radius
// is a nine-pixel staircase that reads as a chipped corner. Two things in the design also have to
// blend over colours that are only known at paint time - the number badge sits on the wallpaper,
// and a modal dims the whole sheet.
//
// Both are solved on the panel's own 32bpp pixels. AlphaBlend is not an option: under a scaled
// mapping mode its coordinates are not something to bet a whole panel on. For the corners the
// straight parts still go through GDI (they are axis-aligned and need no antialiasing), and only
// the four corner squares are rasterised by hand - a card costs four nine-by-nine loops rather
// than a hundred thousand per-pixel tests.

int DeviceX(const PixelSurface& surface, int design)
{
    return static_cast<int>(std::lround(design * surface.scaleX));
}

int DeviceY(const PixelSurface& surface, int design)
{
    return static_cast<int>(std::lround(design * surface.scaleY));
}

[[nodiscard]] RECT ToDeviceRect(const PixelSurface& surface, const RECT& design)
{
    return RECT{ DeviceX(surface, design.left), DeviceY(surface, design.top),
        DeviceX(surface, design.right), DeviceY(surface, design.bottom) };
}

void BlendPixel(PixelSurface& surface, int x, int y, COLORREF color, double alpha)
{
    if (!surface.valid || x < 0 || y < 0 || x >= surface.width || y >= surface.height
        || alpha <= 0.0)
    {
        return;
    }

    const double a = std::clamp(alpha, 0.0, 1.0);
    std::uint32_t& pixel = surface.bits[static_cast<size_t>(y) * surface.width + x];

    const auto mix = [a](int from, int to) {
        return static_cast<std::uint32_t>(std::lround(from * a + to * (1.0 - a)));
    };

    const std::uint32_t blue = mix(GetBValue(color), static_cast<int>(pixel & 0xFF));
    const std::uint32_t green = mix(GetGValue(color), static_cast<int>((pixel >> 8) & 0xFF));
    const std::uint32_t red = mix(GetRValue(color), static_cast<int>((pixel >> 16) & 0xFF));

    pixel = 0xFF000000u | (red << 16) | (green << 8) | blue;
}

/// Opaque fill of a device-space rectangle: the straight parts of an antialiased shape.
void FillDeviceRect(PixelSurface& surface, const RECT& dev, COLORREF color)
{
    if (!surface.valid)
    {
        return;
    }

    const int left = std::max(0, static_cast<int>(dev.left));
    const int right = std::min(surface.width, static_cast<int>(dev.right));
    const int top = std::max(0, static_cast<int>(dev.top));
    const int bottom = std::min(surface.height, static_cast<int>(dev.bottom));
    if (left >= right || top >= bottom)
    {
        return;
    }

    const std::uint32_t pixel = 0xFF000000u | (static_cast<std::uint32_t>(GetRValue(color)) << 16)
        | (static_cast<std::uint32_t>(GetGValue(color)) << 8)
        | static_cast<std::uint32_t>(GetBValue(color));

    for (int y = top; y < bottom; ++y)
    {
        std::fill_n(
            surface.bits + static_cast<size_t>(y) * surface.width + left, right - left, pixel);
    }
}

/// Translucent fill of a device-space rectangle: the halo around a selected group.
///
/// A halo mixed into the board colour is only correct over the board. Over a picture it turns
/// into a second, pale line - which is exactly how it looked. This blends.
void BlendDeviceRect(PixelSurface& surface, const RECT& dev, COLORREF color, double alpha)
{
    if (!surface.valid || alpha <= 0.0)
    {
        return;
    }

    const int left = std::max(0, static_cast<int>(dev.left));
    const int right = std::min(surface.width, static_cast<int>(dev.right));
    const int top = std::max(0, static_cast<int>(dev.top));
    const int bottom = std::min(surface.height, static_cast<int>(dev.bottom));

    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            BlendPixel(surface, x, y, color, alpha);
        }
    }
}

/// Coverage of a pixel by a circle of the given radius: one inside, zero outside, one pixel of
/// feather on the edge.
///
/// The radius may be zero - that is the inner edge of a stroke whose thickness ate the whole
/// corner - and a zero-radius circle covers nothing, which is what the difference below needs.
[[nodiscard]] double CircleCoverage(
    double x, double y, double centreX, double centreY, double radius)
{
    const double distance = std::hypot(x - centreX, y - centreY);
    return std::clamp(0.5 + (radius - distance), 0.0, 1.0);
}

/// Walks the four corner squares of a device rectangle, handing each pixel its arc centre.
/// `roundBottom` is false for shapes that open into whatever sits below them - a tab attached to
/// the page under it has no bottom corners to round, and rounding them anyway would cut the very
/// join the shape exists to make.
template <typename PixelFn>
void ForEachCornerPixel(const RECT& dev, int radius, PixelFn&& pixel, bool roundBottom = true)
{
    if (radius <= 0)
    {
        return;
    }

    const int startX[2] = { dev.left, dev.right - radius };
    const int startY[2] = { dev.top, dev.bottom - radius };
    const double centreX[2] = { static_cast<double>(dev.left + radius),
        static_cast<double>(dev.right - radius) };
    const double centreY[2] = { static_cast<double>(dev.top + radius),
        static_cast<double>(dev.bottom - radius) };

    for (int row = 0; row < (roundBottom ? 2 : 1); ++row)
    {
        for (int column = 0; column < 2; ++column)
        {
            for (int y = 0; y < radius; ++y)
            {
                for (int x = 0; x < radius; ++x)
                {
                    pixel(startX[column] + x, startY[row] + y, centreX[column], centreY[row]);
                }
            }
        }
    }
}

/// The surface a paint pass is writing into, plus its DC. Null outside PaintContent.
struct PaintTarget
{
    HDC dc{};
    PixelSurface surface{};
};

PaintTarget* g_paintTarget = nullptr;

void RasterFillRounded(
    PaintTarget& target, const RECT& design, int radius, COLORREF color, bool openBottom)
{
    PixelSurface& surface = target.surface;
    const RECT dev = ToDeviceRect(surface, design);
    const int width = dev.right - dev.left;
    const int height = dev.bottom - dev.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    const int corner = std::clamp(
        static_cast<int>(std::lround(radius * surface.scaleX)), 0, std::min(width, height) / 2);

    if (corner <= 0)
    {
        FillDeviceRect(surface, dev, color);
        return;
    }

    // One vertical band plus two side bands, leaving exactly the four corner squares over. An open
    // bottom has no corner squares there to leave over: the side bands run all the way down.
    const int sideBottom = openBottom ? dev.bottom : dev.bottom - corner;
    FillDeviceRect(
        surface, RECT{ dev.left + corner, dev.top, dev.right - corner, dev.bottom }, color);
    FillDeviceRect(
        surface, RECT{ dev.left, dev.top + corner, dev.left + corner, sideBottom }, color);
    FillDeviceRect(
        surface, RECT{ dev.right - corner, dev.top + corner, dev.right, sideBottom }, color);

    ForEachCornerPixel(dev, corner,
        [&](int x, int y, double cx, double cy)
        {
            BlendPixel(surface, x, y, color, CircleCoverage(x + 0.5, y + 0.5, cx, cy, corner));
        },
        !openBottom);
}

void RasterStrokeRounded(PaintTarget& target, const RECT& design, int radius, COLORREF color,
    int designWidth, bool openBottom)
{
    PixelSurface& surface = target.surface;
    const RECT dev = ToDeviceRect(surface, design);
    const int width = dev.right - dev.left;
    const int height = dev.bottom - dev.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    // A bare stroke used to be a cosmetic pen: one device pixel at any scale factor. An explicit
    // width scales with the sheet, because it is part of the design rather than a hairline.
    const int thickness = designWidth > 0
        ? std::max(1, static_cast<int>(std::lround(designWidth * surface.scaleX)))
        : 1;

    if (thickness * 2 >= width || thickness * 2 >= height)
    {
        FillDeviceRect(surface, dev, color);
        return;
    }

    const int corner = std::clamp(
        static_cast<int>(std::lround(radius * surface.scaleX)), 0, std::min(width, height) / 2);

    if (corner <= 0)
    {
        FillDeviceRect(surface, RECT{ dev.left, dev.top, dev.right, dev.top + thickness }, color);
        FillDeviceRect(
            surface, RECT{ dev.left, dev.bottom - thickness, dev.right, dev.bottom }, color);
        FillDeviceRect(surface, RECT{ dev.left, dev.top, dev.left + thickness, dev.bottom }, color);
        FillDeviceRect(
            surface, RECT{ dev.right - thickness, dev.top, dev.right, dev.bottom }, color);
        return;
    }

    // Straight runs, one band per edge, each sitting just inside the outline. An open bottom has no
    // bottom run at all: the two side runs carry on to the edge so the outline meets whatever the
    // shape is joined to.
    FillDeviceRect(
        surface, RECT{ dev.left + corner, dev.top, dev.right - corner, dev.top + thickness },
        color);
    if (!openBottom)
    {
        FillDeviceRect(surface,
            RECT{ dev.left + corner, dev.bottom - thickness, dev.right - corner, dev.bottom },
            color);
    }
    const int sideBottom = openBottom ? dev.bottom : dev.bottom - corner;
    FillDeviceRect(
        surface, RECT{ dev.left, dev.top + corner, dev.left + thickness, sideBottom }, color);
    FillDeviceRect(
        surface, RECT{ dev.right - thickness, dev.top + corner, dev.right, sideBottom }, color);

    // Corners: a band of `thickness` centred on the arc that runs half a thickness inside the
    // outline. Measured as a distance to that centreline rather than as "outer circle minus inner
    // circle" - subtracting two feathered discs eats half a pixel at each edge, and a one-pixel
    // border on a light fill then reads as a dotted line instead of a line.
    const double centreline = std::max(0.0, corner - thickness / 2.0);
    const double halfBand = thickness / 2.0;

    ForEachCornerPixel(dev, corner,
        [&](int x, int y, double cx, double cy)
        {
            const double distance = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const double coverage =
                std::clamp(halfBand + 0.5 - std::abs(distance - centreline), 0.0, 1.0);
            BlendPixel(surface, x, y, color, coverage);
        },
        !openBottom);
}

// --- Drawing --------------------------------------------------------------------------------

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF color, bool openBottom = false)
{
    if (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0)
    {
        return;
    }

    if (g_paintTarget != nullptr && g_paintTarget->surface.valid)
    {
        RasterFillRounded(*g_paintTarget, rect, radius, color, openBottom);
        return;
    }

    // Fallback for a DC with no sheet behind it. Nothing in the panel paints that way today; the
    // path exists so this function cannot silently do nothing if something ever does.
    const HBRUSH brush = ::CreateSolidBrush(color);
    const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    const HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));

    if (openBottom)
    {
        // No rounded corner can open, so the only honest square-cornered version of it is a plain
        // rectangle. GDI has no way to round two corners and stop.
        ::FillRect(dc, &rect, brush);
    }
    else
    {
        // RoundRect takes the full width and height of the corner ellipse, not its radius.
        ::RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    }

    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

void StrokeRounded(HDC dc, const RECT& rect, int radius, COLORREF color, int width = 0,
    bool openBottom = false)
{
    if (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0)
    {
        return;
    }

    if (g_paintTarget != nullptr && g_paintTarget->surface.valid)
    {
        RasterStrokeRounded(*g_paintTarget, rect, radius, color, width, openBottom);
        return;
    }

    // Zero is a cosmetic pen: one device pixel whatever the scale factor. As a design unit it
    // would be 1.5 pixels at 150%, which GDI renders as a two-pixel smear on one edge and one on
    // the other - a border that looks thicker than the card it outlines.
    const HPEN pen = ::CreatePen(PS_SOLID, width, color);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    if (openBottom)
    {
        // Three sides, square: GDI cannot stroke a rectangle with two rounded corners and one
        // missing edge, and the raster path above is the one that actually runs.
        ::MoveToEx(dc, rect.left, rect.bottom, nullptr);
        ::LineTo(dc, rect.left, rect.top);
        ::LineTo(dc, rect.right - 1, rect.top);
        ::LineTo(dc, rect.right - 1, rect.bottom);
    }
    else
    {
        // Inset by one design unit: RoundRect strokes on the outline, so an un-inset card looks
        // half a pixel larger than its fill and the border sits outside the card.
        ::RoundRect(
            dc, rect.left, rect.top, rect.right - 1, rect.bottom - 1, radius * 2, radius * 2);
    }

    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void FillBox(HDC dc, const RECT& rect, COLORREF color)
{
    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return;
    }

    const HBRUSH brush = ::CreateSolidBrush(color);
    ::FillRect(dc, &rect, brush);
    ::DeleteObject(brush);
}

/// Square outline, used where two screens meet.
void StrokeBox(HDC dc, const RECT& rect, COLORREF color, int width = 0)
{
    const HPEN pen = ::CreatePen(PS_SOLID, width, color);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);

    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

/// Disc fill. The calendar widget is round, and a rounded rectangle would read as a different
/// thing at the scale the map draws it.
void FillCircle(HDC dc, const RECT& box, COLORREF color)
{
    if (box.right <= box.left || box.bottom <= box.top)
    {
        return;
    }

    const HBRUSH brush = ::CreateSolidBrush(color);
    const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    const HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));

    ::Ellipse(dc, box.left, box.top, box.right, box.bottom);

    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

void StrokeCircle(HDC dc, const RECT& box, COLORREF color, int width = 1)
{
    if (box.right <= box.left || box.bottom <= box.top)
    {
        return;
    }

    const HPEN pen = ::CreatePen(PS_SOLID, width, color);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    ::Ellipse(dc, box.left, box.top, box.right, box.bottom);

    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}
/// Clips rect to bounds, both in client space.
[[nodiscard]] RECT Clip(const RECT& rect, const RECT& bounds)
{
    RECT result{ std::max(rect.left, bounds.left), std::max(rect.top, bounds.top),
        std::min(rect.right, bounds.right), std::min(rect.bottom, bounds.bottom) };
    if (result.right < result.left)
    {
        result.right = result.left;
    }
    if (result.bottom < result.top)
    {
        result.bottom = result.top;
    }
    return result;
}

void DrawTextBlock(
    HDC dc, HFONT font, COLORREF color, const RECT& rect, std::wstring_view text, UINT flags)
{
    if (text.empty() || rect.right <= rect.left)
    {
        return;
    }

    // Chinese is the canonical source string. Both the draw and the measurement resolve it
    // through the same table, so a label is never measured in one language and drawn in another.
    text = util::i18n::Text(text);

    const HGDIOBJ oldFont = ::SelectObject(dc, font);
    const COLORREF oldColor = ::SetTextColor(dc, color);
    const int oldMode = ::SetBkMode(dc, TRANSPARENT);

    RECT target = rect;
    ::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &target,
        flags | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);

    ::SetBkMode(dc, oldMode);
    ::SetTextColor(dc, oldColor);
    ::SelectObject(dc, oldFont);
}

/// Text without the vertical centring, for the places where the baseline itself is the design -
/// the number badge mixes a 14 px figure with a 9 px caption on one baseline.
void DrawTextAt(
    HDC dc, HFONT font, COLORREF color, const RECT& rect, std::wstring_view text, UINT flags)
{
    if (text.empty() || rect.right <= rect.left)
    {
        return;
    }

    // Chinese is the canonical source string. Both the draw and the measurement resolve it
    // through the same table, so a label is never measured in one language and drawn in another.
    text = util::i18n::Text(text);

    const HGDIOBJ oldFont = ::SelectObject(dc, font);
    const COLORREF oldColor = ::SetTextColor(dc, color);
    const int oldMode = ::SetBkMode(dc, TRANSPARENT);

    RECT target = rect;
    ::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &target,
        flags | DT_SINGLELINE | DT_NOPREFIX);

    ::SetBkMode(dc, oldMode);
    ::SetTextColor(dc, oldColor);
    ::SelectObject(dc, oldFont);
}

/// Put one line on a shared baseline, whatever its font size. DT_VCENTER - what DrawTextBlock
/// uses everywhere else - centres each font's own line box, so a 11.5 px tagline lands a pixel
/// below a 12.5 px name and the pair reads as "not aligned at the bottom" without anyone being
/// able to say which glyph moved.
int TextBaseline(HDC dc, HFONT font, int boxHeight)
{
    const HGDIOBJ oldFont = ::SelectObject(dc, font);
    TEXTMETRICW metrics{};
    ::GetTextMetricsW(dc, &metrics);
    ::SelectObject(dc, oldFont);

    return (boxHeight - metrics.tmAscent - metrics.tmDescent) / 2 + metrics.tmAscent;
}

void DrawTextOnBaseline(HDC dc, HFONT font, COLORREF color, const RECT& box, int baseline,
    std::wstring_view text)
{
    const HGDIOBJ oldFont = ::SelectObject(dc, font);
    TEXTMETRICW metrics{};
    ::GetTextMetricsW(dc, &metrics);
    ::SelectObject(dc, oldFont);

    DrawTextAt(dc, font, color,
        RECT{ box.left, baseline - metrics.tmAscent, box.right, baseline + metrics.tmDescent + 4 },
        text, DT_LEFT);
}

SIZE MeasureText(HDC dc, HFONT font, std::wstring_view text)
{
    SIZE size{};
    if (text.empty())
    {
        return size;
    }

    text = util::i18n::Text(text);
    const HGDIOBJ oldFont = ::SelectObject(dc, font);
    ::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
    ::SelectObject(dc, oldFont);
    return size;
}

bool Contains(const RECT& rect, POINT point)
{
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top
        && point.y < rect.bottom;
}

RECT Inflate(RECT rect, int by)
{
    return RECT{ rect.left - by, rect.top - by, rect.right + by, rect.bottom + by };
}

// --- Software alpha -------------------------------------------------------------------------
//
// Two things in this design sit on top of something whose colour is not known when the code is
// written: the number badge sits on the wallpaper, and a modal dims the whole sheet. GDI cannot
// blend a rounded, translucent shape, and AlphaBlend under a scaled mapping mode is not something
// to bet a whole panel on. The sheet is already a 32bpp DIB in this process, so the blend happens
// on the pixels directly: exact, and independent of how GDI interprets a transform.

/// Fills a rounded rectangle with a translucent colour, antialiasing the corners.
void BlendRounded(
    PixelSurface& surface, const RECT& rect, int radius, COLORREF color, double alpha)
{
    if (!surface.valid || rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return;
    }

    const int left = DeviceX(surface, rect.left);
    const int top = DeviceY(surface, rect.top);
    const int right = DeviceX(surface, rect.right);
    const int bottom = DeviceY(surface, rect.bottom);
    const double corner = std::max(0.0, radius * surface.scaleX);

    const double centreX = (left + right) / 2.0;
    const double centreY = (top + bottom) / 2.0;
    const double halfW = (right - left) / 2.0;
    const double halfH = (bottom - top) / 2.0;

    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            // Distance to the rounded box, in device pixels. Negative inside.
            const double dx = std::abs(x + 0.5 - centreX) - (halfW - corner);
            const double dy = std::abs(y + 0.5 - centreY) - (halfH - corner);
            const double outside = std::hypot(std::max(dx, 0.0), std::max(dy, 0.0));
            const double inside = std::min(std::max(dx, dy), 0.0);
            const double distance = outside + inside - corner;

            // One device pixel of feathering is what makes the corner read as round rather than
            // stepped, and it is the only antialiasing GDI is not doing for us here.
            const double coverage = std::clamp(0.5 - distance, 0.0, 1.0);
            BlendPixel(surface, x, y, color, alpha * coverage);
        }
    }
}

/// Dims the whole sheet, for the layer behind a modal card.
void BlendAll(PixelSurface& surface, COLORREF color, double alpha)
{
    if (!surface.valid)
    {
        return;
    }

    for (int y = 0; y < surface.height; ++y)
    {
        for (int x = 0; x < surface.width; ++x)
        {
            BlendPixel(surface, x, y, color, alpha);
        }
    }
}

/// A 10 px ring with a third of it lit, for a button that is working.
void DrawSpinner(HDC dc, const RECT& rect, COLORREF ink, COLORREF trail, int angle)
{
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    const int radius = 5;

    const HPEN faint = ::CreatePen(PS_SOLID, 2, trail);
    const HGDIOBJ oldPen = ::SelectObject(dc, faint);
    ::Arc(dc, cx - radius, cy - radius, cx + radius, cy + radius, cx + radius, cy, cx + radius, cy);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(faint);

    const HPEN pen = ::CreatePen(PS_SOLID, 2, ink);
    ::SelectObject(dc, pen);
    ::MoveToEx(dc, cx + radius, cy, nullptr);
    ::AngleArc(dc, cx, cy, radius, static_cast<FLOAT>(angle), 110.0f);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

/// Width of one button, from its label. Every button in the panel comes through here, which is
/// what keeps the footer's 退出 the same size as the title row's 更换壁纸 (I6).
int ButtonWidth(HDC measure, HFONT font, std::wstring_view text)
{
    return std::max(56, static_cast<int>(MeasureText(measure, font, text).cx) + 2 * kButtonPadX);
}

/// The single button renderer. No variant changes the padding or the font size; the only
/// differences are fill, border and colour. The caption is passed in - including a busy label -
/// so there is exactly one source for every button's text in the panel.
void DrawButton(HDC dc, HDC measure, const Theme& t, HFONT font, const RECT& rect,
    std::wstring_view text, bool primary, bool hot, bool pressed, bool disabled, bool spinner,
    int spinAngle)
{
    // A disabled primary stays blue and gets lighter, rather than turning into an outlined
    // button: the variant says which action is the main one, and a busy state must not also move
    // the visual weight of the row.
    COLORREF fill = t.card2;

    if (primary)
    {
        fill = (hot || pressed) ? t.accent2 : t.accent;
        if (disabled)
        {
            fill = Mix(fill, t.bg, 0.35);
        }
        FillRounded(dc, rect, 7, fill);
    }
    else
    {
        FillRounded(dc, rect, 7, disabled ? t.card2 : (pressed ? t.hover2 : (hot ? t.hover : t.card2)));
        StrokeRounded(dc, rect, 7,
            disabled ? Mix(t.line2, t.bg, 0.55) : (hot ? t.accentLine : t.line2));
    }

    const COLORREF ink = primary ? (disabled ? Mix(RGB(255, 255, 255), fill, 0.8) : RGB(255, 255, 255))
                                 : (disabled ? t.text4 : t.text2);

    const SIZE textSize = MeasureText(measure, font, text);
    const int spinnerWidth = spinner ? 16 : 0;
    const int total = static_cast<int>(textSize.cx) + spinnerWidth;
    const int startX = rect.left + ((rect.right - rect.left) - total) / 2;
    const int centreY = (rect.top + rect.bottom) / 2;

    if (spinner)
    {
        const RECT ring{ startX, centreY - 5, startX + 10, centreY + 5 };
        DrawSpinner(dc, ring, ink, t.dark ? RGB(96, 102, 112) : RGB(198, 202, 212), spinAngle);
    }

    DrawTextBlock(dc, font, ink,
        RECT{ startX + spinnerWidth, rect.top, rect.right, rect.bottom }, text, DT_LEFT);
}

} // namespace

// --- Projection -----------------------------------------------------------------------------

RECT Panel::Projection::ToClient(const core::Rect& rect) const
{
    if (!canvas.IsValid())
    {
        return RECT{};
    }

    // Each edge is rounded on its own, not "position + rounded size": two displays that touch in
    // virtual space then still touch on screen.
    const auto mapX = [this](int value) {
        return drawn.left + static_cast<int>(std::lround((value - canvas.left) * scale));
    };
    const auto mapY = [this](int value) {
        return drawn.top + static_cast<int>(std::lround((value - canvas.top) * scale));
    };

    return RECT{ mapX(rect.left), mapY(rect.top), mapX(rect.Right()), mapY(rect.Bottom()) };
}

Panel::Projection Panel::Fit(const std::vector<core::MonitorSnapshot>& monitors, const RECT& area)
{
    Projection projection;
    projection.area = area;

    if (monitors.empty())
    {
        return projection;
    }

    int left = INT_MAX;
    int top = INT_MAX;
    int right = INT_MIN;
    int bottom = INT_MIN;

    for (const auto& monitor : monitors)
    {
        left = std::min(left, monitor.rect.left);
        top = std::min(top, monitor.rect.top);
        right = std::max(right, monitor.rect.Right());
        bottom = std::max(bottom, monitor.rect.Bottom());
    }

    projection.canvas = core::Rect{ left, top, right - left, bottom - top };

    const int areaWidth = area.right - area.left;
    const int areaHeight = area.bottom - area.top;
    if (!projection.canvas.IsValid() || areaWidth <= 0 || areaHeight <= 0)
    {
        return projection;
    }

    projection.scale = std::min(static_cast<double>(areaWidth) / projection.canvas.width,
        static_cast<double>(areaHeight) / projection.canvas.height);

    const int drawnWidth =
        std::max(1, static_cast<int>(std::lround(projection.canvas.width * projection.scale)));
    const int drawnHeight =
        std::max(1, static_cast<int>(std::lround(projection.canvas.height * projection.scale)));

    const int x = area.left + (areaWidth - drawnWidth) / 2;
    const int y = area.top + (areaHeight - drawnHeight) / 2;
    projection.drawn = RECT{ x, y, x + drawnWidth, y + drawnHeight };

    return projection;
}

// --- Framing --------------------------------------------------------------------------------

namespace
{

/// One wheel notch zooms by 10%. 25% made the picture jump between two framings instead of
/// gliding, and at a 264-unit map there is not much room between them.
constexpr double kZoomStep = 1.10;

/// A single event is not allowed to zoom by more than this many notches.
constexpr int kMaxWheelNotches = 10;
constexpr int kMaxWheelDelta = kMaxWheelNotches * WHEEL_DELTA;

} // namespace

double Panel::ZoomFactor(int wheelDelta)
{
    // Fractional notches, not integer ones: a classic mouse sends 120 units per notch, a precision
    // touchpad 15..40. Integer division dropped everything under a full notch, so slow scrolling
    // did nothing at all and fast scrolling jumped.
    const int delta = std::clamp(wheelDelta, -kMaxWheelDelta, kMaxWheelDelta);
    return std::pow(kZoomStep, static_cast<double>(delta) / WHEEL_DELTA);
}

Panel::MapFrame Panel::FrameFor(const MapContext& context, const core::WallpaperView& view)
{
    MapFrame frame;
    if (!context.IsValid())
    {
        return frame;
    }

    const core::WallpaperView clamped = core::ClampView(view);
    frame.surface =
        core::ComputeSlack(context.canvas, context.source.width, context.source.height, clamped);
    frame.scale = context.scale;
    frame.zoom = clamped.zoom;
    return frame;
}

SIZE Panel::PreviewSize(const MapContext& context)
{
    SIZE size{};

    if (!context.IsValid())
    {
        return size;
    }

    const core::ViewSlack surface = core::ComputeSlack(context.canvas, context.source.width,
        context.source.height, core::WallpaperView{ 1.0, 0.5, 0.5, context.rotation });

    const double factor = context.scale * kMapSupersample;
    size.cx = std::max(1, static_cast<int>(std::lround(surface.scaledWidth * factor)));
    size.cy = std::max(1, static_cast<int>(std::lround(surface.scaledHeight * factor)));
    return size;
}

double Panel::TurnAngle(double start, double delta, int& snap)
{
    // One degree is finer than the eye can see on the map, and coarse enough that a jittering
    // cursor does not re-decode the picture on every mouse message.
    double wanted = std::round(start + delta);

    // Two and a half degrees of capture is "looks square, is square": an angle is easy to land on
    // by eye, and the band is wide enough that a deliberate 89 does not stay 89. Six degrees of
    // release is what stops a drag from sticking at a right angle it has to cross.
    constexpr double kSnapCapture = 2.5;
    constexpr double kSnapRelease = 6.0;

    const double axis = std::round(wanted / 90.0) * 90.0;
    const double distance = std::abs(wanted - axis);

    if (distance <= kSnapCapture)
    {
        snap = static_cast<int>(std::lround(axis)) % 360;
        wanted = axis;
    }
    else if (snap >= 0 && distance <= kSnapRelease)
    {
        wanted = axis;
    }
    else
    {
        snap = -1;
    }

    // ClampView would fold this anyway, but the readout and the settings file both read the stored
    // value, and neither "370 degrees" nor "-0.0000" is an angle anyone wants to see.
    double normalized = std::fmod(wanted, 360.0);
    if (normalized < 0.0)
    {
        normalized += 360.0;
    }

    return normalized == 0.0 ? 0.0 : normalized;
}

core::WallpaperView Panel::PanView(
    const core::WallpaperView& view, const MapFrame& frame, int dx, int dy)
{
    core::WallpaperView result = view;
    const core::ViewSlack& fit = frame.surface;

    // The drag arrives on the canvas's axes, but the picture may be turned: splitting the vector
    // along the picture's own axes is what lets a turned picture be pushed in any direction. The
    // travel it has left is a box in that same frame, so each half clamps on its own and the
    // diagonal keeps going after the canvas axes would have stopped.
    const double along = dx * fit.cosine + dy * fit.sine;
    const double across = -dx * fit.sine + dy * fit.cosine;

    // Map units to canvas pixels, then canvas pixels to focus: the focus spans the whole travel,
    // which is twice the half-extent - or the plain slack where the turn is a right angle.
    const double spanX = fit.turned ? 2.0 * fit.halfX : static_cast<double>(fit.x);
    const double spanY = fit.turned ? 2.0 * fit.halfY : static_cast<double>(fit.y);

    const double travelX = spanX * frame.scale;
    const double travelY = spanY * frame.scale;

    if (travelX > 0.0)
    {
        result.focusX = std::clamp(view.focusX - along / travelX, 0.0, 1.0);
    }

    if (travelY > 0.0)
    {
        result.focusY = std::clamp(view.focusY - across / travelY, 0.0, 1.0);
    }

    return result;
}

core::WallpaperView Panel::ZoomView(const core::WallpaperView& view, const MapContext& context,
    POINT anchor, double factor)
{
    if (!context.IsValid() || !std::isfinite(factor) || factor <= 0.0)
    {
        return view;
    }

    const double zoom = std::clamp(view.zoom * factor, core::kMinZoom, core::kMaxZoom);
    if (zoom == view.zoom)
    {
        return view;
    }

    // Back at the minimum only the aspect-mismatch slack is left, and the honest framing for that
    // is the centred one the renderer uses for an untouched view. The quarter-turn is not zoom,
    // though - it was set by its own button and survives the wheel.
    if (zoom <= core::kMinZoom)
    {
        core::WallpaperView reset;
        reset.rotation = view.rotation;
        return reset;
    }

    const core::WallpaperView before = core::ClampView(view);
    const MapFrame was = FrameFor(context, before);

    core::WallpaperView result = before;
    result.zoom = zoom;
    const MapFrame now = FrameFor(context, result);

    const core::ViewSlack base = core::ComputeSlack(context.canvas, context.source.width,
        context.source.height, core::WallpaperView{ 1.0, 0.5, 0.5, context.rotation });

    const int limitX =
        std::max(0, static_cast<int>(std::lround(context.canvas.width * context.scale)));
    const int limitY =
        std::max(0, static_cast<int>(std::lround(context.canvas.height * context.scale)));
    const double anchorX = std::clamp(static_cast<double>(anchor.x), 0.0, static_cast<double>(limitX));
    const double anchorY = std::clamp(static_cast<double>(anchor.y), 0.0, static_cast<double>(limitY));

    // Which fraction of the picture sits under the cursor, then the offset that keeps that same
    // fraction there at the new zoom - both axes at once, because a turned picture's two offsets
    // are not independent.
    const core::FrameOffset offsetBefore = core::FrameOffsetFor(was.surface, before);
    const double pointX = anchorX / context.scale;
    const double pointY = anchorY / context.scale;

    const double fractionX = (offsetBefore.x + pointX) / (base.scaledWidth * before.zoom);
    const double fractionY = (offsetBefore.y + pointY) / (base.scaledHeight * before.zoom);

    const double movedX = fractionX * base.scaledWidth * zoom - pointX;
    const double movedY = fractionY * base.scaledHeight * zoom - pointY;

    return core::FocusForOffset(now.surface, result, movedX, movedY);
}

Panel::MapSample Panel::SampleFor(
    const MapFrame& frame, const core::Rect& monitorRect, const core::WallpaperView& view)
{
    MapSample sample;

    if (frame.surface.scaledWidth <= 0 || frame.surface.scaledHeight <= 0 || frame.zoom <= 0.0
        || frame.scale <= 0.0)
    {
        return sample;
    }

    const core::WallpaperView clamped = core::ClampView(view);
    const core::FrameOffset offset = core::FrameOffsetFor(frame.surface, clamped);
    const int offsetX = offset.x;
    const int offsetY = offset.y;

    // The cache holds the picture at zoom 1, so a window at zoom z occupies a z times smaller
    // rectangle inside it. Both edges are rounded independently: two displays that touch have to
    // stay touching.
    const double perCanvas = frame.scale * kMapSupersample / frame.zoom;

    const auto edgeX = [&](int value) {
        return static_cast<int>(std::lround((offsetX + value) * perCanvas));
    };
    const auto edgeY = [&](int value) {
        return static_cast<int>(std::lround((offsetY + value) * perCanvas));
    };

    const int left = edgeX(monitorRect.left);
    const int top = edgeY(monitorRect.top);

    sample.x = left;
    sample.y = top;
    sample.width = std::max(1, edgeX(monitorRect.Right()) - left);
    sample.height = std::max(1, edgeY(monitorRect.Bottom()) - top);
    return sample;
}

int Panel::ForDpi(int value, UINT dpi)
{
    // Design units to device pixels. MulDiv rounds to nearest, so a design distance survives a
    // round trip through the display at any scale.
    return ::MulDiv(value, static_cast<int>(dpi == 0 ? kDesignDpi : dpi), kDesignDpi);
}

RECT Panel::FrameBounds(const RECT& groupBox)
{
    return Inflate(groupBox, kFrameGap);
}

int Panel::FrameThickness(bool selected)
{
    return selected ? kFrameThick : kFrameThin;
}

int Panel::ButtonHeight()
{
    return kButtonHeight;
}

int Panel::ListRowHeight()
{
    return kRowHeight;
}

// --- Lifecycle ------------------------------------------------------------------------------

Panel::Panel(ActionHandler onAction, StateProvider readState)
    : m_onAction(std::move(onAction))
    , m_readState(std::move(readState))
{
}

Panel::~Panel()
{
    ReleasePreviews();

    if (m_measure != nullptr)
    {
        ::DeleteDC(m_measure);
        m_measure = nullptr;
    }

    // The window first: destroying it can still deliver messages, and they would read the fonts
    // that DestroyFonts is about to free.
    if (m_window != nullptr)
    {
        ::DestroyWindow(m_window);
        m_window = nullptr;
    }

    DestroyFonts();

    if (m_instance != nullptr)
    {
        ::UnregisterClassW(kPanelClassName, m_instance);
    }
}

void Panel::CreateFonts()
{
    // Microsoft YaHei UI for CJK, falling back automatically if it is somehow missing. Font
    // linking means the Latin glyphs still come from Segoe UI, which is the intended look.
    //
    // ClearType rather than plain antialiasing: at 11-13 px the greyscale rasteriser draws the
    // diagonals of "W" as almost-50%-coverage grey and the letter reads as a stray slash.
    //
    // Heights are design units: a negative lfHeight is a character height in logical units, so one
    // set of fonts serves every scale factor - GDI resolves them against the scaled mapping mode
    // and rasterises at the device size.
    auto make = [](int height, int weight, const wchar_t* face) {
        return ::CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            face);
    };

    m_fontNum = make(14, FW_BOLD, L"Microsoft YaHei UI");
    m_fontSmall = make(9, FW_NORMAL, L"Microsoft YaHei UI");
    m_fontTitle = make(13, FW_SEMIBOLD, L"Microsoft YaHei UI");
    m_fontBody = make(12, FW_NORMAL, L"Microsoft YaHei UI");
    m_fontAux = make(11, FW_NORMAL, L"Microsoft YaHei UI");
    m_fontAuxBold = make(11, FW_SEMIBOLD, L"Microsoft YaHei UI");
    m_fontIcon = make(14, FW_NORMAL, L"Segoe UI Symbol");
}

void Panel::DestroyFonts()
{
    for (HFONT* font :
        { &m_fontNum, &m_fontSmall, &m_fontTitle, &m_fontBody, &m_fontAux, &m_fontAuxBold,
            &m_fontIcon })
    {
        if (*font != nullptr)
        {
            ::DeleteObject(*font);
            *font = nullptr;
        }
    }
}

void Panel::ScaleMeasureDc()
{
    if (m_measure == nullptr)
    {
        return;
    }

    // Both axes are scaled by the same factor, not just the horizontal one. GDI sizes a font from
    // the *vertical* scale: with only X scaled, a nine-unit font is rasterised at nine device
    // pixels while its advance is divided by 1.25 - so every measurement comes back a fifth too
    // small and the whole sheet lays itself out around text that is not the text being drawn.
    ::SetMapMode(m_measure, MM_ANISOTROPIC);
    ::SetWindowExtEx(m_measure, kWidth, kMeasureHeight, nullptr);
    ::SetViewportExtEx(m_measure, std::max(1, ForDpi(kWidth, m_dpi)),
        std::max(1, ForDpi(kMeasureHeight, m_dpi)), nullptr);
}

POINT Panel::ToDesign(POINT device) const
{
    return POINT{ ::MulDiv(device.x, kDesignDpi, static_cast<int>(m_dpi)),
        ::MulDiv(device.y, kDesignDpi, static_cast<int>(m_dpi)) };
}

bool Panel::Create(HINSTANCE instance, HICON icon)
{
    m_instance = instance;

    CreateFonts();
    m_measure = ::CreateCompatibleDC(nullptr);
    if (m_measure == nullptr)
    {
        LogError(L"CreateCompatibleDC failed for the panel");
        return false;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);

    // CS_DBLCLKS: a double click on a screen means "reset the framing", and without the class
    // style the second click never arrives as WM_LBUTTONDBLCLK.
    //
    // CS_DROPSHADOW: the window is borderless, so the shadow a caption would provide has to be
    // asked for explicitly or the panel appears to float on nothing.
    windowClass.style = CS_DBLCLKS | CS_DROPSHADOW;
    windowClass.lpfnWndProc = &Panel::WndProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = icon;
    windowClass.hIconSm = icon;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kPanelClassName;

    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LogError(L"RegisterClassEx failed for the panel");
        return false;
    }

    // Borderless but resizable: WS_THICKFRAME provides the resize behaviour and WM_NCCALCSIZE
    // strips its visual frame, so the design still draws its own 36 px title bar. Dragging and
    // the edge resize handles are answered in WM_NCHITTEST.
    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_SYSMENU;

    // Placed on whichever display the cursor is on: the panel is usually opened from the tray,
    // and the tray lives on the display the user is looking at.
    POINT cursor{};
    ::GetCursorPos(&cursor);
    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    ::GetMonitorInfoW(monitor, &monitorInfo);

    m_window = ::CreateWindowExW(0, kPanelClassName, L"WinTools 多屏工具", style,
        monitorInfo.rcWork.left,
        monitorInfo.rcWork.top, kWidth, kDefaultHeight, nullptr, nullptr, instance, this);

    if (m_window == nullptr)
    {
        LogError(L"CreateWindowEx failed for the panel");
        return false;
    }

    m_dpi = ::GetDpiForWindow(m_window);
    if (m_dpi == 0)
    {
        m_dpi = kDesignDpi;
    }

    ScaleMeasureDc();

    // The default 4:3 sheet has to fit the display it landed on; smaller than that, it shrinks.
    const int wantedWidth = ForDpi(kWidth, m_dpi);
    const int wantedHeight = ForDpi(kDefaultHeight, m_dpi);
    const int workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    const int workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
    if (wantedWidth > workWidth || wantedHeight > workHeight)
    {
        ::SetWindowPos(m_window, nullptr, 0, 0, std::min(wantedWidth, workWidth),
            std::min(wantedHeight, workHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Windows 11 rounds our corners; older builds simply ignore the attribute.
    const auto preference = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(DWMWCP_ROUND);
    ::DwmSetWindowAttribute(
        m_window, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));

    CentreOnCursorMonitor();
    Refresh();

    LogInfo(L"panel on a {} dpi display, sheet {}x{} design units", m_dpi, kWidth, kDefaultHeight);

    return true;
}

void Panel::CentreOnCursorMonitor()
{
    POINT cursor{};
    ::GetCursorPos(&cursor);

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT window{};
    if (!::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &info)
        || !::GetWindowRect(m_window, &window))
    {
        return;
    }

    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    const int workWidth = info.rcWork.right - info.rcWork.left;
    const int workHeight = info.rcWork.bottom - info.rcWork.top;

    const int x = info.rcWork.left + std::max(0, (workWidth - width) / 2);
    const int y = info.rcWork.top + std::max(0, (workHeight - height) / 2);

    ::SetWindowPos(m_window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Panel::ShowPanel()
{
    if (m_window == nullptr)
    {
        return;
    }

    Refresh();
    ::ShowWindow(m_window, SW_SHOW);
    ::SetForegroundWindow(m_window);
}

void Panel::CommitView()
{
    if (m_window != nullptr)
    {
        ::KillTimer(m_window, kZoomCommitTimer);
    }

    if (!m_zoomPending)
    {
        return;
    }

    m_zoomPending = false;

    if (m_onAction)
    {
        m_onAction(PanelRequest{ PanelAction::SetWallpaperView, m_state.activeSlot, 0, m_view });
    }
}

void Panel::HidePanel()
{
    // Hiding with a zoom still in flight would drop it: nothing would ever fire the timer.
    CommitView();
    m_popup = Popup::None;
    m_modal = Modal::None;

    if (m_window != nullptr)
    {
        ::ShowWindow(m_window, SW_HIDE);
    }
}

void Panel::Toggle()
{
    if (m_window == nullptr)
    {
        return;
    }

    if (::IsWindowVisible(m_window))
    {
        HidePanel();
    }
    else
    {
        ShowPanel();
    }
}

void Panel::Refresh()
{
    if (m_readState)
    {
        m_state = m_readState();
    }

    // The active group's framing follows the model, except while a gesture is in flight - a
    // refresh in the middle of a drag would snap the picture back under the user's cursor.
    const core::Group* active = m_state.ActiveGroup();
    if (active != nullptr
        && (m_viewSlot != m_state.activeSlot
            || (!m_panning && !m_zoomPending && !m_rotating)))
    {
        m_view = active->view;
        m_viewSlot = m_state.activeSlot;
    }

    // A screen that is gone cannot stay selected: its chip disappears with it, and a filter that
    // matches nothing looks identical to "no windows recorded" - except the list is empty and
    // there is no way to undo it. Dropping the entry is what makes the filter survive a replug.
    if (!m_filter.empty())
    {
        std::erase_if(m_filter, [this](const std::wstring& entry)
            {
                for (const auto& monitor : m_state.monitors)
                {
                    if (core::Monitor::IEquals(monitor.devicePath, entry))
                    {
                        return false;
                    }
                }

                return true;
            });
    }

    // The window keeps its size; the lists re-fit inside it on the next paint. Scrolling past
    // the end after the data shrank would leave the view showing nothing, so clamp both views.
    const Geometry g = ComputeGeometry();
    const int total = static_cast<int>(VisibleRows().size());
    m_scroll = std::clamp(m_scroll, 0, std::max(0, total - g.listRows));
    const int archived = static_cast<int>(m_state.archivedNotes.size());
    m_archiveScroll = std::clamp(m_archiveScroll, 0,
        std::max(0, archived - std::max(1, g.archiveRowsCapacity)));

    if (m_window != nullptr)
    {
        if (m_state.busy)
        {
            ::SetTimer(m_window, kSpinTimer, kSpinDelayMs, nullptr);
        }
        else
        {
            ::KillTimer(m_window, kSpinTimer);
        }

        ::InvalidateRect(m_window, nullptr, FALSE);
    }
}

void Panel::ConstrainToWorkArea()
{
    if (m_window == nullptr || ::IsIconic(m_window))
    {
        return;
    }

    // The window is user-resizable now, so the only automatic move is this one: keep the whole
    // window inside its monitor's work area, no size changes.
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT window{};
    if (!::GetMonitorInfoW(::MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST), &info)
        || !::GetWindowRect(m_window, &window))
    {
        return;
    }

    const RECT& work = info.rcWork;
    const int x = std::clamp(window.left, work.left,
        std::max(work.left, work.right - (window.right - window.left)));
    const int y = std::clamp(window.top, work.top,
        std::max(work.top, work.bottom - (window.bottom - window.top)));

    if (x != window.left || y != window.top)
    {
        ::SetWindowPos(m_window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// --- Geometry -------------------------------------------------------------------------------

std::vector<int> Panel::VisibleRows() const
{
    std::vector<int> rows;
    rows.reserve(m_state.windows.size());

    for (int index = 0; index < static_cast<int>(m_state.windows.size()); ++index)
    {
        if (m_filter.empty())
        {
            rows.push_back(index);
            continue;
        }

        const std::wstring& path = m_state.windows[static_cast<size_t>(index)].monitorPath;
        for (const auto& selected : m_filter)
        {
            if (core::Monitor::IEquals(selected, path))
            {
                rows.push_back(index);
                break;
            }
        }
    }

    return rows;
}

namespace
{

/// GDI+ is only used for the Lucide glyph on the rotate chip; its anti-aliasing is what keeps
/// a 2px stroke from turning into a staircase. Startup is process-wide and never torn down,
/// exactly as in the sticky-note chrome, so the token outlives every panel repaint.
bool PanelGdiplusReady()
{
    static ULONG_PTR token = 0;
    static const bool ready = [] {
        Gdiplus::GdiplusStartupInput input;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return ready;
}

Gdiplus::Color PanelGdiplusColor(COLORREF colour)
{
    return Gdiplus::Color(255, GetRValue(colour), GetGValue(colour), GetBValue(colour));
}

/// Rectangle intersection area: zero when the boxes only touch or miss.
int OverlapArea(const RECT& a, const RECT& b)
{
    const int width = std::min(a.right, b.right) - std::max(a.left, b.left);
    const int height = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
    return (width > 0 && height > 0) ? width * height : 0;
}

/// atan2 in degrees, wrapped into [0, 360). Screen y grows downwards, so a growing angle is
/// clockwise - exactly the direction the picture turns when the handle is dragged that way.
double PointerAngle(POINT point, POINT centre)
{
    const double angle = std::atan2(static_cast<double>(point.y - centre.y),
        static_cast<double>(point.x - centre.x)) * 180.0 / 3.14159265358979323846;
    return angle < 0.0 ? angle + 360.0 : angle;
}

/// How far the pointer travelled *along* the circle it was circling, in degrees of that circle.
///
/// The turn used to follow the pointer's angle around the group centre, which has two problems at
/// this size: one degree of cursor movement is a degree of picture, far too fast to aim, and the
/// rate explodes as the drag drifts towards the centre - the classic "one pixel, forty degrees"
/// jump. Steering by arc length at the radius the drag started on is linear in what the hand
/// actually does: move the same distance, get the same turn, wherever on the map you are.
double TangentialDelta(POINT from, POINT to, POINT centre, double radius)
{
    if (radius < 1.0)
    {
        return 0.0;
    }

    const double radians = PointerAngle(from, centre) * 3.14159265358979323846 / 180.0;
    const double dx = static_cast<double>(to.x - from.x);
    const double dy = static_cast<double>(to.y - from.y);
    const double along = -std::sin(radians) * dx + std::cos(radians) * dy;
    return along / radius * 180.0 / 3.14159265358979323846;
}

} // namespace

void Panel::PlaceRotateArc(Geometry& g) const
{
    g.rotateWallpaper = {};
    g.rotateRadius = 0;

    if (m_state.monitors.empty())
    {
        return;
    }

    const core::Group* active = m_state.ActiveGroup();
    if (active == nullptr || active->image.empty())
    {
        return;
    }

    const Projection projection = Fit(m_state.monitors, g.mapArea);
    if (!projection.canvas.IsValid() || projection.scale <= 0.0)
    {
        return;
    }

    const std::vector<size_t> members = core::AttachedMembers(*active, m_state.monitors);
    if (members.empty())
    {
        return;
    }

    RECT box{ INT_MAX, INT_MAX, INT_MIN, INT_MIN };
    std::vector<RECT> others;
    // The calendar chip is a control, not a display, but a handle drawn over it would be just as
    // unusable - so it joins the obstacles with a small breathing gap. Only when the widget is
    // actually on the page: a zeroed rect near the top-left corner would push the handle away from
    // a control that is not there.
    if constexpr (kDesktopWidgetsEnabled)
    {
        others.push_back(RECT{ g.canvasSwitch.left - 4, g.canvasSwitch.top - 4,
            g.canvasSwitch.right + 4, g.canvasSwitch.bottom + 4 });
    }
    for (size_t index = 0; index < m_state.monitors.size(); ++index)
    {
        const RECT screen = projection.ToClient(m_state.monitors[index].rect);
        if (std::find(members.begin(), members.end(), index) == members.end())
        {
            others.push_back(screen);
            continue;
        }

        box.left = std::min(box.left, screen.left);
        box.top = std::min(box.top, screen.top);
        box.right = std::max(box.right, screen.right);
        box.bottom = std::max(box.bottom, screen.bottom);
    }

    if (box.left >= box.right || box.top >= box.bottom)
    {
        return;
    }

    constexpr int iconRadius = 13;  // 26-unit chip, the same size as a title-bar icon
    constexpr int gap = 6;          // space between the display edge and the chip
    constexpr int pad = 4;
    const int L = box.left;
    const int T = box.top;
    const int R = box.right;
    const int B = box.bottom;

    struct Corner
    {
        POINT centre;
        RECT hit;
    };

    // The chip sits one radius plus a gap *outside* the group's corner, on the diagonal: its
    // square can never touch a member display, which is what stops the old filled disc from
    // covering the wallpaper. Four corners are offered; the one with the least overlap with
    // other displays (and the calendar chip) wins.
    const int out = iconRadius + gap;
    const Corner corners[] = {
        { { R + out, T - out },
            { R + out - iconRadius - pad, T - out - iconRadius - pad,
                R + out + iconRadius + pad, T - out + iconRadius + pad } },
        { { L - out, T - out },
            { L - out - iconRadius - pad, T - out - iconRadius - pad,
                L - out + iconRadius + pad, T - out + iconRadius + pad } },
        { { R + out, B + out },
            { R + out - iconRadius - pad, B + out - iconRadius - pad,
                R + out + iconRadius + pad, B + out + iconRadius + pad } },
        { { L - out, B + out },
            { L - out - iconRadius - pad, B + out - iconRadius - pad,
                L - out + iconRadius + pad, B + out + iconRadius + pad } },
    };

    const RECT bounds{ g.mapCard.left + 4, g.mapCard.top + 4, g.mapCard.right - 4,
        g.mapCard.bottom - 4 };

    const Corner* best = nullptr;
    int leastOverlap = INT_MAX;
    for (const Corner& corner : corners)
    {
        if (corner.hit.left < bounds.left || corner.hit.top < bounds.top
            || corner.hit.right > bounds.right || corner.hit.bottom > bounds.bottom)
        {
            continue;
        }

        int overlap = 0;
        for (const RECT& other : others)
        {
            overlap += OverlapArea(corner.hit, other);
        }

        if (overlap < leastOverlap)
        {
            leastOverlap = overlap;
            best = &corner;
            if (overlap == 0)
            {
                break;
            }
        }
    }

    if (best == nullptr)
    {
        return;
    }

    g.rotateWallpaper = best->hit;
    g.rotateCentre = best->centre;
    g.rotateRadius = iconRadius;
}

bool Panel::HitRotateArc(POINT point, const Geometry& g) const
{
    return g.rotateRadius > 0 && Contains(g.rotateWallpaper, point);
}

void Panel::PaintRotateArc(HDC dc, const Geometry& g) const
{
    if (g.rotateRadius <= 0)
    {
        return;
    }

    const Theme& t = m_state.darkTheme ? kDark : kLight;
    const bool hovered = m_hot == Hot::RotateWallpaper || m_rotating;
    const bool disabled = m_state.busy;
    const COLORREF ink = disabled ? t.text4 : (hovered ? t.accent : t.text2);

    // A small round chip the size of a title-bar button, floating just outside the group's corner.
    // Its plate is opaque, so the glyph reads over a pale wallpaper as well as a dark one, and it
    // sits outside every member display - which is what the old filled disc failed to do.
    const RECT chip = g.rotateWallpaper;
    const int chipRadius = (chip.right - chip.left) / 2;
    FillRounded(dc, chip, chipRadius, hovered ? t.hover : t.card2);
    StrokeRounded(dc, chip, chipRadius, hovered ? t.accentLine : t.line2);

    if (!PanelGdiplusReady())
    {
        return;
    }

    // Lucide `rotate-cw` (ISC), the icon set the note's flame, star and camera already come from.
    // One 270-degree circular arc carries the turn and the head is an L-shaped bracket:
    //   M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8
    //   M21 3v5h-5
    // `refresh-cw`, the pair of arrows tried first, needs a second non-circular arc per half; at
    // this size those two arcs plus four strokes filled the middle and read as a scribble. The
    // arc here is a plain circle and the shoulder is a cubic, so both transcribe without guessing.
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    // Design units, like every other coordinate in the sheet: GDI+ paints through the DC's
    // mapping mode, so the glyph follows the display's scale factor with the rest of the layout.
    const float size = static_cast<float>(g.rotateRadius) * 1.5F; // a 26-unit grid at radius 13
    const float left = static_cast<float>(g.rotateCentre.x) - size / 2.0F;
    const float top = static_cast<float>(g.rotateCentre.y) - size / 2.0F;
    const auto px = [left, size](float unit) { return left + size * unit / 24.0F; };
    const auto py = [top, size](float unit) { return top + size * unit / 24.0F; };

    const float diameter = size * 18.0F / 24.0F;

    Gdiplus::GraphicsPath glyph;
    glyph.AddArc(px(3.0F), py(3.0F), diameter, diameter, 0.0F, 270.0F);
    glyph.AddBezier(px(12.0F), py(3.0F), px(14.52F), py(3.0F), px(16.93F), py(4.0F),
        px(18.74F), py(5.74F));
    glyph.AddLine(px(18.74F), py(5.74F), px(21.0F), py(8.0F));
    glyph.StartFigure();
    glyph.AddLine(px(21.0F), py(3.0F), px(21.0F), py(8.0F));
    glyph.AddLine(px(21.0F), py(8.0F), px(16.0F), py(8.0F));

    Gdiplus::Pen pen(PanelGdiplusColor(ink), std::max(1.4F, size * 2.0F / 24.0F));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&pen, &glyph);
}

Panel::Geometry Panel::ComputeGeometry() const
{
    Geometry g;

    // The client size is an input now: the window opens at 4:3 and the user resizes it, so the
    // layout adapts to whatever it is given. Design units stay tied to the display's scale
    // factor, not to the window - resizing shows more or fewer rows, it never stretches a font.
    RECT client{};
    ::GetClientRect(m_window, &client);
    const int deviceW = std::max(1, static_cast<int>(client.right - client.left));
    const int deviceH = std::max(1, static_cast<int>(client.bottom - client.top));
    m_clientW = std::max(kMinWidth, ::MulDiv(deviceW, kDesignDpi, static_cast<int>(m_dpi)));
    m_clientH = std::max(kMinHeight, ::MulDiv(deviceH, kDesignDpi, static_cast<int>(m_dpi)));

    const int left = kSheetPaddingX;
    const int right = m_clientW - kSheetPaddingX;

    g.titleBar = { 0, 0, m_clientW, kTitleBarHeight };

    // Four 28x26 buttons at the right end, 10 units from the edge. Theme and language are the two
    // quick switches; the full three-way choices stay on the config page. Both routes write the
    // same settings, so a quick tap and a deliberate choice cannot drift apart.
    const int iconSize = 26;
    const int iconWidth = 28;
    const int iconTop = (kTitleBarHeight - iconSize) / 2;
    const int iconRight = m_clientW - 10;
    g.iconClose = { iconRight - iconWidth, iconTop, iconRight, iconTop + iconSize };
    g.iconHelp = { iconRight - 2 * iconWidth, iconTop, iconRight - iconWidth, iconTop + iconSize };
    g.iconTheme = { iconRight - 3 * iconWidth, iconTop, iconRight - 2 * iconWidth,
        iconTop + iconSize };
    g.iconLanguage = { iconRight - 4 * iconWidth, iconTop, iconRight - 3 * iconWidth,
        iconTop + iconSize };

    const auto centreIn = [](int top, int height) { return top + (height - kButtonHeight) / 2; };

    // --- Tab strip ---
    // Left-aligned tabs sized to their labels, one row directly under the title bar. The tab
    // carries the page's name, so the page itself does not repeat it - the head row keeps only
    // the object selector and the actions.
    int y = kTitleBarHeight + kSheetPaddingTop;
    g.tabBar = { left, y, right, y + kTabBarHeight };

    const std::wstring_view tabLabels[4] = { L"布局", L"壁纸", L"便签", L"配置" };
    RECT* tabRects[4] = { &g.tabLayout, &g.tabWallpaper, &g.tabNotes, &g.tabConfig };
    const int tabPillHeight = kTabBarHeight;
    int tabX = left;
    for (int index = 0; index < 4; ++index)
    {
        const int labelWidth = static_cast<int>(MeasureText(m_measure, m_fontBody, tabLabels[index]).cx);
        *tabRects[index] = { tabX, y, tabX + labelWidth + 2 * kTabPadX, y + tabPillHeight };
        tabX += labelWidth + 2 * kTabPadX + kTabGap;
    }
    y += kTabBarHeight + kContentGapTop;

    // --- Footer, pinned to the bottom of the window ---
    // Anchored from the bottom edge; the page content takes what is left under the tab strip.
    int bottom = m_clientH - kSheetPaddingBottom;
    g.footer = { left, bottom - kFooterHeight - 1 - kFooterPad, right, bottom };
    const int footerContent = bottom - kFooterHeight;
    g.statusDot = { left, footerContent + (kFooterHeight - 7) / 2, left + 7,
        footerContent + (kFooterHeight - 7) / 2 + 7 };
    g.quit = { right - ButtonWidth(m_measure, m_fontBody, L"退出"), footerContent, right,
        footerContent + kButtonHeight };
    g.statusText = { g.statusDot.right + 6, footerContent, g.quit.left - 12,
        footerContent + kFooterHeight };
    const int contentBottom = g.footer.top - kFooterGap;

    switch (m_tab)
    {
        case Tab::Wallpaper:
        {
            // --- Head: chips and actions only; the tab already named the page ---
            g.wallHead = { left, y, right, y + kHeadHeight };

            const int applyWidth = ButtonWidth(m_measure, m_fontBody, L"应用壁纸");
            const int pickWidth = ButtonWidth(m_measure, m_fontBody, L"更换壁纸");

            const int headTop = centreIn(y, kHeadHeight);
            g.applyWallpaper = { right - applyWidth, headTop, right, headTop + kButtonHeight };
            g.pick = { g.applyWallpaper.left - 8 - pickWidth, headTop, g.applyWallpaper.left - 8,
                headTop + kButtonHeight };

            // Group chips flow left to right; the row is clipped at the buttons.
            int chipX = left;

            // Chip order is by label, so "1+2" always sits left of "3+4" regardless of the order
            // the groups were created in. The slot stays with the group; only the drawing sorts.
            std::vector<int> order;
            for (int slot = 0; slot < static_cast<int>(m_state.groups.size()); ++slot)
            {
                order.push_back(slot);
            }

            std::sort(order.begin(), order.end(), [this](int a, int b)
                {
                    const std::wstring leftLabel =
                        core::GroupLabel(m_state.groups[static_cast<size_t>(a)], m_state.monitors);
                    const std::wstring rightLabel =
                        core::GroupLabel(m_state.groups[static_cast<size_t>(b)], m_state.monitors);

                    const int leftFirst = leftLabel.empty() ? 99 : std::stoi(leftLabel);
                    const int rightFirst = rightLabel.empty() ? 99 : std::stoi(rightLabel);
                    return leftFirst < rightFirst;
                });

            for (const int slot : order)
            {
                const std::wstring label =
                    core::GroupLabel(m_state.groups[static_cast<size_t>(slot)], m_state.monitors);
                if (label.empty())
                {
                    // Every member is unplugged: nothing to name, so nothing to show.
                    continue;
                }

                const int labelWidth = static_cast<int>(MeasureText(m_measure, m_fontNum, label).cx);
                const bool active = slot == m_state.activeSlot;
                const int width = 9 + 7 + 6 + labelWidth + 9 + (active ? 24 : 0);

                if (chipX + width > g.pick.left - 8)
                {
                    break;
                }

                const RECT chip{ chipX, headTop, chipX + width, headTop + kButtonHeight };
                g.groupChips.push_back(chip);
                g.chipOrder.push_back(slot);
                g.groupAdds.push_back(
                    active ? RECT{ chip.right - 24, chip.top, chip.right, chip.bottom } : RECT{});
                chipX += width + 6;
            }

            y += kHeadHeight + kHintGapTop;

            // --- Hint row ---
            // The map now spans the full content width; the read-only numbers anchor to its
            // right edge, and the two floating controls live on top of the map itself.
            g.wallHint = { left, y, right, y + kHintHeight };

            const int metaWidth =
                static_cast<int>(MeasureText(m_measure, m_fontAux, L"4 台 · 2 组").cx);
            g.groupMeta = { right - metaWidth, y, right, y + kHintHeight };

            int hintRight = g.groupMeta.left - 10;
            if (const core::Group* active = m_state.ActiveGroup();
                active != nullptr && active->dirty)
            {
                const int dirtyWidth =
                    static_cast<int>(MeasureText(m_measure, m_fontAux, L"● 未应用").cx);
                g.dirtyBadge = { hintRight - dirtyWidth, y, hintRight, y + kHintHeight };
                hintRight = g.dirtyBadge.left - 10;
            }

            g.zoomBadge = { hintRight - 40, y, hintRight, y + kHintHeight };

            // The turn readout and its reset, left of the zoom: the two things on this row that
            // change while the mouse is down, and the only way to see the exact angle.
            hintRight = g.zoomBadge.left - 10;
            const int resetWidth =
                std::max(36, static_cast<int>(MeasureText(m_measure, m_fontAux, L"复原").cx) + 16);
            g.resetRotation = { hintRight - resetWidth, y + 1, hintRight, y + kHintHeight - 1 };
            hintRight = g.resetRotation.left - 6;

            const int angleWidth =
                static_cast<int>(MeasureText(m_measure, m_fontAuxBold, L"888°").cx);
            g.rotateBadge = { hintRight - angleWidth, y, hintRight, y + kHintHeight };

            y += kHintHeight + kHintGapBottom;

            // --- The map, filling down to the footer ---
            // The map takes the whole band the page offers: a taller window shows more map, a
            // shorter one less - and the projection simply fits whatever it is given.
            const int mapHeight = std::max(140, contentBottom - y);
            g.mapCard = { left, y, right, y + mapHeight };

            // Reserve a band around the projection for the turn handle: it has to sit outside
            // the group's corner without being clipped by the card or covering another display.
            g.mapArea = { g.mapCard.left + 14 + kArcMargin,
                g.mapCard.top + 12 + kArcMargin, g.mapCard.right - 14 - kArcMargin,
                g.mapCard.bottom - 12 - kArcMargin };

            // The calendar chip is an obstacle too, not just other displays: the handle must not
            // land on top of a control. Placed before the arc so the arc can avoid it. Shelved
            // widgets leave the rect empty, and nothing is reserved for them.
            if constexpr (kDesktopWidgetsEnabled)
            {
                const int floatRight = g.mapCard.right - kWallFloatInset;
                g.canvasSwitch = { floatRight - 68, g.mapCard.top + 10,
                    floatRight, std::min(g.mapCard.bottom - 8, g.mapCard.top + 10 + 46) };
            }
            else
            {
                // Geometry is default-initialised, so an unset RECT is garbage, not zero.
                g.canvasSwitch = RECT{};
            }

            // The arc handle belongs to the selected group, not to the page: it sits just outside
            // one corner of that group's box in the map, on the side where no other display is
            // covered.
            PlaceRotateArc(g);
            break;
        }

        case Tab::Notes:
        {
            // --- Head: the actions only; the tab already named the page ---
            g.notesHead = { left, y, right, y + kHeadHeight };

            const int newNoteWidth = ButtonWidth(m_measure, m_fontBody, L"新建便签");
            const int showNotesWidth = ButtonWidth(m_measure, m_fontBody, L"显示所有便签");
            const int colorsWidth = ButtonWidth(m_measure, m_fontBody, L"配色");

            const int headTop = centreIn(y, kHeadHeight);
            g.showNotes = { right - showNotesWidth, headTop, right, headTop + kButtonHeight };
            g.newNote = { g.showNotes.left - 8 - newNoteWidth, headTop, g.showNotes.left - 8,
                headTop + kButtonHeight };
            // The palette button comes first in the row: it is a page-wide setting, and the two
            // buttons to its right act on individual notes.
            g.noteColors = { g.newNote.left - 8 - colorsWidth, headTop, g.newNote.left - 8,
                headTop + kButtonHeight };

            y += kHeadHeight + kNotesHeadGap;

            // --- Closed notes, as many rows as the window height allows ---
            // The title and the pager live in their own row above the card, the same way the
            // layout page puts "N 个窗口" above its list. The card itself is just records, so an
            // empty one is one centred line instead of a heading with nothing under it.
            g.archiveMeta = { left, y, right, y + kArchiveMetaHeight };
            y += kArchiveMetaHeight + kNotesCardGapTop;

            const int cardHeight =
                std::max(kArchiveRowHeight + kNotesCardPadBottom, contentBottom - y);
            g.archiveCard = { left, y, right, y + cardHeight };

            const int rowsCapacity =
                std::max(1, (cardHeight - kNotesCardPadBottom) / kArchiveRowHeight);
            const int total = static_cast<int>(m_state.archivedNotes.size());

            // Paging is by whole cardfuls, so the last page starts on a page boundary and holds
            // whatever is left. Clamping the scroll to `total - capacity` instead - which is what
            // one-row wheel scrolling wants - makes the last page repeat the end of the one before
            // it, and the page counter then reads "2 / 2" over nine rows of a twelve-row list.
            const int pages = std::max(1, (total + rowsCapacity - 1) / rowsCapacity);
            const int lastStart = (pages - 1) * rowsCapacity;
            m_archiveScroll = std::clamp(m_archiveScroll, 0, std::min(lastStart, total));
            const int first = m_archiveScroll;
            const int shown = std::min(rowsCapacity, total - first);

            const int restoreWidth = ButtonWidth(m_measure, m_fontAux, L"恢复");
            const int forgetWidth = ButtonWidth(m_measure, m_fontAux, L"删除");
            for (int row = 0; row < shown; ++row)
            {
                const int top = y + 4 + row * kArchiveRowHeight;
                g.archiveRows.push_back({ left + 8, top, right - 8, top + kArchiveRowHeight });
                g.archiveForget.push_back({ right - 12 - forgetWidth, top + 3, right - 12,
                    top + 3 + kButtonHeight - 8 });
                g.archiveRestore.push_back({ right - 20 - forgetWidth - restoreWidth, top + 3,
                    right - 20 - forgetWidth, top + 3 + kButtonHeight - 8 });
            }

            g.archiveFirst = first;
            g.archiveRowsCapacity = rowsCapacity;

            // Page arrows, right-aligned in the meta row. Two 22-unit squares and a "2 / 5" label;
            // all three stay empty on a single page, which keeps them out of the hit test.
            if (pages > 1)
            {
                const RECT meta = g.archiveMeta;
                g.archiveNext = { meta.right - 22, meta.top, meta.right, meta.bottom };
                g.archivePrev = { g.archiveNext.left - 22, meta.top, g.archiveNext.left,
                    meta.bottom };
                const int pageTextWidth =
                    static_cast<int>(MeasureText(m_measure, m_fontAux, L"9 / 9").cx) + 8;
                g.archivePage = { g.archivePrev.left - pageTextWidth, meta.top, g.archivePrev.left,
                    meta.bottom };
            }
            break;
        }

        case Tab::Config:
        {
            // Two columns of sections. One column would need about 440 units of height and the
            // sheet has 364 at its design size; splitting them by kind also puts the two long
            // lists - appearance and shortcuts - beside each other instead of in each other's way.
            const int columnWidth = (right - left - kConfigColumnGap) / 2;
            RECT columns[2] = { { left, y, left + columnWidth, contentBottom },
                { left + columnWidth + kConfigColumnGap, y, right, contentBottom } };
            int cursor[2] = { y, y };
            bool started[2] = { false, false };
            int column = 0;

            const auto heading = [&](const wchar_t* text)
            {
                if (started[column])
                {
                    cursor[column] += kConfigSectionGap;
                }
                started[column] = true;

                Geometry::ConfigRow entry;
                entry.section = true;
                entry.label = text;
                entry.rect = { columns[column].left, cursor[column], columns[column].right,
                    cursor[column] + kConfigSectionHeight };
                cursor[column] += kConfigSectionHeight;
                g.configRows.push_back(std::move(entry));
            };

            const auto take = [&](int height)
            {
                const RECT rect{ columns[column].left, cursor[column], columns[column].right,
                    cursor[column] + height };
                // Nothing is laid out past the footer. A row that does not fit comes back empty:
                // drawing it over the status line, or letting it respond where it is not drawn,
                // would both be worse than leaving it out.
                if (rect.bottom > contentBottom)
                {
                    return RECT{};
                }

                cursor[column] += height;
                return rect;
            };

            /// A switch row: the track hugs the column's right edge, the label the left.
            const auto toggle = [&](const wchar_t* label, Hot hot, bool onValue)
            {
                Geometry::ConfigRow entry;
                entry.kind = ConfigControl::Switch;
                entry.hot = hot;
                entry.label = label;
                entry.on = onValue;
                entry.rect = take(kConfigRowHeight);

                const int top = entry.rect.top + (kConfigRowHeight - kConfigSwitchHeight) / 2;
                entry.control = { entry.rect.right - kConfigSwitchWidth, top, entry.rect.right,
                    top + kConfigSwitchHeight };
                g.configRows.push_back(std::move(entry));
            };

            /// Two or three choices in one control. Measured first, because the control is
            /// right-aligned and its left edge depends on every label inside it - a fixed width
            /// clips "跟随系统" the moment the language changes.
            const auto segmented = [&](const wchar_t* label, Hot hot, int selected,
                                       std::initializer_list<const wchar_t*> choices)
            {
                Geometry::ConfigRow entry;
                entry.kind = ConfigControl::Segments;
                entry.hot = hot;
                entry.label = label;
                entry.selected = selected;
                entry.rect = take(kConfigRowHeight);

                int widths[4] = {};
                int count = 0;
                int total = 4; // the container's own 2 units of padding either side
                for (const wchar_t* choice : choices)
                {
                    widths[count] =
                        static_cast<int>(MeasureText(m_measure, m_fontSmall, choice).cx)
                        + 2 * kConfigSegmentPadX;
                    total += widths[count];
                    entry.choices.emplace_back(choice);
                    ++count;
                }

                const int top = entry.rect.top + (kConfigRowHeight - kConfigSegmentHeight) / 2;
                entry.control = { entry.rect.right - total, top, entry.rect.right,
                    top + kConfigSegmentHeight };

                int segmentLeft = entry.control.left + 2;
                for (int index = 0; index < count; ++index)
                {
                    entry.segments.push_back({ segmentLeft, top + 2, segmentLeft + widths[index],
                        top + kConfigSegmentHeight - 2 });
                    segmentLeft += widths[index];
                }
                g.configRows.push_back(std::move(entry));
            };

            const auto hotkeyRow = [&](core::HotkeyAction action)
            {
                Geometry::ConfigRow entry;
                entry.kind = ConfigControl::Hotkey;
                entry.hot = Hot::ConfigHotkey;
                entry.index = static_cast<int>(action);
                entry.label = core::HotkeyActionLabel(action);
                entry.rect = take(kConfigRowHeight);
                entry.value = m_recordingAction == entry.index
                    ? L"按下组合键…（Esc 取消）"
                    : core::DescribeHotkey(m_state.hotkeys.At(action));
                g.configRows.push_back(std::move(entry));
            };

            const auto link = [&](const wchar_t* label, Hot hot)
            {
                Geometry::ConfigRow entry;
                entry.kind = ConfigControl::Link;
                entry.hot = hot;
                entry.label = label;
                entry.rect = take(kConfigRowHeight);
                g.configRows.push_back(std::move(entry));
            };

            // --- Appearance ---
            heading(L"外观");
            segmented(L"主题", Hot::ConfigTheme, m_state.theme, { L"浅色", L"深色", L"跟随系统" });
            segmented(L"语言", Hot::ConfigLanguage, m_state.language,
                { L"跟随系统", L"简体中文", L"English" });

            // --- Startup ---
            heading(L"启动");
            toggle(L"开机启动", Hot::ConfigAutostart, m_state.autostart);
            toggle(L"重启恢复便签", Hot::ConfigNoteRestore, m_state.noteRestore);

            // --- Information ---
            heading(L"信息");
            link(L"查看日志", Hot::ConfigOpenLog);

            // --- Shortcuts ---
            column = 1;
            heading(L"快捷键");
            for (const core::HotkeyAction action : core::kHotkeyActions)
            {
                hotkeyRow(action);
            }

            // The reset button gets a row of its own: it is not a setting, it undoes all of them.
            cursor[column] += kConfigSectionGap;
            {
                Geometry::ConfigRow entry;
                entry.kind = ConfigControl::Button;
                entry.hot = Hot::ConfigResetHotkeys;
                entry.label = L"恢复默认";
                entry.rect = take(kConfigRowHeight);

                const int width = ButtonWidth(m_measure, m_fontBody, entry.label);
                const int top = entry.rect.top + (kConfigRowHeight - kButtonHeight) / 2;
                entry.control = { entry.rect.right - width, top, entry.rect.right,
                    top + kButtonHeight };
                g.configRows.push_back(std::move(entry));
            }
            break;
        }

        case Tab::Layout:
        {
            // --- Head: chips and actions only; the tab already named the page ---
            g.layoutHead = { left, y, right, y + kHeadHeight };

            const int placeWidth = ButtonWidth(m_measure, m_fontBody, L"恢复布局");
            const int captureWidth = ButtonWidth(m_measure, m_fontBody, L"获取布局");
            const int switchWidth =
                30 + 6 + static_cast<int>(MeasureText(m_measure, m_fontBody, L"自动恢复").cx);

            const int layoutTop = centreIn(y, kHeadHeight);
            g.applyLayout = { right - placeWidth, layoutTop, right, layoutTop + kButtonHeight };
            g.refresh = { g.applyLayout.left - 8 - captureWidth, layoutTop, g.applyLayout.left - 8,
                layoutTop + kButtonHeight };
            g.autoSwitch = { g.refresh.left - 12 - switchWidth, layoutTop, g.refresh.left - 12,
                layoutTop + kButtonHeight };

            // Screen chips: a bare row of numbers, one per attached display, in reading order.
            int chipLeft = left;
            for (const size_t index : core::ReadingOrder(m_state.monitors))
            {
                const std::wstring& path = m_state.monitors[index].devicePath;
                const std::wstring number =
                    std::to_wstring(core::ScreenNumber(path, m_state.monitors));

                int count = 0;
                for (const auto& window : m_state.windows)
                {
                    if (core::Monitor::IEquals(window.monitorPath, path))
                    {
                        ++count;
                    }
                }

                const std::wstring caption = std::format(L"({})", count);
                const int width = 9 + static_cast<int>(MeasureText(m_measure, m_fontNum, number).cx)
                                + 4 + static_cast<int>(MeasureText(m_measure, m_fontSmall, caption).cx)
                                + 9;

                if (chipLeft + width > g.autoSwitch.left - 12)
                {
                    break;
                }

                g.screenChips.push_back(
                    RECT{ chipLeft, layoutTop, chipLeft + width, layoutTop + kButtonHeight });
                g.chipMonitors.push_back(static_cast<int>(index));
                chipLeft += width + 2;
            }

            y += kHeadHeight + kHintGapTop;

            // --- Hint row ---
            g.layoutHint = { left, y, right, y + kHintHeight };

            const int windowMetaWidth =
                static_cast<int>(MeasureText(m_measure, m_fontAux, L"888 / 888 个窗口").cx);
            g.windowMeta = { right - windowMetaWidth, y, right, y + kHintHeight };

            const int snapshotWidth =
                static_cast<int>(
                    MeasureText(m_measure, m_fontAux, L"快照 88-88 88:88 · 不是实时桌面").cx);
            g.snapshotMeta = { g.windowMeta.left - 10 - snapshotWidth, y, g.windowMeta.left - 10,
                y + kHintHeight };

            y += kHintHeight + kHintGapBottom;

            // --- The list, as many rows as the window height allows ---
            const int total = static_cast<int>(VisibleRows().size());
            const int rowsCapacity = std::max(1,
                (contentBottom - y - 2 * kListBorder) / kRowHeight);
            g.listRows = std::min(total, rowsCapacity);
            g.firstRow = std::clamp(m_scroll, 0, std::max(0, total - g.listRows));

            const int listHeight = std::max(1, g.listRows) * kRowHeight + 2 * kListBorder;
            g.listCard = { left, y, right, y + listHeight };
            g.listArea = { g.listCard.left + kListBorder, g.listCard.top + kListBorder,
                g.listCard.right - kListBorder,
                g.listCard.top + kListBorder + g.listRows * kRowHeight };
            break;
        }
    }

    g.clientHeight = m_clientH;
    return g;
}

// --- Hit test -------------------------------------------------------------------------------

Panel::Target Panel::HitTest(POINT point) const
{
    const Geometry g = ComputeGeometry();

    // A modal owns the entire sheet while it is up: nothing behind it is clickable, which is the
    // whole point of a modal.
    if (m_modal != Modal::None)
    {
        return { Hot::None, -1 };
    }

    if (m_popup != Popup::None)
    {
        // -2 means "the click was outside": it closes the menu and is then swallowed, so
        // dismissing a menu cannot also press whatever was underneath it.
        const std::vector<RECT> rects = PopupItemRects(g, PopupRect(g));
        const std::vector<PopupEntry> items = PopupItems();
        for (size_t index = 0; index < rects.size(); ++index)
        {
            if (rects[index].right <= rects[index].left || !Contains(rects[index], point))
            {
                continue;
            }

            // The "×" at the right end of a removable row is its own target: the row edits its
            // paper, the cross drops it.
            if (index < items.size() && items[index].removable
                && point.x >= rects[index].right - 24)
            {
                return { Hot::PopupRemove, static_cast<int>(index) };
            }

            return { Hot::PopupItem, static_cast<int>(index) };
        }

        return { Hot::PopupItem, -2 };
    }

    if (Contains(g.iconLanguage, point))
    {
        return { Hot::IconLanguage, -1 };
    }
    if (Contains(g.iconTheme, point))
    {
        return { Hot::IconTheme, -1 };
    }
    if (Contains(g.iconHelp, point))
    {
        return { Hot::IconHelp, -1 };
    }
    if (Contains(g.iconClose, point))
    {
        return { Hot::IconClose, -1 };
    }

    if (Contains(g.tabLayout, point))
    {
        return { Hot::Tab, 0 };
    }
    if (Contains(g.tabWallpaper, point))
    {
        return { Hot::Tab, 1 };
    }
    if (Contains(g.tabNotes, point))
    {
        return { Hot::Tab, 2 };
    }
    if (Contains(g.tabConfig, point))
    {
        return { Hot::Tab, 3 };
    }

    // The config page's rows. A switch or a button only answers on its own control; a shortcut or
    // a link row answers anywhere along its width, which is why those carry no control rect.
    for (const Geometry::ConfigRow& row : g.configRows)
    {
        if (row.section || row.hot == Hot::None || row.rect.bottom <= row.rect.top)
        {
            continue;
        }

        if (row.kind == ConfigControl::Segments)
        {
            for (size_t index = 0; index < row.segments.size(); ++index)
            {
                if (Contains(row.segments[index], point))
                {
                    return { row.hot, static_cast<int>(index) };
                }
            }
            continue;
        }

        const RECT& target = row.kind == ConfigControl::Switch || row.kind == ConfigControl::Button
            ? row.control
            : row.rect;
        if (Contains(target, point))
        {
            return { row.hot, row.index };
        }
    }

    for (size_t index = 0; index < g.groupChips.size(); ++index)
    {
        const int slot = g.chipOrder[index];
        if (g.groupAdds[index].right > g.groupAdds[index].left
            && Contains(g.groupAdds[index], point))
        {
            return { Hot::GroupAdd, slot };
        }
        if (Contains(g.groupChips[index], point))
        {
            return { Hot::GroupChip, slot };
        }
    }

    if (Contains(g.pick, point))
    {
        return { Hot::Pick, -1 };
    }
    if (Contains(g.applyWallpaper, point))
    {
        return { Hot::ApplyWallpaper, -1 };
    }
    if (HitRotateArc(point, g))
    {
        return { Hot::RotateWallpaper, -1 };
    }
    if (Contains(g.resetRotation, point) && core::ClampView(m_view).rotation > 0.01)
    {
        return { Hot::ResetRotation, -1 };
    }
    if (Contains(g.newNote, point))
    {
        return { Hot::NewNote, -1 };
    }
    if (Contains(g.showNotes, point))
    {
        return { Hot::ShowNotes, -1 };
    }
    if (Contains(g.noteColors, point))
    {
        return { Hot::NoteColors, -1 };
    }

    for (size_t index = 0; index < g.screenChips.size(); ++index)
    {
        if (Contains(g.screenChips[index], point))
        {
            return { Hot::ScreenChip, g.chipMonitors[index] };
        }
    }

    if (Contains(g.autoSwitch, point))
    {
        return { Hot::AutoLayout, -1 };
    }
    if (Contains(g.refresh, point))
    {
        return { Hot::Refresh, -1 };
    }
    if (Contains(g.applyLayout, point))
    {
        return { Hot::ApplyLayout, -1 };
    }

    if (Contains(g.quit, point))
    {
        return { Hot::Quit, -1 };
    }

    if constexpr (kDesktopWidgetsEnabled)
    {
        if (Contains(g.canvasSwitch, point))
        {
            return { Hot::CanvasSwitch, -1 };
        }
    }

    // The number badge is the only route into the group menu, and it sits on top of the map.
    if (Contains(g.mapCard, point))
    {
        const int monitor = MonitorAt(point);
        if (monitor >= 0)
        {
            const MapLayout map = LayoutMap(g);
            const RECT box =
                MonitorRectOnMap(map, m_state.monitors, static_cast<size_t>(monitor));

            if (Contains(BadgeRect(box, monitor), point))
            {
                return { Hot::ScreenBadge, monitor };
            }
        }

        // The calendar is drawn over the pictures, so it is hit-tested over them too - but before
        // the map body, which is the wallpaper drag.
        if (kDesktopWidgetsEnabled && m_state.canvasOn)
        {
            const RECT box = CanvasRectOnMap(LayoutMap(g));
            if (Contains(box, point))
            {
                return { Hot::CanvasBody, -1 };
            }
        }
    }

    if (Contains(g.listArea, point))
    {
        const std::vector<int> rows = VisibleRows();
        for (int slot = 0; slot < g.listRows; ++slot)
        {
            const int position = g.firstRow + slot;
            if (position >= static_cast<int>(rows.size()))
            {
                break;
            }

            const RECT row{ g.listArea.left, g.listArea.top + slot * kRowHeight,
                g.listArea.right, g.listArea.top + (slot + 1) * kRowHeight };
            if (!Contains(row, point))
            {
                continue;
            }

            // Only the 30x30 square changes the tick. Clicking the title selects the row, which
            // is what keeps a misjudged click from silently unremembering a window.
            const RECT hit{ row.left, row.top + 1, row.left + 30, row.top + 31 };
            const int index = rows[static_cast<size_t>(position)];
            return Contains(hit, point) ? Target{ Hot::RowCheck, index }
                                        : Target{ Hot::RowBody, index };
        }
    }

    // Page arrows in the history head come before the rows: they sit above them, and a click that
    // lands on an arrow must never be read as a click on the row underneath.
    if (Contains(g.archivePrev, point))
    {
        return { Hot::ArchivePrev, -1 };
    }
    if (Contains(g.archiveNext, point))
    {
        return { Hot::ArchiveNext, -1 };
    }

    // Closed-note rows: the two buttons win over the row itself, the same way the checkbox wins
    // over the title in the window list. The index reported is the archive's own, not the row's:
    // once the list is paged, row 0 is whatever the first page starts at.
    for (size_t index = 0; index < g.archiveRows.size(); ++index)
    {
        if (!Contains(g.archiveRows[index], point))
        {
            continue;
        }
        const int archiveIndex = g.archiveFirst + static_cast<int>(index);
        if (Contains(g.archiveRestore[index], point))
        {
            return { Hot::ArchiveRestore, archiveIndex };
        }
        if (Contains(g.archiveForget[index], point))
        {
            return { Hot::ArchiveForget, archiveIndex };
        }
        break;
    }

    if (Contains(g.mapCard, point))
    {
        return { Hot::MapBody, -1 };
    }

    return { Hot::None, -1 };
}

// --- Preview cache --------------------------------------------------------------------------

core::ImageSize Panel::SourceSizeFor(const std::wstring& path)
{
    if (path.empty())
    {
        return {};
    }

    for (const auto& entry : m_sources)
    {
        if (entry.path == path)
        {
            return entry.size;
        }
    }

    core::ImageSize size{};
    if (const auto queried = core::QueryImageSize(path); queried.has_value())
    {
        size = *queried;
    }

    // A handful of entries is all a desk with six groups can produce; the cap only stops a user
    // who keeps picking new pictures from growing the list without bound.
    if (m_sources.size() >= 12)
    {
        m_sources.erase(m_sources.begin());
    }

    m_sources.push_back(SourceSizeEntry{ path, size });
    return size;
}

void Panel::ReleasePreviews()
{
    for (auto& preview : m_previews)
    {
        if (preview.bitmap != nullptr)
        {
            ::DeleteObject(preview.bitmap);
        }
    }

    m_previews.clear();
}

HBITMAP Panel::PreviewFor(
    int slot, const std::wstring& path, double rotation, int width, int height, bool coarse)
{
    if (path.empty() || width <= 0 || height <= 0)
    {
        return nullptr;
    }

    for (auto& preview : m_previews)
    {
        if (preview.Matches(slot, path, rotation, coarse, width, height))
        {
            return preview.bitmap;
        }
    }

    // The whole turned surface at zoom 1, uncropped: the map slices windows out of it with the
    // same framing maths the renderer uses, and while a turn is in progress it also shows the
    // margin that no display will get. Measuring by the caller's size alone used to hide it.
    const auto decoded = core::RenderSurface(path, width, height, rotation, coarse);
    if (!decoded.has_value())
    {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = decoded->width;
    info.bmiHeader.biHeight = -decoded->height; // negative: top-down, matching WIC's order
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    const HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr)
    {
        if (bitmap != nullptr)
        {
            ::DeleteObject(bitmap);
        }
        return nullptr;
    }

    std::memcpy(bits, decoded->pixels.data(), decoded->pixels.size() * sizeof(std::uint32_t));

    // Replace whatever this slot held before: one picture per group, so a stale preview for the
    // same slot can never be the right one.
    for (auto& preview : m_previews)
    {
        if (preview.slot == slot)
        {
            if (preview.bitmap != nullptr)
            {
                ::DeleteObject(preview.bitmap);
            }

            preview = GroupPreview{ slot, path, rotation, coarse, decoded->width, decoded->height, bitmap };
            return bitmap;
        }
    }

    if (m_previews.size() >= 8)
    {
        if (m_previews.front().bitmap != nullptr)
        {
            ::DeleteObject(m_previews.front().bitmap);
        }
        m_previews.erase(m_previews.begin());
    }

    m_previews.push_back(
        GroupPreview{ slot, path, rotation, coarse, decoded->width, decoded->height, bitmap });
    return bitmap;
}

Panel::MapContext Panel::ContextForGroup(const Projection& projection, const core::Group& group,
    const core::WallpaperView& view)
{
    MapContext context;
    context.scale = projection.scale;

    std::vector<core::Monitor> members;
    for (const size_t index : core::AttachedMembers(group, m_state.monitors))
    {
        const core::MonitorSnapshot& snapshot = m_state.monitors[index];
        core::Monitor monitor;
        monitor.devicePath = snapshot.devicePath;
        monitor.monitorKey = core::Monitor::BuildKey(snapshot.devicePath);
        monitor.left = snapshot.rect.left;
        monitor.top = snapshot.rect.top;
        monitor.width = snapshot.rect.width;
        monitor.height = snapshot.rect.height;
        members.push_back(std::move(monitor));
    }

    if (members.empty())
    {
        return context;
    }

    context.canvas = core::GetCanvas(members);
    context.source = SourceSizeFor(group.image);
    context.rotation = core::ClampView(view).rotation;
    return context;
}

Panel::BadgeLayout Panel::LayoutBadge(const RECT& box, int index) const
{
    BadgeLayout layout;

    if (index < 0 || index >= static_cast<int>(m_state.monitors.size()))
    {
        return layout;
    }

    const int available = (box.right - box.left) - 2 * kBadgeMargin;
    if (available < 26 || (box.bottom - box.top) < 24)
    {
        return layout;
    }

    const core::MonitorSnapshot& monitor = m_state.monitors[static_cast<size_t>(index)];
    const std::wstring number =
        std::to_wstring(core::ScreenNumber(monitor.devicePath, m_state.monitors));
    const std::wstring resolution =
        std::format(L"{}×{}", monitor.rect.width, monitor.rect.height);

    // One size for both halves of the label. A big bold figure next to a small caption read as
    // two unrelated things stuck together; the number is still first and still brighter, which is
    // enough to tell them apart at this size.
    const int numberWidth = static_cast<int>(MeasureText(m_measure, m_fontSmall, number).cx);
    const int resolutionWidth =
        static_cast<int>(MeasureText(m_measure, m_fontSmall, resolution).cx);

    // Two units of slack: a run of text hinted at the device size can come out a pixel or two
    // wider than the same run measured in design units, and a plate that ends flush against its
    // own text reads as clipped.
    constexpr int kSlack = 2;
    const int numberOnly = 2 * kBadgePaddingX + numberWidth + kSlack;
    const int full = numberOnly + kBadgeResolutionGap + resolutionWidth;

    // The label never grows past the screen it belongs to: a plate that spills onto the
    // neighbour reads as the neighbour's label, and on a narrow arrangement every plate would
    // spill. Full label when it fits, the bare number when only that fits, nothing when neither.
    int width = 0;
    if (full <= available)
    {
        width = full;
        layout.withResolution = true;
    }
    else if (numberOnly <= available)
    {
        width = numberOnly;
    }
    else
    {
        return layout;
    }

    layout.rect = RECT{ box.left + kBadgeMargin, box.top + kBadgeMargin,
        box.left + kBadgeMargin + width, box.top + kBadgeMargin + kBadgeHeight };
    return layout;
}

RECT Panel::BadgeRect(const RECT& box, int index) const
{
    return LayoutBadge(box, index).rect;
}

// --- Painting -------------------------------------------------------------------------------

void Panel::Paint()
{
    PAINTSTRUCT paint{};
    const HDC dc = ::BeginPaint(m_window, &paint);
    if (dc == nullptr)
    {
        return;
    }

    RECT client{};
    ::GetClientRect(m_window, &client);
    PaintContent(dc, client.right - client.left, client.bottom - client.top);
    ::EndPaint(m_window, &paint);
}

void Panel::PaintContent(HDC target, int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    const Geometry g = ComputeGeometry();

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down, matching the order GDI paints in
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    const HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr)
    {
        if (bitmap != nullptr)
        {
            ::DeleteObject(bitmap);
        }
        return;
    }

    const HDC dc = ::CreateCompatibleDC(target);
    const HGDIOBJ oldBitmap = ::SelectObject(dc, bitmap);

    // Scaled mapping mode: one design unit becomes (width / m_clientW) device pixels for
    // everything drawn below - rectangles, corner radii, pen widths and font heights alike. GDI
    // resolves a logical font against this mapping, so text is rasterised at the device size
    // instead of being stretched from a 96-dpi bitmap. The design extent is re-derived from the
    // actual client rect inside ComputeGeometry, so a user resize simply shows more or fewer
    // rows - nothing stretches.
    const int designWidth = m_clientW;
    const int designHeight = m_clientH;

    FillBox(dc, RECT{ 0, 0, width, height }, m_state.darkTheme ? kDark.bg : kLight.bg);

    ::SetMapMode(dc, MM_ANISOTROPIC);
    ::SetWindowExtEx(dc, designWidth, designHeight, nullptr);
    ::SetViewportExtEx(dc, width, height, nullptr);

    m_surface = PixelSurface{ static_cast<std::uint32_t*>(bits), width, height,
        static_cast<double>(width) / designWidth, static_cast<double>(height) / designHeight,
        true };

    PaintTarget paintTarget;
    paintTarget.dc = dc;
    paintTarget.surface = m_surface;
    g_paintTarget = &paintTarget;

    PaintSheet(dc, g);
    PaintOverlays(dc, g);

    g_paintTarget = nullptr;
    m_surface = PixelSurface{};

    // Back to device units before the copy: BitBlt would otherwise run the finished sheet through
    // the same scale factor a second time.
    ::SetMapMode(dc, MM_TEXT);
    ::BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);

    ::SelectObject(dc, oldBitmap);
    ::DeleteObject(bitmap);
    ::DeleteDC(dc);
}

void Panel::PaintSheet(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    FillBox(dc, RECT{ 0, 0, m_clientW, m_clientH }, t.bg);

    PaintTitleBar(dc, geometry);
    PaintTabBar(dc, geometry);

    // Only the current page has geometry, so only it has anything to draw.
    switch (m_tab)
    {
        case Tab::Layout:
            PaintLayoutBlock(dc, geometry);
            break;

        case Tab::Wallpaper:
            PaintWallpaperBlock(dc, geometry);
            break;

        case Tab::Notes:
            PaintNotesHead(dc, geometry);
            PaintArchive(dc, geometry);
            break;

        case Tab::Config:
            PaintConfigBlock(dc, geometry);
            break;
    }

    PaintFooter(dc, geometry);

    // Focus ring last, on top of everything it points at. Two rings so it survives any backdrop:
    // the inner one is the sheet colour, the outer one the accent.
    if (m_focus >= 0)
    {
        const std::vector<Target> ring = FocusRing(geometry);
        if (m_focus < static_cast<int>(ring.size()))
        {
            const RECT rect = RectForTarget(geometry, ring[static_cast<size_t>(m_focus)]);
            if (rect.right > rect.left)
            {
                StrokeRounded(dc, Inflate(rect, 4), 8, t.accent, 2);
                StrokeRounded(dc, Inflate(rect, 2), 7, t.bg, 2);
            }
        }
    }

    // A hairline around the whole sheet: the window is borderless, and a 1 px edge is what stops
    // a white panel over a white wallpaper from dissolving into it.
    StrokeBox(dc, RECT{ 0, 0, m_clientW, m_clientH }, t.line2);
}

/// The panel's one switch: a track, a knob, and no checkbox glyph. A glyph either lands on a pixel
/// boundary or smears at 120 dpi, and this control has to read at a glance - it is the only way to
/// tell "on" from "off" on the pages that use it.
void PaintSwitch(HDC dc, const RECT& track, bool onValue, const Theme& t)
{
    const int height = track.bottom - track.top;
    if (height <= 4 || track.right <= track.left)
    {
        return;
    }

    FillRounded(dc, track, height / 2, onValue ? t.accent : t.line2);

    const int knob = height - 4;
    const int knobX = onValue ? track.right - 2 - knob : track.left + 2;
    FillRounded(dc, RECT{ knobX, track.top + 2, knobX + knob, track.top + 2 + knob }, knob / 2,
        RGB(255, 255, 255));
}

void Panel::PaintTitleBar(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    FillBox(dc, geometry.titleBar, t.card);

    // A 6 px dot, the product name, and what the product does - in that order, because the name
    // alone does not say what the window is for.
    const int dotY = kTitleBarHeight / 2 - 3;
    FillRounded(dc, RECT{ 16, dotY, 22, dotY + 6 }, 3, t.accent);

    const int nameLeft = 16 + 6 + 9;
    const int baseline = TextBaseline(dc, m_fontTitle, kTitleBarHeight);
    DrawTextOnBaseline(dc, m_fontTitle, t.text,
        RECT{ nameLeft, 0, nameLeft + 90, kTitleBarHeight }, baseline, L"WinTools");

    const int nameWidth = static_cast<int>(MeasureText(m_measure, m_fontTitle, L"WinTools").cx);
    DrawTextOnBaseline(dc, m_fontAux, t.text3,
        RECT{ nameLeft + nameWidth + 9, 0, geometry.iconLanguage.left - 12, kTitleBarHeight },
        baseline, L"多屏工具");

    FillBox(dc, RECT{ 0, kTitleBarHeight - 1, m_clientW, kTitleBarHeight }, t.line);

    // The four glyphs come from two fonts, and `DT_VCENTER` centres their line boxes, not the ink
    // the eye actually sees: at 120 dpi the measured ink centres were CN 21.5, circle/? 23 and
    // close 24 device pixels. A one-unit optical nudge on the two outliers puts all four on the
    // same line without moving the hit rectangles.
    const auto icon = [&](const RECT& rect, const wchar_t* glyph, Hot hot, bool danger, int nudge) {
        const bool hovered = m_hot == hot;
        if (hovered)
        {
            FillRounded(dc, rect, 6, danger ? t.dangerWeak : t.hover);
        }

        RECT text = rect;
        text.top += nudge;
        text.bottom += nudge;
        DrawTextBlock(dc, m_fontIcon,
            danger ? (hovered ? t.danger : t.text3) : (hovered ? t.text : t.text3), text, glyph,
            DT_CENTER);
    };

    icon(geometry.iconTheme, L"\u25D0", Hot::IconTheme, false, 0);
    icon(geometry.iconHelp, L"?", Hot::IconHelp, false, 0);
    icon(geometry.iconClose, L"\u00D7", Hot::IconClose, true, -1);

    // The language button shows the effective language (CN / EN), not the mode: "follow system"
    // resolves to one of the two, and a short fixed-width caption never pushes the icons around.
    {
        const bool hovered = m_hot == Hot::IconLanguage;
        const bool pressed = m_pressed == Hot::IconLanguage;
        if (hovered || pressed)
        {
            FillRounded(dc, geometry.iconLanguage, 6, pressed ? t.hover2 : t.hover);
        }
        RECT text = geometry.iconLanguage;
        text.top += 1;
        text.bottom += 1;
        DrawTextBlock(dc, m_fontSmall, (hovered || pressed) ? t.text : t.text2,
            text, util::i18n::LanguageShortLabel(), DT_CENTER | DT_VCENTER);
    }
}

void Panel::PaintTabBar(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    const RECT tabs[4] = { geometry.tabLayout, geometry.tabWallpaper, geometry.tabNotes,
        geometry.tabConfig };
    const std::wstring_view labels[4] = { L"布局", L"壁纸", L"便签", L"配置" };
    const Tab pages[4] = { Tab::Layout, Tab::Wallpaper, Tab::Notes, Tab::Config };

    // The rule the tabs stand on, drawn first so the active one can cover the stretch underneath
    // itself. It spans the content column rather than the window: it is the page's top edge, and a
    // line running into the sheet padding would read as a window divider instead.
    const int ruleY = geometry.tabBar.bottom - 1;
    FillBox(dc, RECT{ geometry.tabBar.left, ruleY, geometry.tabBar.right, ruleY + 1 }, t.line);

    for (int index = 0; index < 4; ++index)
    {
        const bool active = m_tab == pages[index];
        const bool hot = (m_hot == Hot::Tab) && (m_hotIndex == index);
        const bool pressed = (m_pressed == Hot::Tab) && (m_pressedIndex == index);

        // The active tab is the only one with a face; the others are flat labels that light up
        // under the cursor. One treatment, three instances - the tabs are a segmented control,
        // not three buttons of three kinds.
        if (active)
        {
            // Open at the bottom: the fill runs over the rule and the outline has no bottom edge,
            // so the tab and the page below it are one shape - a folder tab, not a chip resting on
            // a line. Rounding the bottom corners here would cut the join this exists to make.
            FillRounded(dc, tabs[index], 7, t.accentWeak, true);
            StrokeRounded(dc, tabs[index], 7, t.accentLine, 0, true);
        }
        else if (hot || pressed)
        {
            FillRounded(dc, tabs[index], 7, pressed ? t.hover2 : t.hover);
        }

        DrawTextBlock(dc, m_fontBody, active ? t.accent2 : (hot ? t.text : t.text3), tabs[index],
            labels[index], DT_CENTER | DT_VCENTER);
    }
}

void Panel::PaintConfigBlock(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    for (const Geometry::ConfigRow& row : geometry.configRows)
    {
        if (row.rect.bottom <= row.rect.top)
        {
            continue;
        }

        if (row.section)
        {
            // A heading, then a hairline to the column's edge. The line is what makes two columns
            // of settings read as four blocks instead of one long list of unrelated rows.
            DrawTextBlock(dc, m_fontAux, t.text4, row.rect, row.label, DT_LEFT | DT_VCENTER);

            const int width = static_cast<int>(MeasureText(m_measure, m_fontAux, row.label).cx);
            const int lineY = (row.rect.top + row.rect.bottom) / 2;
            FillBox(dc, RECT{ row.rect.left + width + 8, lineY, row.rect.right, lineY + 1 }, t.line);
            continue;
        }

        // The row's label column stops short of its control, so the two never meet.
        const RECT label{ row.rect.left + 2, row.rect.top, row.control.right > row.control.left
                    ? row.control.left - 12
                    : row.rect.right - 26,
            row.rect.bottom };

        // Hover only where the click would land: a switch row whose label lit up would look
        // clickable and then ignore the click.
        const bool rowHot = m_hot == row.hot && m_hotIndex == row.index;

        switch (row.kind)
        {
            case ConfigControl::Switch:
                if (rowHot)
                {
                    FillRounded(dc, Inflate(row.control, 3), 6, t.hover);
                }
                PaintSwitch(dc, row.control, row.on, t);
                DrawTextBlock(dc, m_fontBody, t.text2, label, row.label, DT_LEFT | DT_VCENTER);
                break;

            case ConfigControl::Segments:
            {
                FillRounded(dc, row.control, 6, t.card2);
                for (size_t index = 0; index < row.segments.size(); ++index)
                {
                    const bool selected = static_cast<int>(index) == row.selected;
                    const bool segmentHot =
                        m_hot == row.hot && m_hotIndex == static_cast<int>(index);

                    if (selected)
                    {
                        FillRounded(dc, row.segments[index], 5, t.accentWeak);
                    }
                    else if (segmentHot)
                    {
                        FillRounded(dc, row.segments[index], 5, t.hover);
                    }

                    DrawTextBlock(dc, m_fontSmall,
                        selected ? t.accent2 : (segmentHot ? t.text : t.text3), row.segments[index],
                        row.choices[index], DT_CENTER | DT_VCENTER);
                }
                DrawTextBlock(dc, m_fontBody, t.text2, label, row.label, DT_LEFT | DT_VCENTER);
                break;
            }

            case ConfigControl::Hotkey:
            {
                const bool recording = m_recordingAction == row.index;
                if (rowHot || recording)
                {
                    FillRounded(dc, row.rect, 6, t.hover);
                }

                // "Ctrl + Alt + Shift + N" is the widest thing on this page, so the value gets a
                // column of its own rather than whatever the label left over. While recording it
                // takes the rest of the row: the hint is longer than any combination, and a hint
                // cut in half is not a hint.
                const int labelWidth =
                    static_cast<int>(MeasureText(m_measure, m_fontBody, row.label).cx);
                const int afterLabel = label.left + labelWidth + 12;
                const int valueLeft = recording ? afterLabel
                                                : std::max(static_cast<int>(row.rect.right)
                                                          - kConfigHotkeyValueWidth,
                                                      afterLabel);
                DrawTextBlock(dc, m_fontBody, recording ? t.accent2 : t.text2,
                    RECT{ label.left, label.top, valueLeft - 12, label.bottom }, row.label,
                    DT_LEFT | DT_VCENTER);
                DrawTextBlock(dc, m_fontAux, recording ? t.accent : t.text3,
                    RECT{ valueLeft, row.rect.top, row.rect.right, row.rect.bottom }, row.value,
                    DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
                break;
            }

            case ConfigControl::Link:
                if (rowHot)
                {
                    FillRounded(dc, row.rect, 6, t.hover);
                }
                DrawTextBlock(dc, m_fontBody, rowHot ? t.text : t.text2, label, row.label,
                    DT_LEFT | DT_VCENTER);
                DrawTextBlock(dc, m_fontAux, rowHot ? t.text3 : t.text4,
                    RECT{ row.rect.left, row.rect.top, row.rect.right - 2, row.rect.bottom },
                    L"\u203A", DT_RIGHT | DT_VCENTER);
                break;

            case ConfigControl::Button:
                DrawButton(dc, m_measure, t, m_fontBody, row.control, row.label, false, rowHot,
                    m_pressed == row.hot, false, false, m_spinAngle);
                break;
        }
    }
}

void Panel::PaintWallpaperBlock(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;
    const core::Group* active = m_state.ActiveGroup();

    // --- Head ---
    // No title: the tab strip already named the page. The head is the object selector (chips)
    // plus the actions, nothing else.
    for (size_t index = 0; index < geometry.groupChips.size(); ++index)
    {
        const int slot = geometry.chipOrder[index];
        const core::Group& group = m_state.groups[static_cast<size_t>(slot)];
        const std::wstring label = core::GroupLabel(group, m_state.monitors);
        const bool isActive = slot == m_state.activeSlot;
        const bool hot = (m_hot == Hot::GroupChip) && (m_hotIndex == slot);

        const COLORREF tint = t.group[slot % 6];
        const RECT chip = geometry.groupChips[index];

        FillRounded(dc, chip, 8, isActive ? t.accentWeak : (hot ? t.hover : t.card2));
        StrokeRounded(dc, chip, 8, isActive ? Mix(tint, t.accentWeak, 0.45) : t.line2);

        // The pip is the group's identity colour. Besides the outline, it is the only place a
        // group colour is allowed to appear.
        const int pipLeft = chip.left + 9;
        const int pipTop = (chip.top + chip.bottom) / 2 - 3;
        const RECT pip{ pipLeft, pipTop, pipLeft + 7, pipTop + 7 };
        FillRounded(dc, pip, 3, tint);

        if (isActive)
        {
            StrokeRounded(dc, Inflate(pip, 2), 5, Mix(tint, t.accentWeak, 0.3));
        }

        DrawTextBlock(dc, m_fontNum, isActive ? t.text : t.text2,
            RECT{ pipLeft + 7 + 6, chip.top, chip.right - 4, chip.bottom }, label, DT_LEFT);

        const RECT& add = geometry.groupAdds[index];
        if (add.right > add.left)
        {
            FillBox(dc, RECT{ add.left, add.top + 4, add.left + 1, add.bottom - 4 }, t.line);

            const bool addHot = (m_hot == Hot::GroupAdd) && (m_hotIndex == slot);
            if (addHot)
            {
                FillRounded(dc, add, 6, t.hover);
            }

            DrawTextBlock(dc, m_fontBody, addHot ? t.accent : t.text3, add, L"＋", DT_CENTER);
        }
    }

    // --- The block's one primary and its secondary ---
    const bool pickDisabled = m_state.busy || active == nullptr;
    DrawButton(dc, m_measure, t, m_fontBody, geometry.pick,
        (active != nullptr && !active->image.empty()) ? L"更换壁纸" : L"选择壁纸", true,
        m_hot == Hot::Pick, m_pressed == Hot::Pick, pickDisabled, false, m_spinAngle);

    const bool busyCrop = m_state.busy && m_state.busyLabel == L"生成中…";
    const bool applyDisabled = m_state.busy || active == nullptr || active->image.empty();
    DrawButton(dc, m_measure, t, m_fontBody, geometry.applyWallpaper,
        busyCrop ? L"生成中…" : L"应用壁纸", false, m_hot == Hot::ApplyWallpaper,
        m_pressed == Hot::ApplyWallpaper, applyDisabled, busyCrop, m_spinAngle);

    // --- Hint row ---
    DrawTextBlock(dc, m_fontAux, t.text4,
        RECT{ geometry.wallHint.left, geometry.wallHint.top, geometry.rotateBadge.left - 10,
            geometry.wallHint.bottom },
        L"屏上拖动 = 移动 · 滚轮 = 缩放 · 拖圆钮 = 旋转 · 编号 = 改组 · 双击 = 复位",
        DT_LEFT | DT_END_ELLIPSIS);

    // The turn readout, then the button that clears it. The number is printed even at zero: a
    // readout that only appears once something is wrong makes the user look for it.
    const double turned = core::ClampView(m_view).rotation;
    DrawTextBlock(dc, m_fontAuxBold, m_rotating ? t.accent : t.text3, geometry.rotateBadge,
        std::format(L"{}°", static_cast<int>(std::lround(turned))), DT_RIGHT);

    {
        const bool canReset = turned > 0.01;
        const bool resetHot = canReset && m_hot == Hot::ResetRotation;

        if (canReset)
        {
            FillRounded(dc, geometry.resetRotation, 5, resetHot ? t.hover : t.card2);
            StrokeRounded(dc, geometry.resetRotation, 5, resetHot ? t.accentLine : t.line2);
        }

        DrawTextBlock(dc, m_fontAux, canReset ? (resetHot ? t.accent : t.text2) : t.text4,
            geometry.resetRotation, L"复原", DT_CENTER);
    }

    DrawTextBlock(dc, m_fontAuxBold, t.text3, geometry.zoomBadge,
        std::format(L"{}%", static_cast<int>(std::lround(m_view.zoom * 100))), DT_RIGHT);

    if (geometry.dirtyBadge.right > geometry.dirtyBadge.left)
    {
        DrawTextBlock(dc, m_fontAux, t.warn, geometry.dirtyBadge, L"● 未应用", DT_RIGHT);
    }

    DrawTextBlock(dc, m_fontAux, t.text4, geometry.groupMeta,
        util::i18n::Format(L"{} 台 · {} 组", m_state.monitors.size(), m_state.groups.size()),
        DT_RIGHT);

    PaintMap(dc, geometry);

    // --- The wallpaper setting, floating on the map ---
    // Drawn after the map so it sits on top of it. The turn is a two-headed arc pinned outside
    // the selected group's box rather than a button in a column; the calendar keeps its own chip.
    {
        PaintRotateArc(dc, geometry);

        // Desktop calendar switch. Same track and knob as 自动恢复, so there is one switch in
        // the product rather than one per feature - just standing upright here, label under
        // track, inside a small card of its own so it stays readable over any wallpaper.
        if constexpr (kDesktopWidgetsEnabled)
        {
            const RECT& rect = geometry.canvasSwitch;
            const int trackWidth = 30;
            const int trackHeight = 17;
            const int centreX = (rect.left + rect.right) / 2;
            const int blockTop = rect.top + 4;
            const RECT track{ centreX - trackWidth / 2, blockTop, centreX + trackWidth / 2,
                blockTop + trackHeight };

            FillRounded(dc, track, trackHeight / 2, m_state.canvasOn ? t.accent : t.line2);

            const int knobX = m_state.canvasOn ? track.right - 2 - 13 : track.left + 2;
            FillRounded(dc, RECT{ knobX, track.top + 2, knobX + 13, track.top + 15 }, 7,
                RGB(255, 255, 255));

            DrawTextBlock(dc, m_fontBody, m_hot == Hot::CanvasSwitch ? t.accent : t.text2,
                RECT{ rect.left, track.bottom + 2, rect.right, rect.bottom }, L"日历", DT_CENTER);
        }
    }
}

Panel::MapLayout Panel::LayoutMap(const Geometry& geometry) const
{
    MapLayout map;
    if (m_state.monitors.empty())
    {
        return map;
    }

    map.projection = Fit(m_state.monitors, geometry.mapArea);
    map.valid = map.projection.canvas.IsValid()
             && map.projection.drawn.right > map.projection.drawn.left;
    return map;
}

RECT Panel::MonitorRectOnMap(const MapLayout& map,
    const std::vector<core::MonitorSnapshot>& monitors, size_t index) const
{
    if (!map.valid || index >= monitors.size())
    {
        return RECT{};
    }

    return map.projection.ToClient(monitors[index].rect);
}

RECT Panel::GroupBoxOnMap(const MapLayout& map, const core::Group& group) const
{
    const std::vector<size_t> members = core::AttachedMembers(group, m_state.monitors);
    if (members.empty())
    {
        return RECT{};
    }

    int left = INT_MAX;
    int top = INT_MAX;
    int right = INT_MIN;
    int bottom = INT_MIN;

    for (const size_t index : members)
    {
        const RECT box = MonitorRectOnMap(map, m_state.monitors, index);
        left = std::min<int>(left, box.left);
        top = std::min<int>(top, box.top);
        right = std::max<int>(right, box.right);
        bottom = std::max<int>(bottom, box.bottom);
    }

    if (left >= right || top >= bottom)
    {
        return RECT{};
    }

    return RECT{ left, top, right, bottom };
}

core::CanvasRect Panel::ShownCanvas() const
{
    // During a drag the live position is here rather than in the state: the map has to follow the
    // cursor, and the settings file is only written once, on release.
    return m_canvasLive ? m_canvasDrag : m_state.canvas;
}

RECT Panel::CanvasRectOnMap(const MapLayout& map) const
{
    if (!map.valid)
    {
        return RECT{};
    }

    const core::CanvasRect canvas = ShownCanvas();
    const core::Rect box{ canvas.x, canvas.y, canvas.size, canvas.size };
    return map.projection.ToClient(box);
}

bool Panel::MonitorTakesDrag(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_state.monitors.size()))
    {
        return false;
    }

    const int owner =
        core::GroupOf(m_state.groups, m_state.monitors[static_cast<size_t>(index)].devicePath);
    return owner >= 0 && owner < static_cast<int>(m_state.groups.size())
        && !m_state.groups[static_cast<size_t>(owner)].image.empty();
}

void Panel::DrawGroupOutline(HDC dc, const MapLayout& map, const core::Group& group,
    COLORREF color, int offset, int thickness, double alpha) const
{
    // Two painters: opaque bars for the frame, blended ones for the halo.
    const auto bar = [dc, color, alpha](const RECT& rect)
    {
        if (alpha >= 1.0)
        {
            FillBox(dc, rect, color);
        }
        else if (g_paintTarget != nullptr && g_paintTarget->surface.valid)
        {
            // The bar is in design units; the blend works on the device pixels underneath.
            BlendDeviceRect(g_paintTarget->surface,
                ToDeviceRect(g_paintTarget->surface, rect), color, alpha);
        }
    };

    const std::vector<size_t> members = core::AttachedMembers(group, m_state.monitors);
    if (members.empty() || !map.valid || thickness <= 0)
    {
        return;
    }

    std::vector<RECT> boxes;
    boxes.reserve(members.size());
    for (const size_t index : members)
    {
        boxes.push_back(MonitorRectOnMap(map, m_state.monitors, index));
    }

    // The parts of [from, to) that no neighbour already owns.
    const auto gaps = [](int from, int to, std::vector<std::pair<int, int>> covered)
    {
        std::vector<std::pair<int, int>> out;
        std::sort(covered.begin(), covered.end());

        int cursor = from;
        for (const auto& [a, b] : covered)
        {
            if (b <= cursor || a >= to)
            {
                continue;
            }

            if (a > cursor)
            {
                out.emplace_back(cursor, std::min(a, to));
            }

            cursor = std::max(cursor, b);
            if (cursor >= to)
            {
                break;
            }
        }

        if (cursor < to)
        {
            out.emplace_back(cursor, to);
        }

        return out;
    };

    for (size_t index = 0; index < boxes.size(); ++index)
    {
        const RECT& box = boxes[index];

        // Which parts of each edge another member of this same group continues. Only members
        // count: the boundary of a group runs against everything else, including other groups'
        // screens, which is exactly what makes "these two share a picture" visible.
        std::vector<std::pair<int, int>> top;
        std::vector<std::pair<int, int>> bottom;
        std::vector<std::pair<int, int>> left;
        std::vector<std::pair<int, int>> right;

        for (size_t other = 0; other < boxes.size(); ++other)
        {
            if (other == index)
            {
                continue;
            }

            const RECT& neighbour = boxes[other];

            const int overlapLeft = std::max(static_cast<int>(box.left),
                static_cast<int>(neighbour.left));
            const int overlapRight = std::min(static_cast<int>(box.right),
                static_cast<int>(neighbour.right));
            const int overlapTop = std::max(static_cast<int>(box.top),
                static_cast<int>(neighbour.top));
            const int overlapBottom = std::min(static_cast<int>(box.bottom),
                static_cast<int>(neighbour.bottom));

            if (overlapRight > overlapLeft)
            {
                if (neighbour.bottom == box.top)
                {
                    top.emplace_back(overlapLeft, overlapRight);
                }
                if (neighbour.top == box.bottom)
                {
                    bottom.emplace_back(overlapLeft, overlapRight);
                }
            }

            if (overlapBottom > overlapTop)
            {
                if (neighbour.right == box.left)
                {
                    left.emplace_back(overlapTop, overlapBottom);
                }
                if (neighbour.left == box.right)
                {
                    right.emplace_back(overlapTop, overlapBottom);
                }
            }
        }

        // The bar starts `offset` away from the screen edge and grows *away* from it, so the
        // outline never touches the picture and selecting a group thickens the line outwards
        // instead of into the screen. Two flush groups then read as two lines with the
        // neighbour's picture between them, rather than as one fat two-coloured band.
        // (`near` and `far` are macros from the 16-bit era, still defined by the Windows headers.)
        const int inner = offset;
        const int outer = offset + thickness;
        const int extend = outer;

        for (const auto& [from, to] : gaps(box.left, box.right, top))
        {
            bar(RECT{ from - extend, box.top - outer, to + extend, box.top - inner });
        }

        for (const auto& [from, to] : gaps(box.left, box.right, bottom))
        {
            bar(RECT{ from - extend, box.bottom + inner, to + extend, box.bottom + outer });
        }

        for (const auto& [from, to] : gaps(box.top, box.bottom, left))
        {
            bar(RECT{ box.left - outer, from - extend, box.left - inner, to + extend });
        }

        for (const auto& [from, to] : gaps(box.top, box.bottom, right))
        {
            bar(RECT{ box.right + inner, from - extend, box.right + outer, to + extend });
        }
    }
}

int Panel::MonitorAt(POINT point) const
{
    const Geometry g = ComputeGeometry();
    if (m_state.monitors.empty() || !Contains(g.mapCard, point))
    {
        return -1;
    }

    const MapLayout map = LayoutMap(g);
    if (!map.valid)
    {
        return -1;
    }

    // Backwards, so the display drawn on top wins where two rectangles overlap - a mirrored
    // arrangement is the only way they can.
    for (int index = static_cast<int>(m_state.monitors.size()) - 1; index >= 0; --index)
    {
        if (Contains(MonitorRectOnMap(map, m_state.monitors, static_cast<size_t>(index)), point))
        {
            return index;
        }
    }

    return -1;
}

void Panel::PaintWholePicture(
    HDC dc, const Geometry& geometry, const MapLayout& map, int slot, bool coarse)
{
    const core::Group& group = m_state.groups[static_cast<size_t>(slot)];
    if (group.image.empty())
    {
        return;
    }

    const MapContext context = ContextForGroup(map.projection, group, m_view);
    const MapFrame frame = FrameFor(context, m_view);
    const SIZE size = PreviewSize(context);
    if (!context.IsValid() || size.cx <= 0 || size.cy <= 0 || frame.scale <= 0.0)
    {
        return;
    }

    const HBITMAP surface =
        PreviewFor(slot, group.image, context.rotation, size.cx, size.cy, coarse);
    if (surface == nullptr)
    {
        return;
    }

    // The panel's own pixels: the clip below is built in device units, and the mask is blended
    // straight into the buffer. Both entry points (Paint and PaintContent) publish it.
    PixelSurface* target = (g_paintTarget != nullptr) ? &g_paintTarget->surface : nullptr;

    // Where the surface's top-left lands. The canvas sits `offset` into the surface - the same
    // number SampleFor uses to pick each display's window out of this very bitmap, so the spill
    // and the screens can never disagree about where the picture is.
    const core::WallpaperView clamped = core::ClampView(m_view);
    const core::FrameOffset offset = core::FrameOffsetFor(frame.surface, clamped);
    const int offsetX = offset.x;
    const int offsetY = offset.y;
    const RECT canvas = map.projection.ToClient(context.canvas);

    // A bitmap pixel is kMapSupersample times denser than the map, and a zoomed picture covers
    // that many more map pixels per surface pixel - so the drawn size follows zoom directly.
    const double perPixel = frame.zoom / kMapSupersample;
    RECT dest{};
    dest.left = canvas.left - static_cast<int>(std::lround(offsetX * frame.scale));
    dest.top = canvas.top - static_cast<int>(std::lround(offsetY * frame.scale));
    dest.right = dest.left + static_cast<int>(std::lround(size.cx * perPixel));
    dest.bottom = dest.top + static_cast<int>(std::lround(size.cy * perPixel));

    // The picture inside the surface is the source rectangle, scaled and turned about the
    // surface's centre. A point is picture when it survives the turn back into source axes, which
    // is one rotation of a vector rather than an inverse mapping of every pixel.
    const double radians = clamped.rotation * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double halfWidth = context.source.width * frame.surface.scale / 2.0;
    const double halfHeight = context.source.height * frame.surface.scale / 2.0;
    const double centreX = frame.surface.scaledWidth / 2.0 - offsetX;
    const double centreY = frame.surface.scaledHeight / 2.0 - offsetY;

    // The surface's four corners hold matte, not picture: the turn fills them with the average of
    // the source's own corners, and cover-framing is what guarantees no display ever sees them. The
    // spill would, and a pale wedge of averaged corner colour reads as part of the wallpaper - so
    // the blit is clipped to the picture's quad and the wedge stays board.
    const auto corner = [&](double u, double v) {
        return POINT{ static_cast<LONG>(std::lround(
                          canvas.left + (centreX + u * cosine - v * sine) * frame.scale)),
            static_cast<LONG>(std::lround(
                canvas.top + (centreY + u * sine + v * cosine) * frame.scale)) };
    };

    const POINT quad[4] = { corner(-halfWidth, -halfHeight), corner(halfWidth, -halfHeight),
        corner(halfWidth, halfHeight), corner(-halfWidth, halfHeight) };

    {
        const int saved = ::SaveDC(dc);
        ::IntersectClipRect(dc, geometry.mapCard.left + 1, geometry.mapCard.top + 1,
            geometry.mapCard.right - 1, geometry.mapCard.bottom - 1);

        // A clip region is in device units whatever the mapping mode, so the quad is converted by
        // hand rather than left to GDI's design-to-device transform.
        HRGN picture = nullptr;
        if (target != nullptr && target->valid)
        {
            POINT deviceQuad[4];
            for (int index = 0; index < 4; ++index)
            {
                deviceQuad[index].x = static_cast<LONG>(std::lround(quad[index].x * target->scaleX));
                deviceQuad[index].y = static_cast<LONG>(std::lround(quad[index].y * target->scaleY));
            }
            picture = ::CreatePolygonRgn(deviceQuad, 4, ALTERNATE);
        }

        if (picture != nullptr)
        {
            ::ExtSelectClipRgn(dc, picture, RGN_AND);
        }

        const HDC memory = ::CreateCompatibleDC(dc);
        const HGDIOBJ previous = ::SelectObject(memory, surface);
        ::SetStretchBltMode(dc, HALFTONE);
        ::SetBrushOrgEx(dc, map.projection.drawn.left, map.projection.drawn.top, nullptr);
        ::StretchBlt(dc, dest.left, dest.top, dest.right - dest.left, dest.bottom - dest.top, memory,
            0, 0, size.cx, size.cy, SRCCOPY);
        ::SelectObject(memory, previous);
        ::DeleteDC(memory);

        if (picture != nullptr)
        {
            ::SelectClipRgn(dc, nullptr);
            ::DeleteObject(picture);
        }

        ::RestoreDC(dc, saved);
    }

    if (target == nullptr || !target->valid)
    {
        return;
    }

    // --- The mask ---
    // Everything the group's displays will not receive is dimmed by half. Device pixels, not
    // design ones: BlendPixel indexes the panel's own 32bpp buffer, and rounding the rotated test
    // through a second coordinate space would only add edges to find.
    std::vector<RECT> screens;
    const std::vector<size_t> members = core::AttachedMembers(group, m_state.monitors);
    screens.reserve(members.size());
    for (const size_t index : members)
    {
        screens.push_back(ToDeviceRect(*target, MonitorRectOnMap(map, m_state.monitors, index)));
    }

    const RECT card = ToDeviceRect(*target, geometry.mapCard);
    const RECT box = ToDeviceRect(*target, dest);
    const int left = std::max(std::max(static_cast<int>(card.left) + 1, static_cast<int>(box.left)), 0);
    const int top = std::max(std::max(static_cast<int>(card.top) + 1, static_cast<int>(box.top)), 0);
    const int right = std::min(
        std::min(static_cast<int>(card.right) - 1, static_cast<int>(box.right)), target->width);
    const int bottom = std::min(
        std::min(static_cast<int>(card.bottom) - 1, static_cast<int>(box.bottom)), target->height);
    if (left >= right || top >= bottom)
    {
        return;
    }

    constexpr double kMaskAlpha = 0.5;
    constexpr COLORREF kMaskColour = RGB(0, 0, 0);

    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            bool onScreen = false;
            for (const RECT& screen : screens)
            {
                if (x >= screen.left && x < screen.right && y >= screen.top && y < screen.bottom)
                {
                    onScreen = true;
                    break;
                }
            }

            if (onScreen)
            {
                continue;
            }

            const double canvasX = (x / target->scaleX - canvas.left) / frame.scale;
            const double canvasY = (y / target->scaleY - canvas.top) / frame.scale;
            const double dx = canvasX - centreX;
            const double dy = canvasY - centreY;

            if (std::abs(dx * cosine + dy * sine) > halfWidth
                || std::abs(-dx * sine + dy * cosine) > halfHeight)
            {
                continue;
            }

            BlendPixel(*target, x, y, kMaskColour, kMaskAlpha);
        }
    }
}

void Panel::PaintMap(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    // The map is a board the screens sit on, not a card with a fill: the grid says "this is a
    // canvas", and the screens cover it entirely wherever a picture exists.
    FillRounded(dc, geometry.mapCard, 9, t.mapBg);

    {
        const int saved = ::SaveDC(dc);
        ::IntersectClipRect(dc, geometry.mapCard.left + 1, geometry.mapCard.top + 1,
            geometry.mapCard.right - 1, geometry.mapCard.bottom - 1);

        for (int x = geometry.mapCard.left + kGridStep; x < geometry.mapCard.right;
             x += kGridStep)
        {
            FillBox(dc, RECT{ x, geometry.mapCard.top, x + 1, geometry.mapCard.bottom }, t.mapGrid);
        }
        for (int y = geometry.mapCard.top + kGridStep; y < geometry.mapCard.bottom;
             y += kGridStep)
        {
            FillBox(dc, RECT{ geometry.mapCard.left, y, geometry.mapCard.right, y + 1 }, t.mapGrid);
        }

        ::RestoreDC(dc, saved);
    }

    StrokeRounded(dc, geometry.mapCard, 9, t.line);

    if (m_state.monitors.empty())
    {
        DrawTextBlock(dc, m_fontBody, t.text4, geometry.mapCard, L"没有检测到显示器", DT_CENTER);
        return;
    }

    const MapLayout map = LayoutMap(geometry);
    if (!map.valid)
    {
        return;
    }

    // A gesture in flight shows the whole picture, so the user can see what the cover fit is
    // enlarging and what it pushes off every screen - or, while moving, how much picture is left
    // in each direction. Painted first, under every display.
    const bool wholePicture = (m_rotating || m_panning) && m_state.activeSlot >= 0
                           && m_state.activeSlot < static_cast<int>(m_state.groups.size());
    if (wholePicture)
    {
        // Only a turn changes the angle frame by frame, so only a turn needs the coarse decode.
        // A move keeps the rotation and the zoom, which is exactly the pair the display loop's own
        // bitmap was cached on - full resolution, and no second decode to pay for.
        PaintWholePicture(dc, geometry, map, m_state.activeSlot, m_rotating);
    }

    for (int slot = 0; slot < static_cast<int>(m_state.groups.size()); ++slot)
    {
        const core::Group& group = m_state.groups[static_cast<size_t>(slot)];
        const std::vector<size_t> members = core::AttachedMembers(group, m_state.monitors);
        if (members.empty())
        {
            continue;
        }

        // During a drag the live framing is in m_view, not in the model yet - the angle included.
        const core::WallpaperView view =
            (slot == m_state.activeSlot && m_viewSlot == slot) ? m_view : group.view;

        // One bitmap per group, decoded for that group's own canvas. Handing every group the
        // whole desktop's canvas would show the same crop in every group, which is exactly what
        // per-group framing exists to avoid.
        const MapContext context = ContextForGroup(map.projection, group, view);
        const MapFrame frame = FrameFor(context, view);
        const SIZE previewSize = PreviewSize(context);
        const bool coarse = m_rotating && slot == m_state.activeSlot;
        const HBITMAP preview = group.image.empty()
            ? nullptr
            : PreviewFor(slot, group.image, context.rotation, previewSize.cx, previewSize.cy,
                coarse);

        HDC memory = nullptr;
        HGDIOBJ previousBitmap = nullptr;
        if (preview != nullptr)
        {
            memory = ::CreateCompatibleDC(dc);
            previousBitmap = ::SelectObject(memory, preview);
            ::SetStretchBltMode(dc, HALFTONE);
            ::SetBrushOrgEx(dc, map.projection.drawn.left, map.projection.drawn.top, nullptr);
        }

        for (const size_t index : members)
        {
            const RECT box =
                Clip(MonitorRectOnMap(map, m_state.monitors, index), map.projection.drawn);
            const int width = box.right - box.left;
            const int height = box.bottom - box.top;
            if (width <= 0 || height <= 0)
            {
                continue;
            }

            if (memory == nullptr || !context.IsValid())
            {
                FillBox(dc, box, t.monitorFlat);
                DrawTextBlock(dc, m_fontAux, Mix(t.text, t.monitorFlat, 0.55), box,
                    L"未选图片", DT_CENTER);
                continue;
            }

            // The sample is asked for in canvas pixels, relative to the group's canvas; the
            // destination is the already-rounded client rectangle, so screens keep tiling without
            // seams no matter how the two roundings differ.
            const core::MonitorSnapshot& snapshot = m_state.monitors[index];
            const core::Rect slotRect{ snapshot.rect.left - context.canvas.left,
                snapshot.rect.top - context.canvas.top, snapshot.rect.width,
                snapshot.rect.height };
            const MapSample sample = SampleFor(frame, slotRect, view);

            if (sample.width <= 0 || sample.height <= 0)
            {
                FillBox(dc, box, t.monitorFlat);
                continue;
            }

            ::StretchBlt(dc, box.left, box.top, width, height, memory, sample.x, sample.y,
                sample.width, sample.height, SRCCOPY);
        }

        if (memory != nullptr)
        {
            ::SelectObject(memory, previousBitmap);
            ::DeleteDC(memory);
        }
    }

    // Group outlines, after every picture: one line per group, drawn *outside* the screens so no
    // wallpaper pixel is ever covered by a group colour (I1, I2). The line follows the union of
    // the members rather than their bounding box - an L-shaped group's box would otherwise draw
    // a run of colour straight across the screens sitting beside it.
    for (int slot = 0; slot < static_cast<int>(m_state.groups.size()); ++slot)
    {
        const core::Group& group = m_state.groups[static_cast<size_t>(slot)];
        const bool selected = slot == m_state.activeSlot;
        const COLORREF tint = t.group[slot % 6];

        const int thickness = FrameThickness(selected);

        if (selected)
        {
            // A 2-unit halo at 16%, tucked against the outside of the frame - it costs no alpha
            // because it is mixed into the board colour.
            DrawGroupOutline(dc, map, group, tint, kFrameGap + thickness, kFrameGlow, 0.16);
        }

        DrawGroupOutline(dc, map, group, selected ? tint : Mix(tint, t.mapBg, 0.58), kFrameGap,
            thickness);
    }

    // The desktop calendar, over the pictures but under the screen numbers: the numbers are the
    // map's click targets and a widget that hid one would make a screen unreachable. Compiled out
    // while the widget is shelved, together with the switch that would raise it.
    if (kDesktopWidgetsEnabled && m_state.canvasOn)
    {
        const RECT box = CanvasRectOnMap(map);
        if (box.right > box.left && box.bottom > box.top)
        {
            const bool hot = m_hot == Hot::CanvasBody;

            // Inset by one unit so the ring sits inside the square the widget actually occupies.
            FillCircle(dc, box, RGB(0x17, 0x18, 0x1C));
            StrokeCircle(dc, box, hot || m_draggingCanvas ? t.accent : RGB(0xC8, 0xA9, 0x6B),
                hot || m_draggingCanvas ? 2 : 1);

            const int centreX = (box.left + box.right) / 2;
            const int centreY = (box.top + box.bottom) / 2;
            const int dot = std::max(2, static_cast<int>(box.right - box.left) / 12);
            FillCircle(dc, RECT{ centreX - dot, centreY - dot, centreX + dot, centreY + dot },
                RGB(0xF7, 0xF3, 0xEA));
        }
    }

    // Screen numbers last: they are the only click target inside the map, and they have to stay
    // readable over any picture.
    for (int index = 0; index < static_cast<int>(m_state.monitors.size()); ++index)
    {
        DrawScreenBadge(dc, MonitorRectOnMap(map, m_state.monitors, static_cast<size_t>(index)),
            index);
    }
}

void Panel::DrawScreenBadge(HDC dc, const RECT& box, int index)
{
    const BadgeLayout layout = LayoutBadge(box, index);
    if (layout.rect.right <= layout.rect.left)
    {
        return;
    }

    const RECT& badge = layout.rect;
    const core::MonitorSnapshot& monitor = m_state.monitors[static_cast<size_t>(index)];
    const std::wstring number =
        std::to_wstring(core::ScreenNumber(monitor.devicePath, m_state.monitors));

    // Brand blue at 90%, white ink: the plate is unmistakably part of this app rather than a
    // frosted label sitting on the picture, and white on that blue clears 4.5:1 on both themes.
    // A fixed blue rather than the theme accent - the dark theme's accent is too light to carry
    // white text.
    const bool hot = (m_hot == Hot::ScreenBadge) && (m_hotIndex == index);
    BlendRounded(m_surface, badge, kBadgeRadius, kBadgeBlue, hot ? 1.0 : 0.90);

    // Vertically centred rather than offset from the top: both halves share one font, so one
    // centring rule puts them on the same baseline at every scale factor.
    const int numberLeft = badge.left + kBadgePaddingX;

    DrawTextBlock(dc, m_fontSmall, RGB(255, 255, 255),
        RECT{ numberLeft, badge.top, badge.right, badge.bottom }, number, DT_LEFT);

    if (!layout.withResolution)
    {
        return;
    }

    const int numberWidth = static_cast<int>(MeasureText(m_measure, m_fontSmall, number).cx);
    DrawTextBlock(dc, m_fontSmall, RGB(222, 230, 246),
        RECT{ numberLeft + numberWidth + kBadgeResolutionGap, badge.top, badge.right,
            badge.bottom },
        std::format(L"{}×{}", monitor.rect.width, monitor.rect.height),
        DT_LEFT | DT_END_ELLIPSIS);
}

void Panel::PaintLayoutBlock(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    // Screen chips: numbers, not buttons. An unselected chip has no border and no fill, which is
    // what makes the row read as a legend rather than as four toggles. No title - the tab strip
    // already named the page.
    for (size_t index = 0; index < geometry.screenChips.size(); ++index)
    {
        const int monitorIndex = geometry.chipMonitors[index];
        const core::MonitorSnapshot& monitor = m_state.monitors[static_cast<size_t>(monitorIndex)];
        const RECT chip = geometry.screenChips[index];

        bool on = false;
        for (const auto& selected : m_filter)
        {
            if (core::Monitor::IEquals(selected, monitor.devicePath))
            {
                on = true;
                break;
            }
        }

        const bool hot = (m_hot == Hot::ScreenChip) && (m_hotIndex == monitorIndex);
        if (on)
        {
            FillRounded(dc, chip, 6, t.accentWeak);
        }
        else if (hot)
        {
            FillRounded(dc, chip, 6, t.hover);
        }

        int count = 0;
        for (const auto& window : m_state.windows)
        {
            if (core::Monitor::IEquals(window.monitorPath, monitor.devicePath))
            {
                ++count;
            }
        }

        const std::wstring number =
            std::to_wstring(core::ScreenNumber(monitor.devicePath, m_state.monitors));

        const int numberLeft = chip.left + 9;
        DrawTextBlock(dc, m_fontNum, on ? t.accent : t.text2,
            RECT{ numberLeft, chip.top, chip.right, chip.bottom }, number, DT_LEFT);

        const int numberWidth = static_cast<int>(MeasureText(m_measure, m_fontNum, number).cx);
        DrawTextBlock(dc, m_fontSmall, on ? Mix(t.accent, t.accentWeak, 0.72) : t.text4,
            RECT{ numberLeft + numberWidth + 4, chip.top + 1, chip.right, chip.bottom },
            std::format(L"({})", count), DT_LEFT);
    }

    // Auto restore: the only switch in the panel, and it belongs to this block because it is
    // about layouts being re-applied, not about pictures.
    {
        const RECT& rect = geometry.autoSwitch;
        const int trackWidth = 30;
        const int trackHeight = 17;
        const int trackTop = (rect.top + rect.bottom) / 2 - trackHeight / 2;
        const RECT track{ rect.left, trackTop, rect.left + trackWidth, trackTop + trackHeight };

        FillRounded(dc, track, trackHeight / 2, m_state.autoLayout ? t.accent : t.line2);

        const int knobX = m_state.autoLayout ? track.right - 2 - 13 : track.left + 2;
        FillRounded(dc, RECT{ knobX, track.top + 2, knobX + 13, track.top + 15 }, 7,
            RGB(255, 255, 255));

        DrawTextBlock(dc, m_fontBody, t.text2,
            RECT{ track.right + 6, rect.top, rect.right, rect.bottom }, L"自动恢复", DT_LEFT);
    }

    const bool busyCapture = m_state.busy && m_state.busyLabel == L"获取中…";
    DrawButton(dc, m_measure, t, m_fontBody, geometry.refresh,
        busyCapture ? L"获取中…" : L"获取布局", true, m_hot == Hot::Refresh,
        m_pressed == Hot::Refresh, m_state.busy, busyCapture, m_spinAngle);

    DrawButton(dc, m_measure, t, m_fontBody, geometry.applyLayout, L"恢复布局", false,
        m_hot == Hot::ApplyLayout, m_pressed == Hot::ApplyLayout,
        m_state.busy || m_state.windows.empty(), false, m_spinAngle);

    // --- Hint row ---
    DrawTextBlock(dc, m_fontAux, t.text4,
        RECT{ geometry.layoutHint.left, geometry.layoutHint.top, geometry.snapshotMeta.left - 10,
            geometry.layoutHint.bottom },
        L"勾选框 = 记住窗口 · 滚动条 = 翻页", DT_LEFT | DT_END_ELLIPSIS);

    DrawTextBlock(dc, m_fontAux, t.text4, geometry.snapshotMeta,
        m_state.snapshotAt.empty()
            ? std::wstring{}
            : util::i18n::Format(L"快照 {} · 不是实时桌面", m_state.snapshotAt),
        DT_RIGHT);

    const std::vector<int> rows = VisibleRows();
    DrawTextBlock(dc, m_fontAux, t.text4, geometry.windowMeta,
        m_filter.empty()
            ? util::i18n::Format(L"{} 个窗口", m_state.windows.size())
            : util::i18n::Format(L"{} / {} 个窗口", rows.size(), m_state.windows.size()),
        DT_RIGHT);

    PaintWindowList(dc, geometry);
}

void Panel::PaintWindowList(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    FillRounded(dc, geometry.listCard, 9, t.bg);
    StrokeRounded(dc, geometry.listCard, 9, t.line);

    if (geometry.listRows == 0)
    {
        DrawTextBlock(dc, m_fontBody, t.text4, geometry.listCard,
            m_state.windows.empty() ? L"还没有记录" : L"这台屏暂时没有记录的窗口", DT_CENTER);
        return;
    }

    const std::vector<int> rows = VisibleRows();

    // 156 units for the process name and 138 for the position, both fixed: titles then line up
    // down the list, which is what makes a dozen rows scannable at a glance.
    const int appWidth = 156;
    const int whereWidth = 138;

    for (int slot = 0; slot < geometry.listRows; ++slot)
    {
        const int position = geometry.firstRow + slot;
        if (position >= static_cast<int>(rows.size()))
        {
            break;
        }

        const int index = rows[static_cast<size_t>(position)];
        const WindowRow& window = m_state.windows[static_cast<size_t>(index)];

        const RECT row{ geometry.listArea.left, geometry.listArea.top + slot * kRowHeight,
            geometry.listArea.right, geometry.listArea.top + (slot + 1) * kRowHeight };

        const bool hot =
            ((m_hot == Hot::RowCheck) || (m_hot == Hot::RowBody)) && m_hotIndex == index;

        if (hot)
        {
            FillBox(dc, row, t.hover);
        }

        // Tick hit area: 30x30, the only square that changes the tick.
        const RECT tickHit{ row.left, row.top + 1, row.left + 30, row.top + 31 };
        if (hot)
        {
            FillRounded(dc, Inflate(tickHit, -2), 6, t.hover2);
        }

        DrawCheckbox(dc, RECT{ row.left + 6, row.top + 8, row.left + 23, row.top + 25 },
            window.enabled, (m_hot == Hot::RowCheck) && m_hotIndex == index);

        const COLORREF titleInk = window.enabled ? t.text : t.text4;
        const COLORREF appInk = window.enabled ? t.text4 : Mix(t.text4, t.bg, 0.6);

        const int titleLeft = row.left + 39;
        const int appLeft = row.right - 11 - whereWidth - appWidth;
        const int whereLeft = row.right - 11 - whereWidth;

        DrawTextBlock(dc, m_fontBody, titleInk,
            RECT{ titleLeft, row.top, appLeft - 9, row.bottom }, window.title,
            DT_LEFT | DT_END_ELLIPSIS);

        DrawTextBlock(dc, m_fontAux, appInk, RECT{ appLeft, row.top, whereLeft, row.bottom },
            window.app, DT_LEFT | DT_END_ELLIPSIS);

        // "屏 1 · 最大化": the number in the accent colour, because which screen a window belongs
        // to is the one thing the list is scanned for.
        const std::wstring label =
            util::i18n::Format(L"屏 {}",
                core::ScreenNumber(window.monitorPath, m_state.monitors));
        const int labelWidth = static_cast<int>(MeasureText(m_measure, m_fontAuxBold, label).cx);

        DrawTextBlock(dc, m_fontAuxBold,
            window.enabled ? t.accent : Mix(t.accent, t.bg, 0.45),
            RECT{ whereLeft, row.top, whereLeft + labelWidth, row.bottom }, label, DT_LEFT);

        DrawTextBlock(dc, m_fontAux, window.enabled ? t.text3 : Mix(t.text3, t.bg, 0.5),
            RECT{ whereLeft + labelWidth, row.top, row.right - 11, row.bottom },
            L" · " + std::wstring{ util::i18n::Text(window.where) },
            DT_LEFT | DT_END_ELLIPSIS);

        if (slot + 1 < geometry.listRows)
        {
            FillBox(dc, RECT{ row.left, row.bottom - 1, row.right, row.bottom }, t.line);
        }
    }

    PaintScrollbar(dc, geometry);
}

void Panel::DrawCheckbox(HDC dc, const RECT& box, bool checked, bool hot) const
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    if (!checked)
    {
        FillRounded(dc, box, 5, t.card2);
        StrokeRounded(dc, box, 5, hot ? t.accent : t.line2);
        return;
    }

    FillRounded(dc, box, 5, hot ? t.accent2 : t.accent);

    const HPEN pen = ::CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    ::MoveToEx(dc, box.left + 4, box.top + 9, nullptr);
    ::LineTo(dc, box.left + 7, box.top + 13);
    ::LineTo(dc, box.left + 14, box.top + 5);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void Panel::PaintScrollbar(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;
    const int total = static_cast<int>(VisibleRows().size());
    if (total <= geometry.listRows || geometry.listRows <= 0)
    {
        return;
    }

    const RECT track{ geometry.listCard.right - 5, geometry.listArea.top,
        geometry.listCard.right - 3, geometry.listArea.bottom };
    const int trackHeight = track.bottom - track.top;
    if (trackHeight <= 0)
    {
        return;
    }

    FillRounded(dc, track, 1, t.line);

    const int thumbHeight =
        std::max(18, trackHeight * geometry.listRows / std::max(1, total));
    const int maxScroll = total - geometry.listRows;
    const int travel = trackHeight - thumbHeight;
    const int offset =
        (maxScroll > 0) ? travel * std::clamp(m_scroll, 0, maxScroll) / maxScroll : 0;

    FillRounded(dc,
        RECT{ track.left, track.top + offset, track.right, track.top + offset + thumbHeight }, 1,
        t.line2);
}

void Panel::PaintFooter(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    FillBox(dc,
        RECT{ geometry.footer.left, geometry.footer.top, geometry.footer.right,
            geometry.footer.top + 1 },
        t.line);

    COLORREF dot = t.ok;
    if (m_state.busy)
    {
        dot = t.warn;
    }
    else if (m_state.statusIsError)
    {
        dot = t.danger;
    }

    if (!m_state.status.empty() || m_state.busy)
    {
        FillRounded(dc, geometry.statusDot, 3, dot);
    }

    DrawTextBlock(dc, m_fontAux, m_state.statusIsError ? t.danger : t.text3, geometry.statusText,
        m_state.busy
            ? util::i18n::Format(L"正在处理… {}", util::i18n::Text(m_state.busyLabel))
            : m_state.status,
        DT_LEFT | DT_END_ELLIPSIS);

    DrawButton(dc, m_measure, t, m_fontBody, geometry.quit, L"退出", false, m_hot == Hot::Quit,
        m_pressed == Hot::Quit, false, false, m_spinAngle);
}

void Panel::PaintNotesHead(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    // No title: the tab strip already named the page. Just the actions, right-aligned. The
    // palette comes first because it is a page-wide setting; the other two act on one note each.
    DrawButton(dc, m_measure, t, m_fontBody, geometry.noteColors, L"配色", false,
        m_hot == Hot::NoteColors, m_pressed == Hot::NoteColors, m_state.busy, false, m_spinAngle);

    DrawButton(dc, m_measure, t, m_fontBody, geometry.newNote, L"新建便签", true,
        m_hot == Hot::NewNote, m_pressed == Hot::NewNote, m_state.busy, false, m_spinAngle);

    DrawButton(dc, m_measure, t, m_fontBody, geometry.showNotes, L"显示所有便签", false,
        m_hot == Hot::ShowNotes, m_pressed == Hot::ShowNotes, m_state.busy, false, m_spinAngle);
}

/// Cleans a stored preview for display.
///
/// Notes archived before the preview learned its manners still carry control characters: U+FFFC
/// (an embedded object) renders as an empty box, and U+000B (a Shift+Enter soft break) as
/// another. Breaks fold into spaces like fresh previews do; object placeholders are dropped, and
/// a preview that was nothing but a placeholder reads as blank, which is the truth about that
/// note.
std::wstring CleanPreview(const std::wstring& preview)
{
    std::wstring clean;
    clean.reserve(preview.size());
    for (const wchar_t ch : preview)
    {
        if (ch == L'\uFFFC')
        {
            continue;
        }
        if (ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == L'\u000B' || ch == L'\u000C')
        {
            if (!clean.empty() && clean.back() != L' ')
            {
                clean.push_back(L' ');
            }
            continue;
        }
        clean.push_back(ch);
    }

    while (!clean.empty() && clean.back() == L' ')
    {
        clean.pop_back();
    }
    return clean;
}

void Panel::PaintArchive(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    FillRounded(dc, geometry.archiveCard, 8, t.card);
    StrokeRounded(dc, geometry.archiveCard, 8, t.line);

    const int count = static_cast<int>(m_state.archivedNotes.size());
    const std::wstring head = count <= 0
        ? std::wstring{ L"便签历史" }
        : util::i18n::Format(L"便签历史 · {} 条", count);

    // Above the card, in the same small type as the layout page's count line. The pager already
    // says which slice is showing, so the title does not repeat "显示 1-9" as well.
    const int headRight = geometry.archivePage.left > geometry.archiveMeta.left
        ? geometry.archivePage.left - 8
        : geometry.archiveMeta.right;
    const RECT headText{ geometry.archiveMeta.left + 2, geometry.archiveMeta.top, headRight,
        geometry.archiveMeta.bottom };
    DrawTextBlock(dc, m_fontAux, t.text4, headText, head,
        DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    // Paging is explicit: the wheel still works, but a card that only responds to a wheel is a
    // card nobody discovers. Two chevrons and "2 / 5" say there is more and how much more. With a
    // single page neither is drawn at all - a pair of arrows that can never do anything is a
    // control that reports a bug.
    const int capacity = std::max(1, geometry.archiveRowsCapacity);
    const int pages = std::max(1, (count + capacity - 1) / capacity);
    if (pages > 1)
    {
        const int page = std::min(pages, geometry.archiveFirst / capacity + 1);
        DrawTextBlock(dc, m_fontAux, t.text4, geometry.archivePage,
            util::i18n::Format(L"{} / {}", page, pages), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        const bool canBack = geometry.archiveFirst > 0;
        const bool canOn = geometry.archiveFirst + capacity < count;
        const auto chevron = [&](const RECT& box, bool back, bool enabled)
        {
            if (box.right <= box.left)
            {
                return;
            }

            if (enabled && m_hot == (back ? Hot::ArchivePrev : Hot::ArchiveNext))
            {
                FillRounded(dc, box, 5, t.hover);
            }

            const int cx = (box.left + box.right) / 2;
            const int cy = (box.top + box.bottom) / 2;
            const int arm = 4;
            const int tipX = back ? cx - arm / 2 : cx + arm / 2;
            const int tailX = back ? tipX + arm : tipX - arm;

            const HPEN pen =
                ::CreatePen(PS_SOLID, 1, enabled ? t.text3 : Mix(t.text4, t.card, 0.55));
            const HGDIOBJ oldPen = ::SelectObject(dc, pen);
            ::MoveToEx(dc, tailX, cy - arm, nullptr);
            ::LineTo(dc, tipX, cy);
            ::LineTo(dc, tailX, cy + arm);
            ::SelectObject(dc, oldPen);
            ::DeleteObject(pen);
        };
        chevron(geometry.archivePrev, true, canBack);
        chevron(geometry.archiveNext, false, canOn);
    }

    if (geometry.archiveRows.empty())
    {
        // The empty card answers the question the page was opened for: nothing has been closed
        // yet, and that is the whole story. One centred line, no buttons to hover.
        DrawTextBlock(dc, m_fontAux, t.text4,
            RECT{ geometry.archiveCard.left + 12, geometry.archiveCard.top + 4,
                geometry.archiveCard.right - 12, geometry.archiveCard.bottom - 4 },
            util::i18n::Cstr(L"还没有关闭的便签"), DT_CENTER | DT_VCENTER);
        return;
    }

    for (size_t index = 0; index < geometry.archiveRows.size(); ++index)
    {
        // The page can start anywhere in the archive, so everything below is addressed by the
        // record's own index. Indexing by the drawn row was the reason a scrolled list looked
        // frozen: it painted the first page over and over, and its buttons acted on it too.
        const int archiveIndex = geometry.archiveFirst + static_cast<int>(index);
        if (archiveIndex < 0 || archiveIndex >= count)
        {
            break;
        }

        const core::ArchiveRecord& record = m_state.archivedNotes[static_cast<size_t>(archiveIndex)];
        const RECT row = geometry.archiveRows[index];

        if (m_hot == Hot::ArchiveRestore && m_hotIndex == archiveIndex)
        {
            FillRounded(dc, row, 6, t.hover);
        }

        // The paper dot is the note's own colour, so a row is recognisable at a glance before the
        // preview text is read. core stores RRGGBB; COLORREF wants the channels reversed.
        const uint32_t paper = core::NoteThemeAt(record.theme).paper;
        const COLORREF dot = RGB((paper >> 16) & 0xFF, (paper >> 8) & 0xFF, paper & 0xFF);
        FillRounded(dc,
            RECT{ row.left + 6, row.top + 11, row.left + 14, row.top + 19 }, 4, dot);
        StrokeRounded(dc,
            RECT{ row.left + 6, row.top + 11, row.left + 14, row.top + 19 }, 4, t.line2);

        // The star is a column, not decoration: every row shows the glyph, gold when the note
        // carries a star and a hollow outline when it does not, so the eye can run down one place
        // instead of hunting for a symbol that may not be there.
        DrawTextBlock(dc, m_fontBody, record.starred ? kStarGold : Mix(t.text4, t.card, 0.45),
            RECT{ row.left + 18, row.top, row.left + 38, row.top + kArchiveRowHeight },
            record.starred ? L"★" : L"☆", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const int textLeft = row.left + 42;
        DrawTextBlock(dc, m_fontAux, t.text4,
            RECT{ textLeft, row.top, textLeft + ForDpi(74, m_dpi), row.top + kArchiveRowHeight },
            record.closedAt, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        const RECT text = { textLeft + ForDpi(80, m_dpi), row.top,
            geometry.archiveRestore[index].left - 8, row.top + kArchiveRowHeight };
        std::wstring preview = CleanPreview(record.preview);
        if (preview.empty())
        {
            preview = util::i18n::Cstr(L"（空白便签）");
        }
        DrawTextBlock(dc, m_fontBody, t.text2, text, preview,
            DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        const bool hotRestore =
            m_hot == Hot::ArchiveRestore && m_hotIndex == archiveIndex;
        const bool hotForget =
            m_hot == Hot::ArchiveForget && m_hotIndex == archiveIndex;
        DrawButton(dc, m_measure, t, m_fontAux, geometry.archiveRestore[index],
            util::i18n::Cstr(L"恢复"), false, hotRestore,
            m_pressed == Hot::ArchiveRestore && m_pressedIndex == archiveIndex,
            false, false, 0);
        DrawButton(dc, m_measure, t, m_fontAux, geometry.archiveForget[index],
            util::i18n::Cstr(L"删除"), false, hotForget,
            m_pressed == Hot::ArchiveForget && m_pressedIndex == archiveIndex,
            false, false, 0);
    }
}

// --- Popups and modal -----------------------------------------------------------------------

namespace
{

/// Item ids for the two rows of the paper menu that are not a paper. A paper row's id is its own
/// palette index, which is also the value handed back for an edit, so these sit well clear of it.
constexpr int kPaletteAddId = 100;
constexpr int kPaletteResetId = 101;

/// A paper is stored as 0xRRGGBB and shown as six hex digits, the same spelling settings.txt
/// uses, so the menu doubles as a way to read off a value to hand-edit.
std::wstring PaperCode(std::uint32_t paper)
{
    return std::format(L"{:06X}", paper & 0xFFFFFFu);
}

/// 0xRRGGBB to the COLORREF GDI wants. The note windows each keep their own copy of this two-line
/// conversion; the panel needs it now that its menu draws a paper, and folding the two together
/// would mean a shared header for one shift and two masks.
COLORREF PaperRef(std::uint32_t paper)
{
    return RGB(static_cast<BYTE>((paper >> 16) & 0xFFu), static_cast<BYTE>((paper >> 8) & 0xFFu),
        static_cast<BYTE>(paper & 0xFFu));
}

/// The live palette as plain colours, which is the shape the panel edits and the service stores.
std::vector<std::uint32_t> CurrentPapers()
{
    std::vector<std::uint32_t> papers;
    for (const core::NoteTheme& theme : core::NotePalette())
    {
        papers.push_back(theme.paper & 0xFFFFFFu);
    }
    return papers;
}

/// Runs the system colour picker on `start` and returns what was chosen, or nothing on cancel.
///
/// The stock dialog rather than a hand-built wheel: it brings the eye dropper, the hex field and
/// the recently-used row with it, and a colour picker is one of the few controls where the one
/// people already know is also the better one.
std::optional<std::uint32_t> PickPaper(HWND owner, std::uint32_t start)
{
    // Static so the "custom colours" row survives between openings - that row is the reason a
    // user can re-use a colour they mixed a minute ago.
    static COLORREF custom[16]{};

    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = owner;
    chooser.rgbResult = PaperRef(start);
    chooser.lpCustColors = custom;
    chooser.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;

    if (::ChooseColorW(&chooser) == FALSE)
    {
        return std::nullopt;
    }

    const COLORREF chosen = chooser.rgbResult;
    return (static_cast<std::uint32_t>(GetRValue(chosen)) << 16)
         | (static_cast<std::uint32_t>(GetGValue(chosen)) << 8)
         | static_cast<std::uint32_t>(GetBValue(chosen));
}

} // namespace

std::vector<Panel::PopupEntry> Panel::PopupItems() const
{
    std::vector<PopupEntry> items;

    switch (m_popup)
    {
        case Popup::LanguageMenu:
        {
            // id is the value written to settings.txt, so the checked row and the stored mode
            // cannot drift apart. Text goes through the same table as every other label.
            const auto language = util::i18n::CurrentLanguage();
            items.push_back(PopupEntry{ .text = L"跟随系统", .id = 0,
                .checked = language == util::i18n::Language::Auto });
            items.push_back(PopupEntry{ .text = L"简体中文", .id = 1,
                .checked = language == util::i18n::Language::Chinese });
            items.push_back(PopupEntry{ .text = L"English", .id = 2,
                .checked = language == util::i18n::Language::English });
            return items;
        }

        case Popup::GroupJoin:
        {
            if (m_popupIndex < 0 || m_popupIndex >= static_cast<int>(m_state.monitors.size()))
            {
                return items;
            }

            const std::wstring& path =
                m_state.monitors[static_cast<size_t>(m_popupIndex)].devicePath;
            const int current = core::GroupOf(m_state.groups, path);

            for (int slot = 0; slot < static_cast<int>(m_state.groups.size()); ++slot)
            {
                if (slot == current)
                {
                    // The group it is already in is not listed at all: a greyed-out "you are
                    // here" row is a control that can never do anything.
                    continue;
                }

                const core::Group& group = m_state.groups[static_cast<size_t>(slot)];
                PopupEntry entry;
                entry.text = util::i18n::Format(L"加入 {}", core::GroupLabel(group, m_state.monitors));
                entry.trailing = util::i18n::Format(L"{} 台", group.members.size());
                entry.pipIndex = slot;
                entry.id = slot;
                items.push_back(entry);
            }

            if (!items.empty())
            {
                PopupEntry separator;
                separator.separator = true;
                items.push_back(separator);
            }

            PopupEntry fresh;
            fresh.text = L"单独成组";
            fresh.dashed = true;
            fresh.id = -1;
            items.push_back(fresh);
            return items;
        }

        case Popup::GroupMerge:
        {
            if (m_state.ActiveGroup() == nullptr)
            {
                return items;
            }

            for (int index = 0; index < static_cast<int>(m_state.monitors.size()); ++index)
            {
                const std::wstring& path = m_state.monitors[static_cast<size_t>(index)].devicePath;
                const int owner = core::GroupOf(m_state.groups, path);
                if (owner == m_state.activeSlot)
                {
                    continue;
                }

                PopupEntry entry;
                const std::wstring screen =
                    std::to_wstring(core::ScreenNumber(path, m_state.monitors));
                entry.text = util::i18n::Format(L"合并 屏 {}", screen);

                // A group with one screen is named after that screen, so "合并 屏 3 …… 3" says
                // the same thing twice. The trailing column is for the case where it adds
                // information - a screen coming from a group of several.
                const std::wstring label =
                    owner >= 0
                        ? core::GroupLabel(m_state.groups[static_cast<size_t>(owner)],
                              m_state.monitors)
                        : std::wstring{};
                entry.trailing = (label == screen) ? std::wstring{} : label;
                entry.pipIndex = owner;
                entry.id = index;
                items.push_back(entry);
            }

            return items;
        }

        case Popup::NoteColors:
        {
            // The papers in force, not the last palette the user saved: the menu is a view of what
            // the notes on screen are actually wearing, so it opens on the built-in five the first
            // time and on the custom set every time after that.
            const std::vector<core::NoteTheme>& papers = core::NotePalette();
            const int count = static_cast<int>(papers.size());

            for (int index = 0; index < count; ++index)
            {
                PopupEntry entry;

                // The colour is the row's name. "Colour 3" only ever numbered the list the user is
                // looking at, and it pushed the one fact worth reading - the value - into a right
                // column that the "×" then crowded.
                entry.text = PaperCode(papers[static_cast<size_t>(index)].paper);
                entry.swatch = papers[static_cast<size_t>(index)].paper;
                entry.hasSwatch = true;
                // Two papers is the floor: one paper is a preference, not a palette, and a note
                // that can never be a different colour is a worse deal than the built-in five.
                entry.removable = count > 2;
                entry.id = index;
                items.push_back(entry);
            }

            items.push_back(PopupEntry{ .separator = true });
            if (count < core::kNoteThemeCount)
            {
                items.push_back(PopupEntry{ .text = L"添加一种配色", .id = kPaletteAddId,
                    .dashed = true });
            }
            items.push_back(PopupEntry{ .text = L"恢复默认五色", .id = kPaletteResetId });
            return items;
        }

        case Popup::None:
            break;
    }

    return items;
}

std::wstring Panel::PopupHead() const
{
    switch (m_popup)
    {
        case Popup::GroupJoin:
            if (m_popupIndex >= 0 && m_popupIndex < static_cast<int>(m_state.monitors.size()))
            {
                return util::i18n::Format(L"调整 屏 {} 的分组",
                    core::ScreenNumber(
                        m_state.monitors[static_cast<size_t>(m_popupIndex)].devicePath,
                        m_state.monitors));
            }
            return {};

        case Popup::GroupMerge:
            if (const core::Group* active = m_state.ActiveGroup(); active != nullptr)
            {
                return util::i18n::Format(L"合并到「{}」",
                    core::GroupLabel(*active, m_state.monitors));
            }
            return {};

        case Popup::NoteColors:
            return L"便签配色 · 点一行改颜色";

        default:
            return {};
    }
}

RECT Panel::PopupRect(const Geometry& geometry) const
{
    const std::vector<PopupEntry> items = PopupItems();
    const std::wstring head = PopupHead();

    // The language menu has three short labels; a 216-wide card would leave a long empty tail.
    int width = m_popup == Popup::LanguageMenu ? 196 : 216;
    for (const auto& item : items)
    {
        if (item.separator)
        {
            continue;
        }

        const int measured = 5 + 10 + 18
                           + static_cast<int>(MeasureText(m_measure, m_fontBody, item.text).cx)
                           + (item.trailing.empty()
                                   ? 0
                                   : 12 + static_cast<int>(MeasureText(m_measure, m_fontAux,
                                                item.trailing).cx))
                           + (item.removable ? 26 : 0)
                           + 14;
        width = std::max(width, measured);
    }

    width = std::min(width, 262);

    int height = 7;
    if (!head.empty())
    {
        height += 15;
    }
    for (const auto& item : items)
    {
        height += item.separator ? 7 : 26;
    }
    height += 7;

    RECT at{ m_popupAt.x, m_popupAt.y, m_popupAt.x + width, m_popupAt.y + height };

    // Kept inside the sheet: a menu anchored to a screen in the bottom-right corner would
    // otherwise hang off the window.
    if (at.right > m_clientW - 8)
    {
        at.left = m_clientW - 8 - width;
        at.right = m_clientW - 8;
    }
    if (at.bottom > geometry.clientHeight - 8)
    {
        at.top = m_popupAt.y - height - 26;
        at.bottom = at.top + height;
    }
    if (at.left < 8)
    {
        at.left = 8;
        at.right = 8 + width;
    }
    if (at.top < 8)
    {
        at.top = 8;
        at.bottom = 8 + height;
    }

    return at;
}

std::vector<RECT> Panel::PopupItemRects(const Geometry& geometry, const RECT& card) const
{
    // geometry is part of the signature so the caller reads as "rects for the sheet it is
    // drawing", and so a future clamp against the sheet has somewhere to go.
    (void)geometry;

    const std::vector<PopupEntry> items = PopupItems();

    std::vector<RECT> rects;
    rects.reserve(items.size());

    int y = card.top + 7;
    if (!PopupHead().empty())
    {
        y += 15;
    }

    for (const auto& item : items)
    {
        if (item.separator)
        {
            rects.push_back(RECT{});
            y += 7;
            continue;
        }

        rects.push_back(RECT{ card.left + 5, y, card.right - 5, y + 26 });
        y += 26;
    }

    return rects;
}

void Panel::PaintPopup(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;
    const std::vector<PopupEntry> items = PopupItems();
    if (items.empty())
    {
        return;
    }

    const RECT card = PopupRect(geometry);

    // A one-pixel outline twice: the shadow GDI cannot draw, approximated by a neutral ring.
    FillRounded(dc, Inflate(card, 1), 11, Mix(t.line2, t.bg, 0.35));
    FillRounded(dc, card, 10, t.bg);
    StrokeRounded(dc, card, 10, t.line2);

    int y = card.top + 7;
    const std::wstring head = PopupHead();
    if (!head.empty())
    {
        DrawTextBlock(dc, m_fontAux, t.text4, RECT{ card.left + 8, y, card.right - 8, y + 15 },
            head, DT_LEFT | DT_END_ELLIPSIS);
        y += 15;
    }

    const std::vector<RECT> rects = PopupItemRects(geometry, card);

    for (size_t index = 0; index < items.size(); ++index)
    {
        const PopupEntry& item = items[index];
        if (item.separator)
        {
            FillBox(dc, RECT{ card.left + 7, y + 3, card.right - 7, y + 4 }, t.line);
            y += 7;
            continue;
        }

        const RECT row = rects[index];
        const bool rowHot = (m_hot == Hot::PopupItem || m_hot == Hot::PopupRemove)
                         && m_hotIndex == static_cast<int>(index);
        if (rowHot)
        {
            FillRounded(dc, row, 6, t.hover);
        }

        int textLeft = row.left + 10;

        if (m_popup == Popup::LanguageMenu)
        {
            // The check column is reserved even for unchecked rows, so switching language does
            // not slide the labels sideways.
            if (item.checked)
            {
                DrawTextBlock(dc, m_fontBody, t.accent,
                    RECT{ row.left + 4, row.top, row.left + 20, row.bottom }, L"✓", DT_LEFT);
            }
            textLeft = row.left + 20;
        }
        else
        {
            // Every other menu is a list of things - displays, groups, papers - so every row
            // leads with a pip identifying what it is.
            const int pipTop = (row.top + row.bottom) / 2 - 5;

            if (item.hasSwatch)
            {
                // A paper swatch, drawn as a disc rather than the group chips' rounded square: these
                // are colours to be judged against the sheet, and a circle reads as colour where a
                // square reads as a tag.
                const RECT box{ row.left + 7, pipTop - 2, row.left + 21, pipTop + 12 };
                FillCircle(dc, box, PaperRef(item.swatch));
                StrokeCircle(dc, box, PaperRef(core::BlendColor(item.swatch, 0x37352F, 22)), 1);
            }
            else if (item.dashed)
            {
                const RECT box{ row.left + 9, pipTop, row.left + 20, pipTop + 11 };
                StrokeRounded(dc, box, 3, t.accent);
                DrawTextBlock(dc, m_fontSmall, t.accent, box, L"＋", DT_CENTER);
            }
            else
            {
                const COLORREF pip = (item.pipIndex >= 0 && item.pipIndex < 6)
                    ? t.group[item.pipIndex % 6]
                    : t.text4;
                FillRounded(dc, RECT{ row.left + 9, pipTop, row.left + 20, pipTop + 11 }, 3, pip);
            }
            textLeft = row.left + 28;
        }

        DrawTextBlock(dc, m_fontBody, item.dashed ? t.accent : t.text2,
            RECT{ textLeft, row.top, row.right - 8, row.bottom }, item.text,
            DT_LEFT | DT_END_ELLIPSIS);

        // A removable row gives up its right end to a "×", so the trailing column stops short of
        // it. Everything else keeps the full width it has always had.
        const int trailingRight = row.right - (item.removable ? 26 : 8);

        if (!item.trailing.empty())
        {
            DrawTextBlock(dc, m_fontAux, t.text4,
                RECT{ trailingRight - 72, row.top, trailingRight, row.bottom }, item.trailing,
                DT_RIGHT | DT_END_ELLIPSIS);
        }

        if (item.removable)
        {
            // Resting colour one step darker than the hex beside it: the cross has to be findable
            // without hovering, but it is a destructive control sitting next to the value the user
            // came here to read, so it does not get the accent.
            const bool removeHot = m_hot == Hot::PopupRemove && m_hotIndex == static_cast<int>(index);
            DrawTextBlock(dc, m_fontAux, removeHot ? t.danger : t.text3,
                RECT{ row.right - 24, row.top, row.right - 4, row.bottom }, L"×",
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        y += 26;
    }
}

Panel::ModalLayout Panel::LayoutModal(const Geometry& geometry) const
{
    ModalLayout layout;

    // English labels are roughly twice as long as their Chinese counterparts. The card is
    // sized for the longer language so neither version clips a line; 560 still leaves 130
    // design units of sheet on either side.
    const int width = 560;
    const bool help = m_modal == Modal::Help;

    // Height follows the content: seven key lines, four notes, then the button row. A fixed
    // height was leaving a hand's width of blank card under a two-line confirmation.
    const int height = help ? 304 : 168;

    layout.card = RECT{ (m_clientW - width) / 2, (geometry.clientHeight - height) / 2,
        (m_clientW + width) / 2, (geometry.clientHeight + height) / 2 };

    const int buttonWidth = 76;
    const int top = layout.card.bottom - 20 - kButtonHeight;

    if (help)
    {
        layout.buttons.push_back(RECT{ layout.card.right - 20 - buttonWidth, top,
            layout.card.right - 20, top + kButtonHeight });
        return layout;
    }

    layout.buttons.push_back(RECT{ layout.card.right - 20 - buttonWidth * 2 - 8, top,
        layout.card.right - 20 - buttonWidth - 8, top + kButtonHeight });
    layout.buttons.push_back(RECT{ layout.card.right - 20 - buttonWidth, top,
        layout.card.right - 20, top + kButtonHeight });
    return layout;
}

void Panel::PaintModal(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    // The whole sheet goes under a 42% scrim, then the card is drawn on top of the dimmed pixels.
    BlendAll(m_surface, RGB(0, 0, 0), 0.42);

    const ModalLayout layout = LayoutModal(geometry);
    const bool help = m_modal == Modal::Help;

    FillRounded(dc, layout.card, 12, t.bg);
    StrokeRounded(dc, layout.card, 12, t.line2);

    DrawTextBlock(dc, m_fontTitle, t.text,
        RECT{ layout.card.left + 20, layout.card.top + 14, layout.card.right - 20,
            layout.card.top + 38 },
        help ? L"键盘与操作" : L"退出 WinTools？", DT_LEFT);

    if (help)
    {
        // Two columns, not one padded string: the keys line up because they are drawn into their
        // own column, which a run of spaces cannot do across CJK and Latin metrics.
        struct HelpKey
        {
            const wchar_t* keys;
            const wchar_t* what;
        };

        static const HelpKey kKeys[] = {
            { L"Tab / Shift+Tab", L"在控件之间移动" },
            { L"Enter / Space", L"打开分组菜单 / 勾选" },
            { L"1 ~ 9", L"切换当前组合" },
            { L"+  −  0", L"缩放 / 复位当前组合" },
            { L"方向键", L"平移当前组合（Shift 加速）" },
            { L"Ctrl+Z", L"撤销上一次勾选" },
            { L"Esc", L"关弹层，再按隐藏到托盘" },
        };

        static const wchar_t* kNotes[] = {
            L"改分组：点屏左上角的编号，选另一个组合或「单独成组」。",
            L"合并某屏：点当前组合上的 ＋。",
            L"移动壁纸：在屏上拖动，滚轮缩放，只作用当前组合。",
            L"分组轮廓画在屏外侧，同组相邻屏共用一条线。",
        };

        int y = layout.card.top + 46;
        for (const auto& line : kKeys)
        {
            DrawTextBlock(dc, m_fontAux, t.text2,
                RECT{ layout.card.left + 20, y, layout.card.left + 168, y + 16 }, line.keys,
                DT_LEFT);
            DrawTextBlock(dc, m_fontAux, t.text3,
                RECT{ layout.card.left + 176, y, layout.card.right - 20, y + 16 }, line.what,
                DT_LEFT | DT_END_ELLIPSIS);
            y += 18;
        }

        y += 8;
        for (const wchar_t* note : kNotes)
        {
            DrawTextBlock(dc, m_fontAux, t.text2,
                RECT{ layout.card.left + 20, y, layout.card.right - 20, y + 16 }, note,
                DT_LEFT | DT_END_ELLIPSIS);
            y += 18;
        }
    }
    else
    {
        DrawTextBlock(dc, m_fontBody, t.text3,
            RECT{ layout.card.left + 20, layout.card.top + 46, layout.card.right - 20,
                layout.card.top + 68 },
            L"退出后不再监听显示器变化，窗口布局也不会再自动恢复。",
            DT_LEFT | DT_END_ELLIPSIS);
        DrawTextBlock(dc, m_fontBody, t.text3,
            RECT{ layout.card.left + 20, layout.card.top + 70, layout.card.right - 20,
                layout.card.top + 92 },
            L"只想收起面板，请点标题栏右侧的 ×。", DT_LEFT | DT_END_ELLIPSIS);
    }

    if (help)
    {
        DrawButton(dc, m_measure, t, m_fontBody, layout.buttons[0], L"知道了", true,
            (m_hot == Hot::ModalButton), false, false, false, m_spinAngle);
        return;
    }

    DrawButton(dc, m_measure, t, m_fontBody, layout.buttons[0], L"取消", false,
        (m_hot == Hot::ModalButton) && m_hotIndex == 0, false, false, false, m_spinAngle);
    DrawButton(dc, m_measure, t, m_fontBody, layout.buttons[1], L"退出", true,
        (m_hot == Hot::ModalButton) && m_hotIndex == 1, false, false, false, m_spinAngle);
}

void Panel::PaintOverlays(HDC dc, const Geometry& geometry)
{
    if (m_popup != Popup::None)
    {
        PaintPopup(dc, geometry);
    }

    if (m_modal != Modal::None)
    {
        PaintModal(dc, geometry);
    }
}

// --- Focus ring -----------------------------------------------------------------------------

std::vector<Panel::Target> Panel::FocusRing(const Geometry& geometry) const
{
    std::vector<Target> ring;

    ring.push_back({ Hot::IconLanguage, -1 });
    ring.push_back({ Hot::IconTheme, -1 });
    ring.push_back({ Hot::IconHelp, -1 });
    ring.push_back({ Hot::IconClose, -1 });

    // The tabs come first: switching pages is the one action every page shares.
    ring.push_back({ Hot::Tab, 0 });
    ring.push_back({ Hot::Tab, 1 });
    ring.push_back({ Hot::Tab, 2 });
    ring.push_back({ Hot::Tab, 3 });

    // Only the current page's controls exist, so only they join the ring - in the order they
    // stand on the page, because that is the order the arrow keys walk.
    switch (m_tab)
    {
        case Tab::Wallpaper:
            for (const int slot : geometry.chipOrder)
            {
                ring.push_back({ Hot::GroupChip, slot });
                if (slot == m_state.activeSlot)
                {
                    ring.push_back({ Hot::GroupAdd, slot });
                }
            }

            ring.push_back({ Hot::Pick, -1 });
            ring.push_back({ Hot::ApplyWallpaper, -1 });
            ring.push_back({ Hot::RotateWallpaper, -1 });

            // Only while it would do something: a ring around a disabled control is an invitation
            // to press a key that changes nothing.
            if (core::ClampView(m_view).rotation > 0.01)
            {
                ring.push_back({ Hot::ResetRotation, -1 });
            }

            if constexpr (kDesktopWidgetsEnabled)
            {
                ring.push_back({ Hot::CanvasSwitch, -1 });
            }

            for (int index = 0; index < static_cast<int>(m_state.monitors.size()); ++index)
            {
                ring.push_back({ Hot::ScreenBadge, index });
            }
            break;

        case Tab::Layout:
            for (const int monitor : geometry.chipMonitors)
            {
                ring.push_back({ Hot::ScreenChip, monitor });
            }

            ring.push_back({ Hot::AutoLayout, -1 });
            ring.push_back({ Hot::Refresh, -1 });
            ring.push_back({ Hot::ApplyLayout, -1 });

            {
                const std::vector<int> rows = VisibleRows();
                for (int slot = 0; slot < geometry.listRows; ++slot)
                {
                    const int position = geometry.firstRow + slot;
                    if (position < static_cast<int>(rows.size()))
                    {
                        ring.push_back({ Hot::RowCheck, rows[static_cast<size_t>(position)] });
                    }
                }
            }
            break;

        case Tab::Notes:
            ring.push_back({ Hot::NewNote, -1 });
            ring.push_back({ Hot::ShowNotes, -1 });
            ring.push_back({ Hot::NoteColors, -1 });
            ring.push_back({ Hot::ArchivePrev, -1 });
            ring.push_back({ Hot::ArchiveNext, -1 });

            for (size_t index = 0; index < geometry.archiveRows.size(); ++index)
            {
                const int archiveIndex = geometry.archiveFirst + static_cast<int>(index);
                ring.push_back({ Hot::ArchiveRestore, archiveIndex });
                ring.push_back({ Hot::ArchiveForget, archiveIndex });
            }
            break;

        case Tab::Config:
            // The rows in the order they are drawn, headings skipped: the ring is a walk through
            // the settings, and a heading is not something to press.
            for (const Geometry::ConfigRow& row : geometry.configRows)
            {
                if (!row.section && row.hot != Hot::None && row.rect.bottom > row.rect.top)
                {
                    ring.push_back({ row.hot, row.index });
                }
            }
            break;
    }

    ring.push_back({ Hot::Quit, -1 });
    return ring;
}

RECT Panel::RectForTarget(const Geometry& geometry, const Target& target) const
{
    switch (target.hot)
    {
        case Hot::IconLanguage:
            return geometry.iconLanguage;
        case Hot::IconTheme:
            return geometry.iconTheme;
        case Hot::IconHelp:
            return geometry.iconHelp;
        case Hot::IconClose:
            return geometry.iconClose;
        case Hot::ConfigTheme:
        case Hot::ConfigLanguage:
        case Hot::ConfigAutostart:
        case Hot::ConfigNoteRestore:
        case Hot::ConfigHotkey:
        case Hot::ConfigResetHotkeys:
        case Hot::ConfigOpenLog:
            for (const Geometry::ConfigRow& row : geometry.configRows)
            {
                if (row.hot != target.hot || row.index != target.index)
                {
                    continue;
                }

                // A segmented row rings the whole control rather than one segment: the ring says
                // which setting the keyboard is on, and all three choices belong to it.
                if (row.kind == ConfigControl::Segments)
                {
                    return row.control;
                }

                return row.kind == ConfigControl::Switch || row.kind == ConfigControl::Button
                    ? row.control
                    : row.rect;
            }
            return RECT{};
        case Hot::Tab:
            switch (target.index)
            {
                case 0:
                    return geometry.tabLayout;
                case 1:
                    return geometry.tabWallpaper;
                case 2:
                    return geometry.tabNotes;
                case 3:
                    return geometry.tabConfig;
                default:
                    return RECT{};
            }
        case Hot::RotateWallpaper:
            return geometry.rotateWallpaper;
        case Hot::ResetRotation:
            return geometry.resetRotation;
        case Hot::NewNote:
            return geometry.newNote;
        case Hot::ShowNotes:
            return geometry.showNotes;
        case Hot::NoteColors:
            return geometry.noteColors;
        case Hot::Pick:
            return geometry.pick;
        case Hot::ApplyWallpaper:
            return geometry.applyWallpaper;
        case Hot::CanvasSwitch:
            return geometry.canvasSwitch;
        case Hot::AutoLayout:
            return geometry.autoSwitch;
        case Hot::Refresh:
            return geometry.refresh;
        case Hot::ApplyLayout:
            return geometry.applyLayout;
        case Hot::Quit:
            return geometry.quit;
        case Hot::ArchiveRestore:
        case Hot::ArchiveForget:
        {
            // The ring and the clicks carry the archive's own index; the rects are laid out per
            // drawn row, so the page offset comes back off here.
            const int row = target.index - geometry.archiveFirst;
            const std::vector<RECT>& boxes = target.hot == Hot::ArchiveRestore
                ? geometry.archiveRestore
                : geometry.archiveForget;
            if (row >= 0 && row < static_cast<int>(boxes.size()))
            {
                return boxes[static_cast<size_t>(row)];
            }
            return RECT{};
        }
        case Hot::ArchivePrev:
            return geometry.archivePrev;
        case Hot::ArchiveNext:
            return geometry.archiveNext;

        case Hot::GroupChip:
        case Hot::GroupAdd:
            for (size_t index = 0; index < geometry.chipOrder.size(); ++index)
            {
                if (geometry.chipOrder[index] != target.index)
                {
                    continue;
                }

                return target.hot == Hot::GroupAdd ? geometry.groupAdds[index]
                                                   : geometry.groupChips[index];
            }
            return RECT{};

        case Hot::ScreenChip:
            for (size_t index = 0; index < geometry.chipMonitors.size(); ++index)
            {
                if (geometry.chipMonitors[index] == target.index)
                {
                    return geometry.screenChips[index];
                }
            }
            return RECT{};

        case Hot::RowCheck:
        {
            const std::vector<int> rows = VisibleRows();
            for (int position = geometry.firstRow;
                 position < geometry.firstRow + geometry.listRows; ++position)
            {
                if (position < static_cast<int>(rows.size())
                    && rows[static_cast<size_t>(position)] == target.index)
                {
                    const int slot = position - geometry.firstRow;
                    return RECT{ geometry.listArea.left, geometry.listArea.top + slot * kRowHeight,
                        geometry.listArea.left + 30,
                        geometry.listArea.top + slot * kRowHeight + 32 };
                }
            }
            return RECT{};
        }

        case Hot::ScreenBadge:
        {
            const MapLayout map = LayoutMap(geometry);
            if (!map.valid || target.index < 0
                || target.index >= static_cast<int>(m_state.monitors.size()))
            {
                return RECT{};
            }

            return BadgeRect(
                MonitorRectOnMap(map, m_state.monitors, static_cast<size_t>(target.index)),
                target.index);
        }

        default:
            return RECT{};
    }
}

// --- Interaction ----------------------------------------------------------------------------

void Panel::OpenPopup(Popup kind, POINT anchor, int index)
{
    m_popup = kind;
    m_popupAt = anchor;
    m_popupIndex = index;
    m_popupItem = -1;
    m_focus = -1;

    ::InvalidateRect(m_window, nullptr, FALSE);
}

void Panel::ClosePopup()
{
    if (m_popup == Popup::None)
    {
        return;
    }

    m_popup = Popup::None;
    m_popupIndex = -1;
    m_popupItem = -1;

    ::InvalidateRect(m_window, nullptr, FALSE);
}

void Panel::ToggleFilter(int monitorIndex)
{
    if (monitorIndex < 0 || monitorIndex >= static_cast<int>(m_state.monitors.size()))
    {
        return;
    }

    const std::wstring& path = m_state.monitors[static_cast<size_t>(monitorIndex)].devicePath;

    const auto same = [&path](const std::wstring& entry)
    { return core::Monitor::IEquals(entry, path); };

    if (const auto found = std::find_if(m_filter.begin(), m_filter.end(), same);
        found != m_filter.end())
    {
        m_filter.erase(found);
    }
    else
    {
        m_filter.push_back(path);
    }

    // Zero selected is not a filter that matches nothing - it is "no filter" (I9).
    m_scroll = 0;

    ::InvalidateRect(m_window, nullptr, FALSE);
}

void Panel::ActivatePopup(int item)
{
    const std::vector<PopupEntry> items = PopupItems();
    if (item < 0 || item >= static_cast<int>(items.size()) || items[static_cast<size_t>(item)].separator)
    {
        return;
    }

    const PopupEntry entry = items[static_cast<size_t>(item)];
    const Popup kind = m_popup;
    const int index = m_popupIndex;

    ClosePopup();

    auto emit = [this](PanelAction action, int argument = -1, int value = 0,
                    std::wstring target = {}) {
        if (m_onAction)
        {
            PanelRequest request;
            request.action = action;
            request.index = argument;
            request.value = value;
            request.target = std::move(target);
            m_onAction(request);
        }
    };

    switch (kind)
    {
        case Popup::LanguageMenu:
            emit(PanelAction::SetLanguage, -1, entry.id);
            return;

        case Popup::GroupJoin:
            if (index < 0 || index >= static_cast<int>(m_state.monitors.size()))
            {
                return;
            }
            emit(PanelAction::MoveMonitorToGroup, entry.id,
               0, m_state.monitors[static_cast<size_t>(index)].devicePath);
            return;

        case Popup::GroupMerge:
            if (entry.id < 0 || entry.id >= static_cast<int>(m_state.monitors.size()))
            {
                return;
            }
            emit(PanelAction::MoveMonitorToGroup, m_state.activeSlot, 0,
                m_state.monitors[static_cast<size_t>(entry.id)].devicePath);
            return;

        case Popup::NoteColors:
        {
            std::vector<std::uint32_t> papers = CurrentPapers();

            // A paper row's id is its own index, so editing is the plain case and the two commands
            // below sit well outside the range any palette can reach.
            if (entry.id >= 0 && entry.id < static_cast<int>(papers.size()))
            {
                if (const auto picked = PickPaper(m_window, papers[static_cast<size_t>(entry.id)]))
                {
                    papers[static_cast<size_t>(entry.id)] = *picked;
                    ApplyPapers(std::move(papers));
                }
                return;
            }

            if (entry.id == kPaletteAddId
                && papers.size() < static_cast<std::size_t>(core::kNoteThemeCount))
            {
                // A new paper starts as a copy of the last one and is immediately handed to the
                // picker, so "add" either produces a colour the user chose or leaves the palette
                // exactly as it was. Appearing as a duplicate the user has to click again is the
                // behaviour that makes an add button feel broken.
                papers.push_back(papers.back());
                if (const auto picked = PickPaper(m_window, papers.back()))
                {
                    papers.back() = *picked;
                    ApplyPapers(std::move(papers));
                }
                return;
            }

            if (entry.id == kPaletteResetId)
            {
                ApplyPapers({});
                return;
            }

            return;
        }

        case Popup::None:
            return;
    }
}

void Panel::ApplyPapers(std::vector<std::uint32_t> papers, int removed)
{
    if (m_onAction)
    {
        PanelRequest request;
        request.action = PanelAction::SetNotePalette;
        request.index = removed;
        request.colors = std::move(papers);
        m_onAction(request);
    }

    // The menu stays up: recolouring the papers is a run of small decisions, not one. The service
    // has applied the palette by the time this returns, so the menu reopened here reads the new
    // list - and a row that has just been removed is gone rather than clicking into nothing.
    OpenPopup(Popup::NoteColors, m_popupAt, -1);
}

void Panel::ActivatePopupRemove(int item)
{
    const std::vector<PopupEntry> items = PopupItems();
    if (item < 0 || item >= static_cast<int>(items.size()))
    {
        return;
    }

    const PopupEntry entry = items[static_cast<size_t>(item)];
    if (!entry.removable || entry.id < 0)
    {
        return;
    }

    std::vector<std::uint32_t> papers = CurrentPapers();
    if (entry.id >= static_cast<int>(papers.size()) || papers.size() <= 2)
    {
        return;
    }

    papers.erase(papers.begin() + entry.id);

    // The index goes with the palette so the notes can follow it: a note on the paper that just
    // went away has to land somewhere, and every note after it has to move down one slot or it
    // would quietly change colour.
    ApplyPapers(std::move(papers), entry.id);
}

void Panel::ActivateModal(int button)
{
    const Modal modal = m_modal;
    m_modal = Modal::None;

    if (modal == Modal::Quit && button == 1)
    {
        if (m_onAction)
        {
            m_onAction(PanelRequest{ PanelAction::Quit });
        }
        return;
    }

    ::InvalidateRect(m_window, nullptr, FALSE);
}

void Panel::Activate(const Target& target)
{
    // Any click ends the wheel gesture: land the zoom before the click's own action runs, so the
    // service cannot re-read a stale framing in between and leave the picture where it was.
    CommitView();

    auto emit = [this](PanelAction action, int index = -1, int value = 0) {
        if (m_onAction)
        {
            m_onAction(PanelRequest{ action, index, value });
        }
    };

    switch (target.hot)
    {
        case Hot::IconLanguage:
        {
            const Geometry g = ComputeGeometry();
            OpenPopup(Popup::LanguageMenu,
                POINT{ g.iconLanguage.left, g.iconLanguage.bottom + 6 }, -1);
            return;
        }

        case Hot::IconTheme:
            // Quick cycle; the config page offers all three at once for a deliberate pick.
            emit(PanelAction::SetTheme, -1, (m_state.theme + 1) % 3);
            return;

        case Hot::IconHelp:
            m_modal = Modal::Help;
            ::InvalidateRect(m_window, nullptr, FALSE);
            return;

        case Hot::IconClose:
            HidePanel();
            return;

        case Hot::ConfigTheme:
            emit(PanelAction::SetTheme, -1, target.index);
            return;

        case Hot::ConfigLanguage:
            emit(PanelAction::SetLanguage, -1, target.index);
            return;

        case Hot::ConfigAutostart:
            emit(PanelAction::ToggleAutostart);
            return;

        case Hot::ConfigNoteRestore:
            emit(PanelAction::ToggleNoteRestore);
            return;

        case Hot::ConfigOpenLog:
            emit(PanelAction::OpenLog);
            return;

        case Hot::ConfigResetHotkeys:
            m_recordingAction = -1;
            emit(PanelAction::ResetHotkeys);
            emit(PanelAction::ResumeHotkeys);
            return;

        case Hot::ConfigHotkey:
            // The global combinations are given up for as long as one row is recording, otherwise
            // pressing the old Ctrl+Alt+N would fire the action instead of reaching this window.
            if (m_recordingAction == target.index)
            {
                m_recordingAction = -1;
                emit(PanelAction::ResumeHotkeys);
            }
            else
            {
                m_recordingAction = target.index;
                emit(PanelAction::SuspendHotkeys);
                ::SetFocus(m_window);
            }
            ::InvalidateRect(m_window, nullptr, FALSE);
            return;

        case Hot::Tab:
            if (target.index < 0 || target.index > 3)
            {
                return;
            }

            // A page switch repaints: the window keeps its size, the pages just fill it
            // differently. The scroll and the focus belonged to the old page.
            m_tab = static_cast<Tab>(target.index);
            m_scroll = 0;
            m_archiveScroll = 0;
            m_focus = -1;
            ::InvalidateRect(m_window, nullptr, FALSE);
            return;

        case Hot::GroupChip:
            emit(PanelAction::SetActiveGroup, target.index);
            return;

        case Hot::GroupAdd:
        {
            const Geometry g = ComputeGeometry();
            for (size_t index = 0; index < g.chipOrder.size(); ++index)
            {
                if (g.chipOrder[index] == target.index)
                {
                    OpenPopup(Popup::GroupMerge,
                        POINT{ g.groupAdds[index].left - 40, g.groupAdds[index].bottom + 6 },
                        target.index);
                    return;
                }
            }
            return;
        }

        case Hot::Pick:
            emit(PanelAction::PickWallpaper);
            return;

        case Hot::ApplyWallpaper:
            emit(PanelAction::RecropWallpaper);
            return;

        case Hot::RotateWallpaper:
            emit(PanelAction::RotateWallpaper);
            return;

        case Hot::ResetRotation:
        {
            // Back to square through the same commit the drag uses: one write, one re-render, and
            // the framing (zoom, focus) is left exactly where the user put it.
            const core::Group* active = m_state.ActiveGroup();
            if (active == nullptr || active->image.empty())
            {
                return;
            }

            core::WallpaperView square = m_view;
            if (core::ClampView(square).rotation == 0.0)
            {
                return;
            }

            square.rotation = 0.0;
            m_view = square;

            if (m_onAction)
            {
                m_onAction(PanelRequest{ PanelAction::CommitWallpaperRotation, m_state.activeSlot,
                    0, m_view });
            }

            ::InvalidateRect(m_window, nullptr, FALSE);
            return;
        }

        case Hot::NewNote:
            emit(PanelAction::NewNote);
            return;

        case Hot::ShowNotes:
            emit(PanelAction::ShowNotes);
            return;

        case Hot::NoteColors:
        {
            const Geometry g = ComputeGeometry();
            // Left-aligned with the button, so the swatch column of the menu sits under its label
            // instead of hanging off the sheet's right edge.
            OpenPopup(Popup::NoteColors, POINT{ g.noteColors.left, g.noteColors.bottom + 6 }, -1);
            return;
        }

        case Hot::ScreenChip:
            ToggleFilter(target.index);
            return;

        case Hot::AutoLayout:
            emit(PanelAction::ToggleAutoLayout);
            return;

        case Hot::Refresh:
            emit(PanelAction::RefreshLayout);
            return;

        case Hot::ApplyLayout:
            emit(PanelAction::ApplyLayout);
            return;

        case Hot::RowCheck:
        {
            if (target.index < 0 || target.index >= static_cast<int>(m_state.windows.size()))
            {
                return;
            }

            // Send the wanted state rather than "toggle": the handler reads the file, and if the
            // panel's copy were a revision behind, a toggle would flip it back.
            const bool enabled = m_state.windows[static_cast<size_t>(target.index)].enabled;
            m_undoRow = target.index;
            m_undoValue = enabled;
            m_undoValid = true;
            emit(PanelAction::ToggleWindow, target.index, enabled ? 0 : 1);
            return;
        }

        case Hot::RowBody:
            // Clicking the title does nothing on purpose: the row is a record, and the only thing
            // that changes it is its tick box (or the space bar on a focused row).
            return;

        case Hot::ScreenBadge:
        {
            const Geometry g = ComputeGeometry();
            const MapLayout map = LayoutMap(g);
            if (!map.valid || target.index < 0)
            {
                return;
            }

            const RECT box =
                MonitorRectOnMap(map, m_state.monitors, static_cast<size_t>(target.index));
            const RECT badge = BadgeRect(box, target.index);
            OpenPopup(Popup::GroupJoin, POINT{ badge.left, badge.bottom + 4 }, target.index);
            return;
        }

        case Hot::ArchivePrev:
        case Hot::ArchiveNext:
        {
            const Geometry g = ComputeGeometry();
            const int step = std::max(1, g.archiveRowsCapacity);
            const int total = static_cast<int>(m_state.archivedNotes.size());
            const int pages = std::max(1, (total + step - 1) / step);
            // Same bound the layout clamps to: a page starts on a page boundary, so the last one
            // may be short rather than overlapping the page before it.
            const int maxScroll = (pages - 1) * step;
            const int wanted = m_archiveScroll + (target.hot == Hot::ArchiveNext ? step : -step);
            const int next = std::clamp(wanted, 0, maxScroll);
            if (next != m_archiveScroll)
            {
                m_archiveScroll = next;
                ::InvalidateRect(m_window, nullptr, FALSE);
            }
            return;
        }

        case Hot::ArchiveRestore:
        case Hot::ArchiveForget:
        {
            if (target.index < 0
                || target.index >= static_cast<int>(m_state.archivedNotes.size()))
            {
                return;
            }

            PanelRequest request{ target.hot == Hot::ArchiveRestore
                    ? PanelAction::ReopenNote : PanelAction::ForgetNote };
            request.target = m_state.archivedNotes[static_cast<size_t>(target.index)].id;
            if (m_onAction)
            {
                m_onAction(request);
            }
            return;
        }

        case Hot::Quit:
            m_modal = Modal::Quit;
            ::InvalidateRect(m_window, nullptr, FALSE);
            return;

        case Hot::PopupItem:
        case Hot::ModalButton:
            // Both are consumed by their own press/release handlers above; reaching here means a
            // row slipped through, and toggling a widget is certainly not the right fallback.
            return;

        case Hot::CanvasSwitch:
            if constexpr (kDesktopWidgetsEnabled)
            {
                emit(PanelAction::ToggleCanvas);
            }
            return;

        case Hot::MapBody:
        case Hot::CanvasBody:
        case Hot::None:
            return;
    }
}

void Panel::ZoomActiveGroup(double factor, POINT anchor)
{
    const Geometry g = ComputeGeometry();
    const MapLayout map = LayoutMap(g);
    const core::Group* active = m_state.ActiveGroup();
    if (!map.valid || active == nullptr || active->image.empty())
    {
        return;
    }

    POINT at = anchor;
    if (at.x < 0)
    {
        // Keyboard zoom: the middle of the group's own box, so the framing grows around what the
        // user is looking at rather than around the corner of the board.
        const RECT box = GroupBoxOnMap(map, *active);
        at = POINT{ (box.left + box.right) / 2 - map.projection.drawn.left,
            (box.top + box.bottom) / 2 - map.projection.drawn.top };
    }
    else
    {
        at = POINT{ at.x - map.projection.drawn.left, at.y - map.projection.drawn.top };
    }

    const core::WallpaperView zoomed =
        ZoomView(m_view, ContextForGroup(map.projection, *active, m_view), at, factor);

    if (!(zoomed == m_view))
    {
        m_view = zoomed;
        m_zoomPending = true;
        ::SetTimer(m_window, kZoomCommitTimer, kZoomCommitDelayMs, nullptr);
        ::InvalidateRect(m_window, nullptr, FALSE);
    }
}

void Panel::PanActiveGroup(int dx, int dy)
{
    const Geometry g = ComputeGeometry();
    const MapLayout map = LayoutMap(g);
    const core::Group* active = m_state.ActiveGroup();
    if (!map.valid || active == nullptr || active->image.empty())
    {
        return;
    }

    const MapFrame frame = FrameFor(ContextForGroup(map.projection, *active, m_view), m_view);
    const core::WallpaperView moved = PanView(m_view, frame, dx, dy);

    if (!(moved == m_view))
    {
        m_view = moved;
        m_zoomPending = true;
        ::SetTimer(m_window, kZoomCommitTimer, kZoomCommitDelayMs, nullptr);
        ::InvalidateRect(m_window, nullptr, FALSE);
    }
}

void Panel::ResetActiveView()
{
    const core::Group* active = m_state.ActiveGroup();
    if (active == nullptr)
    {
        return;
    }

    // 复位 puts the framing back to cover-centred; the quarter-turn is a property of its own
    // button and stays, the same rule the wheel's zoom-to-minimum reset follows.
    m_view = core::WallpaperView{};
    m_view.rotation = active->view.rotation;
    m_zoomPending = false;
    ::KillTimer(m_window, kZoomCommitTimer);

    if (m_onAction)
    {
        m_onAction(PanelRequest{ PanelAction::SetWallpaperView, m_state.activeSlot, 0, m_view });
    }

    ::InvalidateRect(m_window, nullptr, FALSE);
}

void Panel::OnKeyDown(WPARAM key)
{
    // Recording swallows every key until a combination (or Esc) arrives. Modifier-only presses are
    // ignored rather than bound: binding Ctrl alone is legal to RegisterHotKey but useless as a
    // global shortcut, and it would fire on every Ctrl the user ever typed.
    if (m_recordingAction >= 1
        && m_recordingAction <= static_cast<int>(core::kHotkeyActions.size()))
    {
        if (key == VK_ESCAPE)
        {
            m_recordingAction = -1;
            // The combinations were given up when recording started; cancelling has to hand them
            // back, or every shortcut stays dead until the next launch.
            if (m_onAction)
            {
                m_onAction(PanelRequest{ PanelAction::ResumeHotkeys });
            }
            ::InvalidateRect(m_window, nullptr, FALSE);
            return;
        }

        if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU || key == VK_LWIN
            || key == VK_RWIN)
        {
            return;
        }

        const int action = m_recordingAction;
        m_recordingAction = -1;

        core::Hotkey hotkey;
        if (key != VK_BACK)
        {
            const auto down = [](int vk) { return (::GetKeyState(vk) & 0x8000) != 0; };
            if (down(VK_CONTROL))
            {
                hotkey.modifiers |= 0x0002; // MOD_CONTROL
            }
            if (down(VK_MENU))
            {
                hotkey.modifiers |= 0x0001; // MOD_ALT
            }
            if (down(VK_SHIFT))
            {
                hotkey.modifiers |= 0x0004; // MOD_SHIFT
            }
            if (down(VK_LWIN) || down(VK_RWIN))
            {
                hotkey.modifiers |= 0x0008; // MOD_WIN
            }
            hotkey.vk = static_cast<unsigned int>(key);
        }

        // index carries the combination (low word modifiers, high word vk), value the action.
        const int packed = static_cast<int>((hotkey.vk << 16) | (hotkey.modifiers & 0xFFFF));
        LogInfo(L"recorded hotkey for {}: {}+{}", action, hotkey.modifiers, hotkey.vk);
        if (m_onAction)
        {
            m_onAction(PanelRequest{ PanelAction::SetHotkey, packed, action });
            // Bind, then take the combinations back. RegisterHotKeys runs the whole set again, so
            // the row that was just bound is live straight away.
            m_onAction(PanelRequest{ PanelAction::ResumeHotkeys });
        }
        ClosePopup();
        ::InvalidateRect(m_window, nullptr, FALSE);
        return;
    }

    const Geometry g = ComputeGeometry();

    switch (key)
    {
        case VK_TAB:
        {
            const std::vector<Target> ring = FocusRing(g);
            if (ring.empty())
            {
                return;
            }

            const bool back = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (m_focus < 0)
            {
                m_focus = back ? static_cast<int>(ring.size()) - 1 : 0;
            }
            else
            {
                m_focus = (m_focus + (back ? -1 : 1) + static_cast<int>(ring.size()))
                        % static_cast<int>(ring.size());
            }

            ::InvalidateRect(m_window, nullptr, FALSE);
            return;
        }

        case VK_RETURN:
        case VK_SPACE:
        {
            if (m_focus < 0)
            {
                return;
            }

            const std::vector<Target> ring = FocusRing(g);
            if (m_focus < static_cast<int>(ring.size()))
            {
                Activate(ring[static_cast<size_t>(m_focus)]);
            }
            return;
        }

        case VK_ESCAPE:
            if (m_modal != Modal::None)
            {
                m_modal = Modal::None;
                ::InvalidateRect(m_window, nullptr, FALSE);
                return;
            }
            if (m_popup != Popup::None)
            {
                ClosePopup();
                return;
            }
            HidePanel();
            return;

        case VK_ADD:
        case VK_OEM_PLUS:
            ZoomActiveGroup(kZoomStep, POINT{ -1, -1 });
            return;

        case VK_SUBTRACT:
        case VK_OEM_MINUS:
            ZoomActiveGroup(1.0 / kZoomStep, POINT{ -1, -1 });
            return;

        case VK_NUMPAD0:
        case L'0':
            CommitView();
            ResetActiveView();
            return;

        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        {
            const int step = ((::GetKeyState(VK_SHIFT) & 0x8000) != 0) ? 54 : 18;
            const int dx = (key == VK_LEFT) ? step : (key == VK_RIGHT ? -step : 0);
            const int dy = (key == VK_UP) ? step : (key == VK_DOWN ? -step : 0);
            PanActiveGroup(dx, dy);
            return;
        }

        case L'Z':
            if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0 && m_undoValid)
            {
                m_undoValid = false;
                if (m_onAction)
                {
                    m_onAction(PanelRequest{ PanelAction::ToggleWindow, m_undoRow,
                        m_undoValue ? 1 : 0 });
                }
            }
            return;

        default:
            break;
    }

    // 1..9 switch the active group, in chip order - the same order the chips are drawn in, so
    // "press 2" and "the second chip" are the same group.
    if (key >= L'1' && key <= L'9')
    {
        const size_t wanted = static_cast<size_t>(key - L'1');
        const std::vector<int> order = g.chipOrder;
        if (wanted < order.size())
        {
            Activate(Target{ Hot::GroupChip, order[wanted] });
        }
    }
}

LRESULT CALLBACK Panel::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<Panel*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_window = window;
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    auto* self = reinterpret_cast<Panel*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr)
    {
        return self->HandleMessage(message, wparam, lparam);
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT Panel::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_ERASEBKGND:
            // Everything is painted in WM_PAINT; erasing first would flash white.
            return 1;

        case WM_PAINT:
            Paint();
            return 0;

        case WM_DPICHANGED:
        {
            // Dragged onto a display with another scale factor. The design grid follows the dpi,
            // so the window takes the size Windows suggests for the same physical extent and
            // every design distance comes out the same.
            const UINT dpi = HIWORD(wparam);
            if (dpi != 0)
            {
                m_dpi = dpi;
            }

            ScaleMeasureDc();
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            const int width = suggested->right - suggested->left;
            const int height = suggested->bottom - suggested->top;
            ::SetWindowPos(m_window, nullptr, suggested->left, suggested->top, width, height,
                SWP_NOZORDER | SWP_NOACTIVATE);
            ::InvalidateRect(m_window, nullptr, FALSE);
            return 0;
        }

        case WM_PRINTCLIENT:
        {
            // Lets the panel draw itself into somebody else's DC: PrintWindow, a screenshot tool,
            // or the documentation script.
            RECT client{};
            ::GetClientRect(m_window, &client);
            PaintContent(reinterpret_cast<HDC>(wparam), client.right - client.left,
                client.bottom - client.top);
            return 0;
        }

        case WM_NCCALCSIZE:
            // WS_THICKFRAME is here for its resize behaviour, not its looks: claiming the whole
            // window as client area keeps the custom title bar, and the frame never gets drawn.
            if (wparam != FALSE)
            {
                return 0;
            }
            break;

        case WM_GETMINMAXINFO:
        {
            // The layout degrades gracefully, but below this the footer and the buttons start
            // colliding, so the smallest sheet is clamped at the tracker level.
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = POINT{ ForDpi(kMinWidth, m_dpi), ForDpi(kMinHeight, m_dpi) };
            return 0;
        }

        case WM_SIZE:
            // A user resize re-flows the pages; nothing about the model changes.
            ::InvalidateRect(m_window, nullptr, FALSE);
            return 0;

        case WM_NCHITTEST:
        {
            // The window edge is resize handles first; the title bar drags; everything else is
            // the sheet. The zones are checked in device pixels straight against the window
            // rect, before any design-space conversion - they are a physical affordance.
            const POINT screen{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            RECT window{};
            ::GetWindowRect(m_window, &window);

            constexpr int kResizeBorder = 6;
            const bool leftZone = screen.x - window.left < kResizeBorder;
            const bool rightZone = window.right - screen.x <= kResizeBorder;
            const bool topZone = screen.y - window.top < kResizeBorder;
            const bool bottomZone = window.bottom - screen.y <= kResizeBorder;

            // Corners need both axes wide enough to move independently; at the minimum size
            // every edge still resizes along its own axis.
            const int minEdge = ForDpi(96, m_dpi);
            const bool wide = window.right - window.left > 2 * minEdge;
            const bool tall = window.bottom - window.top > 2 * minEdge;

            if ((leftZone || rightZone || topZone || bottomZone)
                && !(leftZone && rightZone) && !(topZone && bottomZone))
            {
                if (leftZone && topZone && wide && tall)
                {
                    return HTTOPLEFT;
                }
                if (rightZone && topZone && wide && tall)
                {
                    return HTTOPRIGHT;
                }
                if (leftZone && bottomZone && wide && tall)
                {
                    return HTBOTTOMLEFT;
                }
                if (rightZone && bottomZone && wide && tall)
                {
                    return HTBOTTOMRIGHT;
                }
                if (leftZone)
                {
                    return HTLEFT;
                }
                if (rightZone)
                {
                    return HTRIGHT;
                }
                if (topZone)
                {
                    return HTTOP;
                }
                if (bottomZone)
                {
                    return HTBOTTOM;
                }
            }

            POINT point{ screen.x, screen.y };
            ::ScreenToClient(m_window, &point);
            const POINT design = ToDesign(point);
            const Geometry g = ComputeGeometry();

            if (design.y < kTitleBarHeight)
            {
                const bool onIcon = Contains(g.iconLanguage, design) || Contains(g.iconTheme, design)
                                 || Contains(g.iconHelp, design) || Contains(g.iconClose, design);
                return onIcon ? HTCLIENT : HTCAPTION;
            }

            return HTCLIENT;
        }

        case kMsgActivate:
            ShowPanel();
            return 0;

        case WM_TIMER:
            if (wparam == kZoomCommitTimer)
            {
                CommitView();
                return 0;
            }
            if (wparam == kSpinTimer)
            {
                m_spinAngle = (m_spinAngle + 30) % 360;
                ::InvalidateRect(m_window, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_CLOSE:
            // Closing hides to the tray. The app's whole job is to keep watching displays, so a
            // window close must not end the process.
            HidePanel();
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE)
            {
                // Anchored menus belong to the window that opened them; leaving one behind on an
                // inactive window is how a stale menu ends up over somebody else's work.
                ClosePopup();
            }
            return 0;

        case WM_MOUSEMOVE:
        {
            const POINT point = ToDesign(POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) });

            if (m_rotating)
            {
                // No geometry here on purpose: the circle was measured when the press landed, so a
                // drag frame costs one projection and nothing else.
                // Arc-length steering with a gear ratio: the hand travels further than the picture
                // turns, which is the difference between aiming a picture and flicking it. Three
                // degrees of cursor travel per degree of picture still lands on a single degree
                // without a steady hand, and a quarter turn costs a sweep rather than a twitch -
                // the short click is there when a whole quarter is what is wanted.
                constexpr double kTurnGain = 0.32;
                m_rotationDelta +=
                    TangentialDelta(m_rotationLast, point, m_rotationCentre, m_rotationRadius)
                    * kTurnGain;
                m_rotationLast = point;

                if (!m_rotationMoved
                    && (std::abs(m_rotationDelta) > 3.0
                        || std::hypot(static_cast<double>(point.x - m_rotationOrigin.x),
                               static_cast<double>(point.y - m_rotationOrigin.y))
                            > 6.0))
                {
                    m_rotationMoved = true;
                }

                // One degree is finer than the eye can see on the map and coarse enough that a
                // jittering cursor does not re-decode the picture on every message. The right
                // angles are magnetic - see TurnAngle.
                const double wanted =
                    TurnAngle(m_rotationStart.rotation, m_rotationDelta, m_rotationSnap);

                if (wanted != m_view.rotation)
                {
                    m_view = m_rotationStart;
                    m_view.rotation = wanted;
                    ::InvalidateRect(m_window, nullptr, FALSE);
                }
                return 0;
            }

            if (m_draggingCanvas)
            {
                const Geometry g = ComputeGeometry();
                const MapLayout map = LayoutMap(g);
                if (map.valid && map.projection.scale > 0.0)
                {
                    // The map is a scaled copy of the virtual desktop, so a mouse delta becomes a
                    // screen delta by dividing by the same scale that drew it.
                    const int deltaX = static_cast<int>(std::lround(
                        (point.x - m_canvasGrab.x) / map.projection.scale));
                    const int deltaY = static_cast<int>(std::lround(
                        (point.y - m_canvasGrab.y) / map.projection.scale));

                    core::CanvasRect moved = m_canvasStart;
                    moved.x = m_canvasStart.x + deltaX;
                    moved.y = m_canvasStart.y + deltaY;

                    // Kept on a display while it is being dragged: letting it slide into the void
                    // would show a drop that the service then silently undoes.
                    moved = core::ClampCanvas(moved, m_state.monitors);

                    if (!(moved == m_canvasDrag))
                    {
                        m_canvasDrag = moved;
                        m_canvasLive = true;
                        ::InvalidateRect(m_window, nullptr, FALSE);
                    }
                }
                return 0;
            }

            if (m_panning)
            {
                const Geometry g = ComputeGeometry();
                const MapLayout map = LayoutMap(g);
                const core::Group* active = m_state.ActiveGroup();
                if (map.valid && active != nullptr)
                {
                    const MapFrame frame = FrameFor(ContextForGroup(map.projection, *active, m_panStart),
                        m_panStart);
                    const core::WallpaperView moved = PanView(m_panStart, frame,
                        point.x - m_panOrigin.x, point.y - m_panOrigin.y);

                    if (!(moved == m_view))
                    {
                        m_view = moved;
                        ::InvalidateRect(m_window, nullptr, FALSE);
                    }
                }
                return 0;
            }

            bool repaint = false;

            // A modal owns the sheet, so its buttons are the only thing that can be hovered.
            if (m_modal != Modal::None)
            {
                const Geometry g = ComputeGeometry();
                const ModalLayout layout = LayoutModal(g);

                int found = -1;
                for (size_t index = 0; index < layout.buttons.size(); ++index)
                {
                    if (Contains(layout.buttons[index], point))
                    {
                        found = static_cast<int>(index);
                        break;
                    }
                }

                m_hot = (found >= 0) ? Hot::ModalButton : Hot::None;
                m_hotIndex = found;
                ::InvalidateRect(m_window, nullptr, FALSE);
                return 0;
            }

            if (m_popup != Popup::None)
            {
                const Geometry g = ComputeGeometry();
                const std::vector<RECT> rects = PopupItemRects(g, PopupRect(g));
                const std::vector<PopupEntry> items = PopupItems();

                int found = -1;
                bool overRemove = false;
                for (size_t index = 0; index < rects.size(); ++index)
                {
                    if (rects[index].right > rects[index].left && Contains(rects[index], point))
                    {
                        found = static_cast<int>(index);
                        overRemove = index < items.size() && items[index].removable
                                  && point.x >= rects[index].right - 24;
                        break;
                    }
                }

                // The cross lives inside its row, so moving between the two never changes which
                // row is under the pointer - the hot state has to be compared as well.
                const Hot hot = overRemove ? Hot::PopupRemove : Hot::PopupItem;
                if (found != m_popupItem || hot != m_hot)
                {
                    m_popupItem = found;
                    m_hot = hot;
                    m_hotIndex = found;
                    repaint = true;
                }

                if (repaint)
                {
                    ::InvalidateRect(m_window, nullptr, FALSE);
                }
                return 0;
            }

            const Target target = HitTest(point);
            if (target.hot != m_hot || target.index != m_hotIndex)
            {
                m_hot = target.hot;
                m_hotIndex = target.index;
                repaint = true;
            }

            if (repaint)
            {
                ::InvalidateRect(m_window, nullptr, FALSE);
            }

            if (!m_trackingLeave)
            {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, m_window, 0 };
                ::TrackMouseEvent(&track);
                m_trackingLeave = true;
            }

            return 0;
        }

        case WM_MOUSELEAVE:
            m_trackingLeave = false;
            if (m_hot != Hot::None)
            {
                m_hot = Hot::None;
                m_hotIndex = -1;
                ::InvalidateRect(m_window, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
        {
            const POINT point = ToDesign(POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) });
            ::SetFocus(m_window);

            // A modal owns the sheet: only its buttons respond.
            if (m_modal != Modal::None)
            {
                const Geometry g = ComputeGeometry();
                const ModalLayout layout = LayoutModal(g);
                for (size_t index = 0; index < layout.buttons.size(); ++index)
                {
                    if (Contains(layout.buttons[index], point))
                    {
                        m_pressed = Hot::ModalButton;
                        m_pressedIndex = static_cast<int>(index);
                        ::InvalidateRect(m_window, nullptr, FALSE);
                        return 0;
                    }
                }
                return 0;
            }

            if (m_popup != Popup::None)
            {
                const Target hit = HitTest(point);
                if ((hit.hot == Hot::PopupItem || hit.hot == Hot::PopupRemove) && hit.index >= 0)
                {
                    m_pressed = hit.hot;
                    m_pressedIndex = hit.index;
                    return 0;
                }

                ClosePopup();
                return 0;
            }

            const Target target = HitTest(point);

            // The arc is a drag handle first and a button second: hold and turn for any angle,
            // or just click for a quarter turn. Either way the gesture owns the capture.
            if (target.hot == Hot::RotateWallpaper)
            {
                const core::Group* active = m_state.ActiveGroup();
                if (active != nullptr && !active->image.empty())
                {
                    const Geometry g = ComputeGeometry();
                    m_rotating = true;
                    m_rotationMoved = false;
                    m_rotationDelta = 0.0;
                    m_rotationLast = point;

                    // The circle the drag is measured on is centred on the picture, not on the chip:
                    // the chip is a grip, the group's box is what turns. Measuring around the grip
                    // would make a short drag that happens to pass near it sweep half a turn - the
                    // "one flick, ninety degrees" the handle used to have.
                    const MapLayout map = LayoutMap(g);
                    const RECT box = GroupBoxOnMap(map, *active);
                    m_rotationCentre = (box.right > box.left && box.bottom > box.top)
                        ? POINT{ (box.left + box.right) / 2, (box.top + box.bottom) / 2 }
                        : g.rotateCentre;

                    // Whatever radius the press landed at, but never tiny: a press near the centre
                    // would otherwise divide by ~0.
                    const double reach = std::hypot(
                        static_cast<double>(point.x - m_rotationCentre.x),
                        static_cast<double>(point.y - m_rotationCentre.y));
                    m_rotationRadius = std::max(90.0, reach);
                    m_rotationStart = m_view;
                    m_rotationOrigin = point;
                    m_rotationSnap = -1;
                    m_pressed = Hot::RotateWallpaper;
                    ::SetCapture(m_window);
                    ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                    ::InvalidateRect(m_window, nullptr, FALSE);
                    return 0;
                }
            }

            // Dragging the calendar moves it; dragging a screen moves that screen's picture.
            if (kDesktopWidgetsEnabled && target.hot == Hot::CanvasBody)
            {
                m_draggingCanvas = true;
                m_canvasGrab = point;
                m_canvasStart = ShownCanvas();
                m_canvasDrag = m_canvasStart;
                m_canvasLive = true;
                ::SetCapture(m_window);
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
                ::InvalidateRect(m_window, nullptr, FALSE);
                return 0;
            }

            // Dragging anywhere on a screen moves that screen's group picture, and grabbing a
            // different group's screen silently switches to it first - one gesture, no extra
            // click. The number badge is excluded: that is the group menu's trigger (I4).
            if (target.hot == Hot::MapBody)
            {
                const int monitor = MonitorAt(point);
                if (monitor >= 0)
                {
                    const int owner =
                        core::GroupOf(m_state.groups,
                            m_state.monitors[static_cast<size_t>(monitor)].devicePath);

                    if (owner >= 0 && owner != m_state.activeSlot)
                    {
                        Activate(Target{ Hot::GroupChip, owner });
                    }

                    const core::Group* active = m_state.ActiveGroup();
                    if (active != nullptr && !active->image.empty())
                    {
                        m_panning = true;
                        m_panOrigin = point;
                        m_panStart = m_view;
                        ::SetCapture(m_window);
                        ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));

                        // The whole picture and its dimmed margin appear on the press, not on the
                        // first twitch of the mouse: the gesture has to announce what it can do.
                        ::InvalidateRect(m_window, nullptr, FALSE);
                        return 0;
                    }
                }
            }

            m_pressed = target.hot;
            m_pressedIndex = target.index;
            m_pressedAt = point;
            ::SetCapture(m_window);
            ::InvalidateRect(m_window, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            ::ReleaseCapture();

            if (m_rotating)
            {
                m_rotating = false;
                const bool turned = m_rotationMoved;
                m_pressed = Hot::None;
                m_pressedIndex = -1;

                if (m_onAction)
                {
                    // A drag that ends on the angle it started from has nothing to write: the
                    // service would re-slice every group to produce the same files.
                    if (turned && m_view.rotation != core::ClampView(m_rotationStart).rotation)
                    {
                        m_onAction(PanelRequest{ PanelAction::CommitWallpaperRotation,
                            m_state.activeSlot, 0, m_view });
                    }
                    else if (!turned)
                    {
                        // A click without a drag keeps the old one-step 90 degree shortcut.
                        m_onAction(PanelRequest{ PanelAction::RotateWallpaper });
                    }
                }

                ::InvalidateRect(m_window, nullptr, FALSE);
                return 0;
            }

            if (m_draggingCanvas)
            {
                m_draggingCanvas = false;

                const bool moved = m_canvasLive && !(m_canvasDrag == m_canvasStart);
                const core::CanvasRect dropped = m_canvasDrag;
                m_canvasLive = false;

                // Written once, on release - same rule as the wallpaper pan.
                if (moved && m_onAction)
                {
                    m_onAction(PanelRequest{
                        PanelAction::MoveCanvas, -1, 0, core::WallpaperView{}, dropped.x, dropped.y });
                }

                ::InvalidateRect(m_window, nullptr, FALSE);
                return 0;
            }

            if (m_panning)
            {
                m_panning = false;

                // Written once, on release: a settings write per mouse move would put a file flush
                // between the cursor and the picture.
                if (!(m_view == m_panStart))
                {
                    if (m_onAction)
                    {
                        m_onAction(PanelRequest{ PanelAction::SetWallpaperView, m_state.activeSlot,
                            0, m_view });
                    }
                }

                ::InvalidateRect(m_window, nullptr, FALSE);
                return 0;
            }

            const POINT lifted = ToDesign(POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) });
            const Hot pressed = m_pressed;
            const int pressedIndex = m_pressedIndex;
            m_pressed = Hot::None;
            m_pressedIndex = -1;
            ::InvalidateRect(m_window, nullptr, FALSE);

            if (pressed == Hot::None)
            {
                return 0;
            }

            if (pressed == Hot::ModalButton)
            {
                const Geometry g = ComputeGeometry();
                const ModalLayout layout = LayoutModal(g);
                for (size_t index = 0; index < layout.buttons.size(); ++index)
                {
                    if (static_cast<int>(index) == pressedIndex && Contains(layout.buttons[index], lifted))
                    {
                        ActivateModal(pressedIndex);
                        return 0;
                    }
                }
                return 0;
            }

            if (pressed == Hot::PopupItem || pressed == Hot::PopupRemove)
            {
                const Geometry g = ComputeGeometry();
                const std::vector<RECT> rects = PopupItemRects(g, PopupRect(g));
                if (pressedIndex >= 0 && pressedIndex < static_cast<int>(rects.size())
                    && Contains(rects[static_cast<size_t>(pressedIndex)], lifted))
                {
                    if (pressed == Hot::PopupRemove)
                    {
                        ActivatePopupRemove(pressedIndex);
                    }
                    else
                    {
                        ActivatePopup(pressedIndex);
                    }
                }
                return 0;
            }

            const Target released = HitTest(lifted);

            // Only act when press and release land on the same target, so a drag off a button
            // cancels it like every other Windows control.
            if (pressed == released.hot && pressedIndex == released.index)
            {
                Activate(released);
            }

            return 0;
        }

        case WM_LBUTTONDBLCLK:
        {
            const POINT point = ToDesign(POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) });
            if (m_popup != Popup::None || m_modal != Modal::None)
            {
                return 0;
            }

            const Target target = HitTest(point);
            if (target.hot == Hot::MapBody || target.hot == Hot::ScreenBadge)
            {
                CommitView();
                ResetActiveView();
            }
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            // The wheel reports screen coordinates, unlike the other mouse messages.
            POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ::ScreenToClient(m_window, &point);
            point = ToDesign(point);

            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            if (delta == 0 || m_modal != Modal::None || m_popup != Popup::None)
            {
                return 0;
            }

            const Geometry g = ComputeGeometry();

            if (Contains(g.mapCard, point))
            {
                const int monitor = MonitorAt(point);
                if (monitor >= 0)
                {
                    const int owner =
                        core::GroupOf(m_state.groups,
                            m_state.monitors[static_cast<size_t>(monitor)].devicePath);
                    if (owner >= 0 && owner != m_state.activeSlot)
                    {
                        Activate(Target{ Hot::GroupChip, owner });
                    }
                }

                ZoomActiveGroup(ZoomFactor(delta), point);
                return 0;
            }

            // Notes page: the history card wheels through the archive the same way the window
            // list pages through its rows - same step arithmetic, different range.
            if (m_tab == Tab::Notes && Contains(g.archiveCard, point))
            {
                const int total = static_cast<int>(m_state.archivedNotes.size());
                const int maxScroll = std::max(0, total - g.archiveRowsCapacity);
                if (maxScroll <= 0)
                {
                    return 0;
                }

                m_wheelResidual += delta;
                const int step = WHEEL_DELTA / 2;
                const int rows = m_wheelResidual / step;
                if (rows == 0)
                {
                    return 0;
                }

                m_wheelResidual -= rows * step;

                const int next = std::clamp(m_archiveScroll - rows, 0, maxScroll);
                if (next != m_archiveScroll)
                {
                    m_archiveScroll = next;
                    ::InvalidateRect(m_window, nullptr, FALSE);
                }

                return 0;
            }

            if (!Contains(g.listCard, point))
            {
                break;
            }

            const int total = static_cast<int>(VisibleRows().size());
            const int maxScroll = std::max(0, total - g.listRows);
            if (maxScroll <= 0)
            {
                return 0;
            }

            // Two rows per notch, with the remainder carried: a precision wheel sends 15..40 units
            // per event, and dropping everything under a full notch makes it feel dead.
            m_wheelResidual += delta;
            const int step = WHEEL_DELTA / 2;
            const int rows = m_wheelResidual / step;
            if (rows == 0)
            {
                return 0;
            }

            m_wheelResidual -= rows * step;

            const int next = std::clamp(m_scroll - rows, 0, maxScroll);
            if (next != m_scroll)
            {
                m_scroll = next;
                ::InvalidateRect(m_window, nullptr, FALSE);
            }

            return 0;
        }

        case WM_SETCURSOR:
        {
            if (m_panning)
            {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }

            POINT point{};
            ::GetCursorPos(&point);
            ::ScreenToClient(m_window, &point);
            point = ToDesign(point);

            const Target target = HitTest(point);

            switch (target.hot)
            {
                case Hot::CanvasBody:
                    if constexpr (kDesktopWidgetsEnabled)
                    {
                        ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
                    }
                    return TRUE;

                case Hot::MapBody:
                    // The board between screens is not a drag surface, and neither is a screen
                    // with no picture on it: the move cursor appears exactly where dragging does
                    // something.
                    ::SetCursor(::LoadCursorW(nullptr,
                        MonitorTakesDrag(MonitorAt(point)) ? IDC_SIZEALL : IDC_ARROW));
                    return TRUE;

                case Hot::None:
                    ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
                    return TRUE;

                default:
                    ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
            }
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // A shortcut almost always carries Alt, which Windows delivers as WM_SYSKEYDOWN.
            // Routing both here is what lets the recording row see Ctrl+Alt+N and not just the N.
            OnKeyDown(wparam);
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(m_window, message, wparam, lparam);
}

void ActivateExistingPanel()
{
    const HWND existing = ::FindWindowW(kPanelClassName, nullptr);
    if (existing == nullptr)
    {
        return;
    }

    if (::IsIconic(existing))
    {
        ::ShowWindow(existing, SW_RESTORE);
    }

    ::PostMessageW(existing, kMsgActivate, 0, 0);

    // SetForegroundWindow is refused for a process that does not own the foreground, which is
    // exactly our case when launched from a shortcut. Flashing the taskbar button is the
    // documented fallback.
    if (!::SetForegroundWindow(existing))
    {
        ::FlashWindow(existing, TRUE);
    }
}

} // namespace wintools::app
