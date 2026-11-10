#include "platform/win/com.h"

#include "util/log.h"
#include "util/text.h"

namespace wintools::platform
{

ComApartment::ComApartment()
{
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (SUCCEEDED(hr))
    {
        // S_FALSE means the apartment already existed; we still own a reference either way.
        m_shouldUninitialize = true;
        return;
    }

    if (hr == RPC_E_CHANGED_MODE)
    {
        // Somebody else initialised as MTA. Shell interfaces still work for the calls we make,
        // so this is a warning rather than a failure.
        util::LogWarn(L"COM already initialised with a different apartment model; continuing");
        m_shouldUninitialize = false;
        return;
    }

    util::LogError(L"CoInitializeEx failed: {}", util::FormatHresult(hr));
}

ComApartment::~ComApartment()
{
    if (m_shouldUninitialize)
    {
        ::CoUninitialize();
    }
}

} // namespace wintools::platform
