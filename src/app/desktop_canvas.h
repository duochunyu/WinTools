#pragma once

#include <Windows.h>

#include "core/calendar_face.h"
#include "core/canvas.h"

namespace wintools::app
{

/// The desktop widget: a square, layered window parked under every other window.
///
/// This is the "B" approach from docs/DYNAMIC-DESKTOP.md: no undocumented shell structures, no
/// parenting into WorkerW, no video pipeline. The window is an ordinary top-level window that
/// keeps placing itself directly above the desktop host - under every window the user opened,
/// over the wallpaper - which is why it survives shell updates.
///
/// Two things make that stable rather than flickery:
///   - the placement is a position, not a remembered state: repeating it is a no-op, so the
///     widget can re-assert it on a timer without ever fighting itself (see §12.1);
///   - an EVENT_SYSTEM_FOREGROUND hook re-asserts it the moment anything else is activated,
///     because that is when the shell is most likely to have pushed it down.
///
/// The window is click-through on purpose. Desktop widgets that swallow clicks are the reason
/// people uninstall them; interaction, when it comes, will be opted into per widget.
class DesktopCanvas
{
public:
    DesktopCanvas() = default;
    ~DesktopCanvas();

    DesktopCanvas(const DesktopCanvas&) = delete;
    DesktopCanvas& operator=(const DesktopCanvas&) = delete;
    DesktopCanvas(DesktopCanvas&&) = delete;
    DesktopCanvas& operator=(DesktopCanvas&&) = delete;

    [[nodiscard]] bool Start(HINSTANCE instance, const core::CanvasRect& rect);
    void Stop();

    /// Moves and resizes. The caller is responsible for keeping it on a display.
    void SetRect(const core::CanvasRect& rect);

    /// Whether the second hand sweeps or steps. Sweeping is the default; stepping is worth having
    /// because it is what a quartz watch does, and because it is a tenth of the cost.
    void SetSweep(bool sweep);

    /// Puts the window directly above the desktop host, and therefore under everything else.
    ///
    /// Public because the WinEvent hook that drives it is a plain callback: hooks carry no user
    /// data, so the hook has to reach the instance through a file-scope pointer and call this.
    void Restack();

    [[nodiscard]] bool Running() const
    {
        return m_window != nullptr;
    }

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void CreateSurface();
    void DestroySurface();

    /// The static half of the frame: the disc, the dial, the yin-yang hub and the text, already
    /// premultiplied.
    ///
    /// Kept in its own bitmap because a sweeping second hand wants ten redraws a second, and
    /// re-running the text shaping plus the per-pixel alpha pass that often would cost more than
    /// everything else in this program put together. Rebuilt only when the text changes.
    void CreateBase(const core::CalendarFace& face);
    void DestroyBase();

    /// Rebuilds the base, then presents one frame. Called when the face text changed.
    void RenderAll();

    /// Copies the base and draws the hands over it. Called for every frame of the clock.
    void RenderFrame();

    /// One timer beat: re-asserts the widget's place, then decides how much repainting is worth
    /// doing.
    void Tick();

    /// Whether any window other than the shell covers the widget. A covered widget steps the
    /// second hand once a second instead of sweeping, because nobody is looking at it.
    [[nodiscard]] bool IsObstructed() const;

    void ApplyBounds();

    /// Rebuilds the fonts after the widget changed size.
    void RecreateFonts();

    void DestroyFonts();

    HWND m_window{};
    HINSTANCE m_instance{};
    core::CanvasRect m_rect{};

    HDC m_memory{};
    HBITMAP m_bitmap{};
    HGDIOBJ m_oldBitmap{};
    void* m_bits{};
    int m_surface{};

    /// The cached static half of the frame. No DC: it is only ever a memcpy source.
    HBITMAP m_baseBitmap{};
    void* m_baseBits{};
    int m_base{};

    HFONT m_fontHeadline{};
    HFONT m_fontDay{};
    HFONT m_fontLunar{};
    HFONT m_fontFoot{};
    int m_fontSize{};

    HWINEVENTHOOK m_hook{};

    /// Whether a window covers the widget, refreshed a few times a second rather than per frame.
    bool m_obstructed{};
    ULONGLONG m_probedAt{};

    /// True when the second hand sweeps instead of stepping.
    bool m_sweep{ true };

    /// Last minute that was drawn, so the base is only rebuilt when the text actually changed.
    int m_drawnYear{};
    int m_drawnMonth{};
    int m_drawnDay{};
    int m_drawnHour{};
    int m_drawnMinute{};
    int m_drawnSecond{ -1 };
};

} // namespace wintools::app
