#include "app/panel.h"

#include <Windows.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>

#include "core/monitor.h"
#include "util/i18n.h"
#include "util/log.h"

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
// The sheet is one column of four bands, top to bottom:
//
//   title bar -> wallpaper block (head / hint / map) -> layout block (head / hint / list)
//   -> footer (+ optional history strip)
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

constexpr int kTitleBarHeight = 36;
constexpr int kSheetPaddingX = 18;
constexpr int kSheetPaddingTop = 12;
constexpr int kSheetPaddingBottom = 12;

/// A block's first row: title, object picker, one primary, then the secondary actions.
constexpr int kHeadHeight = 32;

/// The hint row: the shortcut sentence on the left, read-only numbers on the right.
constexpr int kHintHeight = 18;
constexpr int kHintGapTop = 4;
constexpr int kHintGapBottom = 7;

constexpr int kMapHeight = 264;
constexpr int kMapHeightCandidates[] = { 264, 220, 176 };

/// Hairline between the two blocks, with the same air on both sides.
constexpr int kDividerBefore = 12;
constexpr int kDividerAfter = 10;

constexpr int kRowHeight = 32;
constexpr int kListBorder = 1;

constexpr int kFooterGap = 11;
constexpr int kFooterPad = 10;
constexpr int kFooterHeight = 30;

constexpr int kButtonHeight = 30;
constexpr int kButtonPadX = 10;

/// The three records that keep a failure visible after the next success overwrote the status
/// line. An error is not removed early; it is only pushed out by three later records.
constexpr int kHistoryItems = 3;

/// The most rows FitSheet will consider, and the fewest it will accept.
constexpr int kMaxListRows = 12;
constexpr int kMinListRows = 3;

/// Furniture: everything in the sheet that is neither the map nor the list - title bar, sheet
/// padding, both block heads and hint rows, the divider, and the footer. The list's own hairline
/// border is part of the list term in SheetHeight, not of this. A test pins the total, so a band
/// that changes height shows up as a red line instead of a clipped footer.
constexpr int kSheetFurniture = 257;

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
template <typename PixelFn>
void ForEachCornerPixel(const RECT& dev, int radius, PixelFn&& pixel)
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

    for (int row = 0; row < 2; ++row)
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

void RasterFillRounded(PaintTarget& target, const RECT& design, int radius, COLORREF color)
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

    // One vertical band plus two side bands, leaving exactly the four corner squares over.
    FillDeviceRect(
        surface, RECT{ dev.left + corner, dev.top, dev.right - corner, dev.bottom }, color);
    FillDeviceRect(
        surface, RECT{ dev.left, dev.top + corner, dev.left + corner, dev.bottom - corner }, color);
    FillDeviceRect(
        surface, RECT{ dev.right - corner, dev.top + corner, dev.right, dev.bottom - corner },
        color);

    ForEachCornerPixel(dev, corner,
        [&](int x, int y, double cx, double cy)
        {
            BlendPixel(surface, x, y, color, CircleCoverage(x + 0.5, y + 0.5, cx, cy, corner));
        });
}

void RasterStrokeRounded(
    PaintTarget& target, const RECT& design, int radius, COLORREF color, int designWidth)
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

    // Straight runs, one band per edge, each sitting just inside the outline.
    FillDeviceRect(
        surface, RECT{ dev.left + corner, dev.top, dev.right - corner, dev.top + thickness },
        color);
    FillDeviceRect(
        surface, RECT{ dev.left + corner, dev.bottom - thickness, dev.right - corner, dev.bottom },
        color);
    FillDeviceRect(
        surface, RECT{ dev.left, dev.top + corner, dev.left + thickness, dev.bottom - corner },
        color);
    FillDeviceRect(
        surface, RECT{ dev.right - thickness, dev.top + corner, dev.right, dev.bottom - corner },
        color);

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
        });
}

// --- Drawing --------------------------------------------------------------------------------

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF color)
{
    if (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0)
    {
        return;
    }

    if (g_paintTarget != nullptr && g_paintTarget->surface.valid)
    {
        RasterFillRounded(*g_paintTarget, rect, radius, color);
        return;
    }

    // Fallback for a DC with no sheet behind it. Nothing in the panel paints that way today; the
    // path exists so this function cannot silently do nothing if something ever does.
    const HBRUSH brush = ::CreateSolidBrush(color);
    const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    const HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));

    // RoundRect takes the full width and height of the corner ellipse, not its radius.
    ::RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);

    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

