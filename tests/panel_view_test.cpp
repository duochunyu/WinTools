#include "test_framework.h"

#include "app/panel.h"
#include "core/layout_math.h"

using namespace wintools::app;
using namespace wintools::core;

namespace
{

// The machine this was written on: a 3440x1440 ultrawide above two 1920x1200 screens is a
// 3840x2640 canvas, and a 16:9 photo on it is cover-scaled to 4694x2640 - so at zoom 1 the picture
// is already 22% wider than the canvas and only that axis can be panned. Exactly the case the map
// used to get wrong.
constexpr int kCanvasWidth = 3840;
constexpr int kCanvasHeight = 2640;
constexpr int kMapWidth = 492;
constexpr int kMapHeight = 338; // what the panel's 400-pixel map leaves for that canvas
constexpr ImageSize kSource{ 4000, 2250 };

Panel::MapContext Context(int mapWidth = kMapWidth)
{
    Panel::MapContext context;
    context.canvas = Rect{ 0, 0, kCanvasWidth, kCanvasHeight };
    context.source = kSource;
    context.scale = static_cast<double>(mapWidth) / kCanvasWidth;
    return context;
}

/// A picture with the canvas's own aspect, so cover leaves no slack on either axis.
Panel::MapContext SquareContext()
{
    Panel::MapContext context = Context();
    context.source = ImageSize{ kCanvasWidth, kCanvasHeight };
    return context;
}

ViewSlack Base(const Panel::MapContext& context)
{
    return ComputeSlack(
        context.canvas, context.source.width, context.source.height, WallpaperView{});
}

/// Which fraction of the picture sits under a point of the drawn map. This is the quantity a zoom
/// has to preserve under the cursor, and the quantity a pan has to move by exactly the drag.
///
/// Canvas pixels are 1:1 with pixels of the picture at whatever zoom it is shown at, so the
/// fraction is (offset + point) over the whole enlarged surface - not over the zoom-1 surface.
double PictureAt(
    const Panel::MapContext& context, const WallpaperView& view, int pointMap, int axis)
{
    const Panel::MapFrame frame = Panel::FrameFor(context, view);
    const ViewSlack base = Base(context);

    const int slack = (axis == 0) ? frame.surface.x : frame.surface.y;
    const double focus = (axis == 0) ? view.focusX : view.focusY;
    const double surfaceOne = (axis == 0) ? base.scaledWidth : base.scaledHeight;

    const double offset = (slack > 0) ? std::lround(slack * focus) : 0.0;
    const double point = pointMap / context.scale;

    return (offset + point) / (surfaceOne * view.zoom);
}

} // namespace

// Zooming in on the middle of the canvas has to leave the middle of the picture in the middle.
// This is the case where a sign error is invisible: the picture still covers the canvas, so only
// the content under the cursor gives it away.
WINTOOLS_TEST(Panel_ZoomAtTheCentreKeepsTheCentre)
{
    const Panel::MapContext context = Context();
    const WallpaperView zoomed = Panel::ZoomView(
        WallpaperView{}, context, POINT{ kMapWidth / 2, kMapHeight / 2 }, 2.0);

    CHECK(zoomed.zoom == 2.0);
    CHECK(std::abs(zoomed.focusX - 0.5) < 0.01);
    CHECK(std::abs(zoomed.focusY - 0.5) < 0.01);
}

// Zooming on a point away from the middle must bring that part of the picture under the cursor,
// not the centre of it. This is the "wheel over the thing you want to look at" behaviour.
WINTOOLS_TEST(Panel_ZoomKeepsThePixelUnderTheCursor)
{
    const Panel::MapContext context = Context();
    const POINT anchor{ kMapWidth / 4, 60 };

    const double wasX = PictureAt(context, WallpaperView{}, anchor.x, 0);
    const double wasY = PictureAt(context, WallpaperView{}, anchor.y, 1);

    const WallpaperView zoomed = Panel::ZoomView(WallpaperView{}, context, anchor, 2.0);
    CHECK(std::abs(PictureAt(context, zoomed, anchor.x, 0) - wasX) < 0.005);
    CHECK(std::abs(PictureAt(context, zoomed, anchor.y, 1) - wasY) < 0.005);

    // Three times over: the same anchor has to hold at any zoom, not just at 2x.
    const WallpaperView further = Panel::ZoomView(zoomed, context, anchor, 1.5);
    CHECK(std::abs(PictureAt(context, further, anchor.x, 0) - wasX) < 0.005);
    CHECK(std::abs(PictureAt(context, further, anchor.y, 1) - wasY) < 0.005);
}

