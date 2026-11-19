#include "core/composer.h"

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <system_error>
#include <vector>

#include "util/log.h"
#include "core/layout_math.h"
#include "platform/win/wic.h"
#include "util/path.h"
#include "util/text.h"

namespace wintools::core
{

namespace
{

using platform::ComPtr;

/// Cap for the source during an interactive turn, in pixels on the longer side. A 720-pixel
/// source turns in a few milliseconds; the full-resolution one is used once, on release.
constexpr int kCoarseMaxSide = 720;

struct DecodedImage
{
    /// The frame, or the frame wrapped in a flip-rotator when a rotation is in force. Stored as
    /// the base interface because a flip rotator is a source, not a frame decode.
    ComPtr<IWICBitmapSource> frame;
    /// The size of `frame`: the picture's own size, or its bounding box once turned.
    int width{};
    int height{};
    /// The picture's own size, before any turn. The framing maths works in these, because a
    /// turned bitmap's bounding box is bigger than the picture inside it.
    int sourceWidth{};
    int sourceHeight{};

    [[nodiscard]] bool IsValid() const
    {
        return frame != nullptr && width > 0 && height > 0;
    }
};

bool OpenImage(const std::filesystem::path& path, DecodedImage& image, double rotation = 0.0,
    int coarseMaxSide = 0);

/// Reads a whole source into 32bpp BGRA. The source must already be a memory-backed bitmap:
/// reading straight from a JPEG frame through a downstream Fant scaler is quadratic, because a
/// turned picture asks for columns and the decoder can only walk rows.
bool ReadPixels(IWICImagingFactory* factory, IWICBitmapSource* surface, int width, int height,
    std::vector<std::uint32_t>& pixels)
{
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        hr = converter->Initialize(surface, GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr))
    {
        return false;
    }

    pixels.assign(static_cast<size_t>(width) * height, 0);
    hr = converter->CopyPixels(nullptr, static_cast<UINT>(width) * 4,
        static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()));
    return SUCCEEDED(hr);
}

