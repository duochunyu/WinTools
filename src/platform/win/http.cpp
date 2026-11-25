#include "platform/win/http.h"

#include <Windows.h>
#include <winhttp.h>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "util/log.h"

namespace wintools::platform::win
{
namespace
{

// A tiny RAII handle: every early return below must close the WinHTTP handles, and there are
// enough failure points that hand-written cleanup would be a bug factory.
struct Handle
{
    HINTERNET value{};
    ~Handle()
    {
        if (value != nullptr)
        {
            ::WinHttpCloseHandle(value);
        }
    }
    Handle() = default;
    explicit Handle(HINTERNET handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::wstring LastErrorText(DWORD code)
{
    return std::format(L"WinHTTP error {}", code);
}

struct PreparedRequest
{
    Handle session;
    Handle connect;
    Handle request;
    unsigned long status{};
};

/// Parses, opens and sends a GET, stopping after the status line. Both the feed fetch and the
/// installer download go through this so the scheme check, proxy choice and timeouts cannot
/// drift between them.
bool BeginRequest(
    std::wstring_view url, unsigned timeoutMs, PreparedRequest& prepared, std::wstring& error)
{
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = 1;
    parts.dwUrlPathLength = 1;
    parts.dwExtraInfoLength = 1;
    std::wstring hostBuffer(256, L'\0');
    std::wstring pathBuffer(1024, L'\0');
    parts.lpszHostName = hostBuffer.data();
    parts.dwHostNameLength = static_cast<DWORD>(hostBuffer.size());
    parts.lpszUrlPath = pathBuffer.data();
    parts.dwUrlPathLength = static_cast<DWORD>(pathBuffer.size());

    if (!::WinHttpCrackUrl(std::wstring{ url }.c_str(), 0, 0, &parts))
    {
        error = LastErrorText(::GetLastError());
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)
    {
        error = L"update URL must be http or https";
        return false;
    }

    prepared.session.value = ::WinHttpOpen(L"WinTools-Update/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (prepared.session.value == nullptr)
    {
        error = LastErrorText(::GetLastError());
        return false;
    }

    const std::wstring host{ parts.lpszHostName, parts.dwHostNameLength };
    prepared.connect.value =
        ::WinHttpConnect(prepared.session.value, host.c_str(), parts.nPort, 0);
    if (prepared.connect.value == nullptr)
    {
        error = LastErrorText(::GetLastError());
        return false;
    }

    const std::wstring path{ parts.lpszUrlPath, parts.dwUrlPathLength };
    prepared.request.value = ::WinHttpOpenRequest(prepared.connect.value, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (prepared.request.value == nullptr)
    {
        error = LastErrorText(::GetLastError());
        return false;
    }

    ::WinHttpSetTimeouts(prepared.request.value, static_cast<int>(timeoutMs),
        static_cast<int>(timeoutMs), static_cast<int>(timeoutMs), static_cast<int>(timeoutMs));

    if (!::WinHttpSendRequest(prepared.request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !::WinHttpReceiveResponse(prepared.request.value, nullptr))
    {
        error = LastErrorText(::GetLastError());
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    ::WinHttpQueryHeaders(prepared.request.value,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    prepared.status = status;
    if (status != 200)
    {
        error = std::format(L"HTTP {}", status);
        return false;
    }
    return true;
}

} // namespace

HttpResult HttpGetText(std::wstring_view url, unsigned timeoutMs)
{
    HttpResult result;
    PreparedRequest request;
    if (!BeginRequest(url, timeoutMs, request, result.error))
    {
        result.status = request.status;
        return result;
    }
    result.status = request.status;

    // Cap the body: an update feed is a few hundred bytes, so a multi-megabyte response is a
    // broken or hostile endpoint and must never be buffered on the UI worker.
    constexpr DWORD kMaxBytes = 64 * 1024;
    std::string body;
    for (;;)
    {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.request.value, &available))
        {
            result.error = LastErrorText(::GetLastError());
            return result;
        }
        if (available == 0)
        {
            break;
        }
        if (body.size() + available > kMaxBytes)
        {
            result.error = L"update feed is too large";
            return result;
        }

        const size_t previous = body.size();
        body.resize(previous + available);
        DWORD read = 0;
        if (!::WinHttpReadData(request.request.value, body.data() + previous, available, &read))
        {
            result.error = LastErrorText(::GetLastError());
            return result;
        }
        body.resize(previous + read);
        if (read == 0)
        {
            break;
        }
    }

    result.ok = true;
    result.body = std::move(body);
    return result;
}

DownloadOutcome HttpDownloadToFile(
    std::wstring_view url, const std::filesystem::path& target, std::uint64_t maxBytes,
    unsigned timeoutMs)
{
    DownloadOutcome result;
    PreparedRequest request;
    if (!BeginRequest(url, timeoutMs, request, result.error))
    {
        result.status = request.status;
        return result;
    }
    result.status = request.status;

    std::error_code errorCode;
    std::filesystem::create_directories(target.parent_path(), errorCode);
    if (errorCode)
    {
        result.error = L"cannot create the update folder";
        return result;
    }

    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        result.error = L"cannot open the update file for writing";
        return result;
    }

    std::array<char, 64 * 1024> buffer{};
    for (;;)
    {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.request.value, &available))
        {
            result.error = LastErrorText(::GetLastError());
            return result;
        }
        if (available == 0)
        {
            break;
        }

        const auto wanted = static_cast<std::uint64_t>(available);
        if (result.bytes + wanted > maxBytes)
        {
            result.error = L"update package is too large";
            return result;
        }

        DWORD read = 0;
        if (!::WinHttpReadData(request.request.value, buffer.data(),
                static_cast<DWORD>(buffer.size()), &read))
        {
            result.error = LastErrorText(::GetLastError());
            return result;
        }
        if (read == 0)
        {
            break;
        }
        output.write(buffer.data(), static_cast<std::streamsize>(read));
        if (!output)
        {
            result.error = L"cannot write the update file";
            return result;
        }
        result.bytes += read;
    }

    output.flush();
    if (!output)
    {
        result.error = L"cannot flush the update file";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace wintools::platform::win
