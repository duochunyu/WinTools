#include "test_framework.h"

#include <limits>
#include <vector>

#include "core/layout_math.h"

using namespace wintools::core;

namespace
{

Monitor MakeMonitor(int left, int top, int width, int height, const wchar_t* path = L"DISPLAY1")
{
    Monitor monitor;
    monitor.devicePath = path;
    monitor.monitorKey = Monitor::BuildKey(path);
    monitor.left = left;
    monitor.top = top;
    monitor.width = width;
    monitor.height = height;
    return monitor;
}

} // namespace

// Two identical 1080p screens side by side, source image exactly the size of the bounding box.
// Every piece must be a straight copy: this is the case the whole 1:1 fast path exists for.
WINTOOLS_TEST(LayoutMath_ExactCopy_WhenSourceMatchesCanvas)
{
    const std::vector<Monitor> monitors{
        MakeMonitor(0, 0, 1920, 1080, L"\\\\?\\DISPLAY#AAA#{guid}"),
        MakeMonitor(1920, 0, 1920, 1080, L"\\\\?\\DISPLAY#BBB#{guid}"),
    };

    const Rect canvas = GetCanvas(monitors);
    CHECK_EQ(canvas.left, 0);
    CHECK_EQ(canvas.top, 0);
    CHECK_EQ(canvas.width, 3840);
    CHECK_EQ(canvas.height, 1080);

    const auto left = ComputeSlice(canvas, 3840, 1080, monitors[0]);
    const auto right = ComputeSlice(canvas, 3840, 1080, monitors[1]);
    CHECK(left.has_value());
    CHECK(right.has_value());

    CHECK_EQ(left->canvasX, 0);
    CHECK_EQ(left->canvasY, 0);
    CHECK_EQ(right->canvasX, 1920);
    CHECK_EQ(right->canvasY, 0);

    CHECK_EQ(left->sourceX, 0);
    CHECK_EQ(right->sourceX, 1920);
    CHECK_EQ(left->sourceWidth, 1920);
    CHECK_EQ(left->sourceHeight, 1080);

    CHECK(left->IsExactCopy());
    CHECK(right->IsExactCopy());
}

// A source smaller than the canvas has to be scaled up to cover it.
WINTOOLS_TEST(LayoutMath_CoversCanvas_WhenSourceIsSmaller)
{
    const std::vector<Monitor> monitors{
        MakeMonitor(0, 0, 1920, 1080, L"D1"),
        MakeMonitor(1920, 0, 1920, 1080, L"D2"),
    };

    const Rect canvas = GetCanvas(monitors);

    // Half size in both axes: scale must be 2, and the whole source is used.
    const auto slice = ComputeSlice(canvas, 1920, 540, monitors[0]);
    CHECK(slice.has_value());

    CHECK_EQ(slice->scaledWidth, 3840);
    CHECK_EQ(slice->scaledHeight, 1080);
    CHECK_EQ(slice->sourceX, 0);
    CHECK_EQ(slice->sourceY, 0);
    CHECK_EQ(slice->sourceWidth, 960);
    CHECK_EQ(slice->sourceHeight, 540);
    CHECK_EQ(slice->outputWidth, 1920);
    CHECK_EQ(slice->outputHeight, 1080);
    CHECK(!slice->IsExactCopy());
    CHECK(!slice->unscaled);

    // The first monitor takes the left half of the source, the second takes the right half.
    CHECK_EQ(slice->sourceX + slice->sourceWidth, 960);

    const auto right = ComputeSlice(canvas, 1920, 540, monitors[1]);
    CHECK(right.has_value());
    CHECK_EQ(right->sourceX, 960);
    CHECK_EQ(right->sourceX + right->sourceWidth, 1920);

    // Vertically the source is used completely.
    CHECK_EQ(slice->sourceY + slice->sourceHeight, 540);
}

