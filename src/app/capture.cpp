#include "app/capture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include <windowsx.h>
#include <wincodec.h>

#include "platform/win/com.h"
#include "platform/win/wic.h"
#include "util/i18n.h"
#include "util/log.h"
#include "util/path.h"
#include "util/text.h"

namespace wintools::app
{

namespace
{

constexpr wchar_t kOverlayClass[] = L"WinTools.Capture.3a91f0d2";

constexpr COLORREF kAccent = RGB(0xE8, 0xC0, 0x6A);
constexpr COLORREF kLabelInk = RGB(0x1A, 0x1A, 0x1E);

/// Brightness of everything the selection does not cover, in 1/256ths. The selected rectangle keeps
/// the original frame, so what the user is about to capture is never tinted.
constexpr unsigned kScrim = 150;

/// Below this, the drag is a click that was meant to cancel, not a selection.
constexpr int kMinimumDrag = 8;

RECT Normalise(POINT from, POINT to)
{
    return RECT{ std::min(from.x, to.x), std::min(from.y, to.y), std::max(from.x, to.x),
        std::max(from.y, to.y) };
}

int RegionWidth(const RECT& rect)
{
    return rect.right - rect.left;
}

int RegionHeight(const RECT& rect)
{
    return rect.bottom - rect.top;
}

/// An empty top-down 32bpp DIB section, sized for the given device.
HBITMAP MakeDib(HDC reference, int width, int height, void** bits)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    const HBITMAP bitmap = ::CreateDIBSection(reference, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr)
    {
        if (bitmap != nullptr)
        {
            ::DeleteObject(bitmap);
        }
        return nullptr;
    }

    *bits = pixels;
    return bitmap;
}

/// GDI leaves the alpha byte at zero, and a PNG that says "fully transparent" is a note with a hole
/// in it. A screenshot is opaque by definition, so the byte is set here rather than carrying a
/// premultiplied format through the encoder.
void MakeOpaque(void* bits, int width, int height)
{
    auto* pixels = static_cast<std::uint32_t*>(bits);
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t index = 0; index < count; ++index)
    {
        pixels[index] |= 0xFF000000u;
    }
}

/// Copies the screen into a top-down 32bpp DIB.
///
/// CAPTUREBLT is required: without it the blit misses layered windows, and the thing being
/// captured is usually a video, a game or, in this project's own case, a widget.
HBITMAP GrabToDib(HDC screen, int x, int y, int width, int height, void** bits)
{
    const HBITMAP bitmap = MakeDib(screen, width, height, bits);
    if (bitmap == nullptr)
    {
        return nullptr;
    }

    const HDC memory = ::CreateCompatibleDC(screen);
    const HGDIOBJ old = ::SelectObject(memory, bitmap);

    const BOOL ok = ::BitBlt(
        memory, 0, 0, width, height, screen, x, y, SRCCOPY | static_cast<DWORD>(CAPTUREBLT));

    ::SelectObject(memory, old);
    ::DeleteDC(memory);

    if (!ok)
    {
        ::DeleteObject(bitmap);
        return nullptr;
    }

    MakeOpaque(*bits, width, height);
    return bitmap;
}

/// Shrinks a DIB in place when it is wider than kCaptureMaxWidth, keeping the aspect ratio.
HBITMAP ScaleDown(HDC screen, HBITMAP source, void* sourceBits, int width, int height,
    void** bits, int& outWidth, int& outHeight)
{
    if (width <= kCaptureMaxWidth)
    {
        *bits = sourceBits;
        outWidth = width;
        outHeight = height;
        return source;
    }

    const double factor = static_cast<double>(kCaptureMaxWidth) / width;
    outWidth = kCaptureMaxWidth;
    outHeight = std::max(1, static_cast<int>(std::lround(height * factor)));

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = outWidth;
    info.bmiHeader.biHeight = -outHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    const HBITMAP scaled = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (scaled == nullptr)
    {
        *bits = sourceBits;
        outWidth = width;
        outHeight = height;
        return source;
    }

    const HDC sourceDc = ::CreateCompatibleDC(screen);
    const HGDIOBJ oldSource = ::SelectObject(sourceDc, source);

    const HDC targetDc = ::CreateCompatibleDC(screen);
    const HGDIOBJ oldTarget = ::SelectObject(targetDc, scaled);

    // HALFTONE rather than COLORONCOLOR: a screenshot shrunk by a third keeps its text legible
    // with a box filter, and turns to aliased mush without one.
    ::SetStretchBltMode(targetDc, HALFTONE);
    ::SetBrushOrgEx(targetDc, 0, 0, nullptr);
    ::StretchBlt(targetDc, 0, 0, outWidth, outHeight, sourceDc, 0, 0, width, height, SRCCOPY);

    ::SelectObject(targetDc, oldTarget);
    ::SelectObject(sourceDc, oldSource);
    ::DeleteDC(targetDc);
    ::DeleteDC(sourceDc);

    auto* pixels32 = static_cast<std::uint32_t*>(pixels);
    const auto count = static_cast<std::size_t>(outWidth) * outHeight;
    for (std::size_t index = 0; index < count; ++index)
    {
        pixels32[index] |= 0xFF000000u;
    }

    ::DeleteObject(source);

    *bits = pixels;
    return scaled;
}

