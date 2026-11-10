#pragma once

#include <Windows.h>

#include <functional>

namespace wintools::core
{

/// Watches for display topology changes and calls back once things have settled.
///
/// WM_DISPLAYCHANGE is broadcast to every top-level window, so a hidden one is enough - no
/// window station hooks, no polling. The debounce matters more than the detection: a
/// power-cycled monitor comes back alone, then the dock's other panels follow, so the first
/// message describes a topology that is still changing. Acting on it would place windows for a
/// layout that is about to be different again.
class DisplayWatcher
{
public:
    using Callback = std::function<void()>;

    /// milliseconds must be at least 200; the intent is "after the last change", not "on every
    /// message".
    DisplayWatcher(Callback onSettled, int debounceMilliseconds);
    ~DisplayWatcher();

    DisplayWatcher(const DisplayWatcher&) = delete;
    DisplayWatcher& operator=(const DisplayWatcher&) = delete;
    DisplayWatcher(DisplayWatcher&&) = delete;
    DisplayWatcher& operator=(DisplayWatcher&&) = delete;

    [[nodiscard]] bool Start();
    void Stop();

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    Callback m_callback;
    UINT m_debounceMilliseconds{};
    HWND m_window{};
    HINSTANCE m_instance{};
    ATOM m_class{};
    bool m_running{};
};

} // namespace wintools::core