// A source wider than the canvas is cropped left and right, never scaled.
WINTOOLS_TEST(LayoutMath_CropsHorizontally_WhenSourceIsWider)
{
    const std::vector<Monitor> monitors{
        MakeMonitor(0, 0, 1920, 1080, L"D1"),
        MakeMonitor(1920, 0, 1920, 1080, L"D2"),
    };

    const Rect canvas = GetCanvas(monitors);
    const auto left = ComputeSlice(canvas, 5120, 1080, monitors[0]);
    const auto right = ComputeSlice(canvas, 5120, 1080, monitors[1]);
    CHECK(left.has_value());
    CHECK(right.has_value());

    // scale == 1: no resampling, the image is merely centred and the sides fall outside.
    CHECK_EQ(left->scaledWidth, 5120);
    CHECK_EQ(left->canvasX, 640);
    CHECK_EQ(left->sourceX, 640);
    CHECK_EQ(right->canvasX, 2560);
    CHECK_EQ(right->sourceX, 2560);

    CHECK(left->IsExactCopy());
    CHECK(right->IsExactCopy());
}

// A monitor at negative coordinates (secondary screen on the left) must not produce a negative
// slice offset once the canvas origin is subtracted.
WINTOOLS_TEST(LayoutMath_HandlesNegativeOrigin)
{
    const std::vector<Monitor> monitors{
        MakeMonitor(-1920, 0, 1920, 1080, L"D1"),
        MakeMonitor(0, 0, 2560, 1440, L"D2"),
    };

    const Rect canvas = GetCanvas(monitors);
    CHECK_EQ(canvas.left, -1920);
    CHECK_EQ(canvas.top, 0);
    CHECK_EQ(canvas.width, 4480);
    CHECK_EQ(canvas.height, 1440);

    const auto left = ComputeSlice(canvas, 4480, 1440, monitors[0]);
    const auto right = ComputeSlice(canvas, 4480, 1440, monitors[1]);
    CHECK(left.has_value());
    CHECK(right.has_value());

    CHECK_EQ(left->canvasX, 0);
    CHECK_EQ(right->canvasX, 1920);
    CHECK(left->IsExactCopy());
    CHECK(right->IsExactCopy());
}

// One monitor: the canvas is that monitor, and a 2x source is scaled down to fit exactly.
WINTOOLS_TEST(LayoutMath_SingleMonitor_UsesWholeSource)
{
    const std::vector<Monitor> monitors{ MakeMonitor(100, 50, 2560, 1440, L"D1") };

    const Rect canvas = GetCanvas(monitors);
    CHECK_EQ(canvas.left, 100);
    CHECK_EQ(canvas.top, 50);
    CHECK_EQ(canvas.width, 2560);
    CHECK_EQ(canvas.height, 1440);

    const auto slice = ComputeSlice(canvas, 5120, 2880, monitors[0]);
    CHECK(slice.has_value());

    CHECK_EQ(slice->canvasX, 0);
    CHECK_EQ(slice->canvasY, 0);
    CHECK_EQ(slice->sourceX, 0);
    CHECK_EQ(slice->sourceY, 0);
    CHECK_EQ(slice->sourceWidth, 5120);
    CHECK_EQ(slice->sourceHeight, 2880);
}

// The classic real-world layout from the original code comments: a 3440x1440 ultrawide with two
// 1920x1200 screens below it. The bounding box is 3840 wide, so the top-right corner of the box
// (3440..3840, 0..1440) has no monitor under it. Slices must still be exact and stay in bounds.
WINTOOLS_TEST(LayoutMath_HandlesUncoveredArea_UltrawidePlusTwo)
{
    const std::vector<Monitor> monitors{
        MakeMonitor(0, 0, 3440, 1440, L"UW"),
        MakeMonitor(0, 1440, 1920, 1200, L"BL"),
        MakeMonitor(1920, 1440, 1920, 1200, L"BR"),
    };

    const Rect canvas = GetCanvas(monitors);
    CHECK_EQ(canvas.width, 3840);
    CHECK_EQ(canvas.height, 2640);

    for (const auto& monitor : monitors)
    {
        const auto slice = ComputeSlice(canvas, 3840, 2640, monitor);
        CHECK(slice.has_value());
        CHECK(slice->IsExactCopy());
        CHECK(slice->sourceX >= 0);
        CHECK(slice->sourceY >= 0);
        CHECK(slice->sourceX + slice->sourceWidth <= 3840);
        CHECK(slice->sourceY + slice->sourceHeight <= 2640);
    }

    const auto bottomRight = ComputeSlice(canvas, 3840, 2640, monitors[2]);
    CHECK_EQ(bottomRight->canvasX, 1920);
    CHECK_EQ(bottomRight->canvasY, 1440);
}

