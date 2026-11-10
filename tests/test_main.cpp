#include "test_framework.h"

#include <Windows.h>

#include <crtdbg.h>
#include <objbase.h>

int main()
{
    // A contract violation in the standard library surfaces as a modal assertion dialog, which in
    // an unattended run just means a hung process with no output. Send the report to stderr and
    // let the process die, so the failure lands in the captured log instead of on someone's screen.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // The preview tests decode real images through WIC, and WIC is a COM component: without an
    // apartment CoCreateInstance fails and every decode silently returns nothing. The application
    // initialises COM the same way at startup, so this keeps the tests honest about the real
    // environment rather than papering over a missing dependency.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com))
    {
        std::printf("CoInitializeEx failed: 0x%08lx\n", static_cast<unsigned long>(com));
        return 2;
    }

    const int result = wintools::test::RunAll();

    // Deliberately not CoUninitialize: see platform/win/wic.cpp for why the WIC factory outlives
    // the apartment in this codebase. The process is about to exit anyway.
    return result;
}