bool EncodePngBits(void* bits, int width, int height, std::vector<std::uint8_t>& out)
{
    IWICImagingFactory* const factory = platform::GetWicFactory();
    if (factory == nullptr)
    {
        util::LogError(L"screen capture: WIC is unavailable");
        return false;
    }

    const UINT stride = static_cast<UINT>(width) * 4;
    platform::ComPtr<IWICBitmap> bitmap;
    HRESULT hr = factory->CreateBitmapFromMemory(static_cast<UINT>(width),
        static_cast<UINT>(height), GUID_WICPixelFormat32bppBGRA, stride,
        stride * static_cast<UINT>(height), static_cast<BYTE*>(bits),
        bitmap.GetAddressOf());
    if (FAILED(hr))
    {
        util::LogError(L"screen capture: CreateBitmapFromMemory failed, {}", util::FormatHresult(hr));
        return false;
    }

    platform::ComPtr<IStream> stream;
    hr = ::CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf());
    if (FAILED(hr))
    {
        util::LogError(L"screen capture: CreateStreamOnHGlobal failed, {}", util::FormatHresult(hr));
        return false;
    }

    platform::ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (FAILED(hr))
    {
        util::LogError(L"screen capture: no PNG encoder, {}", util::FormatHresult(hr));
        return false;
    }

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr))
    {
        return false;
    }

    platform::ComPtr<IWICBitmapFrameEncode> frame;
    platform::ComPtr<IPropertyBag2> properties;
    hr = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
    if (FAILED(hr))
    {
        return false;
    }

    hr = frame->Initialize(properties.Get());
    if (FAILED(hr))
    {
        return false;
    }

    hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    if (FAILED(hr))
    {
        return false;
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr))
    {
        return false;
    }

    hr = frame->WriteSource(bitmap.Get(), nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = frame->Commit();
    if (FAILED(hr))
    {
        return false;
    }

    hr = encoder->Commit();
    if (FAILED(hr))
    {
        return false;
    }

    HGLOBAL memory = nullptr;
    hr = ::GetHGlobalFromStream(stream.Get(), &memory);
    if (FAILED(hr) || memory == nullptr)
    {
        return false;
    }

    const SIZE_T size = ::GlobalSize(memory);
    const void* data = ::GlobalLock(memory);
    if (data == nullptr || size == 0)
    {
        ::GlobalUnlock(memory);
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out.assign(bytes, bytes + size);

    ::GlobalUnlock(memory);
    return true;
}

/// The frozen screen the selector works on: the frame to cut from, and the same frame dimmed
/// everywhere the selection is not.
///
/// Freezing is what lets the overlay be an ordinary window, and an ordinary window is the only kind
/// that can receive the drag. The first version of this file keyed the overlay transparent instead
/// and therefore shipped a selector that nothing could click on: with LWA_COLORKEY the keyed pixels
/// are click-through, so every mouse event went to whatever was underneath.
struct Backdrop
{
    HBITMAP bright{};
    HBITMAP dim{};
    HDC brightDc{};
    HDC dimDc{};
    int width{};
    int height{};
};

void DestroyBackdrop(Backdrop& backdrop)
{
    // The bitmaps are still selected into their DCs; deleting the DC first is what releases them.
    if (backdrop.brightDc != nullptr)
    {
        ::DeleteDC(backdrop.brightDc);
        backdrop.brightDc = nullptr;
    }

    if (backdrop.dimDc != nullptr)
    {
        ::DeleteDC(backdrop.dimDc);
        backdrop.dimDc = nullptr;
    }

    if (backdrop.bright != nullptr)
    {
        ::DeleteObject(backdrop.bright);
        backdrop.bright = nullptr;
    }

    if (backdrop.dim != nullptr)
    {
        ::DeleteObject(backdrop.dim);
        backdrop.dim = nullptr;
    }
}

bool BuildBackdrop(int x, int y, int width, int height, Backdrop& backdrop)
{
    const HDC screen = ::GetDC(nullptr);
    if (screen == nullptr)
    {
        return false;
    }

    void* brightBits = nullptr;
    void* dimBits = nullptr;

    backdrop.bright = GrabToDib(screen, x, y, width, height, &brightBits);
    if (backdrop.bright != nullptr)
    {
        backdrop.dim = MakeDib(screen, width, height, &dimBits);
    }

    if (backdrop.bright == nullptr || backdrop.dim == nullptr
        || (backdrop.brightDc = ::CreateCompatibleDC(screen)) == nullptr
        || (backdrop.dimDc = ::CreateCompatibleDC(screen)) == nullptr)
    {
        ::ReleaseDC(nullptr, screen);
        DestroyBackdrop(backdrop);
        util::LogError(L"screen capture: BitBlt failed at ({}, {}) {}x{}", x, y, width, height);
        return false;
    }

    backdrop.width = width;
    backdrop.height = height;

    ::SelectObject(backdrop.brightDc, backdrop.bright);
    ::SelectObject(backdrop.dimDc, backdrop.dim);
    ::BitBlt(backdrop.dimDc, 0, 0, width, height, backdrop.brightDc, 0, 0, SRCCOPY);

    // Dimmed once, here, rather than per frame: the scrim never changes, so painting it is one
    // BitBlt of one rectangle however the user drags.
    auto* pixels = static_cast<std::uint32_t*>(dimBits);
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t index = 0; index < count; ++index)
    {
        const std::uint32_t pixel = pixels[index];
        const std::uint32_t blue = ((pixel & 0xFFu) * kScrim) >> 8;
        const std::uint32_t green = (((pixel >> 8) & 0xFFu) * kScrim) >> 8;
        const std::uint32_t red = (((pixel >> 16) & 0xFFu) * kScrim) >> 8;
        pixels[index] = 0xFF000000u | (red << 16) | (green << 8) | blue;
    }

    ::ReleaseDC(nullptr, screen);
    return true;
}

