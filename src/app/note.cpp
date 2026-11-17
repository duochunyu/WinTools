#include "app/note.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>

#include <windowsx.h>
#include <richedit.h>
#include <gdiplus.h>

#include "app/capture.h"
#include "core/note_store.h"
#include "core/note_theme.h"
#include "core/rtf.h"
#include "util/i18n.h"
#include "util/log.h"

namespace wintools::app
{

namespace
{

constexpr wchar_t kClassName[] = L"WinTools.Note.7d2a91c4";
constexpr wchar_t kScrollClass[] = L"WinTools.NoteScroll.9e40b7d1";
constexpr wchar_t kFooterClass[] = L"WinTools.NoteFooter.8f3c1e27";
constexpr wchar_t kRichEditClass[] = L"RICHEDIT50W";

/// Where the body's message hook hangs its state. See Note::BodyHook.
constexpr wchar_t kBodyHookProp[] = L"WinTools.NoteBodyHook";

/// How many bytes of a serialised picture are enough to see the RTF it starts with.
constexpr std::size_t kPictureSniff = 4096;

constexpr UINT_PTR kSaveTimer = 1;

/// Long enough that typing a sentence does not write the file twenty times, short enough that a
/// crash costs a word rather than a paragraph.
constexpr UINT kSaveDelayMs = 700;

/// Layout at 96 dpi. Everything else is derived from these.
constexpr int kHeaderDesign = 30;
constexpr int kButtonDesign = 22;
constexpr int kDotDesign = 22;
constexpr int kGripDesign = 6;

/// Between the three boxes. The plate already groups them, so they only need enough air to read as
/// three separate targets - the boxes themselves are invisible until one is hovered.
constexpr int kButtonGapDesign = 4;

/// Where the rightmost box ends. Same number as the left margin, so the two ends of the line agree.
constexpr int kSideInsetDesign = 8;

/// The body starts this far under the time line. That line is all that is left of the chrome most of
/// the time, so this also has to work as the top margin of a sheet of paper: enough that text does not
/// look pinned to the edge.
constexpr int kBodyTopDesign = 4;

/// Paper left below the last line, so text stops short of the bottom edge the way it does at the
/// top. Half the header, per the same rhythm.
constexpr int kBottomPadDesign = 15;

/// Side margins of the text. Wider than the 12 the body shipped with, because a note reads as
/// paper only when the text is not trying to touch its edges.
constexpr int kMarginDesign = 14;

constexpr int kScrollWidthDesign = 4;
constexpr int kScrollThumbDesign = 18;

/// The ink is the same on all six papers: switching colour must not have to touch the colour of
/// the text the user already pasted in.
constexpr COLORREF kInk = RGB(0x37, 0x35, 0x2F);

/// For the one state worth interrupting a quiet note for: text that is not on disk yet.
constexpr COLORREF kAccent = RGB(0xB4, 0x64, 0x2F);

/// The style sample renders the 24-unit Lucide canvas at 19 px with a 1.8 stroke, so the same 19
/// design units reproduces both the glyph size and its weight. Drawing the full 24 filled the whole
/// 22-unit button and read as a fat, stepped outline - that is what looked crude.
constexpr float kIconDesign = 19.0F;

/// 1.8 units of stroke on the 24-unit grid, scaled down with the canvas (1.425 design units). GDI+
/// takes a fractional pen, so this stays at the sample's weight instead of rounding up to 2 px.
constexpr float kIconStrokeDesign = kIconDesign * 1.8F / 24.0F;

/// The rule and the timestamp are one composition: a line box kStampLine tall, centred in the header
/// (the icons use the same centre). The browser sample positions both absolutely at top:9px with a
/// 20px line-height, which is that same box on a 30-unit header.
constexpr float kStampLine = 20.0F;

/// Timestamp size in the same terms as the rest of the layout (the body is 15). One step under the
/// body, and deliberately the smaller step: at 13 the digits sit so close to the body's cap height
/// that the line reads as a second body row rather than as metadata.
constexpr float kStampDesign = 12.0F;

/// GDI sizes a face by cell height, GDI+ by em. For Microsoft YaHei those differ by about 15%, so a
/// layout written in GDI units (the body's CreateFontW(-15), this stamp's 12) has to be converted
/// before it is handed to GDI+ - otherwise the same number draws 15% narrower than GDI did.
constexpr float kCellToEm = 15.0F / 13.0F;

enum Button
{
    kButtonNone = -1,
    kButtonDot = 0,
    kButtonShot,
    kButtonClose,
};

/// EM_SETSCROLLPOS: RichEdit scrolls in pixels, which is what lets a hand-drawn thumb land where
/// the pointer says instead of at the nearest line boundary.
constexpr UINT kEditSetScrollPos = WM_USER + 222;

COLORREF ToRef(std::uint32_t rgb)
{
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

/// GDI+ draws the chrome: its anti-aliasing is what separates a Lucide glyph from a staircase of
/// 1 px line segments. Startup is process-wide and deliberately never shut down - the token has
/// to outlive every window, and the process exit cleans up the rest.
bool GdiplusReady()
{
    static ULONG_PTR token = 0;
    static const bool ready = [] {
        Gdiplus::GdiplusStartupInput input;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return ready;
}

Gdiplus::Color ToGdiplus(COLORREF colour)
{
    return Gdiplus::Color(255, GetRValue(colour), GetGValue(colour), GetBValue(colour));
}

/// A rounded rectangle as a path, because GDI+ has no rounded-rect primitive and a GDI RoundRect
/// dropped into an anti-aliased drawing would be the one hard-edged shape left on the paper.
void RoundedRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& box, float radius)
{
    const float diameter = radius * 2.0F;
    path.AddArc(box.X, box.Y, diameter, diameter, 180.0F, 90.0F);
    path.AddArc(box.X + box.Width - diameter, box.Y, diameter, diameter, 270.0F, 90.0F);
    path.AddArc(box.X + box.Width - diameter, box.Y + box.Height - diameter, diameter, diameter,
        0.0F, 90.0F);
    path.AddArc(box.X, box.Y + box.Height - diameter, diameter, diameter, 90.0F, 90.0F);
    path.CloseFigure();
}

/// One 24-unit Lucide canvas mapped into a button. All three glyphs go through this, so a change
/// to the icon size moves them together instead of re-tuning three sets of magic numbers.
struct IconGrid
{
    float left;
    float top;
    float size;

    [[nodiscard]] float X(float unit) const { return left + size * unit / 24.0F; }
    [[nodiscard]] float Y(float unit) const { return top + size * unit / 24.0F; }
};

/// Where a stream-out writes to. RichEdit calls the callback repeatedly until the document ends.
struct SaveSink
{
    std::string bytes;
};

DWORD CALLBACK SaveCallback(DWORD_PTR cookie, LPBYTE buffer, LONG size, LONG* written)
{
    auto* sink = reinterpret_cast<SaveSink*>(cookie);
    if (sink == nullptr || buffer == nullptr || size <= 0)
    {
        if (written != nullptr)
        {
            *written = 0;
        }
        return 0;
    }

    sink->bytes.append(reinterpret_cast<const char*>(buffer), static_cast<std::size_t>(size));
    *written = size;
    return 0;
}

/// Where a stream-in reads from. Handed over as raw bytes: see note_store.h for why the RTF is
/// never converted on the way through.
struct LoadSource
{
    const char* data{};
    std::size_t size{};
    std::size_t offset{};
};

DWORD CALLBACK LoadCallback(DWORD_PTR cookie, LPBYTE buffer, LONG size, LONG* written)
{
    auto* source = reinterpret_cast<LoadSource*>(cookie);
    if (source == nullptr || buffer == nullptr || size <= 0)
    {
        if (written != nullptr)
        {
            *written = 0;
        }
        return 0;
    }

    const std::size_t remaining = source->offset < source->size ? source->size - source->offset : 0;
    const auto take = std::min(remaining, static_cast<std::size_t>(size));
    if (take > 0)
    {
        std::memcpy(buffer, source->data + source->offset, take);
    }

    source->offset += take;
    *written = static_cast<LONG>(take);
    return 0;
}

/// Serialising one character, only far enough to see whether it is a picture.
///
/// RichEdit has no other way to answer the question: a pasted screenshot is an ordinary-looking
/// space in the text stream, with no OLE object behind it (the control reports zero objects) and no
/// object mark to look for. Stopping the stream as soon as the word appears keeps this to a few
/// hundred bytes instead of the whole screenshot.
struct PictureProbe
{
    std::string bytes;
    bool found{};
};

DWORD CALLBACK ProbeCallback(DWORD_PTR cookie, LPBYTE buffer, LONG size, LONG* written)
{
    auto* probe = reinterpret_cast<PictureProbe*>(cookie);
    if (written != nullptr)
    {
        *written = 0;
    }
    if (probe == nullptr || buffer == nullptr || size <= 0)
    {
        return 1;
    }

    probe->bytes.append(reinterpret_cast<const char*>(buffer), static_cast<std::size_t>(size));

    if (probe->bytes.find("\\pict") != std::string::npos)
    {
        probe->found = true;
        return 1;
    }
    if (probe->bytes.size() > kPictureSniff)
    {
        return 1;
    }

    if (written != nullptr)
    {
        *written = size;
    }
    return 0;
}

/// When the body file was last written, in local time.
///
/// A note that comes back from disk reports when its text was actually saved, not the moment the
/// app reopened it: the status line is the only evidence the user has that the text is on disk, and
/// a timestamp that always reads "just now" is worse than no timestamp at all.
SYSTEMTIME WrittenAt(const std::filesystem::path& file)
{
    SYSTEMTIME local{};
    ::GetLocalTime(&local);

    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(file, ec);
    if (ec)
    {
        return local;
    }

    const auto seconds = std::chrono::system_clock::to_time_t(
        std::chrono::clock_cast<std::chrono::system_clock>(stamp));

    std::tm parts{};
    if (localtime_s(&parts, &seconds) != 0)
    {
        return local;
    }

    local.wYear = static_cast<WORD>(parts.tm_year + 1900);
    local.wMonth = static_cast<WORD>(parts.tm_mon + 1);
    local.wDay = static_cast<WORD>(parts.tm_mday);
    local.wHour = static_cast<WORD>(parts.tm_hour);
    local.wMinute = static_cast<WORD>(parts.tm_min);
    local.wSecond = static_cast<WORD>(parts.tm_sec);
    local.wMilliseconds = 0;
    return local;
}

/// Wraps a PNG in the smallest RTF document that draws it at the requested size.
///
/// The size has to be given twice: picw/pich are the bitmap's real pixel size (RichEdit needs it
/// to keep the aspect ratio) and picwgoal/pichgoal are how big it should be drawn, in twips.
std::string BuildPictureRtf(
    const std::vector<std::uint8_t>& png, int pixelWidth, int pixelHeight, int goalTwipsX,
    int goalTwipsY)
{
    std::string rtf = "{\\rtf1\\ansi\\deff0\\line{\\pict\\pngblip\\picw" + std::to_string(pixelWidth)
        + "\\pich" + std::to_string(pixelHeight) + "\\picwgoal" + std::to_string(goalTwipsX)
        + "\\pichgoal" + std::to_string(goalTwipsY) + "\n";

    static constexpr char kDigits[] = "0123456789abcdef";
    rtf.reserve(rtf.size() + png.size() * 2 + 8);

    for (const std::uint8_t byte : png)
    {
        rtf.push_back(kDigits[byte >> 4]);
        rtf.push_back(kDigits[byte & 0x0F]);
    }

    rtf += "}}";
    return rtf;
}

bool ReadWholeFile(const std::filesystem::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }

    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out = std::move(bytes);
    return true;
}

bool WriteWholeFile(const std::filesystem::path& path, const std::string& bytes)
{
    std::error_code ec;
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return false;
    }

    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool StreamingEdit(HWND control, UINT message, DWORD flags, void* cookie,
    EDITSTREAMCALLBACK callback)
{
    EDITSTREAM stream{};
    stream.dwCookie = reinterpret_cast<DWORD_PTR>(cookie);
    stream.dwError = 0;
    stream.pfnCallback = callback;

    const LRESULT result = ::SendMessageW(control, message, flags,
        reinterpret_cast<LPARAM>(&stream));

    return stream.dwError == 0 && result != 0;
}

} // namespace

// Defined near the other window procedures below, but CreateScrollbar needs it earlier.
LRESULT CALLBACK FooterProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

Note::~Note()
{
    if (m_window != nullptr)
    {
        Destroy();
    }
}

bool Note::Create(HINSTANCE instance, std::wstring id, const core::Rect& rect, int theme)
{
    m_instance = instance;
    m_id = std::move(id);
    m_rect = rect;
    m_theme = core::ClampNoteTheme(theme);

    // Windows 7 and later ship the RichEdit that understands pictures; without it the point of the
    // note is gone, so this is a hard requirement rather than a degraded mode.
    if (::LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) == nullptr)
    {
        util::LogError(L"sticky note: Msftedit.dll is missing, cannot create a note");
        return false;
    }

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Note::WndProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;
        windowClass.style = CS_DBLCLKS;

