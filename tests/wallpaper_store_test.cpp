#include "test_framework.h"

#include <cwctype>
#include <filesystem>
#include <fstream>

#include "core/wallpaper_store.h"

using namespace wintools::core;

namespace
{

/// A file that cleans up after itself: every case here writes a real file, because the parsing is
/// the thing under test and a memory-only fake would not exercise the CRLF and encoding paths.
class TempFile
{
public:
    explicit TempFile(const std::wstring& name)
        : m_path(std::filesystem::temp_directory_path() / name)
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const
    {
        return m_path;
    }

    void WriteRaw(const std::string& bytes) const
    {
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

private:
    std::filesystem::path m_path;
};

const wchar_t kLeft[] = L"\\\\?\\DISPLAY#SAM0F72#5&1e2a3b4c&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
const wchar_t kRight[] = L"\\\\?\\DISPLAY#GSM5B7F#5&1e2a3b4c&0&UID4353#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";

} // namespace

WINTOOLS_TEST(WallpaperStore_RoundTripsAssignments)
{
    TempFile file(L"wintools-wallpapers-roundtrip.txt");

    MonitorWallpapers saved;
    saved.push_back({ kLeft, L"C:\\pics\\left.jpg" });
    saved.push_back({ kRight, L"\\\\nas\\共享\\right 图.png" });
    CHECK(SaveMonitorWallpapers(saved, file.Path()));

    const auto loaded = LoadMonitorWallpapers(file.Path());
    CHECK(loaded.has_value());
    if (!loaded.has_value())
    {
        return;
    }

    CHECK(loaded->size() == 2);
    CHECK(loaded->at(0).devicePath == kLeft);
    CHECK(loaded->at(0).image == L"C:\\pics\\left.jpg");
    // Non-ASCII in a UNC path has to survive the UTF-8 round trip.
    CHECK(loaded->at(1).image == L"\\\\nas\\共享\\right 图.png");
}

// "Follows the spanning wallpaper" is the absence of a row, not a row with an empty field: an
// empty value in the file would be indistinguishable from a truncated write.
WINTOOLS_TEST(WallpaperStore_OmitsDisplaysWithoutTheirOwnPicture)
{
    TempFile file(L"wintools-wallpapers-empty.txt");

    MonitorWallpapers list;
    list.push_back({ kLeft, L"" });
    list.push_back({ kRight, L"C:\\pics\\right.jpg" });
    CHECK(SaveMonitorWallpapers(list, file.Path()));

    const auto loaded = LoadMonitorWallpapers(file.Path());
    CHECK(loaded.has_value());
    if (!loaded.has_value())
    {
        return;
    }

    CHECK(loaded->size() == 1);
    CHECK(loaded->at(0).devicePath == kRight);
}

WINTOOLS_TEST(WallpaperStore_MissingFileIsNotAnError)
{
    TempFile file(L"wintools-wallpapers-absent.txt");
    CHECK(!LoadMonitorWallpapers(file.Path()).has_value());
}

// Notepad, an editor or a git checkout turns the file into CRLF. Without stripping the carriage
// return it sticks to the image path and every row looks broken.
WINTOOLS_TEST(WallpaperStore_ReadsCrlfFiles)
{
    TempFile file(L"wintools-wallpapers-crlf.txt");
    file.WriteRaw("# WinTools wallpapers v1\r\nmonitor\t" + std::string("disp-a") + "\tC:\\a.jpg\r\n"
                  "monitor\tdisp-b\tC:\\b.jpg\r\n");

    const auto loaded = LoadMonitorWallpapers(file.Path());
    CHECK(loaded.has_value());
    if (!loaded.has_value())
    {
        return;
    }

    CHECK(loaded->size() == 2);
    CHECK(loaded->at(0).image == L"C:\\a.jpg");
    CHECK(loaded->at(1).image == L"C:\\b.jpg");
}

// A file written by a tool that translates newlines twice ends up with \r\r\n, and one leftover
// carriage return turns a valid path into an invalid one - the picture then silently falls back
// to the spanning slice.
WINTOOLS_TEST(WallpaperStore_SurvivesADoubleCarriageReturn)
{
    TempFile file(L"wintools-wallpapers-crcrlf.txt");
    file.WriteRaw("# WinTools wallpapers v1\r\r\nmonitor\tdisp-a\tC:\\a.jpg\r\r\n");

    const auto loaded = LoadMonitorWallpapers(file.Path());
    CHECK(loaded.has_value());
    if (!loaded.has_value())
    {
        return;
    }

    CHECK(loaded->size() == 1);
    CHECK(loaded->at(0).image == L"C:\\a.jpg");
}

// A hand-edited file must not take the whole list down: a bad line is skipped and the rest loads.
WINTOOLS_TEST(WallpaperStore_SkipsUnusableRows)
{
    TempFile file(L"wintools-wallpapers-broken.txt");
    file.WriteRaw("# WinTools wallpapers v1\n"
                  "nonsense\n"
                  "monitor\tonly-one-field\n"
                  "monitor\t\tC:\\no-device.jpg\n"
                  "monitor\tdisp-c\t\n"
                  "monitor\tdisp-d\tC:\\d.jpg\n");

    const auto loaded = LoadMonitorWallpapers(file.Path());
    CHECK(loaded.has_value());
    if (!loaded.has_value())
    {
        return;
    }

    CHECK(loaded->size() == 1);
    CHECK(loaded->at(0).devicePath == L"disp-d");
    CHECK(loaded->at(0).image == L"C:\\d.jpg");
}

WINTOOLS_TEST(WallpaperStore_MatchesDevicePathsCaseInsensitively)
{
    MonitorWallpapers list;
    SetMonitorImage(list, kLeft, L"C:\\one.jpg");

    CHECK(FindMonitorImage(list, kLeft) == L"C:\\one.jpg");

    // A display re-enumerated after a replug can come back with different casing; an exact
    // comparison would add a second row for the same screen and the old picture would win or lose
    // depending on order.
    std::wstring lower = kLeft;
    for (wchar_t& character : lower)
    {
        character = static_cast<wchar_t>(std::towlower(character));
    }

    SetMonitorImage(list, lower, L"C:\\two.jpg");
    CHECK(list.size() == 1);
    CHECK(FindMonitorImage(list, kLeft) == L"C:\\two.jpg");
}

WINTOOLS_TEST(WallpaperStore_ClearingRemovesTheRow)
{
    MonitorWallpapers list;
    SetMonitorImage(list, kLeft, L"C:\\one.jpg");
    SetMonitorImage(list, kRight, L"C:\\two.jpg");
    CHECK(list.size() == 2);

    SetMonitorImage(list, kLeft, L"");
    CHECK(list.size() == 1);
    CHECK(FindMonitorImage(list, kLeft).empty());
    CHECK(FindMonitorImage(list, kRight) == L"C:\\two.jpg");
}