/// Cuts the selected rectangle out of the frozen frame and encodes it.
///
/// No second grab of the screen: what the user outlined is what was on screen when the selection
/// started, which is also what they were looking at while they dragged it.
bool CropToPng(const Backdrop& backdrop, const RECT& region, CapturedImage& image)
{
    const int width = RegionWidth(region);
    const int height = RegionHeight(region);
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    void* bits = nullptr;
    HBITMAP crop = MakeDib(backdrop.brightDc, width, height, &bits);
    if (crop == nullptr)
    {
        return false;
    }

    const HDC memory = ::CreateCompatibleDC(backdrop.brightDc);
    const HGDIOBJ old = ::SelectObject(memory, crop);
    ::BitBlt(memory, 0, 0, width, height, backdrop.brightDc, region.left, region.top, SRCCOPY);
    ::SelectObject(memory, old);
    ::DeleteDC(memory);

    int finalWidth = width;
    int finalHeight = height;
    void* finalBits = nullptr;
    crop = ScaleDown(
        backdrop.brightDc, crop, bits, width, height, &finalBits, finalWidth, finalHeight);

    const bool ok = EncodePngBits(finalBits, finalWidth, finalHeight, image.png);
    if (ok)
    {
        image.width = finalWidth;
        image.height = finalHeight;

        // Kept alongside the PNG so the clipboard does not have to decode what this line just
        // encoded. The pixels belong to the bitmap above and die with it, hence the copy.
        const std::size_t stride = static_cast<std::size_t>(finalWidth) * 4;
        const std::size_t bytes = stride * static_cast<std::size_t>(finalHeight);
        const auto* pixels = static_cast<const std::uint8_t*>(finalBits);
        image.bgra.assign(pixels, pixels + bytes);
    }

    ::DeleteObject(crop);
    return ok;
}