/// Turns a decoded picture by an arbitrary clockwise angle, producing a new memory bitmap whose
/// bounding box is RotatedBounds(). Each destination pixel maps back into the source and samples
/// bilinearly; samples outside the source take a matte colour. The framing maths keeps that matte
/// off every display, but it is still what the four corners of the box hold - see ComputeSlack.
bool RotatePixels(IWICImagingFactory* factory, const std::vector<std::uint32_t>& source,
    int width, int height, double degrees, const RotatedBox& box, ComPtr<IWICBitmap>& result)
{
    const double radians = degrees * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double centreX = width / 2.0;
    const double centreY = height / 2.0;
    const double turnedX = box.width / 2.0;
    const double turnedY = box.height / 2.0;

    std::vector<std::uint32_t> turned(static_cast<size_t>(box.width) * box.height, 0);

    // An oblique turn grows the bounding box, so its four corners have no source pixels. Clamping
    // the sampler to the edge smears those corners into long stripes - visible exactly where the
    // picture no longer covers the canvas. Blend the source's four corner colours into one quiet
    // fill instead; it reads as a matte behind the turned picture, not as broken pixels.
    const auto channel = [](std::uint32_t pixel, int shift) -> double
    {
        return static_cast<double>((pixel >> shift) & 0xFFu);
    };
    const std::uint32_t corners[] = {
        source[0],
        source[static_cast<size_t>(width) - 1],
        source[static_cast<size_t>(height - 1) * width],
        source[static_cast<size_t>(height - 1) * width + width - 1],
    };
    std::uint32_t fill = 0;
    for (const int shift : { 0, 8, 16, 24 })
    {
        double sum = 0.0;
        for (const std::uint32_t corner : corners)
        {
            sum += channel(corner, shift);
        }
        fill |= static_cast<std::uint32_t>(std::lround(sum / 4.0)) << shift;
    }

    const auto fetch = [&source, width, height, fill](int x, int y) -> std::uint32_t
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
        {
            return fill;
        }
        return source[static_cast<size_t>(y) * width + x];
    };

    for (int dy = 0; dy < box.height; ++dy)
    {
        const double v = dy + 0.5 - turnedY;
        for (int dx = 0; dx < box.width; ++dx)
        {
            const double u = dx + 0.5 - turnedX;

            // Inverse of the clockwise turn: R(-theta).
            const double sx = u * cosine + v * sine + centreX - 0.5;
            const double sy = -u * sine + v * cosine + centreY - 0.5;

            const int x0 = static_cast<int>(std::floor(sx));
            const int y0 = static_cast<int>(std::floor(sy));
            const double fx = sx - x0;
            const double fy = sy - y0;

            const std::uint32_t p00 = fetch(x0, y0);
            const std::uint32_t p10 = fetch(x0 + 1, y0);
            const std::uint32_t p01 = fetch(x0, y0 + 1);
            const std::uint32_t p11 = fetch(x0 + 1, y0 + 1);

            std::uint32_t pixel = 0;
            for (const int shift : { 0, 8, 16, 24 })
            {
                const double top = channel(p00, shift) * (1.0 - fx) + channel(p10, shift) * fx;
                const double bottom = channel(p01, shift) * (1.0 - fx) + channel(p11, shift) * fx;
                const double value = top * (1.0 - fy) + bottom * fy;
                pixel |= static_cast<std::uint32_t>(std::clamp(value, 0.0, 255.0) + 0.5) << shift;
            }
            turned[static_cast<size_t>(dy) * box.width + dx] = pixel;
        }
    }

    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = factory->CreateBitmap(static_cast<UINT>(box.width), static_cast<UINT>(box.height),
        GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnDemand, bitmap.GetAddressOf());
    if (FAILED(hr))
    {
        return false;
    }

    {
        WICRect whole{ 0, 0, box.width, box.height };
        ComPtr<IWICBitmapLock> locked;
        hr = bitmap->Lock(&whole, WICBitmapLockWrite, locked.GetAddressOf());
        UINT stride = 0;
        BYTE* data = nullptr;
        UINT size = 0;
        if (SUCCEEDED(hr))
        {
            hr = locked->GetStride(&stride);
        }
        if (SUCCEEDED(hr))
        {
            hr = locked->GetDataPointer(&size, &data);
        }
        if (SUCCEEDED(hr) && data != nullptr)
        {
            for (int y = 0; y < box.height; ++y)
            {
                std::memcpy(data + static_cast<size_t>(y) * stride,
                    &turned[static_cast<size_t>(y) * box.width], static_cast<size_t>(box.width) * 4);
            }
        }
        if (FAILED(hr))
        {
            return false;
        }
    }

    result = std::move(bitmap);
    return true;
}