void StrokeRounded(HDC dc, const RECT& rect, int radius, COLORREF color, int width = 0)
{
    if (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0)
    {
        return;
    }

    if (g_paintTarget != nullptr && g_paintTarget->surface.valid)
    {
        RasterStrokeRounded(*g_paintTarget, rect, radius, color, width);
        return;
    }

    // Zero is a cosmetic pen: one device pixel whatever the scale factor. As a design unit it
    // would be 1.5 pixels at 150%, which GDI renders as a two-pixel smear on one edge and one on
    // the other - a border that looks thicker than the card it outlines.
    const HPEN pen = ::CreatePen(PS_SOLID, width, color);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    // Inset by one design unit: RoundRect strokes on the outline, so an un-inset card looks half a
    // pixel larger than its fill and the border sits outside the card.
    ::RoundRect(dc, rect.left, rect.top, rect.right - 1, rect.bottom - 1, radius * 2, radius * 2);

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

/// Where the visible window starts inside the enlarged picture, in canvas pixels.
///
/// Mirrors core's FrameOffset: an untouched view keeps the truncating division the original
/// pipeline produced, anything the user moved rounds.
int WindowOffset(int slack, double focus, bool untouchedDefault)
{
    if (slack <= 0)
    {
        return 0;
    }

    if (untouchedDefault)
    {
        return slack / 2;
    }

    return std::clamp(static_cast<int>(std::lround(slack * focus)), 0, slack);
}

bool IsUntouched(const core::WallpaperView& view)
{
    return view.zoom == core::kMinZoom && view.focusX == 0.5 && view.focusY == 0.5;
}

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

    const core::ViewSlack surface = core::ComputeSlack(
        context.canvas, context.source.width, context.source.height, core::WallpaperView{});

    const double factor = context.scale * kMapSupersample;
    size.cx = std::max(1, static_cast<int>(std::lround(surface.scaledWidth * factor)));
    size.cy = std::max(1, static_cast<int>(std::lround(surface.scaledHeight * factor)));
    return size;
}