        if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            util::LogError(L"sticky note: cannot register the window class");
            return false;
        }

        registered = true;
    }

    // TOPMOST is the feature; TOOLWINDOW keeps a note out of Alt+Tab and off the taskbar, because
    // it is always on screen anyway and the window list only gets longer for it.
    // THICKFRAME buys real resizing and DWM shadow; WM_NCCALCSIZE removes the visible frame.
    m_window = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, L"", 
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, rect.left, rect.top, rect.width, rect.height,
        nullptr, nullptr, instance, this);

    if (m_window == nullptr)
    {
        util::LogError(L"sticky note: cannot create the window");
        return false;
    }

    m_dpi = ::GetDpiForWindow(m_window);
    if (m_dpi <= 0)
    {
        m_dpi = 96;
    }

    const auto makeFont = [this](int pixels, int weight, DWORD quality)
    {
        return ::CreateFontW(-Scale(pixels), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, quality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    };

    m_fontBody = makeFont(15, FW_NORMAL, ANTIALIASED_QUALITY);

    ApplyTheme(m_theme);

    Metrics();
    CreateBody();
    CreateScrollbar();

    // A note that has never been written to disk still shows a time, and the honest one is when it
    // was made: the line says "this note last changed then", and a brand-new note changed now.
    ::GetLocalTime(&m_savedAt);

    ::ShowWindow(m_window, SW_SHOWNOACTIVATE);
    LayoutChildren();

    util::LogInfo(L"sticky note {} opened at ({}, {}) {}x{}", m_id, rect.left, rect.top,
        rect.width, rect.height);
    return true;
}

void Note::Destroy()
{
    if (m_window == nullptr)
    {
        return;
    }

    Flush();

    m_palette.Hide();

    ::KillTimer(m_window, kSaveTimer);
    ::DestroyWindow(m_window);
    m_window = nullptr;
    m_body = nullptr;
    m_scroll = nullptr;
    m_footer = nullptr;

    // Create() builds these per note, so closing a note has to give them back: a note opened and
    // closed a few dozen times would otherwise hold GDI objects for the life of the process.
    for (HGDIOBJ handle : { static_cast<HGDIOBJ>(m_fontBody), static_cast<HGDIOBJ>(m_paper) })
    {
        ::DeleteObject(handle);
    }

    m_fontBody = nullptr;
    m_paper = nullptr;

    util::LogInfo(L"sticky note {} closed", m_id);
}

void Note::ApplyTheme(int theme)
{
    m_theme = core::ClampNoteTheme(theme);

    const core::NoteTheme palette = core::NoteThemeAt(m_theme);
    m_paperColor = ToRef(palette.paper);
    m_headerColor = ToRef(palette.header);
    m_ruleColor = ToRef(palette.rule);
    m_mutedColor = ToRef(palette.muted);

    // GDI has no alpha, so the tint behind a hovered icon is mixed here against the chip it sits on
    // rather than drawn with an alpha brush.
    const COLORREF hover = ToRef(core::BlendColor(palette.header, 0x37352F, 12));
    m_hoverColor = hover;

    // Icons a shade darker than the save time: that line is metadata and may whisper, but a
    // control the user is meant to hit has to be visible without hunting for it.
    m_iconColor = ToRef(core::BlendColor(palette.muted, 0x37352F, 40));

    const auto swap = [](HBRUSH& brush, COLORREF color)
    {
        if (brush != nullptr)
        {
            ::DeleteObject(brush);
        }
        brush = ::CreateSolidBrush(color);
    };

    // Only the paper is still a GDI brush: the chrome paints through GDI+ now, which takes the
    // hover tint as a colour. The header tone is used as a COLORREF straight from the palette -
    // there is no bar left to fill with it.
    swap(m_paper, m_paperColor);

    if (m_body != nullptr)
    {
        ::SendMessageW(m_body, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(m_paperColor));
    }

    // The footer keeps its own paper colour, so a theme change has to reach it too.
    if (m_footer != nullptr)
    {
        ::SetWindowLongPtrW(m_footer, GWLP_USERDATA, static_cast<LONG_PTR>(m_paperColor));
        ::InvalidateRect(m_footer, nullptr, FALSE);
    }

    if (m_window != nullptr)
    {
        ::InvalidateRect(m_window, nullptr, TRUE);
    }
}

void Note::SetTheme(int theme)
{
    const int resolved = core::ClampNoteTheme(theme);
    if (resolved == m_theme)
    {
        return;
    }

    ApplyTheme(resolved);

    if (themeChanged)
    {
        themeChanged(*this);
    }
}

void Note::Metrics()
{
    m_headerHeight = Scale(kHeaderDesign);
    m_border = std::max(1, Scale(1));
}

int Note::Scale(int value) const
{
    return ::MulDiv(value, m_dpi, 96);
}

float Note::ScaleF(float value) const
{
    return value * static_cast<float>(m_dpi) / 96.0F;
}