// One notch is a 10% step, and a fraction of a notch is a fraction of that step in log terms: the
// same total delta has to land on the same zoom however it is chopped up. That is what makes a
// slow scroll a slow zoom - the old integer division dropped every event under a full notch, so
// a precision wheel did nothing at all until it happened to cross 120.
WINTOOLS_TEST(Panel_ZoomStepIsProportionalToTheWheel)
{
    CHECK(std::abs(Panel::ZoomFactor(120) - 1.10) < 1e-9);
    CHECK(std::abs(Panel::ZoomFactor(-120) - 1.0 / 1.10) < 1e-9);
    CHECK(Panel::ZoomFactor(0) == 1.0);

    // A precision touchpad tick has to move the picture a little, not nothing.
    const double fine = Panel::ZoomFactor(15);
    CHECK(fine > 1.005);
    CHECK(fine < 1.02);

    // Eight of those add up to exactly one notch.
    double product = 1.0;
    for (int i = 0; i < 8; ++i)
    {
        product *= Panel::ZoomFactor(15);
    }
    CHECK(std::abs(product - Panel::ZoomFactor(120)) < 1e-12);
}

// A driver that reports an absurd delta must not teleport the zoom: one event is capped, so the
// picture cannot jump from 1x to 8x on a single flick.
WINTOOLS_TEST(Panel_ZoomFactorCapsASingleFlick)
{
    CHECK(std::abs(Panel::ZoomFactor(12000) - Panel::ZoomFactor(1200)) < 1e-12);
    CHECK(Panel::ZoomFactor(12000) < 3.0);
    CHECK(std::abs(Panel::ZoomFactor(-12000) * Panel::ZoomFactor(1200) - 1.0) < 1e-12);
}

// The bug this locks down: at zoom 1 the map used to compute its own slack from the drawn box, so
// it claimed 0/0 and refused to pan - while the renderer, given the same view, was cropping a
// picture that is genuinely wider than the canvas. Dragging did nothing and looked broken.
WINTOOLS_TEST(Panel_PanWorksAtZoomOneWhenTheAspectsDiffer)
{
    const Panel::MapContext context = Context();
    const Panel::MapFrame frame = Panel::FrameFor(context, WallpaperView{});

    CHECK(frame.surface.x > 0);
    CHECK(frame.surface.y == 0);

    const WallpaperView moved = Panel::PanView(WallpaperView{}, frame, 100, 100);
    CHECK(moved.focusX < 0.5);

    // Nothing to move vertically, so that axis must come back untouched rather than clamped to an
    // arbitrary end.
    CHECK(moved.focusY == 0.5);
    CHECK(moved.zoom == 1.0);
}

// Dragging right reveals what is to the left of the picture, so the focus moves back by exactly
// the fraction of the slack the cursor travelled. Slack is in canvas pixels, the drag in map
// pixels, and getting that scale wrong makes the picture run ahead of or behind the cursor.
WINTOOLS_TEST(Panel_PanMovesThePictureWithTheCursor)
{
    const Panel::MapContext context = Context();
    WallpaperView view;
    view.zoom = 2.0;

    const Panel::MapFrame frame = Panel::FrameFor(context, view);
    const double slackX = frame.surface.x * context.scale;
    const double slackY = frame.surface.y * context.scale;

    const WallpaperView moved = Panel::PanView(view, frame, 100, -40);
    CHECK(std::abs(moved.focusX - (0.5 - 100.0 / slackX)) < 0.0001);
    CHECK(std::abs(moved.focusY - (0.5 + 40.0 / slackY)) < 0.0001);

    // The picture under the cursor moves with it: a 100-pixel drag shifts the visible crop by the
    // same 100 map pixels. In surface-1 units that is 100 / (scale * zoom), hence the factors.
    const double zoom = view.zoom;
    const double before = PictureAt(context, view, 200, 0);
    const double after = PictureAt(context, moved, 200, 0);
    const double shiftedMapPx =
        (before - after) * Base(context).scaledWidth * context.scale * zoom;
    CHECK(std::abs(shiftedMapPx - 100.0) < 2.0);
}

