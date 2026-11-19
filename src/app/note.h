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

    /// Re-reads the colours of the paper index this note already has, after the palette itself was
    /// edited. Silently does nothing when the index is unchanged, which is why SetTheme cannot be
    /// used for this: same index, new colour is exactly the case it short-circuits on.
    void RepaintPaper();

    [[nodiscard]] bool Running() const
    {
        return m_window != nullptr;
    }

    [[nodiscard]] HWND Handle() const
    {
        return m_window;
    }

    /// Runs one screenshot capture and puts the result in the body at the caret.
    ///
    /// The camera button's path, reached from the global capture hotkey: the note is not
    /// necessarily the window with the focus when that key is pressed, so it is raised and focused
    /// first - a picture inserted into a note the user cannot see is indistinguishable from no
    /// picture at all, which is the bug this method exists to settle.
    void CaptureScreenshot();

    /// Reads the body back from disk. Called once after Create, so the window exists without
    /// blocking on the file during creation.
    void LoadBody();

    /// Writes the body out now if it is dirty.
    void Flush();

    /// First line or two of the body, folded to one line, for the closed-note history list.
    [[nodiscard]] std::wstring Preview() const;

    /// True when the sheet carries something worth keeping: a visible character or an embedded
    /// object (a pasted screenshot is U+FFFC and no text at all). Whitespace and empty paragraphs
    /// are not content - a note the user opened and closed without typing is not history.
    [[nodiscard]] bool HasContent() const;

    void Focus();

    /// Raised when the user finished dragging or resizing, so the index can be rewritten.
    Callback moved;

    /// Raised by the close button. The owner decides whether the note is deleted or just hidden.
    Callback closeRequested;

    /// Raised when the user picked another paper, after the window has already been repainted.
    Callback themeChanged;

    /// Raised after the delete button was confirmed, so the owner can drop the note for good -
    /// no archive copy, no retired body. The confirmation lives in the note: the click that
    /// answers it never reaches the owner, and the owner only ever sees a settled decision.
    Callback deleteRequested;

    /// Raised when the star toggled, after the header has been repainted.
    Callback starChanged;

    /// Sets the star without raising starChanged: the owner uses this when the note is built.
    void SetStarred(bool starred);

    [[nodiscard]] bool Starred() const
    {
        return m_starred;
    }

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    /// Device pixels from the 96 dpi numbers the layout is written in.
    [[nodiscard]] int Scale(int value) const;
    [[nodiscard]] float ScaleF(float value) const;

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

    /// The star button: an outline while plain, a filled yellow star once starred. Kept apart from
    /// PaintIconButton because its two states differ in fill as well as in ink.
    void PaintStarButton(HDC dc, const RECT& box, bool active);

    /// Runs the action a header button stands for. Called on the release, never on the press: two
    /// of the three open a window of their own, and that window would receive the release.
    void FireButton(int button);

    void CreateBody();
    void StyleBody();

    /// Puts the loaded text back on the note's body size. RTF carries a \fs per run, so a note
    /// written before the body grew a size would otherwise keep the smaller text next to anything
    /// typed today. Size only - colour and face are the document's business and are left alone.
    void MatchBodySize();

    /// Paragraph spacing only. Kept apart from StyleBody because it has to run again after a body
    /// has been streamed in - RTF carries its own \sl and would otherwise put the text back to
    /// single spacing - while the character format must not, or every pasted colour would be
    /// flattened to the default ink.
    void ApplyLineSpacing();

    void SaveBody();
    void InsertPicture(const std::vector<std::uint8_t>& png, int width, int height);

    /// Inserts the picture on the clipboard, if there is one in any format this note can read.
    ///
    /// Used when RichEdit's own paste came out empty-handed: it accepts the paste, finds nothing it
    /// can use, and says nothing, which reads to the user as a broken shortcut.
    bool PasteClipboardPicture();

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

    /// The line the body stops at, in client coordinates: everything below is paper the note paints
    /// and no child window covers.
    [[nodiscard]] int BodyBottom(int clientHeight) const;

    /// Repaints that strip. Anything that can let the system draw a frame over the note - the body
    /// taking the focus, a scroll, a frame change - has to call this, or the bottom margin keeps
    /// whatever the system left there.
    void InvalidateMargin();

    HINSTANCE m_instance{};
    HWND m_window{};
    HWND m_body{};
    HWND m_scroll{};
    std::wstring m_id;
    core::Rect m_rect{};

    HFONT m_fontBody{};
    HBRUSH m_paper{};

    /// The paper the note is written on, as resolved colours. Rebuilt from core::NoteThemeAt
    /// whenever SetTheme runs; GDI wants COLORREF, the palette is stored as plain 0xRRGGBB.
    int m_theme{ 0 };
    COLORREF m_paperColor{};
    COLORREF m_headerColor{};
    COLORREF m_ruleColor{};
    COLORREF m_mutedColor{};
    COLORREF m_iconColor{};
    COLORREF m_hoverColor{};

    int m_dpi{ 96 };

    int m_headerHeight{};
    int m_border{ 1 };

    /// Button boxes, recomputed on every size change. Index into this with ButtonAt.
    /// Physically ordered delete | dot | shot | close | star, laid out right to left.
    RECT m_delete{};
    RECT m_star{};
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

    /// Whether the note carries the yellow star. Drawn even when the rest of the chrome is hidden:
    /// a star the user has to hunt for is a star that stopped doing its job.
    bool m_starred{};

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
