#include "test_framework.h"

#include "app/settings.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace
{

// Restores the real path even if an assertion aborts the test body early.
struct ScopedSettingsFile
{
    explicit ScopedSettingsFile(const fs::path& file)
    {
        wintools::app::Settings::SetFileForTests(file);
    }
    ~ScopedSettingsFile()
    {
        wintools::app::Settings::SetFileForTests({});
    }
};

fs::path ScratchFile(const wchar_t* name)
{
    const fs::path dir = fs::temp_directory_path() / L"WinTools-settings-test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / name;
}

void WriteUtf8(const fs::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

} // namespace

WINTOOLS_TEST(Settings_V1FileWithoutHeaderStillLoads)
{
    const fs::path file = ScratchFile(L"v1.txt");
    WriteUtf8(file, "# WinTools settings\nlanguage=2\ntheme=1\nlayout.autoRestore=0\n");
    ScopedSettingsFile scope{ file };

    const auto settings = wintools::app::Settings::Load();
    CHECK_EQ(settings.language, 2);
    CHECK_EQ(settings.theme, 1);
    CHECK(!settings.autoRestoreLayout);
}

WINTOOLS_TEST(Settings_V2HeaderIsAcceptedAndRoundTrips)
{
    const fs::path file = ScratchFile(L"v2.txt");
    WriteUtf8(file, "# WinTools settings v2\nlanguage=1\nnote.restore=0\nlanguage=2\n");
    ScopedSettingsFile scope{ file };

    const auto settings = wintools::app::Settings::Load();
    CHECK_EQ(settings.language, 2); // later duplicate wins, same as v1
    CHECK(!settings.restoreNotes);

    CHECK(settings.Save());
    std::ifstream in(file, std::ios::binary);
    const std::string saved{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    CHECK(saved.starts_with("# WinTools settings v2\n"));
}

WINTOOLS_TEST(Settings_UnknownKeysAreIgnored)
{
    const fs::path file = ScratchFile(L"unknown.txt");
    WriteUtf8(file, "# WinTools settings v2\nfuture.key=whatever\ntheme=2\n");
    ScopedSettingsFile scope{ file };

    const auto settings = wintools::app::Settings::Load();
    CHECK_EQ(settings.theme, 2);
}