// When the picture has the canvas's own aspect there is genuinely nothing to pan at zoom 1, and
// the view must come back untouched instead of with a focus the renderer would ignore anyway.
WINTOOLS_TEST(Panel_PanDoesNothingAtCoverSize)
{
    const Panel::MapContext context = SquareContext();
    const Panel::MapFrame frame = Panel::FrameFor(context, WallpaperView{});

    CHECK(frame.surface.x == 0);
    CHECK(frame.surface.y == 0);
    CHECK(Panel::PanView(WallpaperView{}, frame, 120, 90) == WallpaperView{});
}

// The edges are hard stops, not elastic: dragging far past the end clamps at the edge instead of
// asking the renderer for a crop that does not exist.
WINTOOLS_TEST(Panel_PanStopsAtTheEdges)
{
    const Panel::MapContext context = Context();
    WallpaperView view;
    view.zoom = 2.0;

    const Panel::MapFrame frame = Panel::FrameFor(context, view);

    const WallpaperView pushed = Panel::PanView(view, frame, -5000, 5000);
    CHECK(pushed.focusX == 1.0);
    CHECK(pushed.focusY == 0.0);
}

// Zooming all the way back out returns the centred crop, which is the one the renderer produces
// for an untouched view - byte for byte what the app did before any of this existed.
WINTOOLS_TEST(Panel_ZoomingOutRestoresTheDefaultView)
{
    const Panel::MapContext context = Context();

    WallpaperView view;
    view.zoom = 1.6;
    view.focusX = 0.2;
    view.focusY = 0.8;

    CHECK(Panel::ZoomView(view, context, POINT{ 40, 40 }, 0.1) == WallpaperView{});
}

// Nonsense must not escape: a zero or negative wheel factor, a bad context or a missing picture
// leaves the view alone instead of producing a NaN that would blank the map and poison every slice.
WINTOOLS_TEST(Panel_ZoomRejectsNonsense)
{
    const Panel::MapContext context = Context();
    WallpaperView view;
    view.zoom = 2.0;

    CHECK(Panel::ZoomView(view, context, POINT{ 10, 10 }, 0.0) == view);
    CHECK(Panel::ZoomView(view, context, POINT{ 10, 10 }, -2.0) == view);

    Panel::MapContext empty = context;
    empty.source = ImageSize{};
    CHECK(Panel::ZoomView(view, empty, POINT{ 10, 10 }, 1.5) == view);
    CHECK(Panel::PanView(view, Panel::FrameFor(empty, view), 10, 10) == view);
}

// The preview bitmap has to hold the whole picture at zoom 1, margins included, and every sample
// has to land inside it at any zoom - a sample that runs off the edge draws whatever is in the
// bitmap's tail, which is nothing at all.
WINTOOLS_TEST(Panel_PreviewHoldsEverythingTheMapSamples)
{
    const Panel::MapContext context = Context();
    const SIZE size = Panel::PreviewSize(context);

    const ViewSlack base = Base(context);
    CHECK(size.cx >= static_cast<int>(std::lround(base.scaledWidth * context.scale)));
    CHECK(size.cy >= static_cast<int>(std::lround(base.scaledHeight * context.scale)));

    // Wider than the drawn canvas, because that extra margin is what a pan at zoom 1 moves into.
    CHECK(size.cx > static_cast<int>(std::lround(kCanvasWidth * context.scale)));

    const WallpaperView views[] = { WallpaperView{}, WallpaperView{ 1.0, 0.0, 0.0 },
        WallpaperView{ 1.0, 1.0, 1.0 }, WallpaperView{ 2.0, 0.5, 0.5 },
        WallpaperView{ 8.0, 0.0, 0.0 }, WallpaperView{ 8.0, 1.0, 1.0 } };

    const Rect whole{ 0, 0, kCanvasWidth, kCanvasHeight };

    for (const auto& view : views)
    {
        const Panel::MapFrame frame = Panel::FrameFor(context, view);
        const Panel::MapSample sample = Panel::SampleFor(frame, whole, view);

        CHECK(sample.x >= 0);
        CHECK(sample.y >= 0);
        CHECK(sample.x + sample.width <= size.cx);
        CHECK(sample.y + sample.height <= size.cy);
    }
}