/// The rectangle selector. One instance at a time, so the window procedure can find it.
struct SelectionState
{
    RECT rect{};
    RECT drawn{};   // the selection whose frame is currently on screen
    POINT origin{};
    bool dragging{};
    bool finished{};
    bool cancelled{ true };
    HWND window{};
    const Backdrop* backdrop{};
};

// A frame is a 2 px pen plus a label that can sit above the selection and be wider than it. The
// rectangle we dirty on each move is the union of the old and the new selection, grown enough to
// carry both frames - still a few thousand pixels instead of the whole virtual desktop.
constexpr int kFrameMarginX = 128;
constexpr int kFrameMarginY = 40;

RECT FrameDamage(const RECT& oldSelection, const RECT& newSelection)
{
    RECT dirty{};
    if (::IsRectEmpty(&oldSelection))
    {
        dirty = newSelection;
    }
    else if (::IsRectEmpty(&newSelection))
    {
        dirty = oldSelection;
    }
    else
    {
        ::UnionRect(&dirty, &oldSelection, &newSelection);
    }

    ::InflateRect(&dirty, kFrameMarginX, kFrameMarginY);
    return dirty;
}

SelectionState* g_selection = nullptr;

void DrawLabel(HDC dc, const RECT& selection)
{
    const int width = RegionWidth(selection);
    const int height = RegionHeight(selection);
    if (width < kMinimumDrag || height < kMinimumDrag)
    {
        return;
    }

    const std::wstring text = util::i18n::Format(L"{} × {}", width, height);

    HDC measure = dc;
    const HGDIOBJ oldFont = ::SelectObject(measure, ::GetStockObject(DEFAULT_GUI_FONT));
    SIZE extent{};
    ::GetTextExtentPoint32W(measure, text.c_str(), static_cast<int>(text.size()), &extent);

    const int padding = 8;
    const int boxWidth = static_cast<int>(extent.cx) + padding * 2;
    const int boxHeight = static_cast<int>(extent.cy) + padding;

    // Above the selection when there is room, inside it when the selection is against the top edge
    // of the screen - a label that runs off the screen is worse than one that covers a few pixels.
    int left = selection.left;
    int top = selection.top - boxHeight - 6;
    if (top < 0)
    {
        top = selection.top + 6;
    }

    RECT box{ left, top, left + boxWidth, top + boxHeight };
    const HBRUSH brush = ::CreateSolidBrush(kAccent);
    ::FillRect(dc, &box, brush);
    ::DeleteObject(brush);

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, kLabelInk);
    RECT textBox = box;
    textBox.left += padding;
    ::DrawTextW(dc, text.c_str(), -1, &textBox, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(measure, oldFont);
}

LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    SelectionState* state = g_selection;

    switch (message)
    {
        case WM_SETCURSOR:
            ::SetCursor(::LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;

        case WM_LBUTTONDOWN:
            if (state == nullptr)
            {
                return 0;
            }
            {
                state->origin = POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                const RECT previous = state->rect;
                state->rect = RECT{ state->origin.x, state->origin.y, state->origin.x,
                    state->origin.y };
                state->dragging = true;
                ::SetCapture(window);
                const RECT dirty = FrameDamage(previous, state->rect);
                ::InvalidateRect(window, &dirty, FALSE);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (state == nullptr || !state->dragging)
            {
                return 0;
            }
            {
                const POINT here{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                const RECT previous = state->rect;
                state->rect = Normalise(state->origin, here);
                const RECT dirty = FrameDamage(previous, state->rect);
                ::InvalidateRect(window, &dirty, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (state == nullptr || !state->dragging)
            {
                return 0;
            }
            state->dragging = false;
            ::ReleaseCapture();

            if (RegionWidth(state->rect) < kMinimumDrag
                || RegionHeight(state->rect) < kMinimumDrag)
            {
                state->cancelled = true;
            }
            else
            {
                state->cancelled = false;
            }

            state->finished = true;
            return 0;

        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if (state != nullptr)
            {
                state->cancelled = true;
                state->finished = true;
            }
            return 0;

        case WM_KEYDOWN:
            if (state != nullptr && wparam == VK_ESCAPE)
            {
                state->cancelled = true;
                state->finished = true;
            }
            return 0;

        case WM_ERASEBKGND:
            // Everything is repainted in WM_PAINT; letting the default erase run would flash the
            // desktop white on every mouse move.
            return 1;

        case WM_PAINT:
            {
                PAINTSTRUCT paint{};
                const HDC dc = ::BeginPaint(window, &paint);

                // Only the rectangle the system says went stale. On a three-monitor desktop the
                // full frame is 3840x2640 - about 40 MB copied for every mouse move. That was the
                // flicker: the blit simply could not keep up with the pointer, and frames tore.
                // Everything outside the selection never changes, so it is painted once when the
                // window first appears and left alone afterwards.
                const RECT& clip = paint.rcPaint;
                const int clipWidth = clip.right - clip.left;
                const int clipHeight = clip.bottom - clip.top;

                if (state != nullptr && state->backdrop != nullptr && clipWidth > 0
                    && clipHeight > 0)
                {
                    const Backdrop& backdrop = *state->backdrop;
                    ::BitBlt(dc, clip.left, clip.top, clipWidth, clipHeight, backdrop.dimDc,
                        clip.left, clip.top, SRCCOPY);

                    if (state->dragging || state->finished)
                    {
                        const RECT& selection = state->rect;
                        if (RegionWidth(selection) > 0 && RegionHeight(selection) > 0)
                        {
                            // The selection keeps the untouched frame: the user has to see exactly
                            // the pixels they are about to keep, not a tinted version of them.
                            RECT bright = clip;
                            if (::IntersectRect(&bright, &clip, &selection) != FALSE)
                            {
                                ::BitBlt(dc, bright.left, bright.top, bright.right - bright.left,
                                    bright.bottom - bright.top, backdrop.brightDc, bright.left,
                                    bright.top, SRCCOPY);
                            }

                            const HPEN pen = ::CreatePen(PS_SOLID, 2, kAccent);
                            const HGDIOBJ oldPen = ::SelectObject(dc, pen);
                            const HGDIOBJ oldBrush = ::SelectObject(dc,
                                ::GetStockObject(NULL_BRUSH));
                            ::Rectangle(dc, selection.left, selection.top, selection.right,
                                selection.bottom);
                            ::SelectObject(dc, oldBrush);
                            ::SelectObject(dc, oldPen);
                            ::DeleteObject(pen);

                            DrawLabel(dc, selection);
                        }
                    }

                    state->drawn = state->rect;
                }

                ::EndPaint(window, &paint);
            }
            return 0;

        case WM_DESTROY:
            if (state != nullptr)
            {
                state->window = nullptr;
            }
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

bool EncodePng(const void* bits, int width, int height, std::vector<std::uint8_t>& out)
{
    // const_cast, not a copy: WIC reads the buffer and promises not to write to it, but its API
    // predates const and still asks for a non-const pointer.
    return EncodePngBits(const_cast<void*>(bits), width, height, out);
}

bool PickRegion(HINSTANCE instance, CapturedImage& image)
{
    const int originX = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int originY = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    // Snapshot first, overlay second, so the frame being selected from has no overlay in it.
    Backdrop backdrop;
    if (!BuildBackdrop(originX, originY, width, height, backdrop))
    {
        return false;
    }

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &OverlayProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_CROSS);
        windowClass.lpszClassName = kOverlayClass;

        if (::RegisterClassExW(&windowClass) == 0
            && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            util::LogError(L"screen capture: cannot register the overlay class");
            return false;
        }

        registered = true;
    }

    SelectionState state;
    state.backdrop = &backdrop;
    g_selection = &state;

    // No WS_EX_LAYERED, no colour key, no WS_EX_TRANSPARENT: the overlay is an ordinary opaque
    // window showing a frozen frame, which is the only arrangement that both looks right and
    // receives the mouse.
    const HWND overlay = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass, L"",
        WS_POPUP, originX, originY, width, height, nullptr, nullptr, instance, nullptr);

    if (overlay == nullptr)
    {
        g_selection = nullptr;
        DestroyBackdrop(backdrop);
        util::LogError(L"screen capture: cannot create the overlay");
        return false;
    }

    state.window = overlay;

    // Paint the frozen frame synchronously before the compositor presents the window. ShowWindow
    // alone leaves one frame on screen with no pixels in it, which reads as a single bright flash
    // at the start of every capture.
    ::ShowWindow(overlay, SW_SHOWNOACTIVATE);
    ::UpdateWindow(overlay);
    ::SetForegroundWindow(overlay);
    ::SetFocus(overlay);

    // A modal-ish loop of its own: the selection is a single gesture, and letting it interleave
    // with the rest of the app's messages would mean every other handler had to know about it.
    MSG message{};
    while (!state.finished && ::GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    ::ReleaseCapture();

    const RECT selection = state.rect;
    const bool cancelled = state.cancelled;

    ::DestroyWindow(overlay);
    g_selection = nullptr;

    const bool ok = !cancelled && CropToPng(backdrop, selection, image);
    DestroyBackdrop(backdrop);

    return ok;
}

UINT PngClipboardFormat()
{
    static const UINT id = ::RegisterClipboardFormatW(L"PNG");
    return id;
}

/// Puts a capture on the clipboard as a picture.
///
/// Two formats, because the receiving program chooses: CF_DIB for everything that pastes pixels -
/// Word, Paint, most chat clients - and the registered "PNG" format for the ones that would rather
/// have a real image. Both are built from the same pixels here instead of from `image.png`, because
/// a screenshot is opaque and the alpha byte has to say so: an image that claims to be transparent
/// pastes as nothing at all in the programs that honour it.
bool CopyPictureToClipboard(const CapturedImage& image)
{
    if (image.bgra.empty() || image.width <= 0 || image.height <= 0)
    {
        return false;
    }

    std::vector<std::uint8_t> pixels = image.bgra;
    for (std::size_t index = 3; index < pixels.size(); index += 4)
    {
        pixels[index] = 0xFF;
    }

    const std::size_t bytes = static_cast<std::size_t>(image.width) * 4
        * static_cast<std::size_t>(image.height);
    const std::size_t dibSize = sizeof(BITMAPINFOHEADER) + bytes;

    HGLOBAL dib = ::GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (dib == nullptr)
    {
        util::LogError(L"screen capture: cannot allocate the clipboard bitmap");
        return false;
    }

    auto* memory = static_cast<std::uint8_t*>(::GlobalLock(dib));
    if (memory == nullptr)
    {
        ::GlobalFree(dib);
        return false;
    }

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = image.width;
    header.biHeight = -image.height;   // top-down, which is the order the pixels are already in
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(bytes);
    std::memcpy(memory, &header, sizeof(header));
    std::memcpy(memory + sizeof(header), pixels.data(), bytes);
    ::GlobalUnlock(dib);

    std::vector<std::uint8_t> png;
    HGLOBAL picture = nullptr;
    if (EncodePngBits(pixels.data(), image.width, image.height, png) && !png.empty())
    {
        picture = ::GlobalAlloc(GMEM_MOVEABLE, png.size());
        if (picture != nullptr)
        {
            if (void* target = ::GlobalLock(picture))
            {
                std::memcpy(target, png.data(), png.size());
                ::GlobalUnlock(picture);
            }
        }
    }

    bool ok = false;
    if (::OpenClipboard(nullptr) == FALSE)
    {
        util::LogError(L"screen capture: the clipboard is locked by another program");
    }
    else
    {
        ::EmptyClipboard();

        // SetClipboardData takes ownership on success only, which is why the handles are dropped
        // as soon as each call reports one - freeing them afterwards would free the clipboard's
        // own memory.
        if (::SetClipboardData(CF_DIB, dib) != nullptr)
        {
            dib = nullptr;
            ok = true;
        }

        if (picture != nullptr && ::SetClipboardData(PngClipboardFormat(), picture) != nullptr)
        {
            picture = nullptr;
        }

        ::CloseClipboard();
    }

    if (dib != nullptr)
    {
        ::GlobalFree(dib);
    }

    if (picture != nullptr)
    {
        ::GlobalFree(picture);
    }

    if (ok)
    {
        util::LogInfo(L"capture copied to the clipboard, {}x{}", image.width, image.height);
    }

    return ok;
}

CaptureResult CopyRegionToClipboard(HINSTANCE instance)
{
    CapturedImage image;
    if (!PickRegion(instance, image))
    {
        return CaptureResult::Cancelled;
    }

    return CopyPictureToClipboard(image) ? CaptureResult::Copied : CaptureResult::Failed;
}

} // namespace wintools::app
