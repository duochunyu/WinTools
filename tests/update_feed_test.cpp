#include "test_framework.h"

#include "core/update_feed.h"

#include <format>
#include <string>

using wintools::core::ParseUpdateFeed;

namespace
{
constexpr std::wstring_view kValidHash =
    L"0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF";
}

WINTOOLS_TEST(Feed_ParsesACompleteFeed)
{
    const std::wstring text = std::format(
        L"schema=1\n"
        L"version=0.3.0\n"
        L"size=1234567\n"
        L"sha256={}\n"
        L"url=https://gitee.com/duochunyu/main/releases/download/v0.3.0/WinTools.exe\n"
        L"urlGithub=https://github.com/duochunyu/WinTools/releases/download/v0.3.0/WinTools.exe\n"
        L"notes.zh=新增自动更新\n"
        L"notes.en=Add auto-update\n",
        kValidHash);

    const auto feed = ParseUpdateFeed(text);
    CHECK(feed.has_value());
    CHECK_EQ(feed->version.Text(), std::wstring(L"0.3.0"));
    CHECK_EQ(feed->size, 1234567ull);
    CHECK_EQ(feed->sha256, std::wstring(kValidHash));
    CHECK(feed->url.find(L"gitee.com") != std::wstring::npos);
    CHECK(feed->urlGithub.find(L"github.com") != std::wstring::npos);
    CHECK_EQ(feed->notesZh, std::wstring(L"新增自动更新"));
    CHECK_EQ(feed->notesEn, std::wstring(L"Add auto-update"));
}

// The mirror key has to match what the release workflow actually writes. It once did not: the
// parser read "url.github" while .github/workflows/release.yml emitted "urlGithub", so the GitHub
// fallback was always empty and only a live download could have revealed it - every unit test
// passed because the fixture used the spelling the parser expected.
WINTOOLS_TEST(Feed_ReadsTheMirrorKeyTheWorkflowWrites)
{
    const std::wstring text = std::format(
        L"schema=1\n"
        L"version=0.3.0\n"
        L"size=1290240\n"
        L"sha256={}\n"
        L"url=https://gitee.com/YingKeWangLuo_admin/main/releases/download/v0.3.0/WinTools.exe\n"
        L"urlGithub=https://github.com/duochunyu/WinTools/releases/download/v0.3.0/WinTools.exe\n"
        L"notes.zh=修复问题\n"
        L"notes.en=Bug fixes\n",
        kValidHash);

    const auto feed = ParseUpdateFeed(text);
    CHECK(feed.has_value());
    CHECK_EQ(feed->urlGithub,
        std::wstring(L"https://github.com/duochunyu/WinTools/releases/download/v0.3.0/WinTools.exe"));
}

// The dotted spelling is not generated any more but is still tolerated, so a feed written against
// the old parser keeps its mirror instead of silently losing it.
WINTOOLS_TEST(Feed_StillAcceptsTheDottedMirrorKey)
{
    const std::wstring text = std::format(
        L"schema=1\nversion=1.0.0\nsize=10\nsha256={}\nurl.github=https://example.com/a.exe\n",
        kValidHash);

    const auto feed = ParseUpdateFeed(text);
    CHECK(feed.has_value());
    CHECK_EQ(feed->urlGithub, std::wstring(L"https://example.com/a.exe"));
}

// A publisher on Windows may write the feed with CRLF. Values are trimmed, so both encodings must
// parse identically; the release attachment is CRLF while the raw repository file is LF.
WINTOOLS_TEST(Feed_ParsesCrlfLineEndings)
{
    const std::wstring text = std::format(
        L"schema=1\r\n"
        L"version=1.2.3\r\n"
        L"size=99\r\n"
        L"sha256={}\r\n"
        L"url=https://gitee.com/x/y/releases/download/v1.2.3/WinTools.exe\r\n"
        L"urlGithub=https://github.com/x/y/releases/download/v1.2.3/WinTools.exe\r\n"
        L"notes.en=Crlf feed\r\n",
        kValidHash);

    const auto feed = ParseUpdateFeed(text);
    CHECK(feed.has_value());
    CHECK_EQ(feed->version.Text(), std::wstring(L"1.2.3"));
    CHECK_EQ(feed->size, 99ull);
    CHECK_EQ(feed->sha256, std::wstring(kValidHash));
    CHECK_EQ(feed->notesEn, std::wstring(L"Crlf feed"));
    CHECK(feed->urlGithub.find(L"github.com") != std::wstring::npos);
}

WINTOOLS_TEST(Feed_IgnoresCommentsAndUnknownKeys)
{
    const std::wstring text = std::format(
        L"# WinTools update feed\n"
        L"\n"
        L"schema=1\n"
        L"version=1.0.0\n"
        L"size=10\n"
        L"sha256={}\n"
        L"future.key=something\n"
        L"published=2026-11-26T10:00:00Z\n",
        kValidHash);

    const auto feed = ParseUpdateFeed(text);
    CHECK(feed.has_value());
    CHECK_EQ(feed->version.Text(), std::wstring(L"1.0.0"));
}

WINTOOLS_TEST(Feed_RejectsMissingRequiredFields)
{
    const std::wstring base = std::format(L"schema=1\nversion=1.0.0\nsize=10\nsha256={}\n", kValidHash);
    CHECK(ParseUpdateFeed(base).has_value());

    CHECK(!ParseUpdateFeed(std::format(L"version=1.0.0\nsize=10\nsha256={}\n", kValidHash)));
    CHECK(!ParseUpdateFeed(std::format(L"schema=1\nsize=10\nsha256={}\n", kValidHash)));
    CHECK(!ParseUpdateFeed(std::format(L"schema=1\nversion=1.0.0\nsha256={}\n", kValidHash)));
    CHECK(!ParseUpdateFeed(L"schema=1\nversion=1.0.0\nsize=10\n"));
}

WINTOOLS_TEST(Feed_RejectsWrongSchemaAndBadHash)
{
    CHECK(!ParseUpdateFeed(std::format(L"schema=2\nversion=1.0.0\nsize=10\nsha256={}\n", kValidHash)));
    CHECK(!ParseUpdateFeed(L"schema=1\nversion=1.0.0\nsize=10\nsha256=deadbeef\n"));
    CHECK(!ParseUpdateFeed(
        L"schema=1\nversion=1.0.0\nsize=10\nsha256=zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\n"));
}

WINTOOLS_TEST(Feed_RejectsBadVersionAndSize)
{
    CHECK(!ParseUpdateFeed(std::format(L"schema=1\nversion=1.0-beta\nsize=10\nsha256={}\n", kValidHash)));
    CHECK(!ParseUpdateFeed(std::format(L"schema=1\nversion=1.0.0\nsize=12abc\nsha256={}\n", kValidHash)));
}

WINTOOLS_TEST(Feed_CapsNotesLength)
{
    const std::wstring huge(9000, L'x');
    const std::wstring text = std::format(
        L"schema=1\nversion=1.0.0\nsize=10\nsha256={}\nnotes.zh={}\n", kValidHash, huge);

    const auto feed = ParseUpdateFeed(text);
    CHECK(feed.has_value());
    CHECK_EQ(feed->notesZh.size(), 4096u);
}