// The map must show the very slices the renderer will write. Both sides derive the crop from the
// same framing, so this asserts the second half - turning a monitor rectangle into a piece of the
// preview - against core::ComputeSlice, which is what actually produces the files.
WINTOOLS_TEST(Panel_MapSampleAgreesWithTheRenderedSlice)
{
    Monitor left;
    left.left = 0;
    left.top = 0;
    left.width = 1920;
    left.height = 2640;

    Monitor right;
    right.left = 1920;
    right.top = 0;
    right.width = 1920;
    right.height = 2640;

    const Panel::MapContext context = Context();

    const WallpaperView views[] = {
        WallpaperView{},
        WallpaperView{ 1.0, 0.0, 0.0 },
        WallpaperView{ 1.0, 1.0, 1.0 },
        WallpaperView{ 2.0, 0.5, 0.5 },
        WallpaperView{ 3.5, 0.25, 0.75 },
    };

    for (const auto& view : views)
    {
        const Panel::MapFrame frame = Panel::FrameFor(context, view);

        for (const Monitor* monitor : { &left, &right })
        {
            const auto slice = ComputeSlice(context.canvas, context.source.width,
                context.source.height, *monitor, view);
            CHECK(slice.has_value());
            if (!slice.has_value())
            {
                continue;
            }

            const Rect slot{ monitor->left - context.canvas.left, monitor->top - context.canvas.top,
                monitor->width, monitor->height };
            const Panel::MapSample sample = Panel::SampleFor(frame, slot, view);

            // The slice reports its position inside the enlarged picture in canvas pixels; the
            // preview holds the zoom-1 picture, so the position divides by zoom and multiplies by
            // the map scale and the supersample.
            const double per = context.scale * kMapSupersample / view.zoom;
            CHECK(std::abs(sample.x - static_cast<int>(std::lround(slice->canvasX * per))) <= 1);
            CHECK(std::abs(sample.y - static_cast<int>(std::lround(slice->canvasY * per))) <= 1);

            // A display's piece has to be the display's size, or the map would draw it at the
            // wrong scale and the seams would not line up.
            CHECK(std::abs(sample.width - static_cast<int>(std::lround(monitor->width * per))) <= 1);
        }
    }
}

// Two displays side by side must not leave a one-pixel white line between them on the map: that
// reads as a gap in the arrangement and sends people hunting for a problem that is not there.
WINTOOLS_TEST(Panel_AdjacentDisplaysMeetWithoutASeam)
{
    const Panel::MapContext context = Context();

    const WallpaperView views[] = { WallpaperView{}, WallpaperView{ 2.0, 0.0, 0.0 },
        WallpaperView{ 1.7, 0.8, 0.2 } };

    for (const auto& view : views)
    {
        const Panel::MapFrame frame = Panel::FrameFor(context, view);

        const Panel::MapSample first =
            Panel::SampleFor(frame, Rect{ 0, 0, 1920, 2640 }, view);
        const Panel::MapSample second =
            Panel::SampleFor(frame, Rect{ 1920, 0, 1920, 2640 }, view);

        CHECK(first.x + first.width == second.x);
    }
}