void Note::CreateBody()
{
    // WS_VSCROLL stays, but the bar it draws never becomes visible: LayoutChildren makes the body
    // wider than the client area, so the last seventeen pixels - the scrollbar - are clipped away
    // by the note's own WS_CLIPCHILDREN. Everything RichEdit does with scrolling keeps working,
    // which is a great deal cheaper than reimplementing it, and the note draws its own 3 px thumb
    // on top.
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_WANTRETURN
        | ES_AUTOVSCROLL | WS_VSCROLL;

    const int top = m_headerHeight + Scale(kBodyTopDesign);

    m_body = ::CreateWindowExW(0, kRichEditClass, L"", style, m_border, top,
        std::max(1, m_rect.width - m_border * 2),
        std::max(1, m_rect.height - top - m_border), m_window, nullptr, m_instance, nullptr);

    if (m_body == nullptr)
    {
        util::LogError(L"sticky note: cannot create the rich-text body");
        return;
    }

    if (m_fontBody != nullptr)
    {
        ::SendMessageW(m_body, WM_SETFONT, reinterpret_cast<WPARAM>(m_fontBody), FALSE);
    }

    ::SendMessageW(m_body, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(m_paperColor));
    ::SendMessageW(m_body, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(Scale(kMarginDesign), Scale(kMarginDesign)));
    ::SendMessageW(m_body, EM_SETUNDOLIMIT, 200, 0);

    m_bodyHook.owner = this;
    m_bodyHook.previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        m_body, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Note::BodyProc)));
    ::SetPropW(m_body, kBodyHookProp, reinterpret_cast<HANDLE>(&m_bodyHook));

    StyleBody();
}

void Note::StyleBody()
{
    if (m_body == nullptr)
    {
        return;
    }

    m_suspendNotify = true;

    // Only the empty-document default, so text that arrives from the RTF keeps its own formatting.
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
    format.crTextColor = kInk;

    // yHeight is in twips, and a twip is 1/20 point: 1440/dpi converts a pixel size to twips at
    // this window's scaling.
    format.yHeight = ::MulDiv(15, 1440, m_dpi);

    const std::wstring face = L"Microsoft YaHei UI";
    wcsncpy_s(format.szFaceName, face.c_str(), _TRUNCATE);

    ::SendMessageW(m_body, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    ApplyLineSpacing();
    ::SendMessageW(m_body, EM_SETMODIFY, FALSE, 0);

    m_suspendNotify = false;
}

void Note::ApplyLineSpacing()
{
    if (m_body == nullptr)
    {
        return;
    }

    // Chinese set at RichEdit's default single spacing reads as a solid block; half a line more is
    // the difference between "a wall of text" and something that looks like it was written on
    // paper. Rule 1 (one and a half lines) rather than an exact height, because an exact height
    // clips whatever is taller than it - which on a note means the screenshots.
    PARAFORMAT2 spacing{};
    spacing.cbSize = sizeof(spacing);
    spacing.dwMask = PFM_LINESPACING;
    spacing.bLineSpacingRule = 1;

    ::SendMessageW(m_body, EM_SETPARAFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&spacing));
}

void Note::LayoutChildren()
{
    RECT client{};
    ::GetClientRect(m_window, &client);

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int top = m_headerHeight + Scale(kBodyTopDesign);

    // Paper below the last line as well as above the first one: a note whose text runs into the
    // bottom edge reads as a text box, not as a sheet.
    const int bottom = std::max(top + 1, height - m_border - Scale(kBottomPadDesign));

    if (m_body != nullptr)
    {
        // The extra width is exactly one native scrollbar, so RichEdit's client area - its window
        // minus the scrollbar it always reserves - stays the full width of the paper, and the text
        // wraps in the same place whether or not the note currently overflows. The bar itself ends
        // up outside the client rectangle and is clipped away.
        const int native = std::max(0, ::GetSystemMetrics(SM_CXVSCROLL));

        ::MoveWindow(m_body, m_border, top, std::max(1, width - m_border * 2 + native),
            std::max(1, bottom - top), TRUE);
    }

    if (m_scroll != nullptr)
    {
        ::MoveWindow(m_scroll, width - m_border - Scale(kScrollWidthDesign), top,
            Scale(kScrollWidthDesign), std::max(1, bottom - top), TRUE);
    }

    // The footer starts exactly where the text and our own scrollbar stop. It covers only the
    // margin, so nothing the user typed can be hidden by it.
    if (m_footer != nullptr)
    {
        ::SetWindowPos(m_footer, HWND_TOP, 0, bottom, width, std::max(1, height - bottom),
            SWP_NOACTIVATE);
    }

    const int button = Scale(kButtonDesign);
    const int buttonTop = (m_headerHeight - button) / 2;
    const int gap = Scale(kButtonGapDesign);

    // Walked right to left from one inset, with a constant step: the old version measured each box
    // from the glyph before it, which made the pitch 31 / 34 instead of a single number.
    int right = width - Scale(kSideInsetDesign) - m_border;

    m_close.right = right;
    m_close.left = right - button;
    m_close.top = buttonTop;
    m_close.bottom = buttonTop + button;
    right = m_close.left - gap;

    m_shot.right = right;
    m_shot.left = right - button;
    m_shot.top = buttonTop;
    m_shot.bottom = buttonTop + button;
    right = m_shot.left - gap;

    m_dot.right = right;
    m_dot.left = right - Scale(kDotDesign);
    m_dot.top = buttonTop;
    m_dot.bottom = buttonTop + button;

    ::InvalidateRect(m_window, nullptr, FALSE);
    UpdateScrollbar();
}

void Note::SetRect(const core::Rect& rect)
{
    m_rect = rect;
    if (m_window == nullptr)
    {
        return;
    }

    ::SetWindowPos(m_window, nullptr, rect.left, rect.top, rect.width, rect.height,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

void Note::LoadBody()
{
    if (m_body == nullptr)
    {
        return;
    }

    m_suspendNotify = true;

    std::string bytes;
    if (!ReadWholeFile(core::NoteStore{}.BodyPath(m_id), bytes) || bytes.empty())
    {
        ::SendMessageW(m_body, EM_SETMODIFY, FALSE, 0);
        m_suspendNotify = false;
        return;
    }

    // Bodies written before pictures learned to start their own line carry a \pict in the middle of
    // a paragraph, which makes RichEdit reserve the picture's height in that line and leave a blank
    // band behind. Patching the bytes on the way in repairs notes written by the earlier builds,
    // and storing the patched bytes means the first save writes the repaired form.
    bytes = core::BreakBeforePictures(bytes);
    m_loadedBytes = bytes;

    LoadSource source{ bytes.data(), bytes.size(), 0 };
    if (!StreamingEdit(m_body, EM_STREAMIN, SF_RTF, &source, &LoadCallback))
    {
        util::LogWarn(L"sticky note {}: the saved body could not be read back", m_id);
    }

    // The stream carried the note's own \sl values, which are single spacing from the builds
    // before the body knew better.
    ApplyLineSpacing();
    UpdateScrollbar();

    ::SendMessageW(m_body, EM_SETMODIFY, FALSE, 0);
    m_suspendNotify = false;

    // Streaming in is reported as EN_UPDATE, which has already armed the save timer on a document
    // that is on disk. Disarm it and settle the status line on "saved" rather than "unsaved".
    if (m_window != nullptr)
    {
        ::KillTimer(m_window, kSaveTimer);
    }

    m_dirty = false;
    m_savedAt = WrittenAt(core::NoteStore{}.BodyPath(m_id));
    RefreshStatus();
}

void Note::SaveBody()
{
    if (m_body == nullptr || !m_dirty)
    {
        return;
    }

    // An empty document owns no file. This keeps a brand-new note from writing anything until the
    // user actually types, and gives the disk back when someone clears a note out.
    if (::SendMessageW(m_body, WM_GETTEXTLENGTH, 0, 0) == 0)
    {
        std::error_code ec;
        std::filesystem::remove(core::NoteStore{}.BodyPath(m_id), ec);
        m_dirty = false;
        return;
    }

    SaveSink sink;
    if (!StreamingEdit(m_body, EM_STREAMOUT, SF_RTF, &sink, &SaveCallback))
    {
        util::LogError(L"sticky note {}: could not serialise the body", m_id);
        return;
    }

    // Untouched text serialises to exactly the bytes it arrived as, so this is also the answer to
    // "did the user actually change anything".
    if (sink.bytes == m_loadedBytes)
    {
        ::SendMessageW(m_body, EM_SETMODIFY, FALSE, 0);
        m_dirty = false;
        RefreshStatus();
        return;
    }

    if (!WriteWholeFile(core::NoteStore{}.BodyPath(m_id), sink.bytes))
    {
        util::LogError(L"sticky note {}: could not write the body", m_id);
        return;
    }

    ::SendMessageW(m_body, EM_SETMODIFY, FALSE, 0);
    m_loadedBytes.clear();
    m_dirty = false;
    MarkSaved();
}

void Note::Flush()
{
    if (m_window != nullptr)
    {
        ::KillTimer(m_window, kSaveTimer);
    }

    SaveBody();
}

std::wstring Note::Preview() const
{
    // EM_GETTEXTEX with GT_DEFAULT gives plain text out of a rich body without dragging the RTF
    // control's formatting along. A short cap keeps an archived row to one line in the panel.
    if (m_body == nullptr)
    {
        return {};
    }

    constexpr std::size_t kCap = 64;
    const LONG length = static_cast<LONG>(::SendMessageW(m_body, WM_GETTEXTLENGTH, 0, 0));
    if (length <= 0)
    {
        return {};
    }

    std::wstring raw(static_cast<std::size_t>(length) + 1, L'\0');
    GETTEXTEX get{};
    get.cb = static_cast<DWORD>(raw.size() * sizeof(wchar_t));
    get.codepage = 1200; // CP_UNICODE
    get.flags = GT_DEFAULT;
    ::SendMessageW(m_body, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&get),
        reinterpret_cast<LPARAM>(raw.data()));
    raw.resize(wcsnlen(raw.c_str(), raw.size()));

    std::wstring flat;
    for (wchar_t ch : raw)
    {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t')
        {
            if (!flat.empty() && flat.back() != L' ')
            {
                flat.push_back(L' ');
            }
        }
        else if (flat.size() < kCap)
        {
            flat.push_back(ch);
        }
    }

    while (!flat.empty() && flat.back() == L' ')
    {
        flat.pop_back();
    }

    return flat;
}

void Note::MarkDirty()
{
    // EN_UPDATE arrives on every keystroke and on every reformat, so this runs often.
    const bool first = !m_dirty;
    m_dirty = true;

    if (m_window == nullptr)
    {
        return;
    }

    // Restarting the timer on every change is the debounce: the save only fires once the typing
    // pauses.
    ::SetTimer(m_window, kSaveTimer, kSaveDelayMs, nullptr);

    if (first)
    {
        RefreshStatus();
    }
}

void Note::MarkSaved()
{
    ::GetLocalTime(&m_savedAt);
    RefreshStatus();
}

void Note::InvalidateHeader()
{
    if (m_window == nullptr)
    {
        return;
    }

    RECT client{};
    ::GetClientRect(m_window, &client);

    RECT header{ 0, 0, client.right, m_headerHeight };
    ::InvalidateRect(m_window, &header, FALSE);
}

void Note::RefreshStatus()
{
    // The save state lives in the title now, so this repaints the header strip and nothing else.
    InvalidateHeader();
}

void Note::InsertPicture(const std::vector<std::uint8_t>& png, int width, int height)
{
    if (m_body == nullptr || png.empty() || width <= 0 || height <= 0)
    {
        return;
    }

    RECT client{};
    ::GetClientRect(m_body, &client);
    const int available = std::max(1, static_cast<int>(client.right - client.left) - Scale(24));

    // Fit the width, and give up width to the height when the picture is portrait: a screenshot
    // that runs off the bottom of the note is one the user has to scroll to see the top of.
    const int maxHeight = std::max(1, static_cast<int>(client.bottom - client.top) - Scale(8));

    int shownWidth = std::min(width, available);
    int shownHeight = std::max(1, static_cast<int>(
        std::lround(static_cast<double>(shownWidth) * height / width)));

    if (shownHeight > maxHeight)
    {
        shownHeight = maxHeight;
        shownWidth = std::max(1, static_cast<int>(
            std::lround(static_cast<double>(shownHeight) * width / height)));
    }

    // Twips, not pixels: RichEdit lays pictures out in twentieths of a point.
    const int goalX = ::MulDiv(shownWidth, 1440, m_dpi);
    const int goalY = ::MulDiv(shownHeight, 1440, m_dpi);

    const std::string rtf = BuildPictureRtf(png, width, height, goalX, goalY);
    LoadSource source{ rtf.data(), rtf.size(), 0 };

    // Focus first, then insert. The overlay that just closed had the focus, and an unfocused
    // RichEdit drops SFF_SELECTION content at the top of the document instead of at the caret -
    // which is where a screenshot taken from the note used to land, above the text that asked for
    // it. Restoring the focus is what makes "insert at the caret" mean what it says.
    ::SetFocus(m_body);

    if (!StreamingEdit(m_body, EM_STREAMIN, SF_RTF | SFF_SELECTION, &source, &LoadCallback))
    {
        util::LogWarn(L"sticky note {}: the picture could not be inserted", m_id);
        return;
    }

    MarkDirty();
}

void Note::TakeScreenshot()
{
    // Remember where the caret was. The overlay takes the focus for the duration of the drag, and a
    // RichEdit that has lost focus answers the later SFF_SELECTION insert at position 0 - which is
    // how the first version of this put every screenshot above the text that asked for it.
    CHARRANGE caret{};
    if (m_body != nullptr)
    {
        ::SendMessageW(m_body, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&caret));
    }

    CapturedImage image;
    if (!PickRegion(m_instance, image))
    {
        return;
    }

    if (m_body != nullptr)
    {
        ::SetFocus(m_body);
        ::SendMessageW(m_body, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&caret));
    }

    InsertPicture(image.png, image.width, image.height);
}

