#include "platform/win/wic.h"

#include <mutex>

#include "util/log.h"
#include "util/text.h"

namespace wintools::platform
{

IWICImagingFactory* GetWicFactory()
{
    static std::once_flag once;

    // Deliberately a raw pointer that is never released.
    //
    // A static ComPtr here crashes the process on exit with a very confusing 0xC0000005: local
    // statics are destroyed after wWinMain returns, which is after the ComApartment destructor
    // has already called CoUninitialize, and releasing a COM object on an uninitialised apartment
    // is a different kind of illegal. The factory lives exactly as long as the process needs it,
    // so leaking it costs a single object and removes an entire class of shutdown ordering bug.
    static IWICImagingFactory* factory = nullptr;

    std::call_once(once, [] {
        // WICImagingFactory2 adds CreateImageEncoder, which we do not need, but it is the
        // recommended class id on Windows 10 and up and costs nothing.
        IWICImagingFactory* created = nullptr;
        HRESULT hr = ::CoCreateInstance(
            CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&created));

        if (FAILED(hr))
        {
            created = nullptr;
            hr = ::CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&created));
        }

        if (FAILED(hr))
        {
            util::LogError(L"could not create the WIC imaging factory: {}", util::FormatHresult(hr));
            return;
        }

        factory = created;
    });

    return factory;
}

} // namespace wintools::platform