// Locks down the rounding rule. C# Math.Round is banker's rounding, so 2.5 becomes 2, not 3.
// Using std::round here would change every slice on layouts where the ratio lands on .5.
WINTOOLS_TEST(LayoutMath_RoundsHalfToEven_LikeCSharp)
{
    // canvas 10x10, source 5x5 -> scale 2. A 5px wide monitor therefore maps to 5/2 = 2.5.
    const std::vector<Monitor> monitors{ MakeMonitor(0, 0, 5, 10, L"D1") };
    const Rect canvas{ 0, 0, 10, 10 };

    const auto slice = ComputeSlice(canvas, 5, 5, monitors[0]);
    CHECK(slice.has_value());

    // 2.5 -> 2 (even), 10/2 = 5 exactly.
    CHECK_EQ(slice->sourceWidth, 2);
    CHECK_EQ(slice->sourceHeight, 5);
}

// Invalid inputs are rejected rather than producing a garbage slice.
WINTOOLS_TEST(LayoutMath_RejectsInvalidInput)
{
    const std::vector<Monitor> monitors{ MakeMonitor(0, 0, 1920, 1080, L"D1") };
    const Rect canvas = GetCanvas(monitors);

    CHECK(!ComputeSlice(canvas, 0, 1080, monitors[0]).has_value());
    CHECK(!ComputeSlice(canvas, 1920, 0, monitors[0]).has_value());
    CHECK(!ComputeSlice(canvas, -1, 1080, monitors[0]).has_value());
    CHECK(!ComputeSlice(Rect{}, 1920, 1080, monitors[0]).has_value());

    const std::vector<Monitor> none;
    CHECK_EQ(GetCanvas(none).width, 0);
}

// A monitor key written by the settings UI has no "#{guid}" suffix; both forms must match.
WINTOOLS_TEST(Monitor_BuildKey_StripsInterfaceGuid)
{
    const auto key = Monitor::BuildKey(L"\\\\?\\DISPLAY#DELA0B9#5&c3b1ae0&0&UID260#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}");
    CHECK(key == L"\\\\?\\DISPLAY#DELA0B9#5&c3b1ae0&0&UID260");

    // No suffix: the input comes back untouched.
    CHECK(Monitor::BuildKey(L"\\\\?\\DISPLAY#ABC") == L"\\\\?\\DISPLAY#ABC");

    const Monitor monitor = MakeMonitor(0, 0, 100, 100, L"\\\\?\\DISPLAY#ABC#{guid}");
    CHECK(monitor.MatchesKey(L"\\\\?\\DISPLAY#ABC"));
    CHECK(monitor.MatchesKey(L"\\\\?\\display#abc#{GUID}"));
    CHECK(!monitor.MatchesKey(L"\\\\?\\DISPLAY#OTHER"));
    CHECK(!monitor.MatchesKey(L""));
}

// The default framing must not move by a single pixel. Everything the panel touched was built on
// top of "cover and centre", and the acceptance criterion for the whole port was pixel equality
// with the original: a default that rounds differently would quietly invalidate that.
WINTOOLS_TEST(LayoutMath_DefaultViewMatchesCentring)
{
    const std::vector<Monitor> monitors{
        MakeMonitor(0, 0, 1920, 1080, L"\\\\?\\DISPLAY#AAA#{guid}"),
        MakeMonitor(0, 1080, 1920, 1080, L"\\\\?\\DISPLAY#BBB#{guid}"),
    };
    const Rect canvas = GetCanvas(monitors);

    // 2000x1000 into a 1920x2160 box: cover scale decided by height, so the picture is much wider
    // than the box and there is real horizontal slack to get wrong.
    const auto implicit = ComputeSlice(canvas, 2000, 1000, monitors[0]);
    const auto explicitDefault = ComputeSlice(canvas, 2000, 1000, monitors[0], WallpaperView{});
    CHECK(implicit.has_value());
    CHECK(explicitDefault.has_value());
    CHECK_EQ(implicit->canvasX, explicitDefault->canvasX);
    CHECK_EQ(implicit->sourceX, explicitDefault->sourceX);
    CHECK_EQ(implicit->scaledWidth, explicitDefault->scaledWidth);
}

