#pragma once

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "app/note_palette.h"
#include "core/slice.h"

namespace wintools::app
{

/// One sticky note: a topmost window with a rich-text body that saves itself.
///
/// The window is drawn by hand - the bar, the icons, the scrollbar - while the body is a real
/// RichEdit control, because re-implementing selection, wrapping, undo, IME and clipboard handling
/// on top of GDI is a project of its own and Microsoft ships a working one.
///
/// The chrome is deliberately quiet: with the pointer outside the note there is nothing but paper
/// and one line of save time, and the chip with the three icons only appears once the pointer comes
/// in or the caret is in the text. The geometry never changes between the two states, so nothing
/// reflows and the text does not jump when the chip shows up.
///
/// Persistence is deliberately split: geometry and paper colour live in the note index (the
/// service owns it), and the text lives in its own RTF file next to it, written on a short debounce
/// after the user stops typing. A crash therefore costs at most the last keystroke or two, and
/// never the note.
class Note
{
public:
    using Callback = std::function<void(Note&)>;

    Note() = default;
    ~Note();

    Note(const Note&) = delete;
    Note& operator=(const Note&) = delete;
    Note(Note&&) = delete;
    Note& operator=(Note&&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance, std::wstring id, const core::Rect& rect,
        int theme);

    /// Saves the body one last time and tears the window down.
    void Destroy();

    void SetRect(const core::Rect& rect);

    [[nodiscard]] const core::Rect& Rect() const
    {
        return m_rect;
    }

    [[nodiscard]] const std::wstring& Id() const
    {
        return m_id;
    }

    [[nodiscard]] int Theme() const
    {
        return m_theme;
    }

    /// Repaints the note on another paper. Raised through themeChanged so the index can record it.
    void SetTheme(int theme);

    [[nodiscard]] bool Running() const
    {
        return m_window != nullptr;
    }

    /// Reads the body back from disk. Called once after Create, so the window exists without
    /// blocking on the file during creation.
    void LoadBody();

    /// Writes the body out now if it is dirty.
    void Flush();

    /// First line or two of the body, folded to one line, for the closed-note history list.
    [[nodiscard]] std::wstring Preview() const;

    void Focus();

    /// Raised when the user finished dragging or resizing, so the index can be rewritten.
    Callback moved;

    /// Raised by the close button. The owner decides whether the note is deleted or just hidden.
    Callback closeRequested;

    /// Raised when the user picked another paper, after the window has already been repainted.
    Callback themeChanged;

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    /// Device pixels from the 96 dpi numbers the layout is written in.
    [[nodiscard]] int Scale(int value) const;

    void Metrics();
    void LayoutChildren();

    /// Resolves the palette and rebuilds the brushes. The window repaints afterwards, but the
    /// owner is not told: SetTheme is the one that raises themeChanged.
    void ApplyTheme(int theme);

    void Paint();
    void PaintChrome(HDC dc);
    void PaintTimestamp(HDC dc);
    void PaintIconButton(HDC dc, const RECT& box, int icon, bool active);
    void PaintColourDot(HDC dc, const RECT& box, bool active);

    /// Runs the action a header button stands for. Called on the release, never on the press: two
    /// of the three open a window of their own, and that window would receive the release.
    void FireButton(int button);

    void CreateBody();
    void StyleBody();

    /// Paragraph spacing only. Kept apart from StyleBody because it has to run again after a body
    /// has been streamed in - RTF carries its own \sl and would otherwise put the text back to
    /// single spacing - while the character format must not, or every pasted colour would be
    /// flattened to the default ink.
    void ApplyLineSpacing();

    void SaveBody();
    void InsertPicture(const std::vector<std::uint8_t>& png, int width, int height);
    void TakeScreenshot();

    /// "Write something…" on an empty note. Drawn by the note rather than by RichEdit, which has
    /// no notion of a placeholder.
    void DrawPlaceholder(HWND body);

    /// RichEdit holds a pasted screenshot as a single object-replacement character. Dragging one is
    /// handled here rather than by the control: see MovePicture.
    [[nodiscard]] int PictureCharAt(POINT client) const;
    [[nodiscard]] bool BodyCharIsPicture(int position) const;
    [[nodiscard]] wchar_t BodyCharAt(int position) const;
    [[nodiscard]] int CaretAt(POINT client) const;
    void MovePicture(int from, int to);