LRESULT CALLBACK Note::BodyProc(HWND body, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* hook = reinterpret_cast<BodyHook*>(::GetPropW(body, kBodyHookProp));
    if (hook == nullptr || hook->owner == nullptr)
    {
        return ::DefWindowProcW(body, message, wparam, lparam);
    }

    return hook->owner->HandleBodyMessage(body, message, wparam, lparam, hook->previous);
}

LRESULT Note::HandleBodyMessage(
    HWND body, UINT message, WPARAM wparam, LPARAM lparam, WNDPROC previous)
{
    const auto forward = [&]()
    { return ::CallWindowProcW(previous, body, message, wparam, lparam); };

    switch (message)
    {
        case WM_LBUTTONDOWN:
            {
                // A press in the text puts the swatches away, exactly as a press anywhere else on
                // the note does - the palette is a child window, so this is the only place its
                // dismissal can come from.
                m_palette.Hide();

                // A press on a picture is taken over before RichEdit sees it. The control moves an
                // object by dragging it through OLE, which inserts a second copy and leaves the
                // first one's footprint on the line it came from: the blank stays, the line never
                // re-wraps, and the move is not on the undo stack at all. Everything below does the
                // same move as two ordinary edits instead - see MovePicture.
                const POINT here{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                const int picture = PictureCharAt(here);
                if (picture < 0)
                {
                    break;
                }

                m_dragFrom = picture;
                m_dragTo = picture;
                m_dragOrigin = here;
                m_dragMoved = false;

                ::SetFocus(body);

                CHARRANGE picked{ picture, picture + 1 };
                ::SendMessageW(body, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&picked));
                ::SetCapture(body);
                return 0;
            }

        case WM_MOUSEMOVE:
            // The body covers almost the whole note, so entering the note usually means entering
            // this control and never the note window: the chip would stay hidden without this.
            PointerEntered();
            TrackPointer(body, false);

            if (m_dragFrom >= 0 && ::GetCapture() == body)
            {
                const POINT here{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                const int threshold = std::max(2, ::GetSystemMetrics(SM_CXDRAG) / 2);

                if (!m_dragMoved
                    && (std::abs(here.x - m_dragOrigin.x) > threshold
                        || std::abs(here.y - m_dragOrigin.y) > threshold))
                {
                    m_dragMoved = true;
                }

                if (m_dragMoved)
                {
                    // The caret is the drop mark: it is where the picture will land, and keeping it
                    // on the hover position means RichEdit scrolls the target into view by itself.
                    m_dragTo = CaretAt(here);

                    CHARRANGE drop{ m_dragTo, m_dragTo };
                    ::SendMessageW(body, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&drop));
                    return 0;
                }
            }
            break;

        case WM_LBUTTONUP:
            if (m_dragFrom >= 0 && ::GetCapture() == body)
            {
                const int from = m_dragFrom;
                const int to = m_dragTo;
                const bool dragged = m_dragMoved;

                // Cleared before the capture goes, because releasing it sends WM_CAPTURECHANGED.
                m_dragFrom = -1;
                m_dragMoved = false;
                ::ReleaseCapture();

                if (dragged)
                {
                    MovePicture(from, to);
                }
                return 0;
            }
            break;

        case WM_PAINT:
            {
                // The control paints itself, and then the hint goes on top: a blank note would
                // otherwise be an empty coloured rectangle with nothing to say it is editable.
                const LRESULT result = forward();
                DrawPlaceholder(body);
                return result;
            }

        case WM_MOUSELEAVE:
            RefreshPointer();
            break;

        case WM_CAPTURECHANGED:
            // Anything else that takes the mouse away ends the gesture, so a press that never
            // became a drag cannot leave a pending move behind.
            m_dragFrom = -1;
            m_dragMoved = false;
            break;

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            // The focus decides both the chrome and whether the hint is drawn, and neither of them
            // repaints on its own: a note left empty would keep or lose the hint at random.
            m_bodyFocused = message == WM_SETFOCUS;
            InvalidateHeader();
            ::InvalidateRect(body, nullptr, FALSE);
            break;

        case WM_VSCROLL:
        case WM_MOUSEWHEEL:
            {
                // RichEdit scrolls itself and tells nobody, so the thumb is refreshed around the
                // call instead of being told about it afterwards.
                const LRESULT result = forward();
                UpdateScrollbar();
                return result;
            }

        case WM_NCDESTROY:
            ::RemovePropW(body, kBodyHookProp);
            break;

        default:
            break;
    }

    return forward();
}

