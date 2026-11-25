#include "test_framework.h"

#include "core/update_version.h"

#include <string>

using wintools::core::AppVersion;
using wintools::core::Compare;

WINTOOLS_TEST(Version_ParsesThreeNumericComponents)
{
    const auto version = AppVersion::Parse(L"1.20.3");
    CHECK(version.has_value());
    CHECK_EQ(version->major, 1u);
    CHECK_EQ(version->minor, 20u);
    CHECK_EQ(version->patch, 3u);
    CHECK_EQ(version->Text(), std::wstring(L"1.20.3"));
}

WINTOOLS_TEST(Version_AcceptsLeadingZeros)
{
    const auto version = AppVersion::Parse(L"01.002.0003");
    CHECK(version.has_value());
    CHECK_EQ(version->major, 1u);
    CHECK_EQ(version->minor, 2u);
    CHECK_EQ(version->patch, 3u);
    CHECK_EQ(version->Text(), std::wstring(L"1.2.3"));
}

WINTOOLS_TEST(Version_RejectsPrereleaseSuffix)
{
    CHECK(!AppVersion::Parse(L"1.2.3-beta"));
    CHECK(!AppVersion::Parse(L"1.2.3+build"));
    CHECK(!AppVersion::Parse(L"v1.2.3"));
}

WINTOOLS_TEST(Version_RejectsMalformedComponents)
{
    CHECK(!AppVersion::Parse(L""));
    CHECK(!AppVersion::Parse(L"1.2"));
    CHECK(!AppVersion::Parse(L"1.2.3.4"));
    CHECK(!AppVersion::Parse(L"1..3"));
    CHECK(!AppVersion::Parse(L"1.2."));
    CHECK(!AppVersion::Parse(L"-1.2.3"));
    CHECK(!AppVersion::Parse(L"1.2.3abc"));
    CHECK(!AppVersion::Parse(L"1.2.99999999999"));
}

WINTOOLS_TEST(Version_CompareIsNumericNotLexical)
{
    const auto older = AppVersion::Parse(L"1.9.0");
    const auto newer = AppVersion::Parse(L"1.10.0");
    CHECK(older.has_value());
    CHECK(newer.has_value());
    CHECK(Compare(*older, *newer) < 0);
    CHECK(Compare(*newer, *older) > 0);
}

WINTOOLS_TEST(Version_CompareWalksComponentsInOrder)
{
    const auto base = AppVersion::Parse(L"2.3.4");
    CHECK(base.has_value());
    CHECK_EQ(Compare(*base, *AppVersion::Parse(L"2.3.4")), 0);
    CHECK(Compare(*base, *AppVersion::Parse(L"3.0.0")) < 0);
    CHECK(Compare(*base, *AppVersion::Parse(L"2.4.0")) < 0);
    CHECK(Compare(*base, *AppVersion::Parse(L"2.3.5")) < 0);
}