core::WallpaperView Panel::PanView(
    const core::WallpaperView& view, const MapFrame& frame, int dx, int dy)
{
    core::WallpaperView result = view;

    // Dragging right reveals what is to the left of the picture, so the focus moves back by the
    // fraction of the slack the cursor travelled.
    const double slackX = frame.surface.x * frame.scale;
    const double slackY = frame.surface.y * frame.scale;

    if (slackX > 0.0)
    {
        result.focusX = std::clamp(view.focusX - dx / slackX, 0.0, 1.0);
    }

    if (slackY > 0.0)
    {
        result.focusY = std::clamp(view.focusY - dy / slackY, 0.0, 1.0);
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
    // is the centred one the renderer uses for an untouched view.
    if (zoom <= core::kMinZoom)
    {
        return core::WallpaperView{};
    }

    const core::WallpaperView before = core::ClampView(view);
    const MapFrame was = FrameFor(context, before);

    core::WallpaperView result = before;
    result.zoom = zoom;
    const MapFrame now = FrameFor(context, result);

    const core::ViewSlack base = core::ComputeSlack(
        context.canvas, context.source.width, context.source.height, core::WallpaperView{});
    const bool untouched = IsUntouched(before);

    const auto keep = [&](int slackBefore, double focusBefore, int surfaceOne, int slackAfter,
                          int pointMap) -> double
    {
        if (slackAfter <= 0 || surfaceOne <= 0 || before.zoom <= 0.0)
        {
            return 0.5;
        }

        const double point = pointMap / context.scale;
        const double offset = WindowOffset(slackBefore, focusBefore, untouched);

        // Which fraction of the whole picture sits under the cursor, then the offset that keeps
        // that same fraction there at the new zoom.
        const double picture = (offset + point) / (surfaceOne * before.zoom);
        const double moved = picture * surfaceOne * zoom - point;

        return std::clamp(moved / slackAfter, 0.0, 1.0);
    };

    const int limitX =
        std::max(0, static_cast<int>(std::lround(context.canvas.width * context.scale)));
    const int limitY =
        std::max(0, static_cast<int>(std::lround(context.canvas.height * context.scale)));
    const int anchorX = std::clamp(static_cast<int>(anchor.x), 0, limitX);
    const int anchorY = std::clamp(static_cast<int>(anchor.y), 0, limitY);

    result.focusX = keep(was.surface.x, before.focusX, base.scaledWidth, now.surface.x, anchorX);
    result.focusY = keep(was.surface.y, before.focusY, base.scaledHeight, now.surface.y, anchorY);
    return result;
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
    const bool untouched = IsUntouched(clamped);

    const int offsetX = WindowOffset(frame.surface.x, clamped.focusX, untouched);
    const int offsetY = WindowOffset(frame.surface.y, clamped.focusY, untouched);

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

// --- Sheet arithmetic -----------------------------------------------------------------------

int Panel::SheetHeight(int mapHeight, int rows)
{
    // Two terms that vary plus one constant, mirroring the cursor chain in ComputeGeometry: the
    // map, the window list (32 per row plus its hairline border), and the bands around them.
    const int listHeight = rows * kRowHeight + 2 * kListBorder;
    return mapHeight + listHeight + kSheetFurniture;
}

Panel::SheetFit Panel::FitSheet(int workAreaHeight)
{
    // The map first, then as many rows as fit beside it.
    //
    // This is the opposite order from the pre-groups panel, and deliberately so: there the map
    // was a corner widget and the list was the point, while here the map is what the whole block
    // exists to show - it is where the framing is judged. A row that does not fit costs one
    // scroll; a map that shrinks costs the thing the user opened the panel for.
    for (const int mapHeight : kMapHeightCandidates)
    {
        for (int rows = kMaxListRows; rows >= kMinListRows; --rows)
        {
            if (SheetHeight(mapHeight, rows) + kWindowChrome <= workAreaHeight)
            {
                return SheetFit{ mapHeight, rows };
            }
        }
    }

    return SheetFit{ kMapHeightCandidates[std::size(kMapHeightCandidates) - 1], kMinListRows };
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

    // Borderless: the design draws its own 36 px title bar, and a caption above it would be a
    // second, differently styled one. Dragging is handled in WM_NCHITTEST instead.
    const DWORD style = WS_POPUP;

    const int sheetHeight = ComputeGeometry().clientHeight;

    // Placed on whichever display the cursor is on: the panel is usually opened from the tray,
    // and the tray lives on the display the user is looking at.
    POINT cursor{};
    ::GetCursorPos(&cursor);
    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    ::GetMonitorInfoW(monitor, &monitorInfo);

    m_window = ::CreateWindowExW(0, kPanelClassName, L"WinTools", style, monitorInfo.rcWork.left,
        monitorInfo.rcWork.top, kWidth, sheetHeight, nullptr, nullptr, instance, this);

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

    // How tall the sheet may be depends on the work area at this scale, and the fit arithmetic is
    // in design units, so the work area is converted down before it is handed over. Rounded down
    // rather than to nearest: the question is how many whole design units the display has room
    // for, and ResizeToContent rounds the answer back up.
    MONITORINFO landed{};
    landed.cbSize = sizeof(landed);
    if (::GetMonitorInfoW(::MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST), &landed))
    {
        const int workHeight = landed.rcWork.bottom - landed.rcWork.top;
        const int designHeight = workHeight * kDesignDpi / static_cast<int>(m_dpi);
        const SheetFit fit = FitSheet(designHeight);
        m_maxRows = fit.rows;
        m_mapHeight = fit.mapHeight;
    }

    // Windows 11 rounds our corners; older builds simply ignore the attribute.
    const auto preference = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(DWMWCP_ROUND);
    ::DwmSetWindowAttribute(
        m_window, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));

    CentreOnCursorMonitor();
    Refresh();

    LogInfo(L"panel on a {} dpi display, sheet {}x{} design units", m_dpi, kWidth,
        ComputeGeometry().clientHeight);

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
    if (active != nullptr && (m_viewSlot != m_state.activeSlot || (!m_panning && !m_zoomPending)))
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

    ResizeToContent();

    // Scrolling past the end after the list shrank would leave the view showing nothing.
    const int total = static_cast<int>(VisibleRows().size());
    const int maxScroll = std::max(0, total - m_maxRows);
    m_scroll = std::clamp(m_scroll, 0, maxScroll);

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

void Panel::ResizeToContent()
{
    if (m_window == nullptr || ::IsIconic(m_window))
    {
        return;
    }

    const int wantedWidth = ForDpi(kWidth, m_dpi);
    const int wantedHeight = ForDpi(ComputeGeometry().clientHeight, m_dpi);

    RECT client{};
    ::GetClientRect(m_window, &client);
    if (client.right - client.left == wantedWidth && client.bottom - client.top == wantedHeight)
    {
        return;
    }

    // Borderless, so the client area is the window: no chrome to add back.
    ::SetWindowPos(m_window, nullptr, 0, 0, wantedWidth, wantedHeight,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Then put it back inside the work area: a sheet that grew by a row can end up with its
    // footer under the taskbar.
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

Panel::Geometry Panel::ComputeGeometry() const
{
    Geometry g;
    const int left = kSheetPaddingX;
    const int right = kWidth - kSheetPaddingX;

    g.titleBar = { 0, 0, kWidth, kTitleBarHeight };

    // Four 28x26 buttons at the right end, 10 units from the edge.
    const int iconSize = 26;
    const int iconWidth = 28;
    const int iconTop = (kTitleBarHeight - iconSize) / 2;
    int iconRight = kWidth - 10;
    g.iconClose = { iconRight - iconWidth, iconTop, iconRight, iconTop + iconSize };
    iconRight -= iconWidth;
    g.iconHelp = { iconRight - iconWidth, iconTop, iconRight, iconTop + iconSize };
    iconRight -= iconWidth;
    g.iconTheme = { iconRight - iconWidth, iconTop, iconRight, iconTop + iconSize };
    iconRight -= iconWidth;
    g.iconMenu = { iconRight - iconWidth, iconTop, iconRight, iconTop + iconSize };

    const auto centreIn = [](int top, int height) { return top + (height - kButtonHeight) / 2; };

    int y = kTitleBarHeight + kSheetPaddingTop;

    // --- Wallpaper block: head ---
    g.wallHead = { left, y, right, y + kHeadHeight };

    const int titleWidth =
        static_cast<int>(MeasureText(m_measure, m_fontTitle, L"壁纸").cx) + 9 + 1 + 9;

    const int applyWidth = ButtonWidth(m_measure, m_fontBody, L"应用壁纸");
    const int pickWidth = ButtonWidth(m_measure, m_fontBody, L"更换壁纸");

    const int headTop = centreIn(y, kHeadHeight);
    g.applyWallpaper = { right - applyWidth, headTop, right, headTop + kButtonHeight };
    g.pick = { g.applyWallpaper.left - 8 - pickWidth, headTop, g.applyWallpaper.left - 8,
        headTop + kButtonHeight };

    // Group chips flow left to right after the title; the row is clipped at the buttons.
    const int chipCapacity = g.pick.left - 8 - (left + titleWidth);
    int chipX = left + titleWidth;

    // Chip order is by label, so "1+2" always sits left of "3+4" regardless of the order the
    // groups were created in. The slot stays with the group; only the drawing sorts.
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

        if (chipX + width > left + titleWidth + chipCapacity)
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

    // --- Wallpaper block: hint row ---
    g.wallHint = { left, y, right, y + kHintHeight };

    const int metaWidth = static_cast<int>(MeasureText(m_measure, m_fontAux, L"4 台 · 2 组").cx);
    g.groupMeta = { right - metaWidth, y, right, y + kHintHeight };

    int hintRight = g.groupMeta.left - 10;
    if (const core::Group* active = m_state.ActiveGroup();
        active != nullptr && active->dirty)
    {
        const int dirtyWidth = static_cast<int>(MeasureText(m_measure, m_fontAux, L"● 未应用").cx);
        g.dirtyBadge = { hintRight - dirtyWidth, y, hintRight, y + kHintHeight };
        hintRight = g.dirtyBadge.left - 10;
    }

    g.zoomBadge = { hintRight - 40, y, hintRight, y + kHintHeight };

    y += kHintHeight + kHintGapBottom;

    // --- Wallpaper block: the map ---
    g.mapCard = { left, y, right, y + m_mapHeight };
    g.mapArea = { g.mapCard.left + 14, g.mapCard.top + 12, g.mapCard.right - 14,
        g.mapCard.bottom - 12 };
    y += m_mapHeight;

    y += kDividerBefore;
    g.divider = y;
    y += 1 + kDividerAfter;

    // --- Layout block: head ---
    g.layoutHead = { left, y, right, y + kHeadHeight };

    const int layoutTitleWidth =
        static_cast<int>(MeasureText(m_measure, m_fontTitle, L"布局").cx) + 9 + 1 + 9;

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
    int chipLeft = left + layoutTitleWidth;
    for (const size_t index : core::ReadingOrder(m_state.monitors))
    {
        const std::wstring& path = m_state.monitors[index].devicePath;
        const std::wstring number = std::to_wstring(core::ScreenNumber(path, m_state.monitors));

        int count = 0;
        for (const auto& window : m_state.windows)
        {
            if (core::Monitor::IEquals(window.monitorPath, path))
            {
                ++count;
            }
        }

        const std::wstring caption = std::format(L"({})", count);
        const int width = 9 + static_cast<int>(MeasureText(m_measure, m_fontNum, number).cx) + 4
                        + static_cast<int>(MeasureText(m_measure, m_fontSmall, caption).cx) + 9;

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

    // --- Layout block: hint row ---
    g.layoutHint = { left, y, right, y + kHintHeight };

    const int windowMetaWidth =
        static_cast<int>(MeasureText(m_measure, m_fontAux, L"888 / 888 个窗口").cx);
    g.windowMeta = { right - windowMetaWidth, y, right, y + kHintHeight };

    const int snapshotWidth =
        static_cast<int>(MeasureText(m_measure, m_fontAux, L"快照 88-88 88:88 · 不是实时桌面").cx);
    g.snapshotMeta = { g.windowMeta.left - 10 - snapshotWidth, y, g.windowMeta.left - 10,
        y + kHintHeight };

    y += kHintHeight + kHintGapBottom;

    // --- Layout block: the list ---
    const int total = static_cast<int>(VisibleRows().size());
    g.listRows = std::min(total, m_maxRows);
    g.firstRow = std::clamp(m_scroll, 0, std::max(0, total - g.listRows));

    const int listHeight = std::max(1, g.listRows) * kRowHeight + 2 * kListBorder;
    g.listCard = { left, y, right, y + listHeight };
    g.listArea = { g.listCard.left + kListBorder, g.listCard.top + kListBorder,
        g.listCard.right - kListBorder, g.listCard.top + kListBorder + g.listRows * kRowHeight };
    y += listHeight;

    // --- Footer ---
    y += kFooterGap;
    const int footerContent = y + 1 + kFooterPad;
    g.footer = { left, y, right, footerContent + kFooterHeight };
    g.statusDot = { left, footerContent + (kFooterHeight - 7) / 2, left + 7,
        footerContent + (kFooterHeight - 7) / 2 + 7 };
    g.quit = { right - ButtonWidth(m_measure, m_fontBody, L"退出"), footerContent, right,
        footerContent + kButtonHeight };
    g.statusText = { g.statusDot.right + 6, footerContent, g.quit.left - 12,
        footerContent + kFooterHeight };

    y = footerContent + kFooterHeight;

    // --- Optional history strip ---
    if (m_historyOpen)
    {
        // One line even when there is nothing to list: "no records yet" is information, and a
        // strip reserved for three entries that are not there is just a hole in the sheet.
        y += 8;
        const size_t lines = std::max<size_t>(1, m_state.history.size());
        const int height = 8 + 15 + static_cast<int>(lines) * 17 + 8;
        g.history = { left, y, right, y + height };
        y += height;
    }

    g.clientHeight = y + kSheetPaddingBottom;
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
        for (size_t index = 0; index < rects.size(); ++index)
        {
            if (rects[index].right > rects[index].left && Contains(rects[index], point))
            {
                return { Hot::PopupItem, static_cast<int>(index) };
            }
        }

        return { Hot::PopupItem, -2 };
    }

    if (Contains(g.iconMenu, point))
    {
        return { Hot::IconMenu, -1 };
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

HBITMAP Panel::PreviewFor(int slot, const std::wstring& path, int width, int height)
{
    if (path.empty() || width <= 0 || height <= 0)
    {
        return nullptr;
    }

    for (auto& preview : m_previews)
    {
        if (preview.Matches(slot, path, width, height))
        {
            return preview.bitmap;
        }
    }

    const auto decoded = core::RenderPreview(path, width, height);
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

            preview = GroupPreview{ slot, path, decoded->width, decoded->height, bitmap };
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

    m_previews.push_back(GroupPreview{ slot, path, decoded->width, decoded->height, bitmap });
    return bitmap;
}

Panel::MapContext Panel::ContextForGroup(const Projection& projection, const core::Group& group)
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

    // Scaled mapping mode: one design unit becomes (width / kWidth) device pixels for everything
    // drawn below - rectangles, corner radii, pen widths and font heights alike. GDI resolves a
    // logical font against this mapping, so text is rasterised at the device size instead of being
    // stretched from a 96-dpi bitmap.
    // The device extent is derived from the design size rather than taken from the client
    // rectangle. If the two ever disagree - a state change that altered the row count without the
    // window being re-measured yet - using the client size would stretch the sheet along one axis
    // and every label would be clipped horizontally, which looks like a font bug and is not one.
    // A strip of background is a much better failure than a distorted panel.
    const int deviceWidth = ForDpi(kWidth, m_dpi);
    const int deviceHeight = ForDpi(g.clientHeight, m_dpi);

    FillBox(dc, RECT{ 0, 0, width, height }, m_state.darkTheme ? kDark.bg : kLight.bg);

    ::SetMapMode(dc, MM_ANISOTROPIC);
    ::SetWindowExtEx(dc, kWidth, g.clientHeight, nullptr);
    ::SetViewportExtEx(dc, std::min(deviceWidth, width), std::min(deviceHeight, height), nullptr);

    m_surface = PixelSurface{ static_cast<std::uint32_t*>(bits), width, height,
        static_cast<double>(width) / kWidth, static_cast<double>(height) / g.clientHeight, true };

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

    FillBox(dc, RECT{ 0, 0, kWidth, geometry.clientHeight }, t.bg);

    PaintTitleBar(dc, geometry);
    PaintWallpaperBlock(dc, geometry);

    // Divider: one hairline, both blocks equally far from it.
    FillBox(dc,
        RECT{ kSheetPaddingX, geometry.divider, kWidth - kSheetPaddingX, geometry.divider + 1 },
        t.line);

    PaintLayoutBlock(dc, geometry);
    PaintFooter(dc, geometry);

    if (m_historyOpen)
    {
        PaintHistory(dc, geometry);
    }

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
    StrokeBox(dc, RECT{ 0, 0, kWidth, geometry.clientHeight }, t.line2);
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
    DrawTextBlock(dc, m_fontTitle, t.text, RECT{ nameLeft, 0, nameLeft + 90, kTitleBarHeight },
        L"WinTools", DT_LEFT);

    const int nameWidth = static_cast<int>(MeasureText(m_measure, m_fontTitle, L"WinTools").cx);
    DrawTextBlock(dc, m_fontAux, t.text3,
        RECT{ nameLeft + nameWidth + 9, 0, geometry.iconMenu.left - 12, kTitleBarHeight },
        L"跨屏壁纸与窗口布局", DT_LEFT);

    FillBox(dc, RECT{ 0, kTitleBarHeight - 1, kWidth, kTitleBarHeight }, t.line);

    const auto icon = [&](const RECT& rect, const wchar_t* glyph, Hot hot, bool danger) {
        const bool hovered = m_hot == hot;
        if (hovered)
        {
            FillRounded(dc, rect, 6, danger ? t.dangerWeak : t.hover);
        }

        DrawTextBlock(dc, m_fontIcon,
            danger ? (hovered ? t.danger : t.text3) : (hovered ? t.text : t.text3), rect, glyph,
            DT_CENTER);
    };

    icon(geometry.iconMenu, L"\u2699", Hot::IconMenu, false);
    icon(geometry.iconTheme, L"\u25D0", Hot::IconTheme, false);
    icon(geometry.iconHelp, L"?", Hot::IconHelp, false);
    icon(geometry.iconClose, L"\u00D7", Hot::IconClose, true);
}

void Panel::PaintWallpaperBlock(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;
    const core::Group* active = m_state.ActiveGroup();

    // --- Head ---
    const int titleRight = geometry.wallHead.left
                         + static_cast<int>(MeasureText(m_measure, m_fontTitle, L"壁纸").cx) + 9;
    DrawTextBlock(dc, m_fontTitle, t.text,
        RECT{ geometry.wallHead.left, geometry.wallHead.top, titleRight,
            geometry.wallHead.bottom },
        L"壁纸", DT_LEFT);
    FillBox(dc,
        RECT{ titleRight + 8, geometry.wallHead.top + 8, titleRight + 9,
            geometry.wallHead.bottom - 8 },
        t.line2);

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
        RECT{ geometry.wallHint.left, geometry.wallHint.top, geometry.zoomBadge.left - 10,
            geometry.wallHint.bottom },
        L"屏上拖动 = 移动壁纸 · 滚轮 = 缩放 · 点屏左上角编号 = 改组 · 双击 = 复位",
        DT_LEFT | DT_END_ELLIPSIS);

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

    for (int slot = 0; slot < static_cast<int>(m_state.groups.size()); ++slot)
    {
        const core::Group& group = m_state.groups[static_cast<size_t>(slot)];
        const std::vector<size_t> members = core::AttachedMembers(group, m_state.monitors);
        if (members.empty())
        {
            continue;
        }

        // During a drag the live framing is in m_view, not in the model yet.
        const core::WallpaperView view =
            (slot == m_state.activeSlot && m_viewSlot == slot) ? m_view : group.view;

        // One bitmap per group, decoded for that group's own canvas. Handing every group the
        // whole desktop's canvas would show the same crop in every group, which is exactly what
        // per-group framing exists to avoid.
        const MapContext context = ContextForGroup(map.projection, group);
        const MapFrame frame = FrameFor(context, view);
        const SIZE previewSize = PreviewSize(context);
        const HBITMAP preview = group.image.empty()
            ? nullptr
            : PreviewFor(slot, group.image, previewSize.cx, previewSize.cy);

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

    const int titleRight = geometry.layoutHead.left
                         + static_cast<int>(MeasureText(m_measure, m_fontTitle, L"布局").cx) + 9;
    DrawTextBlock(dc, m_fontTitle, t.text,
        RECT{ geometry.layoutHead.left, geometry.layoutHead.top, titleRight,
            geometry.layoutHead.bottom },
        L"布局", DT_LEFT);
    FillBox(dc,
        RECT{ titleRight + 8, geometry.layoutHead.top + 8, titleRight + 9,
            geometry.layoutHead.bottom - 8 },
        t.line2);

    // Screen chips: numbers, not buttons. An unselected chip has no border and no fill, which is
    // what makes the row read as a legend rather than as four toggles.
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

void Panel::PaintHistory(HDC dc, const Geometry& geometry)
{
    const Theme& t = m_state.darkTheme ? kDark : kLight;

    FillRounded(dc, geometry.history, 8, t.card);
    StrokeRounded(dc, geometry.history, 8, t.line);

    DrawTextBlock(dc, m_fontAux, t.text4,
        RECT{ geometry.history.left + 10, geometry.history.top + 6, geometry.history.right - 10,
            geometry.history.top + 22 },
        L"最近 3 条状态（错误不会被下一次成功悄悄抹掉）", DT_LEFT | DT_END_ELLIPSIS);

    int y = geometry.history.top + 22;

    if (m_state.history.empty())
    {
        DrawTextBlock(dc, m_fontAux, t.text4,
            RECT{ geometry.history.left + 20, y, geometry.history.right - 10, y + 15 },
            L"暂无记录", DT_LEFT);
        return;
    }

    for (int index = 0; index < kHistoryItems; ++index)
    {
        if (index >= static_cast<int>(m_state.history.size()))
        {
            break;
        }

        const StatusEntry& entry = m_state.history[static_cast<size_t>(index)];
        const COLORREF ink = entry.kind == StatusKind::Error ? t.danger
            : entry.kind == StatusKind::Ok               ? t.ok
                                                         : t.text3;

        FillRounded(dc,
            RECT{ geometry.history.left + 12, y + 6, geometry.history.left + 15, y + 9 }, 2, ink);

        DrawTextBlock(dc, m_fontAux, ink,
            RECT{ geometry.history.left + 20, y, geometry.history.right - 10, y + 15 },
            entry.text, DT_LEFT | DT_END_ELLIPSIS);
        y += 17;
    }
}

// --- Popups and modal -----------------------------------------------------------------------

std::vector<Panel::PopupEntry> Panel::PopupItems() const
{
    std::vector<PopupEntry> items;

    switch (m_popup)
    {
        case Popup::AppMenu:
            // ids, not row indexes: the separator occupies an index, and clicking the row after
            // it used to fall through to nothing.
            items.push_back(PopupEntry{ .text = L"查看日志", .id = 0 });
            items.push_back(PopupEntry{ .text = L"状态记录", .id = 1, .checked = m_historyOpen });
            items.push_back(PopupEntry{ .separator = true });
            items.push_back(PopupEntry{ .text = L"开机启动", .id = 2, .checked = m_state.autostart });
            items.push_back(PopupEntry{
                .text = std::wstring{ util::i18n::LanguageLabel() }, .id = 3 });
            return items;

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

        default:
            return {};
    }
}

RECT Panel::PopupRect(const Geometry& geometry) const
{
    const std::vector<PopupEntry> items = PopupItems();
    const std::wstring head = PopupHead();

    int width = m_popup == Popup::AppMenu ? 196 : 216;
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
    if (at.right > kWidth - 8)
    {
        at.left = kWidth - 8 - width;
        at.right = kWidth - 8;
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
        if ((m_hot == Hot::PopupItem) && m_hotIndex == static_cast<int>(index))
        {
            FillRounded(dc, row, 6, t.hover);
        }

        int textLeft = row.left + 10;

        if (m_popup == Popup::AppMenu)
        {
            // The check column is always reserved, so labels do not shift when one is ticked.
            if (item.checked)
            {
                DrawTextBlock(dc, m_fontBody, t.accent,
                    RECT{ row.left + 4, row.top, row.left + 20, row.bottom }, L"✓", DT_LEFT);
            }
            textLeft = row.left + 20;
        }
        else
        {
            const int pipTop = (row.top + row.bottom) / 2 - 5;

            if (item.dashed)
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

        if (!item.trailing.empty())
        {
            DrawTextBlock(dc, m_fontAux, t.text4,
                RECT{ row.right - 72, row.top, row.right - 8, row.bottom }, item.trailing,
                DT_RIGHT | DT_END_ELLIPSIS);
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

    layout.card = RECT{ (kWidth - width) / 2, (geometry.clientHeight - height) / 2,
        (kWidth + width) / 2, (geometry.clientHeight + height) / 2 };

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

    ring.push_back({ Hot::IconMenu, -1 });
    ring.push_back({ Hot::IconTheme, -1 });
    ring.push_back({ Hot::IconHelp, -1 });
    ring.push_back({ Hot::IconClose, -1 });

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

    for (const int monitor : geometry.chipMonitors)
    {
        ring.push_back({ Hot::ScreenChip, monitor });
    }

    ring.push_back({ Hot::AutoLayout, -1 });
    ring.push_back({ Hot::Refresh, -1 });
    ring.push_back({ Hot::ApplyLayout, -1 });

    const std::vector<int> rows = VisibleRows();
    for (int slot = 0; slot < geometry.listRows; ++slot)
    {
        const int position = geometry.firstRow + slot;
        if (position < static_cast<int>(rows.size()))
        {
            ring.push_back({ Hot::RowCheck, rows[static_cast<size_t>(position)] });
        }
    }

    for (int index = 0; index < static_cast<int>(m_state.monitors.size()); ++index)
    {
        ring.push_back({ Hot::ScreenBadge, index });
    }

    ring.push_back({ Hot::Quit, -1 });
    return ring;
}

RECT Panel::RectForTarget(const Geometry& geometry, const Target& target) const
{
    switch (target.hot)
    {
        case Hot::IconMenu:
            return geometry.iconMenu;
        case Hot::IconTheme:
            return geometry.iconTheme;
        case Hot::IconHelp:
            return geometry.iconHelp;
        case Hot::IconClose:
            return geometry.iconClose;
        case Hot::Pick:
            return geometry.pick;
        case Hot::ApplyWallpaper:
            return geometry.applyWallpaper;
        case Hot::AutoLayout:
            return geometry.autoSwitch;
        case Hot::Refresh:
            return geometry.refresh;
        case Hot::ApplyLayout:
            return geometry.applyLayout;
        case Hot::Quit:
            return geometry.quit;

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

    // The list just changed length, so the sheet has to be re-measured before it is painted -
    // otherwise the panel keeps a height that belonged to the previous row count.
    ResizeToContent();
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
        case Popup::AppMenu:
            switch (entry.id)
            {
                case 0:
                    emit(PanelAction::OpenLog);
                    break;
                case 1:
                    m_historyOpen = !m_historyOpen;
                    ResizeToContent();
                    ::InvalidateRect(m_window, nullptr, FALSE);
                    break;
                case 2:
                    emit(PanelAction::ToggleAutostart);
                    break;
                case 3:
                    emit(PanelAction::CycleLanguage);
                    break;
                default:
                    break;
            }
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

        case Popup::None:
            return;
    }
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

    ResizeToContent();
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
        case Hot::IconMenu:
        {
            const Geometry g = ComputeGeometry();
            OpenPopup(Popup::AppMenu, POINT{ g.iconMenu.left - 150, g.iconMenu.bottom + 6 }, -1);
            return;
        }

        case Hot::IconTheme:
            emit(PanelAction::CycleTheme);
            return;

        case Hot::IconHelp:
            m_modal = Modal::Help;
            ::InvalidateRect(m_window, nullptr, FALSE);
            return;

        case Hot::IconClose:
            HidePanel();
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

        case Hot::Quit:
            m_modal = Modal::Quit;
            ::InvalidateRect(m_window, nullptr, FALSE);
            return;

        case Hot::PopupItem:
        case Hot::ModalButton:
        case Hot::MapBody:
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
        ZoomView(m_view, ContextForGroup(map.projection, *active), at, factor);

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

    const MapFrame frame = FrameFor(ContextForGroup(map.projection, *active), m_view);
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

    m_view = core::WallpaperView{};
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
                ResizeToContent();
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
            // Dragged onto a display with another scale factor. Nothing about the sheet changes -
            // it is all design units - so this only has to re-measure it.
            const UINT dpi = HIWORD(wparam);
            if (dpi != 0)
            {
                m_dpi = dpi;
            }

            ScaleMeasureDc();
            ResizeToContent();
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

        case WM_NCHITTEST:
        {
            // No caption to drag by, so the title bar is declared to be one. The icon buttons opt
            // back out, or they could never be clicked.
            POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ::ScreenToClient(m_window, &point);
            const POINT design = ToDesign(point);
            const Geometry g = ComputeGeometry();

            if (design.y < kTitleBarHeight)
            {
                const bool onIcon = Contains(g.iconMenu, design) || Contains(g.iconTheme, design)
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

            if (m_panning)
            {
                const Geometry g = ComputeGeometry();
                const MapLayout map = LayoutMap(g);
                const core::Group* active = m_state.ActiveGroup();
                if (map.valid && active != nullptr)
                {
                    const MapFrame frame = FrameFor(ContextForGroup(map.projection, *active),
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
                for (size_t index = 0; index < rects.size(); ++index)
                {
                    if (rects[index].right > rects[index].left && Contains(rects[index], point))
                    {
                        found = static_cast<int>(index);
                        break;
                    }
                }

                if (found != m_popupItem)
                {
                    m_popupItem = found;
                    m_hot = Hot::PopupItem;
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
                if (hit.hot == Hot::PopupItem && hit.index >= 0)
                {
                    m_pressed = Hot::PopupItem;
                    m_pressedIndex = hit.index;
                    return 0;
                }

                ClosePopup();
                return 0;
            }

            const Target target = HitTest(point);

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

            if (pressed == Hot::PopupItem)
            {
                const Geometry g = ComputeGeometry();
                const std::vector<RECT> rects = PopupItemRects(g, PopupRect(g));
                if (pressedIndex >= 0 && pressedIndex < static_cast<int>(rects.size())
                    && Contains(rects[static_cast<size_t>(pressedIndex)], lifted))
                {
                    ActivatePopup(pressedIndex);
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