void Note::DrawPlaceholder(HWND body)
{
    if (::SendMessageW(body, WM_GETTEXTLENGTH, 0, 0) != 0 || ::GetFocus() == body)
    {
        // With the caret in it an empty note already says what it is; the hint would only sit
        // under the blinking caret.
        return;
    }

    const HDC dc = ::GetDC(body);
    if (dc == nullptr)
    {
        return;
    }

    const std::wstring_view hint = util::i18n::Text(L"写点什么…");

    RECT client{};
    ::GetClientRect(body, &client);
    client.left += Scale(kMarginDesign);
    client.top += Scale(2);

    const HGDIOBJ previousFont = ::SelectObject(dc, m_fontBody);
    const int previousMode = ::SetBkMode(dc, TRANSPARENT);
    const COLORREF previousInk = ::SetTextColor(dc, m_mutedColor);

    ::DrawTextW(dc, hint.data(), static_cast<int>(hint.size()), &client,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    ::SetTextColor(dc, previousInk);
    ::SetBkMode(dc, previousMode);
    ::SelectObject(dc, previousFont);
    ::ReleaseDC(body, dc);
}

int Note::PictureCharAt(POINT client) const
{
    if (m_body == nullptr)
    {
        return -1;
    }

    const LRESULT hit = ::SendMessageW(m_body, EM_CHARFROMPOS, 0, reinterpret_cast<LPARAM>(&client));
    const int nearest = static_cast<int>(LOWORD(hit));

    // A point inside the picture snaps to whichever character boundary is closer, so the picture
    // can be the character before the caret as well as the one under it.
    for (const int candidate : { nearest, nearest - 1 })
    {
        if (candidate < 0)
        {
            continue;
        }

        // A picture reads back as a space, and so does a real space, so only spaces are worth the
        // serialisation. Nothing else in a note can be an image.
        if (BodyCharAt(candidate) == L' ' && BodyCharIsPicture(candidate))
        {
            return candidate;
        }
    }

    return -1;
}

bool Note::BodyCharIsPicture(int position) const
{
    if (m_body == nullptr)
    {
        return false;
    }

    CHARRANGE range{ position, position + 1 };
    ::SendMessageW(m_body, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));

    PictureProbe probe;
    EDITSTREAM stream{};
    stream.dwCookie = reinterpret_cast<DWORD_PTR>(&probe);
    stream.dwError = 0;
    stream.pfnCallback = ProbeCallback;

    // The stream is aborted on purpose once the answer is known, so dwError is expected to be set.
    ::SendMessageW(m_body, EM_STREAMOUT, SF_RTF | SFF_SELECTION,
        reinterpret_cast<LPARAM>(&stream));

    return probe.found;
}

wchar_t Note::BodyCharAt(int position) const
{
    wchar_t text[2]{};

    TEXTRANGE range{};
    range.chrg.cpMin = position;
    range.chrg.cpMax = position + 1;
    range.lpstrText = text;

    ::SendMessageW(m_body, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&range));
    return text[0];
}

int Note::CaretAt(POINT client) const
{
    if (m_body == nullptr)
    {
        return 0;
    }

    LRESULT hit = ::SendMessageW(m_body, EM_CHARFROMPOS, 0, reinterpret_cast<LPARAM>(&client));
    const auto length = static_cast<int>(::SendMessageW(m_body, WM_GETTEXTLENGTH, 0, 0));
    int position = static_cast<int>(LOWORD(hit));

    if (position < 0)
    {
        position = 0;
    }
    if (position > length)
    {
        position = length;
    }

    return position;
}

void Note::MovePicture(int from, int to)
{
    if (m_body == nullptr || from < 0 || to < 0)
    {
        return;
    }

    // A picture that starts its own line has a soft break in front of it (0x0B, which is what
    // RichEdit makes of \line). Taking the picture without that break would leave an empty line
    // where it used to be - the small version of the bug this whole method exists to fix.
    int start = from;
    int span = 1;
    if (start > 0 && BodyCharAt(start - 1) == L'\v')
    {
        --start;
        span = 2;
    }

    CHARRANGE picture{ start, start + span };
    ::SendMessageW(m_body, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&picture));
    SaveSink sink;
    if (!StreamingEdit(m_body, EM_STREAMOUT, SF_RTF | SFF_SELECTION, &sink, &SaveCallback)
        || sink.bytes.empty())
    {
        util::LogWarn(L"sticky note {}: the picture could not be picked up", m_id);
        return;
    }

    // Taking the picture out is what makes the line it sat on re-wrap. This is an edit, so it also
    // lands on the undo stack - which is the other half of what dragging it used to lose.
    ::SendMessageW(m_body, WM_CLEAR, 0, 0);

    if (to >= start + span)
    {
        to -= span;
    }
    else if (to > start)
    {
        to = start;
    }

    CHARRANGE drop{ to, to };
    ::SendMessageW(m_body, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&drop));
    ::SetFocus(m_body);

    // Same soft break the insert uses: RichEdit reserves a picture's box in the line it is added to
    // and then draws the picture on the line below, so a picture put down mid-line leaves a blank
    // band behind it as tall as the picture. Starting it at the head of a new line avoids that.
    const std::string rtf = core::BreakBeforePictures(sink.bytes);

    LoadSource source{ rtf.data(), rtf.size(), 0 };
    if (!StreamingEdit(m_body, EM_STREAMIN, SF_RTF | SFF_SELECTION, &source, &LoadCallback))
    {
        util::LogWarn(L"sticky note {}: the picture could not be put down", m_id);
        return;
    }

    // Leave the picture selected where it landed: it is the only confirmation the user gets, and
    // Delete then removes the thing they just moved.
    CHARRANGE landed{};
    ::SendMessageW(m_body, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&landed));
    if (landed.cpMax > 0)
    {
        CHARRANGE mark{ landed.cpMax - 1, landed.cpMax };
        ::SendMessageW(m_body, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&mark));
    }

    util::LogInfo(L"sticky note {}: picture moved from {} to {}", m_id, from, to);
    MarkDirty();
}

