#pragma once

#include <Windows.h>

#include <functional>

namespace wintools::app
{

/// The six papers, offered as swatches right under the note's colour dot.
///
/// A child window of the note rather than a popup with the mouse capture. The capture version was
/// the reason the note's own buttons stopped responding: while the palette held it, every click on
/// the note went to the palette instead - sometimes not until the next click, because the message
/// sat in the queue - so the note never saw a press and nothing on it could be hit. A child window
/// needs no capture at all: clicks the swatches do not take simply fall through to the note, which
/// hides the palette and gets on with what it was asked to do.
///
/// It is clipped to the note, which is where the swatches belong anyway - they are papers, and the
/// paper is the note.
class PalettePopup
{
public:
    using Picked = std::function<void(int)>;

    PalettePopup() = default;
    ~PalettePopup();

    PalettePopup(const PalettePopup&) = delete;
    PalettePopup& operator=(const PalettePopup&) = delete;
    PalettePopup(PalettePopup&&) = delete;
    PalettePopup& operator=(PalettePopup&&) = delete;

    /// `anchor` is in the parent's client coordinates: the bottom-left corner of the dot that
    /// opened it. `backdrop` is the note's header tone, so the chip belongs to the paper it sits on.
    void Show(HWND parent, int dpi, POINT anchor, COLORREF backdrop, int current, Picked picked);

    void Hide();

    /// Raised once the palette is gone. The note uses it to repaint the dot it came from.
    std::function<void()> closed;

    [[nodiscard]] bool Visible() const
    {
        return m_window != nullptr;
    }

    /// True when `window` is the palette itself. The note asks before deciding that the pointer has
    /// left it: moving onto the swatches is moving onto part of the note.
    [[nodiscard]] bool Owns(HWND window) const
    {
        return m_window != nullptr && window == m_window;
    }

    /// True when the point, in the parent's client coordinates, is inside the palette. The note
    /// asks before hiding: a press the palette owns must not also count as a press on the note.
    [[nodiscard]] bool Contains(POINT parentClient) const;

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    void Paint(HDC dc) const;
    [[nodiscard]] RECT SwatchRect(int index) const;
    [[nodiscard]] int SwatchAt(POINT client) const;
    [[nodiscard]] int Scale(int value) const;

    HWND m_window{};
    HINSTANCE m_instance{};
    int m_dpi{ 96 };
    int m_current{};
    int m_hover{ -1 };
    COLORREF m_backdrop{ RGB(0xFF, 0xFF, 0xFF) };
    Picked m_picked;
};

} // namespace wintools::app