bool OpenImage(const std::filesystem::path& path, DecodedImage& image, double rotation,
    int coarseMaxSide)
{
    const auto factory = platform::GetWicFactory();
    if (factory == nullptr)
    {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(hr))
    {
        util::LogError(L"cannot open image '{}': {}", path.filename().wstring(), util::FormatHresult(hr));
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr))
    {
        util::LogError(L"cannot read the first frame of '{}': {}", path.filename().wstring(), util::FormatHresult(hr));
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0)
    {
        util::LogError(L"cannot read the size of '{}': {}", path.filename().wstring(), util::FormatHresult(hr));
        return false;
    }

    int sourceWidth = static_cast<int>(width);
    int sourceHeight = static_cast<int>(height);
    ComPtr<IWICBitmapSource> surface = frame;

    // A drag re-renders the preview on every mouse move. Downscale the source once for those
    // frames; every later WIC stage (and the turn itself) then walks a small in-memory bitmap.
    if (coarseMaxSide > 0 && std::max(sourceWidth, sourceHeight) > coarseMaxSide)
    {
        const double factor = static_cast<double>(coarseMaxSide)
            / static_cast<double>(std::max(sourceWidth, sourceHeight));
        const UINT smallWidth = std::max<UINT>(1, static_cast<UINT>(std::lround(width * factor)));
        const UINT smallHeight = std::max<UINT>(1, static_cast<UINT>(std::lround(height * factor)));

        ComPtr<IWICBitmapScaler> downscale;
        HRESULT scaled = factory->CreateBitmapScaler(downscale.GetAddressOf());
        if (SUCCEEDED(scaled))
        {
            scaled = downscale->Initialize(
                frame.Get(), smallWidth, smallHeight, WICBitmapInterpolationModeLinear);
        }
        if (SUCCEEDED(scaled))
        {
            surface = downscale;
            sourceWidth = static_cast<int>(smallWidth);
            sourceHeight = static_cast<int>(smallHeight);
        }
    }

    // Materialize before anything downstream reads it. WIC's flip rotator (and any other lazy
    // source wrapping a JPEG decoder) is quadratic here: a turned picture asks for columns while
    // a decoder can only walk rows. A memory bitmap makes every later stage random-access.
    ComPtr<IWICBitmap> materialized;
    hr = factory->CreateBitmapFromSource(
        surface.Get(), WICBitmapCacheOnDemand, materialized.GetAddressOf());
    if (FAILED(hr))
    {
        util::LogError(L"cannot materialize '{}': {}", path.filename().wstring(),
            util::FormatHresult(hr));
        return false;
    }

    image.width = sourceWidth;
    image.height = sourceHeight;
    image.sourceWidth = sourceWidth;
    image.sourceHeight = sourceHeight;
    image.frame = materialized;

    const double angle = ClampView(WallpaperView{ 1.0, 0.5, 0.5, rotation }).rotation;
    if (angle < 0.001 || angle > 359.999)
    {
        return true;
    }

    const RotatedBox box = RotatedBounds(sourceWidth, sourceHeight, angle);
    if (box.width <= 0 || box.height <= 0)
    {
        return false;
    }

    std::vector<std::uint32_t> pixels;
    if (!ReadPixels(factory, materialized.Get(), sourceWidth, sourceHeight, pixels))
    {
        util::LogError(L"cannot read the pixels of '{}'", path.filename().wstring());
        return false;
    }

    ComPtr<IWICBitmap> turned;
    if (!RotatePixels(factory, pixels, sourceWidth, sourceHeight, angle, box, turned))
    {
        util::LogError(L"cannot turn '{}' by {:.2f} degrees", path.filename().wstring(), angle);
        return false;
    }

    image.width = box.width;
    image.height = box.height;
    image.frame = turned;
    return true;
}

/// Clamps a crop rectangle so WIC never sees coordinates outside the source surface.
///
/// ComputeSlice already guarantees this for well-formed layouts, but an integer overflow from a
/// pathological monitor arrangement would otherwise surface as a bare E_INVALIDARG from deep
/// inside WIC, which is much harder to diagnose than a visible clamp here.
WICRect SafeCrop(int x, int y, int width, int height, int surfaceWidth, int surfaceHeight)
{
    const int left = std::clamp(x, 0, std::max(0, surfaceWidth - 1));
    const int top = std::clamp(y, 0, std::max(0, surfaceHeight - 1));
    const int right = std::clamp(left + width, left + 1, surfaceWidth);
    const int bottom = std::clamp(top + height, top + 1, surfaceHeight);

    return WICRect{ left, top, right - left, bottom - top };
}

bool EncodePng(IWICBitmapSource* source, const std::filesystem::path& outputPath, int width, int height)
{
    const auto factory = platform::GetWicFactory();
    if (factory == nullptr)
    {
        return false;
    }

    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        hr = stream->InitializeFromFilename(outputPath.c_str(), GENERIC_WRITE);
    }

    if (FAILED(hr))
    {
        util::LogError(L"cannot create '{}': {}", outputPath.filename().wstring(), util::FormatHresult(hr));
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (SUCCEEDED(hr))
    {
        hr = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
    }

    if (SUCCEEDED(hr))
    {
        hr = frame->Initialize(properties.Get());
    }

    if (SUCCEEDED(hr))
    {
        hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    }

    if (SUCCEEDED(hr))
    {
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && !::IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA))
        {
            util::LogError(L"the PNG encoder refused 32bppBGRA for '{}'", outputPath.filename().wstring());
            hr = E_FAIL;
        }
    }

    if (SUCCEEDED(hr))
    {
        hr = frame->WriteSource(source, nullptr);
    }

    if (SUCCEEDED(hr))
    {
        hr = frame->Commit();
    }

    if (SUCCEEDED(hr))
    {
        hr = encoder->Commit();
    }

    if (FAILED(hr))
    {
        util::LogError(L"cannot write '{}': {}", outputPath.filename().wstring(), util::FormatHresult(hr));
        return false;
    }

    return true;
}

