#include "test_framework.h"

#include "util/version.h"

#include <format>
#include <string>

WINTOOLS_TEST(Version_ComesFromGeneratedHeader)
{
    // These assertions are deliberately tautological at runtime. Their value is at build time:
    // if project(VERSION) is edited, the generated header changes, and this test compiles the
    // new value straight into the test binary from the same source the exe and RC file use.
    // A hard-coded copy of the version anywhere else in the tree is therefore a build failure
    // waiting to be caught by the tag check in CI, not a silent drift.
    const std::wstring version = wintools::kVersion;
    CHECK(!version.empty());
    CHECK(version.find(L'.') != std::wstring::npos);
    CHECK_EQ(version, std::wstring(WINTOOLS_VERSION_STRING));
}

WINTOOLS_TEST(Version_ResourceNumbersMatchText)
{
    // The numeric FILEVERSION fields must describe the same release as the user-facing string.
    // This is the exact pair that used to disagree (0,1,0,0 vs "0.2.0.0" in app.rc).
    const std::wstring expected = std::format(L"{}.{}.{}.0",
        WINTOOLS_VERSION_MAJOR, WINTOOLS_VERSION_MINOR, WINTOOLS_VERSION_PATCH);
    CHECK_EQ(std::wstring(WINTOOLS_VERSION_FILESTRING), expected);
}