// The sheet has to fit the screen it opens on. This machine's work area is 1150 px, so without
// these cases the short-screen branch would never run anywhere.
WINTOOLS_TEST(Panel_SheetFitsCommonWorkAreas)
{
    // Pinned against a hand-measured value: a 264-unit map and twelve rows is the 907-unit sheet
    // the design is drawn at. If the geometry chain moves, this goes red first.
    CHECK(Panel::SheetHeight(264, 12) == 907);

    // 1440p and up: the designed map height and a full list.
    CHECK(Panel::FitSheet(1440).mapHeight == 264);
    CHECK(Panel::FitSheet(1440).rows == 12);

    // 1150 px - this machine: the full sheet still fits at 100% scaling.
    CHECK(Panel::FitSheet(1150).mapHeight == 264);
    CHECK(Panel::FitSheet(1150).rows == 12);

    // 1080p at 100%.
    CHECK(Panel::FitSheet(1040).mapHeight == 264);
    CHECK(Panel::FitSheet(1040).rows == 12);

    // 900 px: rows give way first, and only three of them are gone.
    const Panel::SheetFit small = Panel::FitSheet(900);
    CHECK(small.mapHeight == 264);
    CHECK(small.rows == 10);
}

// The map is the part the sheet refuses to give up: rows are given back first, and only when even
// the smallest list does not fit does the map drop a size.
WINTOOLS_TEST(Panel_ShortWorkAreaShrinksTheMap)
{
    const Panel::SheetFit small = Panel::FitSheet(800);
    CHECK(small.mapHeight == 264);
    CHECK(small.rows == 7);
    CHECK(small.rows < 12);

    // At 600 units even the smallest list has to take the smallest map.
    const Panel::SheetFit tiny = Panel::FitSheet(600);
    CHECK(tiny.mapHeight == 176);
    CHECK(tiny.rows == 3);
}

// Whatever the work area, the sheet must not be taller than it - that is the whole point of the
// function - and it must never claim a list it cannot show.
WINTOOLS_TEST(Panel_SheetNeverExceedsTheWorkArea)
{
    for (int workArea = 600; workArea <= 2160; workArea += 20)
    {
        const Panel::SheetFit fit = Panel::FitSheet(workArea);
        CHECK(fit.rows >= 3);
        CHECK(fit.rows <= 12);

        const int height = Panel::SheetHeight(fit.mapHeight, fit.rows);
        if (height + 40 > workArea)
        {
            // Nothing fits: the fallback has to be the smallest sheet there is.
            CHECK(fit.mapHeight == 176);
            CHECK(fit.rows == 3);
        }
    }
}

// The chosen fit must be tight on both axes: one more row does not fit, and neither does the next
// map size up with the row count it settled on. Catches a loop that stops one candidate early.
WINTOOLS_TEST(Panel_SheetUsesTheTallestMapThatFits)
{
    const int workArea = 800;
    const Panel::SheetFit fit = Panel::FitSheet(workArea);

    CHECK(Panel::SheetHeight(fit.mapHeight, fit.rows + 1) + 40 > workArea);

    // Where the map had to shrink, the next size up must genuinely not fit either.
    const Panel::SheetFit tiny = Panel::FitSheet(600);
    CHECK(tiny.mapHeight == 176);
    CHECK(Panel::SheetHeight(220, tiny.rows) + 40 > 600);
}

// --- DPI ------------------------------------------------------------------------------------

// The sheet is one design, stencilled at whatever size the display asks for. Doubling the scale
// factor must double every distance and change nothing about the layout.
WINTOOLS_TEST(Panel_ScalesTheSheetByTheDisplayDpi)
{
    CHECK(Panel::ForDpi(900, 96) == 900);
    CHECK(Panel::ForDpi(900, 120) == 1125);
    CHECK(Panel::ForDpi(900, 144) == 1350);
    CHECK(Panel::ForDpi(900, 192) == 1800);

    // A design distance has to survive the round trip through the display, because the mouse
    // position is divided by the same factor it was multiplied by.
    for (const UINT dpi : { 96u, 120u, 144u, 168u, 192u, 240u })
    {
        for (int design = 0; design < 900; design += 7)
        {
            const int device = Panel::ForDpi(design, dpi);
            CHECK(::MulDiv(device, 96, static_cast<int>(dpi)) == design);
        }
    }
}

