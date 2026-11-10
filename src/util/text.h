#pragma once

#include <string>
#include <string_view>

namespace wintools::util
{

/// UTF-8 <-> UTF-16 conversion. All public APIs in this project take wide strings, because
/// every Win32 and WIC entry point does; the narrow form only exists for logs on disk and for
/// interop with tooling.
std::string ToUtf8(std::wstring_view text);
std::wstring FromUtf8(std::string_view text);

/// Formats an HRESULT as "0xXXXXXXXX (message)" so a log line is readable without a lookup.
std::wstring FormatHresult(long hr);

} // namespace wintools::util
