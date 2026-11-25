#include "test_framework.h"

#include "core/update_check.h"
#include "util/text.h"

#include <format>
#include <string>

using wintools::core::AppVersion;
using wintools::core::CheckForUpdate;
using wintools::core::SourcesFor;

namespace
{
constexpr std::wstring_view kHash =
    L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string Feed(std::wstring_view version)
{
    return wintools::util::ToUtf8(std::format(
        L"schema=1\nversion={}\nsize=100\nsha256={}\nnotes.zh=测试\n", version, kHash));
}

} // namespace

WINTOOLS_TEST(Sources_CustomComesFirstAndDefaultsFollow)
{
    const auto sources = SourcesFor(L"http://localhost/feed.txt");
    CHECK_EQ(sources.size(), 3u);
    CHECK_EQ(sources.front(), std::wstring(L"http://localhost/feed.txt"));
    CHECK(sources[1].find(L"gitee.com") != std::wstring::npos);
    CHECK(sources[2].find(L"githubusercontent.com") != std::wstring::npos);
}

WINTOOLS_TEST(Check_ReportsNewerVersion)
{
    const auto current = AppVersion::Parse(L"0.2.0");
    CHECK(current.has_value());

    const auto result = CheckForUpdate(
        [](std::wstring_view, std::string& body)
        {
            body = Feed(L"0.3.0");
            return true;
        },
        L"", *current);

    CHECK(result.available);
    CHECK(!result.error);
    CHECK(result.feed.has_value());
    CHECK_EQ(result.feed->version.Text(), std::wstring(L"0.3.0"));
}

WINTOOLS_TEST(Check_EqualVersionIsNotAnUpdate)
{
    const auto current = AppVersion::Parse(L"0.2.0");
    CHECK(current.has_value());

    const auto result = CheckForUpdate(
        [](std::wstring_view, std::string& body)
        {
            body = Feed(L"0.2.0");
            return true;
        },
        L"", *current);

    CHECK(!result.available);
    CHECK(!result.error);
    // A present feed is how the panel decides to offer an install, so an up-to-date check must
    // not hand one back. Missing this assertion is what let 0.3.0 offer to update to 0.3.0.
    CHECK(!result.feed.has_value());
}

WINTOOLS_TEST(Check_OlderVersionIsNotAnUpdate)
{
    const auto current = AppVersion::Parse(L"0.3.0");
    CHECK(current.has_value());

    const auto result = CheckForUpdate(
        [](std::wstring_view, std::string& body)
        {
            body = Feed(L"0.2.9");
            return true;
        },
        L"", *current);

    CHECK(!result.available);
    CHECK(!result.error);
    CHECK(!result.feed.has_value());
}

WINTOOLS_TEST(Check_FallsBackWhenFirstSourceFails)
{
    const auto current = AppVersion::Parse(L"0.2.0");
    CHECK(current.has_value());
    int calls = 0;

    const auto result = CheckForUpdate(
        [&calls](std::wstring_view, std::string& body)
        {
            if (calls++ == 0)
            {
                return false;
            }
            body = Feed(L"0.4.0");
            return true;
        },
        L"http://localhost/custom", *current);

    CHECK_EQ(calls, 2);
    CHECK(result.available);
    CHECK_EQ(result.feed->version.Text(), std::wstring(L"0.4.0"));
}

WINTOOLS_TEST(Check_MalformedFeedIsSkipped)
{
    const auto current = AppVersion::Parse(L"0.2.0");
    CHECK(current.has_value());

    const auto result = CheckForUpdate(
        [](std::wstring_view, std::string& body)
        {
            body = "schema=1\nversion=not-a-version\n";
            return true;
        },
        L"", *current);

    CHECK(result.error);
    CHECK(!result.available);
}

WINTOOLS_TEST(Check_AllSourcesDownIsSilentError)
{
    const auto current = AppVersion::Parse(L"0.2.0");
    CHECK(current.has_value());

    const auto result = CheckForUpdate(
        [](std::wstring_view, std::string&) { return false; }, L"", *current);

    CHECK(result.error);
    CHECK(!result.available);
}