// The fit question is asked in design units, so a scaled display has to be converted down first -
// and the answer has to fit back into the display it came from, chrome included.
WINTOOLS_TEST(Panel_FitOnAScaledDisplayFitsTheRealWorkArea)
{
    const int smallest = Panel::SheetHeight(176, 3) + 40;

    for (const UINT dpi : { 96u, 120u, 144u, 192u })
    {
        for (int deviceWorkArea = 1040; deviceWorkArea <= 2160; deviceWorkArea += 80)
        {
            if (Panel::ForDpi(smallest, dpi) > deviceWorkArea)
            {
                continue; // nothing fits at this size, so there is no claim to check
            }

            // Rounded down, mirroring Create: a work area of 1360 px at 150% is 906.7 design
            // units, and claiming the extra third of a unit makes the fitted sheet one pixel
            // taller than the screen it was fitted to.
            const int design =
                static_cast<int>(deviceWorkArea) * 96 / static_cast<int>(dpi);
            const Panel::SheetFit fit = Panel::FitSheet(design);

            const int deviceHeight =
                Panel::ForDpi(Panel::SheetHeight(fit.mapHeight, fit.rows) + 40, dpi);
            CHECK(deviceHeight <= deviceWorkArea);
            CHECK(fit.rows >= 3);
        }
    }
}

// A scaled display is a smaller work area in design units: a 1200-px monitor at 125% offers 920
// units. The full twelve rows and the designed map are 947 units, which does not go into 920, so
// one row goes - the map keeps its height, because a slightly shorter list is a scroll and a
// slightly shorter map is a worse thumbnail.
WINTOOLS_TEST(Panel_KeepsAUsableListOnAScaledDisplay)
{
    const int workArea = (1200 - 50) * 96 / 120;
    const Panel::SheetFit fit = Panel::FitSheet(workArea);

    CHECK(fit.rows == 11);
    CHECK(fit.mapHeight == 264);
    CHECK(Panel::SheetHeight(fit.mapHeight, fit.rows) + 40 <= workArea);
}

// --- Invariants -----------------------------------------------------------------------------

// I3: selecting a group changes the outline's thickness, never its geometry. Both states share
// the outer edge, so the picture cannot move when the user clicks a chip.
WINTOOLS_TEST(Panel_GroupFrameGrowsInwardOnly)
{
    const RECT box{ 100, 50, 400, 250 };
    const RECT frame = Panel::FrameBounds(box);

    CHECK(frame.left == box.left - 2);
    CHECK(frame.top == box.top - 2);
    CHECK(frame.right == box.right + 2);
    CHECK(frame.bottom == box.bottom + 2);

    CHECK(Panel::FrameThickness(false) == 1);
    CHECK(Panel::FrameThickness(true) == 2);
    CHECK(Panel::FrameThickness(true) > Panel::FrameThickness(false));
}

// I6: one button size in the whole panel. The footer's 退出 is the same control as the title
// row's 更换壁纸, and a test is the only thing that keeps a future variant from "just" being two
// pixels taller.
WINTOOLS_TEST(Panel_HasOneButtonAndOneRowSize)
{
    CHECK(Panel::ButtonHeight() == 30);
    CHECK(Panel::ListRowHeight() == 32);
}

// The sheet's fixed furniture is the difference between the whole sheet and its two variable
// bands, and the fit arithmetic depends on it. Pinning it separately means a change to the chained
// geometry shows up as one red line rather than as dozens of unexplained failures.
WINTOOLS_TEST(Panel_SheetFurnitureIsConstant)
{
    for (const int map : { 176, 220, 264 })
    {
        for (const int rows : { 3, 7, 12 })
        {
            CHECK(Panel::SheetHeight(map, rows) == map + rows * Panel::ListRowHeight() + 259);
        }
    }
}