bool RenderPiece(const DecodedImage& image, const Slice& slice, const std::filesystem::path& outputPath)
{
    const auto factory = platform::GetWicFactory();
    if (factory == nullptr)
    {
        return false;
    }

    ComPtr<IWICBitmapSource> surface = image.frame;

    // Fast path: nothing is scaled, so clip straight out of the decoded frame using source
    // coordinates. Going through a scaler here would resample and could shift flat colours.
    if (!slice.unscaled)
    {
        ComPtr<IWICBitmapScaler> scaler;
        HRESULT hr = factory->CreateBitmapScaler(scaler.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            hr = scaler->Initialize(
                image.frame.Get(),
                static_cast<UINT>(slice.scaledWidth),
                static_cast<UINT>(slice.scaledHeight),
                WICBitmapInterpolationModeFant);
        }

        if (FAILED(hr))
        {
            util::LogError(L"scaling to {}x{} failed: {}", slice.scaledWidth, slice.scaledHeight, util::FormatHresult(hr));
            return false;
        }

        surface = scaler;
    }

    const int surfaceWidth = slice.unscaled ? image.width : slice.scaledWidth;
    const int surfaceHeight = slice.unscaled ? image.height : slice.scaledHeight;

    const int cropX = slice.unscaled ? slice.sourceX : slice.canvasX;
    const int cropY = slice.unscaled ? slice.sourceY : slice.canvasY;

    const WICRect crop = SafeCrop(cropX, cropY, slice.outputWidth, slice.outputHeight, surfaceWidth, surfaceHeight);
    if (crop.Width != slice.outputWidth || crop.Height != slice.outputHeight)
    {
        // Not fatal, but it means the geometry and the image disagree; the piece will be
        // stretched by the encoder instead of being a clean crop.
        util::LogWarn(
            L"crop {}x{} at ({},{}) was clamped to {}x{} inside {}x{}",
            slice.outputWidth,
            slice.outputHeight,
            cropX,
            cropY,
            crop.Width,
            crop.Height,
            surfaceWidth,
            surfaceHeight);
    }

    ComPtr<IWICBitmapClipper> clipper;
    HRESULT hr = factory->CreateBitmapClipper(clipper.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        hr = clipper->Initialize(surface.Get(), &crop);
    }

    if (FAILED(hr))
    {
        util::LogError(L"clipping failed: {}", util::FormatHresult(hr));
        return false;
    }

    // Normalise to 32bppBGRA so the encoder path is identical regardless of what the source file
    // turned out to be (indexed PNG, 16-bit TIFF, CMYK JPEG...).
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        hr = converter->Initialize(
            clipper.Get(),
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
    }

    if (FAILED(hr))
    {
        util::LogError(L"pixel format conversion failed: {}", util::FormatHresult(hr));
        return false;
    }

    return EncodePng(converter.Get(), outputPath, slice.outputWidth, slice.outputHeight);
}

} // namespace

std::optional<ImageSize> QueryImageSize(const std::filesystem::path& sourceImage)
{
    DecodedImage image;
    if (!OpenImage(sourceImage, image))
    {
        return std::nullopt;
    }

    return ImageSize{ image.width, image.height };
}

