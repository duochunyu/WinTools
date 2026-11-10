#include "core/display_watcher.h"

#include "util/log.h"

namespace wintools::core
{

namespace
{

constexpr wchar_t kWindowClass[] = L"WinTools.DisplayWatcher.7c4a1b93";
constexpr UINT_PTR kDebounceTimerId = 1;

} // namespace

DisplayWatcher::DisplayWatcher(Callback onSettled, int debounceMilliseconds)
    : m_callback(std::move(onSettled))
    , m_debounceMilliseconds(static_cast<UINT>(debounceMilliseconds < 200 ? 200 : debounceMilliseconds))
    , m_instance(::GetModuleHandleW(nullptr))
{
}

DisplayWatcher::~DisplayWatcher()
{
    Stop();
}

bool DisplayWatcher::Start()
{
    if (m_running)
    {
        return true;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &DisplayWatcher::WndProc;
    windowClass.hInstance = m_instance;
    windowClass.lpszClassName = kWindowClass;

    m_class = ::RegisterClassExW(&windowClass);
    if (m_class == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        util::LogError(L"RegisterClassEx failed for the display watcher");
        return false;
    }

    // A plain hidden window, not HWND_MESSAGE: message-only windows do not receive broadcast
    // messages, and WM_DISPLAYCHANGE is a broadcast.
    m_window = ::CreateWindowExW(
        0,
        kWindowClass,
        L"WinTools display watcher",
        WS_POPUP,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        m_instance,
        this);

    if (m_window == nullptr)
    {
        util::LogError(L"CreateWindowEx failed for the display watcher");
        ::UnregisterClassW(kWindowClass, m_instance);
        m_class = 0;
        return false;
    }

    m_running = true;
    util::LogInfo(L"display watcher started ({} ms debounce)", m_debounceMilliseconds);
    return true;
}

void DisplayWatcher::Stop()
{
    if (m_window != nullptr)
    {
        ::KillTimer(m_window, kDebounceTimerId);
        ::DestroyWindow(m_window);
        m_window = nullptr;
    }

    if (m_class != 0)
    {
        ::UnregisterClassW(kWindowClass, m_instance);
        m_class = 0;
    }

    m_running = false;
}

LRESULT CALLBACK DisplayWatcher::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<DisplayWatcher*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_window = window;
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    auto* self = reinterpret_cast<DisplayWatcher*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr)
    {
        return self->HandleMessage(message, wparam, lparam);
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT DisplayWatcher::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_DISPLAYCHANGE:
            // Every change restarts the timer, so the callback fires once after the churn stops
            // rather than once per panel.
            ::KillTimer(m_window, kDebounceTimerId);
            ::SetTimer(m_window, kDebounceTimerId, m_debounceMilliseconds, nullptr);
            return 0;

        case WM_TIMER:
            if (wparam == kDebounceTimerId)
            {
                ::KillTimer(m_window, kDebounceTimerId);
                util::LogInfo(L"displays settled, running the restore chain");
                if (m_callback)
                {
                    m_callback();
                }
                return 0;
            }
            break;

        default:
            break;
    }

    return ::DefWindowProcW(m_window, message, wparam, lparam);
}

} // namespace wintools::core