void Note::CreateScrollbar()
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Note::ScrollProc;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kScrollClass;

        if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            util::LogError(L"sticky note: cannot register the scrollbar class");
            return;
        }

        registered = true;
    }

    m_scroll = ::CreateWindowExW(0, kScrollClass, L"", WS_CHILD, 0, 0, 0, 0, m_window, nullptr,
        m_instance, this);

    if (m_scroll != nullptr)
    {
        // Above the body, or the 3 px strip would be underneath the text it belongs to: the body
        // is wider than the note and its own edge covers exactly this column.
        ::SetWindowPos(m_scroll, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    static bool footerRegistered = false;
    if (!footerRegistered)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &FooterProc;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kFooterClass;

        if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            util::LogError(L"sticky note: cannot register the footer class");
            return;
        }

        footerRegistered = true;
    }

    m_footer = ::CreateWindowExW(WS_EX_TRANSPARENT, kFooterClass, L"", WS_CHILD, 0, 0, 0, 0,
        m_window, nullptr, m_instance, nullptr);
    if (m_footer != nullptr)
    {
        ::SetWindowLongPtrW(m_footer, GWLP_USERDATA, static_cast<LONG_PTR>(m_paperColor));
        ::SetWindowPos(m_footer, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void Note::UpdateScrollbar()
{
    if (m_scroll == nullptr || m_body == nullptr)
    {
        return;
    }

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_ALL;

    bool overflows = false;
    if (::GetScrollInfo(m_body, SB_VERT, &info) != FALSE)
    {
        overflows = static_cast<int>(info.nMax - info.nMin + 1) > static_cast<int>(info.nPage);
    }

    if (overflows != m_scrollVisible)
    {
        m_scrollVisible = overflows;
        ::ShowWindow(m_scroll, overflows ? SW_SHOWNOACTIVATE : SW_HIDE);

        if (overflows)
        {
            ::SetWindowPos(m_scroll, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    if (overflows)
    {
        ::InvalidateRect(m_scroll, nullptr, FALSE);
    }
}

bool Note::ScrollMetrics(int& trackTop, int& thumbTop, int& thumbHeight) const
{
    if (m_scroll == nullptr || m_body == nullptr)
    {
        return false;
    }

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_ALL;
    if (::GetScrollInfo(m_body, SB_VERT, &info) == FALSE)
    {
        return false;
    }

    // RichEdit reports its vertical range in pixels, so everything below is a straight ratio and
    // the thumb lands where the content actually is.
    const int range = info.nMax - info.nMin + 1;
    const int page = static_cast<int>(info.nPage);
    if (range <= page)
    {
        return false;
    }

    RECT client{};
    ::GetClientRect(m_scroll, &client);
    trackTop = client.top;

    const int track = client.bottom - client.top;
    thumbHeight = std::max(Scale(kScrollThumbDesign), ::MulDiv(track, page, range));
    thumbHeight = std::min(thumbHeight, track);

    const int span = range - page;
    thumbTop = trackTop + ::MulDiv(track - thumbHeight, info.nPos - info.nMin, span);
    return true;
}

void Note::ScrollToPixel(int pixel)
{
    if (m_body == nullptr)
    {
        return;
    }

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_ALL;
    if (::GetScrollInfo(m_body, SB_VERT, &info) == FALSE)
    {
        return;
    }

    const int lowest = info.nMin;
    const int highest = std::max(lowest, info.nMax - static_cast<int>(info.nPage) + 1);
    const int target = std::clamp(pixel, lowest, highest);
    if (target == info.nPos)
    {
        return;
    }

    POINTL point{ 0, target };
    ::SendMessageW(m_body, kEditSetScrollPos, 0, reinterpret_cast<LPARAM>(&point));
}

void Note::ScrollbarPage(bool down)
{
    if (m_body == nullptr)
    {
        return;
    }

    ::SendMessageW(m_body, WM_VSCROLL, MAKEWPARAM(down ? SB_PAGEDOWN : SB_PAGEUP, 0), 0);
    UpdateScrollbar();
}

// The strip of paper below the text. RichEdit draws its own scrollbars, and when the body gets the
// focus a horizontal one can appear in this margin - a grey Windows strip across the bottom of a
// sheet. This child covers the margin with the paper colour after the body has painted, and is
// transparent to the mouse, so the note still sees clicks there. It changes nothing about the text
// layout or about our own three-pixel scrollbar, which stops exactly where this starts.
LRESULT CALLBACK FooterProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            {
                PAINTSTRUCT paint{};
                const HDC dc = ::BeginPaint(window, &paint);
                RECT client{};
                ::GetClientRect(window, &client);
                const HBRUSH brush = ::CreateSolidBrush(
                    static_cast<COLORREF>(::GetWindowLongPtrW(window, GWLP_USERDATA)));
                ::FillRect(dc, &client, brush);
                ::DeleteObject(brush);
                ::EndPaint(window, &paint);
            }
            return 0;

        case WM_NCHITTEST:
            return HTTRANSPARENT;

        default:
            break;
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK Note::ScrollProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<Note*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    auto* self = reinterpret_cast<Note*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr)
    {
        return self->HandleScrollMessage(window, message, wparam, lparam);
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT Note::HandleScrollMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_PAINT:
            {
                PAINTSTRUCT paint{};
                const HDC dc = ::BeginPaint(window, &paint);

                RECT client{};
                ::GetClientRect(window, &client);
                ::FillRect(dc, &client, m_paper);

                int trackTop = 0;
                int thumbTop = 0;
                int thumbHeight = 0;
                if (ScrollMetrics(trackTop, thumbTop, thumbHeight))
                {
                    const COLORREF tint = ToRef(core::BlendColor(
                        core::NoteThemeAt(m_theme).rule, 0x37352F, m_scrollHot || m_scrollGrab >= 0 ? 55 : 38));

                    const HBRUSH thumb = ::CreateSolidBrush(tint);
                    const HGDIOBJ oldBrush = ::SelectObject(dc, thumb);
                    const HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));

                    const int inset = std::max(0,
                        (static_cast<int>(client.right - client.left) - Scale(2)) / 2);
                    ::RoundRect(dc, client.left + inset, thumbTop, client.right - inset,
                        thumbTop + thumbHeight, Scale(2), Scale(2));

                    ::SelectObject(dc, oldPen);
                    ::SelectObject(dc, oldBrush);
                    ::DeleteObject(thumb);
                }

                ::EndPaint(window, &paint);
            }
            return 0;

        case WM_MOUSEMOVE:
            {
                if (::GetCapture() == window && m_scrollGrab >= 0)
                {
                    const int y = GET_Y_LPARAM(lparam);

                    int trackTop = 0;
                    int thumbTop = 0;
                    int thumbHeight = 0;
                    if (ScrollMetrics(trackTop, thumbTop, thumbHeight))
                    {
                        RECT client{};
                        ::GetClientRect(window, &client);
                        const int track = std::max(1, static_cast<int>(client.bottom - client.top));
                        const int travel = std::max(1, track - thumbHeight);

                        SCROLLINFO info{};
                        info.cbSize = sizeof(info);
                        info.fMask = SIF_ALL;
                        if (::GetScrollInfo(m_body, SB_VERT, &info) != FALSE)
                        {
                            const int span = std::max(1, info.nMax - info.nMin
                                - static_cast<int>(info.nPage) + 1);
                            const int offset = std::clamp(y - m_scrollGrab - trackTop, 0, travel);

                            ScrollToPixel(info.nMin + ::MulDiv(offset, span, travel));
                            ::InvalidateRect(window, nullptr, FALSE);
                        }
                    }
                    return 0;
                }

                if (!m_scrollHot)
                {
                    m_scrollHot = true;
                    ::InvalidateRect(window, nullptr, FALSE);
                    TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                    ::TrackMouseEvent(&track);
                }
            }
            return 0;

        case WM_MOUSELEAVE:
            m_scrollHot = false;
            ::InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN:
            {
                const int y = GET_Y_LPARAM(lparam);

                int trackTop = 0;
                int thumbTop = 0;
                int thumbHeight = 0;
                if (!ScrollMetrics(trackTop, thumbTop, thumbHeight))
                {
                    return 0;
                }

                if (y >= thumbTop && y < thumbTop + thumbHeight)
                {
                    m_scrollGrab = y - thumbTop;
                    ::SetCapture(window);
                }
                else
                {
                    // A press on the track is a page, the way it is everywhere else on Windows.
                    ScrollbarPage(y >= thumbTop);
                }
            }
            return 0;

        case WM_LBUTTONUP:
            if (::GetCapture() == window)
            {
                m_scrollGrab = -1;
                ::ReleaseCapture();
                ::InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_CAPTURECHANGED:
            m_scrollGrab = -1;
            ::InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL:
            // The bar is 4 px wide; the wheel over it has to reach the text, not stop at the strip.
            if (m_body != nullptr)
            {
                ::SendMessageW(m_body, message, wparam, lparam);
                UpdateScrollbar();
            }
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

void Note::FireButton(int button)
{
    // One line per click, deliberately kept in the shipping build: "the buttons do nothing" cannot
    // be told apart from "the click never reached the window" after the fact, and this is the only
    // evidence a released binary can leave behind. Clicking a header button is rare enough that
    // this never becomes noise.
    util::LogInfo(L"sticky note {}: header button {} fired", m_id, button);

    switch (button)
    {
        case kButtonDot:
            OpenPalette();
            break;

        case kButtonShot:
            TakeScreenshot();
            break;

        case kButtonClose:
            if (closeRequested)
            {
                closeRequested(*this);
            }
            break;

        default:
            break;
    }
}

void Note::OpenPalette()
{
    // A second click on the dot closes the palette again: the first thing a user tries when the
    // swatches are already up is the button that put them there.
    if (m_palette.Visible())
    {
        m_palette.Hide();
        return;
    }

    if (m_window == nullptr)
    {
        return;
    }

    // Client coordinates now, not screen: the palette is a child of the note and is clipped to the
    // paper, which is also what keeps it from needing the mouse capture.
    const POINT anchor{ m_dot.left, m_dot.bottom + Scale(2) };

    m_palette.closed = [this] { InvalidateHeader(); };
    m_palette.Show(m_window, m_dpi, anchor, m_headerColor, m_theme,
        [this](int picked) { SetTheme(picked); });
}

void Note::TrackPointer(HWND target, bool nonClient)
{
    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = nonClient ? (TME_LEAVE | TME_NONCLIENT) : TME_LEAVE;
    track.hwndTrack = target;
    ::TrackMouseEvent(&track);
}

void Note::PointerEntered()
{
    if (m_pointerInside)
    {
        return;
    }

    m_pointerInside = true;
    InvalidateHeader();
}

void Note::RefreshPointer()
{
    POINT cursor{};
    ::GetCursorPos(&cursor);

    // Asks where the pointer actually is instead of trusting the message that prompted the call:
    // a window that held the capture - the palette, the screenshot overlay - swallows the moves
    // and the leave, and a note that only listens to its own messages would stay lit afterwards.
    //
    // Leaving one part of the note for another is not leaving the note either: the chip has to stay
    // up when the pointer crosses from the header into the text, and tracking has to move with it
    // or the next leave would never be reported.
    const HWND under = ::WindowFromPoint(cursor);

    // The swatches are part of the note as far as the pointer is concerned: moving onto them is not
    // leaving, and the chip has to stay up while they are showing.
    if (m_palette.Owns(under))
    {
        m_pointerInside = true;
        return;
    }

    if (under == m_window || under == m_body)
    {
        // Still on the note, so this leave was one part of it handing over to another - the header
        // to the text, or the text back to the header. Only the state is corrected here; tracking
        // is NOT re-armed.
        //
        // Re-arming it was a livelock: `TrackMouseEvent(TME_NONCLIENT)` called from inside
        // WM_NCMOUSELEAVE fires again at once while the pointer sits over the client area, so each
        // pixel of movement cost thousands of messages - the note took seconds to answer a click,
        // could not be dragged, and its palette appeared late and then vanished. The message that
        // brings the pointer in already arms the tracking it needs: WM_MOUSEMOVE for the client
        // area, WM_NCMOUSEMOVE for the caption.
        if (!m_pointerInside)
        {
            m_pointerInside = true;
            InvalidateHeader();
        }
        return;
    }

    if (!m_pointerInside)
    {
        return;
    }

    m_pointerInside = false;
    m_hover = kButtonNone;
    m_palette.Hide();
    InvalidateHeader();
}

void Note::Focus()
{
    if (m_window == nullptr)
    {
        return;
    }

    ::SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    ::ShowWindow(m_window, SW_SHOW);

    if (m_body != nullptr)
    {
        ::SetFocus(m_body);
    }
}

LRESULT CALLBACK Note::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<Note*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_window = window;
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    auto* self = reinterpret_cast<Note*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr)
    {
        return self->HandleMessage(message, wparam, lparam);
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT Note::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_NCCALCSIZE:
            // The frame is drawn by hand, so the client area is the whole window. Returning 0 keeps
            // the resize borders that WS_THICKFRAME installs, which is the whole point of using it.
            if (wparam == TRUE)
            {
                return 0;
            }
            break;

        case WM_NCHITTEST:
            return HitTest(POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) });

        case WM_GETMINMAXINFO:
            {
                auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
                info->ptMinTrackSize.x = Scale(220);
                info->ptMinTrackSize.y = Scale(150);
            }
            return 0;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            {
                // Both are a press. A second click inside the double-click interval arrives as
                // WM_LBUTTONDBLCLK, and the class registers CS_DBLCLKS, so treating it as "do
                // nothing" made every fast second click vanish - which is what "clicking twice does
                // nothing" was. Nothing is run here; the action waits for the release.
                const POINT here{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                const int button = ButtonAt(here);

                // Any press on the note but the dot itself puts the swatches away. The palette is a
                // child window, so its own clicks never reach here; everything else does, which is
                // why dismissal needs no capture and cannot swallow a click either.
                if (m_palette.Visible() && button != kButtonDot)
                {
                    m_palette.Hide();
                }

                if (button == kButtonNone)
                {
                    m_pressedButton = kButtonNone;
                    break;
                }

                m_pressedButton = button;

                // The capture is the point: without it the release goes to whatever window the note
                // opened on the press, and the button never hears that its click ended.
                ::SetCapture(m_window);
                InvalidateHeader();
            }
            return 0;

        case WM_LBUTTONUP:
            {
                const int pressed = m_pressedButton;
                m_pressedButton = kButtonNone;

                // Order matters: releasing the capture sends WM_CAPTURECHANGED, which clears the
                // press that was just read.
                if (::GetCapture() == m_window)
                {
                    ::ReleaseCapture();
                }

                InvalidateHeader();

                // A press that slid off the button before coming up is a change of mind, not a
                // click - the same rule every other button on the desktop follows.
                const POINT here{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                if (pressed != kButtonNone && ButtonAt(here) == pressed)
                {
                    FireButton(pressed);
                }
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (m_pressedButton != kButtonNone)
            {
                m_pressedButton = kButtonNone;
                InvalidateHeader();
            }
            break;

        case WM_MOUSEMOVE:
            {
                PointerEntered();
                TrackPointer(m_window, false);

                const POINT here{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                const int over = ButtonAt(here);

                if (over != m_hover)
                {
                    m_hover = over;
                    InvalidateHeader();
                }
            }
            break;

        case WM_NCMOUSEMOVE:
            // The header is HTCAPTION, so the pointer over it sends non-client messages, and the
            // note never sees a single WM_MOUSEMOVE for the header the chrome lives in.
            PointerEntered();
            TrackPointer(m_window, true);
            break;

        case WM_MOUSELEAVE:
        case WM_NCMOUSELEAVE:
            RefreshPointer();
            return 0;

        case WM_EXITSIZEMOVE:
            {
                RECT window{};
                ::GetWindowRect(m_window, &window);
                m_rect = core::Rect{ static_cast<int>(window.left),
                    static_cast<int>(window.top), static_cast<int>(window.right - window.left),
                    static_cast<int>(window.bottom - window.top) };

                if (moved)
                {
                    moved(*this);
                }
            }
            return 0;

        case WM_SIZE:
            // The palette is anchored in client coordinates, so a resize would leave it stranded.
            m_palette.Hide();
            LayoutChildren();
            return 0;

        case WM_TIMER:
            if (wparam == kSaveTimer)
            {
                ::KillTimer(m_window, kSaveTimer);
                SaveBody();
                return 0;
            }
            break;

        case WM_COMMAND:
            // Measured, not assumed: Msftedit's RichEdit 4.1 never sends EN_CHANGE through
            // WM_COMMAND - it sends EN_UPDATE, and EN_UPDATE alone. So EN_UPDATE is the only
            // signal a note ever gets that its text changed. It also arrives when the control
            // merely reformats itself, which costs at most a write of identical bytes; checking
            // the modify flag here was tried and rejected, because the flag is not reliably set
            // yet at the moment the notification arrives.
            if (HIWORD(wparam) == EN_UPDATE && reinterpret_cast<HWND>(lparam) == m_body)
            {
                // The control's own dirty flag is the second half of the filter: streaming a note in
                // also raises EN_UPDATE, sometimes after the load has finished, and a note that has
                // just been read from disk is not a note the user edited.
                if (!m_suspendNotify && ::SendMessageW(m_body, EM_GETMODIFY, 0, 0) != 0)
                {
                    MarkDirty();
                }

                // The text is what decides whether the note has anything to scroll, and this is
                // the only notification a note ever gets that it changed.
                UpdateScrollbar();
                return 0;
            }
            break;

        case WM_ACTIVATE:
            // Leaving the note is a natural save point: the user has moved on, and whatever they
            // typed is finished. The swatches had their moment too.
            if (LOWORD(wparam) == WA_INACTIVE)
            {
                m_palette.Hide();
                Flush();
            }
            else if (HIWORD(wparam) != 0)
            {
                if (m_body != nullptr)
                {
                    ::SetFocus(m_body);
                }
            }
            return 0;

        case WM_SETFOCUS:
            if (m_body != nullptr)
            {
                ::SetFocus(m_body);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            Paint();
            return 0;

        case WM_CLOSE:
            if (closeRequested)
            {
                closeRequested(*this);
            }
            return 0;

        case WM_DESTROY:
            m_window = nullptr;
            m_body = nullptr;
            m_scroll = nullptr;
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(m_window, message, wparam, lparam);
}

int Note::ButtonAt(POINT client) const
{
    const auto inside = [&client](const RECT& box)
    {
        return client.x >= box.left && client.x < box.right && client.y >= box.top
            && client.y < box.bottom;
    };

    if (inside(m_dot))
    {
        return kButtonDot;
    }
    if (inside(m_shot))
    {
        return kButtonShot;
    }
    if (inside(m_close))
    {
        return kButtonClose;
    }

    return kButtonNone;
}

LRESULT Note::HitTest(POINT screen) const
{
    RECT window{};
    ::GetWindowRect(m_window, &window);

    const int grip = std::max(3, Scale(kGripDesign));

    const bool left = screen.x >= window.left && screen.x < window.left + grip;
    const bool right = screen.x > window.right - grip && screen.x <= window.right;
    const bool top = screen.y >= window.top && screen.y < window.top + grip;
    const bool bottom = screen.y > window.bottom - grip && screen.y <= window.bottom;

    if (top && left)
    {
        return HTTOPLEFT;
    }
    if (top && right)
    {
        return HTTOPRIGHT;
    }
    if (bottom && left)
    {
        return HTBOTTOMLEFT;
    }
    if (bottom && right)
    {
        return HTBOTTOMRIGHT;
    }
    if (left)
    {
        return HTLEFT;
    }
    if (right)
    {
        return HTRIGHT;
    }
    if (top)
    {
        return HTTOP;
    }
    if (bottom)
    {
        return HTBOTTOM;
    }

    POINT client = screen;
    ::ScreenToClient(m_window, &client);

    // Buttons have to report HTCLIENT or the drag that starts on them never reaches the window.
    if (ButtonAt(client) != kButtonNone)
    {
        return HTCLIENT;
    }

    if (client.y < m_headerHeight)
    {
        return HTCAPTION;
    }

    return HTCLIENT;
}

void Note::Paint()
{
    PAINTSTRUCT paint{};
    const HDC dc = ::BeginPaint(m_window, &paint);

    RECT client{};
    ::GetClientRect(m_window, &client);

    // Paper first, always: with the pointer outside the note this single fill is the entire
    // window, which is what makes a note read as a piece of paper rather than as a dialog.
    ::FillRect(dc, &client, m_paper);

    PaintChrome(dc);

    const HPEN pen = ::CreatePen(PS_SOLID, m_border, m_ruleColor);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, 0, 0, client.right, client.bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);

    ::EndPaint(m_window, &paint);
}

void Note::PaintChrome(HDC dc)
{
    // S3 "page-edge rule": the paper stays one unbroken sheet. The save time is the only thing
    // drawn unconditionally - it is the one line on a note worth reading at a glance - and a short
    // rule at its left turns it from decoration into metadata. The three icons arrive on hover with
    // no plate behind them: they sit straight on the paper, the way a pencil mark would.
    PaintTimestamp(dc);

    if (!ChromeVisible())
    {
        return;
    }

    const bool held = m_pressedButton != kButtonNone;
    PaintColourDot(dc, m_dot, m_hover == kButtonDot || (held && m_pressedButton == kButtonDot));
    PaintIconButton(
        dc, m_shot, kButtonShot, m_hover == kButtonShot || (held && m_pressedButton == kButtonShot));
    PaintIconButton(dc, m_close, kButtonClose,
        m_hover == kButtonClose || (held && m_pressedButton == kButtonClose));
}

void Note::PaintTimestamp(HDC dc)
{
    // MM-DD HH:MM:SS, always, one size below the body. "Not saved yet" is this same line in the
    // accent colour rather than a word of its own: the time never disappears, so a note that is
    // merely older than the last keystroke still tells you when it was last written.
    const std::wstring stamp = std::format(L"{:02}-{:02} {:02}:{:02}:{:02}", m_savedAt.wMonth,
        m_savedAt.wDay, m_savedAt.wHour, m_savedAt.wMinute, m_savedAt.wSecond);

    if (!GdiplusReady())
    {
        return;
    }

    // The page-edge rule is the one state worth interrupting a quiet note for: the same accent as
    // the timestamp uses when there are unsaved bytes, muted otherwise. Both live on the paper, so
    // the rule reads as a margin mark rather than as a piece of chrome.
    const COLORREF ink = m_dirty ? kAccent : m_mutedColor;
    const float scale = ScaleF(1.0F);
    const float ruleX = ScaleF(static_cast<float>(kMarginDesign)) + static_cast<float>(m_border);

    // One line box, centred in the header. The icons sit on that same centre line, so the title row
    // reads as a single thing instead of three elements pinned to different offsets.
    const float lineHeight = scale * kStampLine;
    const float lineTop = (static_cast<float>(m_headerHeight) - lineHeight) / 2.0F;

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    {
        // A 2-unit vertical line on integer pixels has nothing to anti-alias, but the round caps
        // still look better than the square ends a raw GDI pen leaves behind. It spans the line box,
        // so it brackets the digits instead of hanging off one end of them.
        Gdiplus::Pen rule(ToGdiplus(ink), scale * 2.0F);
        rule.SetStartCap(Gdiplus::LineCapRound);
        rule.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawLine(&rule, ruleX, lineTop, ruleX, lineTop + lineHeight);
    }

    // The timestamp starts one margin clear of the rule, so the two never touch.
    const float textLeft = ruleX + scale * 8.0F;
    const float textRight = static_cast<float>(m_dot.left) - scale * 8.0F;
    if (textRight <= textLeft)
    {
        return;
    }

    Gdiplus::FontFamily preferred(L"Microsoft YaHei UI");
    Gdiplus::FontFamily fallback(L"Microsoft YaHei");
    const Gdiplus::FontFamily* family = preferred.IsAvailable() ? &preferred : &fallback;

    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentNear);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    // Filled outline, not DrawString. Measured against the style sample, the two rasterisers land
    // in different places: DrawString grid-fits every stem onto whole pixels and comes out a weight
    // light, while filling the unhinted outline reproduces the sample's 108x12 ink box within a
    // pixel and its ink weight within 5% (296 vs 310 covered pixels). The browser that drew the
    // sample does the same thing - it positions glyphs at fractional offsets.
    const float em = scale * kStampDesign * kCellToEm;
    Gdiplus::GraphicsPath path;
    path.AddString(stamp.c_str(), -1, family, Gdiplus::FontStyleRegular, em,
        Gdiplus::RectF(textLeft, lineTop, textRight - textLeft, lineHeight), &format);

    Gdiplus::SolidBrush brush(ToGdiplus(ink));
    graphics.FillPath(&brush, &path);
}

void Note::PaintIconButton(HDC dc, const RECT& box, int icon, bool active)
{
    if (!GdiplusReady())
    {
        return;
    }

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const Gdiplus::RectF plate(static_cast<float>(box.left), static_cast<float>(box.top),
        static_cast<float>(box.right - box.left), static_cast<float>(box.bottom - box.top));

    if (active)
    {
        Gdiplus::GraphicsPath hover;
        RoundedRect(hover, plate, ScaleF(6.0F));
        // The tint is already mixed against the paper (GDI had no alpha), so this is an opaque
        // fill with the same colour the old GDI brush used.
        Gdiplus::SolidBrush wash(ToGdiplus(m_hoverColor));
        graphics.FillPath(&wash, &hover);
    }

    // Lucide draws on a 24-unit canvas with a 2-unit stroke. The style sample renders that canvas
    // at 18 units, so the grid is 18 - not 24. The 24-unit version filled the whole 22-unit button
    // and read as a fat, stepped outline rather than the sample's light line.
    const float gridSize = ScaleF(kIconDesign);
    const IconGrid grid{
        plate.X + (plate.Width - gridSize) / 2.0F,
        plate.Y + (plate.Height - gridSize) / 2.0F,
        gridSize,
    };

    const COLORREF ink = active ? kInk : m_iconColor;
    Gdiplus::Pen pen(ToGdiplus(ink), ScaleF(kIconStrokeDesign));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    if (icon == kButtonClose)
    {
        // Lucide "x": two diagonals across the 6..18 span, with round caps so the ends do not look
        // like clipped pixel staircases.
        graphics.DrawLine(&pen, grid.X(6.0F), grid.Y(6.0F), grid.X(18.0F), grid.Y(18.0F));
        graphics.DrawLine(&pen, grid.X(18.0F), grid.Y(6.0F), grid.X(6.0F), grid.Y(18.0F));
    }
    else
    {
        // Lucide "camera", transcribed from the sample's SVG rather than approximated: body
        // 2..22 x 7..20 with 2-unit corners, a trapezoid bump along the top edge (top 10..14 at
        // y=4, shoulders reaching 6 and 18 at y=7) and a 3-unit lens. The old version used a 4-unit
        // radius and a narrower bump, which at 19 px read as a rounded blob instead of a camera.
        const float r = grid.size * 2.0F / 24.0F;   // corner / fillet radius, 2 units
        Gdiplus::GraphicsPath camera;

        camera.AddLine(grid.X(10.0F), grid.Y(4.0F), grid.X(14.0F), grid.Y(4.0F));
        // Shoulders are 2-unit fillets in the SVG; a cubic lands within a fraction of a pixel at
        // this size and keeps the tangents smooth.
        camera.AddBezier(grid.X(14.0F), grid.Y(4.0F), grid.X(15.0F), grid.Y(4.0F),
            grid.X(15.4F), grid.Y(4.6F), grid.X(16.25F), grid.Y(5.95F));
        camera.AddBezier(grid.X(16.25F), grid.Y(5.95F), grid.X(16.6F), grid.Y(6.6F),
            grid.X(17.0F), grid.Y(7.0F), grid.X(18.0F), grid.Y(7.0F));
        camera.AddLine(grid.X(18.0F), grid.Y(7.0F), grid.X(20.0F), grid.Y(7.0F));
        camera.AddArc(grid.X(18.0F), grid.Y(7.0F), r, r, 270.0F, 90.0F);
        camera.AddLine(grid.X(22.0F), grid.Y(9.0F), grid.X(22.0F), grid.Y(18.0F));
        camera.AddArc(grid.X(18.0F), grid.Y(16.0F), r, r, 0.0F, 90.0F);
        camera.AddLine(grid.X(20.0F), grid.Y(20.0F), grid.X(4.0F), grid.Y(20.0F));
        camera.AddArc(grid.X(2.0F), grid.Y(16.0F), r, r, 90.0F, 90.0F);
        camera.AddLine(grid.X(2.0F), grid.Y(18.0F), grid.X(2.0F), grid.Y(9.0F));
        camera.AddArc(grid.X(2.0F), grid.Y(7.0F), r, r, 180.0F, 90.0F);
        camera.AddLine(grid.X(4.0F), grid.Y(7.0F), grid.X(6.0F), grid.Y(7.0F));
        camera.AddBezier(grid.X(6.0F), grid.Y(7.0F), grid.X(7.0F), grid.Y(7.0F),
            grid.X(7.4F), grid.Y(6.6F), grid.X(7.76F), grid.Y(5.95F));
        camera.AddBezier(grid.X(7.76F), grid.Y(5.95F), grid.X(8.6F), grid.Y(4.6F),
            grid.X(9.0F), grid.Y(4.0F), grid.X(10.0F), grid.Y(4.0F));
        camera.CloseFigure();
        graphics.DrawPath(&pen, &camera);

        // The lens is a separate circle, exactly as upstream draws it.
        graphics.DrawEllipse(&pen, grid.X(9.0F), grid.Y(10.0F), grid.size * 6.0F / 24.0F,
            grid.size * 6.0F / 24.0F);
    }
}

void Note::PaintColourDot(HDC dc, const RECT& box, bool active)
{
    if (!GdiplusReady())
    {
        return;
    }

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const Gdiplus::RectF plate(static_cast<float>(box.left), static_cast<float>(box.top),
        static_cast<float>(box.right - box.left), static_cast<float>(box.bottom - box.top));

    if (active)
    {
        Gdiplus::GraphicsPath hover;
        RoundedRect(hover, plate, ScaleF(6.0F));
        Gdiplus::SolidBrush wash(ToGdiplus(m_hoverColor));
        graphics.FillPath(&wash, &hover);
    }

    // Lucide "circle-dot" on the same 18-unit canvas as its two neighbours: an outlined r=9 ring
    // and a filled r=3 centre. Both are ink - a paper-coloured centre would vanish into the paper
    // it sits on and leave an empty ring, which is not the glyph in the style sample.
    const float gridSize = ScaleF(kIconDesign);
    const IconGrid grid{
        plate.X + (plate.Width - gridSize) / 2.0F,
        plate.Y + (plate.Height - gridSize) / 2.0F,
        gridSize,
    };

    const COLORREF ink = active ? kInk : m_iconColor;
    Gdiplus::Pen pen(ToGdiplus(ink), ScaleF(kIconStrokeDesign));
    Gdiplus::SolidBrush fill(ToGdiplus(ink));

    graphics.DrawEllipse(&pen, grid.X(3.0F), grid.Y(3.0F), grid.size * 18.0F / 24.0F,
        grid.size * 18.0F / 24.0F);
    graphics.FillEllipse(&fill, grid.X(9.0F), grid.Y(9.0F), grid.size * 6.0F / 24.0F,
        grid.size * 6.0F / 24.0F);
}

} // namespace wintools::app
