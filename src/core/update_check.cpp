#include "core/update_check.h"

#include "util/log.h"
#include "util/text.h"

namespace wintools::core
{
namespace
{

// The feed is a plain file in the main branch of each repository. A fixed path keeps the client
// from having to know the latest tag before it can discover that a newer tag exists.
constexpr std::wstring_view kGiteeFeed =
    L"https://gitee.com/YingKeWangLuo_admin/main/raw/main/update.txt";
constexpr std::wstring_view kGithubFeed =
    L"https://raw.githubusercontent.com/duochunyu/WinTools/main/update.txt";

} // namespace

std::vector<std::wstring> SourcesFor(std::wstring_view custom)
{
    std::vector<std::wstring> sources;
    if (!custom.empty())
    {
        sources.emplace_back(custom);
    }
    sources.emplace_back(kGiteeFeed);
    sources.emplace_back(kGithubFeed);
    return sources;
}

UpdateCheck CheckForUpdate(
    const FetchText& fetch, std::wstring_view customSource, const AppVersion& current)
{
    UpdateCheck result;
    // Whether any source produced a feed at all: tells "everything said up to date" apart from
    // "nothing answered", which is the difference between a quiet success and a visible error.
    bool reachedASource = false;
    for (const std::wstring& source : SourcesFor(customSource))
    {
        std::string body;
        bool fetched = false;
        try
        {
            fetched = fetch(source, body);
        }
        catch (...)
        {
            fetched = false;
        }
        if (!fetched)
        {
            continue;
        }

        const auto feed = ParseUpdateFeed(util::FromUtf8(body));
        if (!feed)
        {
            util::LogWarn(L"update feed at this source is malformed; trying the next one");
            continue;
        }

        reachedASource = true;
        if (Compare(feed->version, current) <= 0)
        {
            // A mirror can lag the primary by a release or more, and this list is ordered
            // Gitee-first for mainland users. Stopping at the first source that parses would let
            // a stale mirror hide a version the fallback already publishes: on 2026-11-25 the
            // Gitee feed still said 0.3.0 while GitHub said 0.3.1, and every Gitee-first client
            // concluded it was up to date. Try the remaining sources, and only call it "up to
            // date" once none of them is newer.
            continue;
        }

        result.feed = feed;
        result.available = true;
        result.message = std::format(L"有新版本 {}", feed->version.Text());
        return result;
    }

    if (reachedASource)
    {
        result.message = L"已是最新版本";
        return result;
    }

    result.error = true;
    result.message = L"检查更新失败：没有可用的更新源";
    return result;
}

} // namespace wintools::core
