#pragma once

#include <Windows.h>
#include <wrl/client.h>

namespace wintools::platform
{

/// COM smart pointer. WRL ships with the Windows SDK, so this stays dependency-free.
template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

/// RAII wrapper around CoInitializeEx/CoUninitialize.
///
/// Apartments are process-wide and reference counted, so one instance at the top of main() is
/// enough. Created as STA because IDesktopWallpaper is an STA object and marshalling calls into
/// it from an MTA thread would need a message pump we do not have.
class ComApartment
{
public:
    ComApartment();
    ~ComApartment();

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ComApartment(ComApartment&&) = delete;
    ComApartment& operator=(ComApartment&&) = delete;

    /// False when COM could not be initialised at all; the process can keep running but every
    /// shell call will fail, so callers normally bail out early.
    [[nodiscard]] bool IsValid() const
    {
        return m_shouldUninitialize;
    }

private:
    bool m_shouldUninitialize{};
};

} // namespace wintools::platform