// Zoom crops further into the picture, so less of the source fits across the same box.
WINTOOLS_TEST(LayoutMath_ZoomEnlargesThePicture)
{
    const std::vector<Monitor> monitors{ MakeMonitor(0, 0, 1920, 1080) };
    const Rect canvas = GetCanvas(monitors);

    WallpaperView view;
    view.zoom = 2.0;

    const auto plain = ComputeSlice(canvas, 3840, 2160, monitors[0]);
    const auto zoomed = ComputeSlice(canvas, 3840, 2160, monitors[0], view);
    CHECK(plain.has_value());
    CHECK(zoomed.has_value());

    CHECK_EQ(zoomed->scaledWidth, plain->scaledWidth * 2);
    CHECK(zoomed->sourceWidth < plain->sourceWidth);
}

// Panning to an edge must show that edge of the picture and nothing beyond it: the slice is a
// window onto the source, so at focus 1 the last source pixel is on screen.
WINTOOLS_TEST(LayoutMath_FocusPansToTheEdgeOfThePicture)
{
    const std::vector<Monitor> monitors{ MakeMonitor(0, 0, 1920, 1080) };
    const Rect canvas = GetCanvas(monitors);

    // Square source in a 16:9 box: the picture has to be scaled up to cover the width, which
    // leaves vertical slack only - so it is the vertical focus that can move.
    const auto centred = ComputeSlice(canvas, 1080, 1080, monitors[0]);
    CHECK(centred.has_value());
    CHECK(centred->sourceY > 0);

    WallpaperView top;
    top.focusY = 0.0;
    const auto atTop = ComputeSlice(canvas, 1080, 1080, monitors[0], top);
    CHECK(atTop.has_value());
    CHECK_EQ(atTop->sourceY, 0);
    CHECK(atTop->sourceY < centred->sourceY);

    WallpaperView bottom;
    bottom.focusY = 1.0;
    const auto atBottom = ComputeSlice(canvas, 1080, 1080, monitors[0], bottom);
    CHECK(atBottom.has_value());
    CHECK(atBottom->sourceY > centred->sourceY);
    CHECK_EQ(atBottom->sourceY + atBottom->sourceHeight, 1080);

    // ... and horizontal focus is a no-op on the axis that covers exactly.
    WallpaperView sideways;
    sideways.focusX = 0.0;
    const auto shifted = ComputeSlice(canvas, 1080, 1080, monitors[0], sideways);
    CHECK(shifted.has_value());
    CHECK_EQ(shifted->sourceX, centred->sourceX);
}

// The slack is what a mouse drag is divided by, so it has to agree with the geometry that is
// actually rendered: at zoom 1.0 the axis that decided the cover scale has none.
WINTOOLS_TEST(LayoutMath_SlackIsZeroOnTheCoveredAxis)
{
    const std::vector<Monitor> monitors{ MakeMonitor(0, 0, 1920, 1080) };
    const Rect canvas = GetCanvas(monitors);

    // Wide source: height decides the cover scale, so the picture is wider than the box.
    const ViewSlack wide = ComputeSlack(canvas, 4000, 1080, WallpaperView{});
    CHECK_EQ(wide.y, 0);
    CHECK(wide.x > 0);

    WallpaperView twice;
    twice.zoom = 2.0;
    const ViewSlack zoomed = ComputeSlack(canvas, 4000, 1080, twice);
    CHECK(zoomed.x > wide.x);
    CHECK(zoomed.y > 0);
}

// Values from a settings file are user-editable text, and a NaN or a zero would otherwise reach
// the scaler and produce a blank or enormous slice.
WINTOOLS_TEST(LayoutMath_ClampViewRejectsNonsense)
{
    const WallpaperView low = ClampView(WallpaperView{ 0.0, -3.0, 42.0 });
    CHECK_EQ(low.zoom, kMinZoom);
    CHECK_EQ(low.focusX, 0.0);
    CHECK_EQ(low.focusY, 1.0);

    const WallpaperView high = ClampView(WallpaperView{ 99.0, 0.5, 0.5 });
    CHECK_EQ(high.zoom, kMaxZoom);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const WallpaperView broken = ClampView(WallpaperView{ nan, nan, nan });
    CHECK_EQ(broken.zoom, kMinZoom);
    CHECK_EQ(broken.focusX, 0.5);
}