bool RenderSpan(
    const std::filesystem::path& sourceImage,
    std::span<const Monitor> monitors,
    const std::filesystem::path& outputFolder,
    std::vector<RenderedPiece>& pieces,
    const WallpaperView& view,
    const MonitorImageLookup& monitorImage)
{
    if (monitors.empty())
    {
        util::LogError(L"no monitors to render for");
        return false;
    }

    // No spanning image is a usable state when every display brought its own picture; only ask
    // the decoder for one if there is a file to decode.
    const bool hasSpan = !sourceImage.empty();

    DecodedImage image;
    if (hasSpan && !OpenImage(sourceImage, image, view.rotation))
    {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputFolder, ec);
    if (ec)
    {
        util::LogError(L"cannot create '{}': {}", outputFolder.wstring(), util::FromUtf8(ec.message()));
        return false;
    }

    const Rect canvas = GetCanvas(monitors);

    // Per-display pictures are decoded here, before the loop: one decode per distinct file, and
    // reserving up front means the pointers handed out below stay valid while more are added.
    struct OwnPicture
    {
        std::filesystem::path path;
        DecodedImage image;
    };

    std::vector<OwnPicture> own;
    own.reserve(monitors.size());
    std::vector<const DecodedImage*> assigned(monitors.size(), nullptr);

    if (monitorImage)
    {
        for (size_t index = 0; index < monitors.size(); ++index)
        {
            const std::filesystem::path path = monitorImage(monitors[index].devicePath);
            if (path.empty())
            {
                continue;
            }

            const auto same = [&path](const OwnPicture& entry) { return entry.path == path; };
            if (auto found = std::find_if(own.begin(), own.end(), same); found != own.end())
            {
                assigned[index] = &found->image;
                continue;
            }

            // A picture that will not decode must not take the whole wallpaper down: the display
            // falls back to the spanning slice and the reason goes to the log.
            OwnPicture entry;
            if (!OpenImage(path, entry.image))
            {
                util::LogError(L"display picture '{}' unusable, falling back to the span",
                    path.wstring());
                continue;
            }

            entry.path = path;
            own.push_back(std::move(entry));
            assigned[index] = &own.back().image;
        }
    }

    pieces.clear();
    pieces.reserve(monitors.size());

    bool allOk = true;
    for (size_t index = 0; index < monitors.size(); ++index)
    {
        const auto& monitor = monitors[index];
        const DecodedImage* picture = assigned[index];

        if (picture == nullptr && !hasSpan)
        {
            util::LogError(L"display ({},{}) has no picture and there is no span to fall back on",
                monitor.left,
                monitor.top);
            allOk = false;
            continue;
        }

        // One display on its own picture is its own canvas - cover, centred - while the rest keep
        // sharing the spanning slice. Both go through the same ComputeSlice, so a mixed layout is
        // still one arithmetic path.
        const Rect frame = (picture != nullptr) ? GetCanvas(std::span<const Monitor>{ &monitor, 1 })
                                                : canvas;
        const WallpaperView framing = (picture != nullptr) ? WallpaperView{} : view;
        const DecodedImage& source = (picture != nullptr) ? *picture : image;

        const auto slice = ComputeSlice(frame, source.sourceWidth, source.sourceHeight, monitor,
            framing);
        if (!slice.has_value())
        {
            util::LogError(L"cannot compute a slice for display ({},{})", monitor.left, monitor.top);
            allOk = false;
            continue;
        }

        const std::filesystem::path outputPath = outputFolder / util::BuildPieceName(monitor, *slice);
        if (!RenderPiece(source, *slice, outputPath))
        {
            allOk = false;
            continue;
        }

        util::LogInfo(
            L"piece {}: src ({},{}) {}x{} -> {}x{}{}{}",
            outputPath.filename().wstring(),
            slice->sourceX,
            slice->sourceY,
            slice->sourceWidth,
            slice->sourceHeight,
            slice->outputWidth,
            slice->outputHeight,
            slice->unscaled ? L" (1:1)" : L"",
            (picture != nullptr) ? L" (own picture)" : L"");

        pieces.push_back(RenderedPiece{ outputPath, *slice, index });
    }

    return allOk;
}

bool RenderForMonitor(
    const std::filesystem::path& sourceImage,
    const Monitor& monitor,
    const std::filesystem::path& outputFolder,
    RenderedPiece& piece)
{
    DecodedImage image;
    if (!OpenImage(sourceImage, image))
    {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputFolder, ec);
    if (ec)
    {
        util::LogError(L"cannot create '{}': {}", outputFolder.wstring(), util::FromUtf8(ec.message()));
        return false;
    }

    // Each monitor is its own canvas: the slice covers the whole image, and the output is the
    // monitor size. Cover, not fit, so no letterboxing appears.
    const Rect canvas = GetCanvas(std::span<const Monitor>{ &monitor, 1 });
    const auto slice = ComputeSlice(canvas, image.width, image.height, monitor);
    if (!slice.has_value())
    {
        util::LogError(L"cannot compute a slice for display ({},{})", monitor.left, monitor.top);
        return false;
    }

    const std::filesystem::path outputPath = outputFolder / util::BuildPieceName(monitor, *slice);
    if (!RenderPiece(image, *slice, outputPath))
    {
        return false;
    }

    piece = RenderedPiece{ outputPath, *slice, 0 };
    return true;
}