    static LRESULT CALLBACK BodyProc(HWND body, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleBodyMessage(
        HWND body, UINT message, WPARAM wparam, LPARAM lparam, WNDPROC previous);

    /// The 3 px scrollbar is a window of its own rather than paint on the note, because it has to
    /// sit above the body: the body is a child window and would draw over anything the note put in
    /// the same place.
    void CreateScrollbar();
    void UpdateScrollbar();
    [[nodiscard]] bool ScrollMetrics(int& trackTop, int& thumbTop, int& thumbHeight) const;
    void ScrollToPixel(int pixel);
    void ScrollbarPage(bool down);
    static LRESULT CALLBACK ScrollProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleScrollMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    void OpenPalette();

    [[nodiscard]] LRESULT HitTest(POINT screen) const;
    [[nodiscard]] int ButtonAt(POINT client) const;

    /// Quiet chrome bookkeeping: the chip is up while the pointer is anywhere over the note or the
    /// caret is in the text, and gone once both are. The save time is drawn either way.
    void PointerEntered();
    void RefreshPointer();
    void TrackPointer(HWND target, bool nonClient);
    [[nodiscard]] bool ChromeVisible() const
    {
        return m_pointerInside || m_bodyFocused;
    }

    void MarkDirty();
    void MarkSaved();
    void RefreshStatus();
    void InvalidateHeader();

    HINSTANCE m_instance{};
    HWND m_window{};
    HWND m_body{};
    HWND m_scroll{};
    HWND m_footer{};   // paper-coloured sheet over the bottom margin
    std::wstring m_id;
    core::Rect m_rect{};

    HFONT m_fontSmall{};
    HFONT m_fontBody{};
    HBRUSH m_paper{};
    HBRUSH m_hoverFill{};

    /// The paper the note is written on, as resolved colours. Rebuilt from core::NoteThemeAt
    /// whenever SetTheme runs; GDI wants COLORREF, the palette is stored as plain 0xRRGGBB.
    int m_theme{ 0 };
    COLORREF m_paperColor{};
    COLORREF m_headerColor{};
    COLORREF m_ruleColor{};
    COLORREF m_mutedColor{};
    COLORREF m_iconColor{};

    int m_dpi{ 96 };

    int m_headerHeight{};
    int m_border{ 1 };

    /// Button boxes, recomputed on every size change. Index into this with ButtonAt.
    RECT m_shot{};
    RECT m_close{};
    RECT m_dot{};

    /// Which header button the mouse is over, and whether the pointer is inside the note at all.
    int m_hover{ -1 };
    bool m_pointerInside{};
    bool m_bodyFocused{};

    /// The button a press landed on, held until the release. Actions fire on release only: the
    /// colour palette and the screenshot overlay are windows of their own, and one of them appeared
    /// in time to swallow the release that was meant for the note.
    int m_pressedButton{ -1 };

    /// Set while the pointer is being dragged on the scrollbar: where inside the thumb it grabbed.
    int m_scrollGrab{ -1 };
    bool m_scrollHot{};
    bool m_scrollVisible{};

    PalettePopup m_palette;

    bool m_dirty{};

    /// The RTF bytes this note was loaded from. A load raises the same EN_UPDATE a keystroke does,
    /// and that notification can arrive after the guard has been lifted; comparing bytes is what
    /// stops every app start from rewriting every untouched note.
    std::string m_loadedBytes;
    SYSTEMTIME m_savedAt{};

    /// True while the body is being filled or restyled. RichEdit reports those as EN_UPDATE too,
    /// and a note that has just loaded is not a note the user edited.
    bool m_suspendNotify{};

    /// The body's window procedure is wrapped so a press on a picture can be taken over before
    /// RichEdit starts its own drag. The hook lives in a window property, not in GWLP_USERDATA,
    /// because the control owns that slot.
    struct BodyHook
    {
        Note* owner{};
        WNDPROC previous{};
    };

    BodyHook m_bodyHook{};

    /// The picture under the mouse while it is being dragged, and the character the caret sits on.
    int m_dragFrom{ -1 };
    int m_dragTo{ -1 };
    POINT m_dragOrigin{};
    bool m_dragMoved{};
};

} // namespace wintools::app
