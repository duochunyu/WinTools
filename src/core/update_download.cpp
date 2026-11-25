#include "core/update_download.h"

#include <algorithm>
#include <format>
#include <system_error>

#include "util/log.h"
#include "util/path.h"

namespace wintools::core
{
namespace
{

void RemoveQuietly(const std::filesystem::path& file)
{
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}

} // namespace

std::vector<std::wstring> DownloadUrls(const UpdateFeed& feed)
{
    std::vector<std::wstring> urls;
    for (const std::wstring& candidate : { feed.url, feed.urlGithub })
    {
        if (candidate.empty())
        {
            continue;
        }
        if (std::find(urls.begin(), urls.end(), candidate) != urls.end())
        {
            continue;
        }
        urls.push_back(candidate);
    }
    return urls;
}

std::filesystem::path UpdateFilePath(const UpdateFeed& feed)
{
    return util::UpdateFolder() / std::format(L"WinTools-{}.exe.new", feed.version.Text());
}

DownloadResult DownloadUpdate(const FetchFile& fetch, const HashFile& hash,
    const UpdateFeed& feed, const std::filesystem::path& target)
{
    DownloadResult result;
    result.path = target;

    // A previous attempt may have left a partial file with the same version. Never let it be
    // mistaken for a completed download.
    RemoveQuietly(target);

    const std::vector<std::wstring> urls = DownloadUrls(feed);
    if (urls.empty())
    {
        result.message = L"feed has no download URL";
        return result;
    }
    if (!fetch || !hash)
    {
        result.message = L"download transport is missing";
        return result;
    }

    for (const std::wstring& url : urls)
    {
        std::string error;
        if (!fetch(url, target, error))
        {
            const std::wstring reason{ error.begin(), error.end() };
            util::LogWarn(L"update download from {} failed: {}", url, reason);
            RemoveQuietly(target);
            result.message = std::wstring{ error.begin(), error.end() };
            continue;
        }

        std::error_code sizeError;
        const auto size = std::filesystem::file_size(target, sizeError);
        if (sizeError || size != feed.size)
        {
            util::LogWarn(L"update size mismatch: got {} bytes, expected {}", size, feed.size);
            RemoveQuietly(target);
            result.message = std::format(L"size {} != {}", size, feed.size);
            continue;
        }

        std::string actual;
        const bool hashed = hash(target, actual);
        const std::wstring actualWide{ actual.begin(), actual.end() };
        if (!hashed || actualWide != feed.sha256)
        {
            util::LogWarn(L"update SHA-256 mismatch: got {}, expected {}", actualWide,
                feed.sha256);
            RemoveQuietly(target);
            result.message = L"SHA-256 mismatch";
            continue;
        }

        result.ok = true;
        result.message = L"ok";
        return result;
    }

    if (result.message.empty())
    {
        result.message = L"no usable download mirror";
    }
    return result;
}

} // namespace wintools::core