namespace
{

/// True when `name` has exactly the shape BuildPieceName produces: `m` + 8 hex + `_` + three
/// signed integers separated by `_`/`x` + `.png`.
///
/// Parsed by hand rather than with a regex: this runs over a handful of files, and hand-written
/// parsing makes the accepted set explicit- and stops a `.*` from ever matching something else in
/// the folder.
bool IsPieceName(std::wstring_view name)
{
    const auto digits = [&name](size_t& at) {
        size_t seen = 0;
        if (at < name.size() && (name[at] == L'-' || name[at] == L'+'))
        {
            ++at;
        }

        while (at < name.size() && name[at] >= L'0' && name[at] <= L'9')
        {
            ++at;
            ++seen;
        }

        return seen > 0;
    };

    const auto literal = [&name](size_t& at, wchar_t wanted) {
        if (at >= name.size() || name[at] != wanted)
        {
            return false;
        }

        ++at;
        return true;
    };

    if (name.size() < 24 || name[0] != L'm')
    {
        return false;
    }

    size_t at = 1;
    for (int index = 0; index < 8; ++index)
    {
        const wchar_t ch = name[at++];
        const bool hex = (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
        if (!hex)
        {
            return false;
        }
    }

    if (!literal(at, L'_') || !digits(at) || !literal(at, L'_') || !digits(at)
        || !literal(at, L'_') || !digits(at) || !literal(at, L'x') || !digits(at))
    {
        return false;
    }

    return name.substr(at) == L".png";
}

} // namespace

int RemoveStalePieces(
    const std::filesystem::path& outputFolder, std::span<const RenderedPiece> keep)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(outputFolder, ec))
    {
        return 0;
    }

    // Compare by file name only: the caller's paths may be relative or use a different separator,
    // and everything in this folder is a flat set of names.
    std::vector<std::wstring> wanted;
    wanted.reserve(keep.size());
    for (const auto& piece : keep)
    {
        wanted.push_back(piece.path.filename().wstring());
    }

    int removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(outputFolder, ec))
    {
        if (ec)
        {
            break;
        }

        if (!entry.is_regular_file(ec) || ec)
        {
            continue;
        }

        const std::wstring name = entry.path().filename().wstring();
        if (!IsPieceName(name))
        {
            continue;
        }

        if (std::find(wanted.begin(), wanted.end(), name) != wanted.end())
        {
            continue;
        }

        // The desktop may still be showing the previous piece for a moment after a partial apply;
        // the caller only asks for this once everything was written and applied.
        std::error_code removeError;
        if (std::filesystem::remove(entry.path(), removeError))
        {
            ++removed;
        }
    }

    return removed;
}

std::optional<PreviewBitmap> RenderSurface(
    const std::filesystem::path& sourceImage, int width, int height, double rotation, bool coarse)
{
    if (width <= 0 || height <= 0)
    {
        return std::nullopt;
    }

    DecodedImage image;
    if (!OpenImage(sourceImage, image, rotation, coarse ? kCoarseMaxSide : 0))
    {
        return std::nullopt;
    }

    const auto factory = platform::GetWicFactory();
    if (factory == nullptr)
    {
        return std::nullopt;
    }

    // Scale the surface the caller asked for, not a canvas it stands in for. The old code fed
    // this size back in as "the canvas" and asked ComputeSlice to cover it, which re-derives a
    // cover from a box that is already cover-scaled: at 45 degrees that is a second 1.41x, and the
    // map showed a crop the desktop never had. Here the size is taken literally.
    ComPtr<IWICBitmapSource> surface = image.frame;
    if (image.width != width || image.height != height)
    {
        ComPtr<IWICBitmapScaler> scaler;
        HRESULT hr = factory->CreateBitmapScaler(scaler.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            hr = scaler->Initialize(image.frame.Get(), static_cast<UINT>(width),
                static_cast<UINT>(height), WICBitmapInterpolationModeFant);
        }

        if (FAILED(hr))
        {
            util::LogError(L"preview scaling failed: {}", util::FormatHresult(hr));
            return std::nullopt;
        }

        surface = scaler;
    }

    PreviewBitmap preview;
    preview.width = width;
    preview.height = height;

    if (!ReadPixels(factory, surface.Get(), width, height, preview.pixels) || !preview.IsValid())
    {
        util::LogError(L"preview copy failed for {}x{}", width, height);
        return std::nullopt;
    }

    return preview;
}

} // namespace wintools::core
