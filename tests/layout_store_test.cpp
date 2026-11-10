#include "test_framework.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/layout_store.h"
#include "core/window_layout.h"

using namespace wintools::core;

namespace
{

std::filesystem::path TempFile(const wchar_t* name)
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / L"wintools-tests";
    std::filesystem::create_directories(folder);
    return folder / name;
}

void WriteRaw(const std::filesystem::path& file, const std::string& utf8)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

Layout SampleLayout()
{
    Layout layout;

    MonitorSnapshot first;
    first.devicePath = L"\\\\?\\DISPLAY#AAA#5&1&0&UID256#{guid}";
    first.rect = Rect{ 0, 0, 1920, 1200 };
    layout.monitors.push_back(first);

    MonitorSnapshot second;
    second.devicePath = L"\\\\?\\DISPLAY#BBB#5&1&0&UID257#{guid}";
    second.rect = Rect{ 181, -1440, 3440, 1440 };
    layout.monitors.push_back(second);

    WindowSnapshot editor;
    editor.exePath = L"D:\\Apps\\Code.exe";
    editor.appName = L"Code.exe";
    editor.title = L"main.cpp - project";
    editor.monitorPath = first.devicePath;
    editor.rect = Rect{ 100, 80, 900, 700 };
    editor.dock = DockMode::Left;
    layout.windows.push_back(editor);

    WindowSnapshot fullscreen;
    fullscreen.exePath = L"C:\\Apps\\Player.exe";
    fullscreen.appName = L"Player.exe";
    fullscreen.title = L"movie";
    fullscreen.monitorPath = second.devicePath;
    fullscreen.rect = Rect{ 200, -1400, 3000, 1300 };
    fullscreen.maximized = true;
    layout.windows.push_back(fullscreen);

    WindowSnapshot hiddenOne;
    hiddenOne.exePath = L"C:\\Apps\\Chat.exe";
    hiddenOne.appName = L"Chat.exe";
    hiddenOne.title = L"chat";
    hiddenOne.monitorPath = first.devicePath;
    hiddenOne.rect = Rect{ 20, 30, 400, 300 };
    hiddenOne.minimized = true;
    hiddenOne.enabled = false;
    layout.windows.push_back(hiddenOne);

    return layout;
}

} // namespace

WINTOOLS_TEST(LayoutStore_RoundTripsEveryField)
{
    const std::filesystem::path file = TempFile(L"roundtrip.txt");
    const Layout written = SampleLayout();

    CHECK(SaveLayout(written, file));

    const auto read = LoadLayout(file);
    CHECK(read.has_value());
    if (!read.has_value())
    {
        return;
    }

    CHECK_EQ(read->monitors.size(), written.monitors.size());
    CHECK_EQ(read->windows.size(), written.windows.size());

    for (size_t index = 0; index < written.monitors.size(); ++index)
    {
        CHECK(read->monitors[index].devicePath == written.monitors[index].devicePath);
        CHECK_EQ(read->monitors[index].rect.left, written.monitors[index].rect.left);
        CHECK_EQ(read->monitors[index].rect.top, written.monitors[index].rect.top);
        CHECK_EQ(read->monitors[index].rect.width, written.monitors[index].rect.width);
        CHECK_EQ(read->monitors[index].rect.height, written.monitors[index].rect.height);
    }

    for (size_t index = 0; index < written.windows.size(); ++index)
    {
        const auto& expected = written.windows[index];
        const auto& actual = read->windows[index];

        CHECK(actual.exePath == expected.exePath);
        CHECK(actual.title == expected.title);
        CHECK(actual.monitorPath == expected.monitorPath);
        CHECK_EQ(actual.rect.left, expected.rect.left);
        CHECK_EQ(actual.rect.height, expected.rect.height);
        CHECK_EQ(actual.minimized, expected.minimized);
        CHECK_EQ(actual.maximized, expected.maximized);
        CHECK_EQ(static_cast<int>(actual.dock), static_cast<int>(expected.dock));
        CHECK_EQ(actual.enabled, expected.enabled);
    }
}

// A layout file that has been through Notepad, an editor or a git checkout is CRLF. Reading it
// must not leave a carriage return on the last field of each record - that silently makes every
// monitor device path differ from the one the shell reports.
WINTOOLS_TEST(LayoutStore_StripsCarriageReturns)
{
    const std::filesystem::path file = TempFile(L"crlf.txt");

    WriteRaw(file,
        "# hand-edited\r\n"
        "monitor\t0\t0\t1920\t1200\t\\\\?\\DISPLAY#AAA#5&1&0&UID256#{guid}\r\n"
        "window\t100\t80\t900\t700\t0\t\\\\?\\DISPLAY#AAA#5&1&0&UID256#{guid}\tD:\\Apps\\Code.exe\t"
        "main.cpp\r\n");

    const auto read = LoadLayout(file);
    CHECK(read.has_value());
    if (!read.has_value())
    {
        return;
    }

    CHECK_EQ(read->monitors.size(), size_t{ 1 });
    CHECK_EQ(read->windows.size(), size_t{ 1 });

    const std::wstring expected = L"\\\\?\\DISPLAY#AAA#5&1&0&UID256#{guid}";
    CHECK(read->monitors[0].devicePath == expected);
    CHECK(read->windows[0].monitorPath == expected);
    CHECK(read->windows[0].title == L"main.cpp");
    CHECK(read->windows[0].exePath == L"D:\\Apps\\Code.exe");
}

WINTOOLS_TEST(LayoutStore_ReturnsNothingForMissingFile)
{
    const std::filesystem::path file = TempFile(L"does-not-exist.txt");
    std::error_code ec;
    std::filesystem::remove(file, ec);

    CHECK(!LoadLayout(file).has_value());
}

// Corrupt records are skipped one by one rather than failing the whole load: a single bad line
// should not cost the user every other window position.
WINTOOLS_TEST(LayoutStore_SkipsMalformedRecords)
{
    const std::filesystem::path file = TempFile(L"malformed.txt");

    WriteRaw(file,
        "monitor\tnot-a-number\t0\t1920\t1200\t\\\\?\\DISPLAY#BAD#{guid}\n"
        "monitor\t0\t0\t1920\t1200\t\\\\?\\DISPLAY#AAA#{guid}\n"
        "window\tmissing\tfields\n"
        "window\t10\t20\t300\t400\tx\t\\\\?\\DISPLAY#AAA#{guid}\tC:\\Apps\\Player.exe\tclip\n"
        "unrelated\tline\n");

    const auto read = LoadLayout(file);
    CHECK(read.has_value());
    if (!read.has_value())
    {
        return;
    }

    CHECK_EQ(read->monitors.size(), size_t{ 1 });
    CHECK(read->monitors[0].devicePath == L"\\\\?\\DISPLAY#AAA#{guid}");

    CHECK_EQ(read->windows.size(), size_t{ 1 });
    CHECK_EQ(read->windows[0].maximized, true);
    CHECK(read->windows[0].appName == L"Player.exe");
}

// Tabs are not allowed inside a window title when saving, because they are the field separator.
WINTOOLS_TEST(LayoutStore_FlattensTabsInTitles)
{
    const std::filesystem::path file = TempFile(L"tabs.txt");

    Layout layout = SampleLayout();
    layout.windows[0].title = L"before\tafter";

    CHECK(SaveLayout(layout, file));

    const auto read = LoadLayout(file);
    CHECK(read.has_value());
    if (!read.has_value())
    {
        return;
    }

    CHECK(read->windows[0].title == L"before after");
    CHECK_EQ(read->windows.size(), layout.windows.size());
}

// Files written before the dock column existed must keep loading. They are the one thing a user
// cannot rebuild from memory after an update, so an old layout has to survive the format change.
WINTOOLS_TEST(LayoutStore_ReadsVersionOneFilesAsPosition)
{
    const std::filesystem::path file = TempFile(L"v1.txt");

    WriteRaw(file,
        "# WinTools layout snapshot\n"
        "monitor\t0\t0\t1920\t1200\t\\\\?\\DISPLAY#AAA#{guid}\n"
        "window\t100\t80\t900\t700\t0\t\\\\?\\DISPLAY#AAA#{guid}\tD:\\Apps\\Code.exe\tmain.cpp\n"
        "window\t10\t20\t300\t400\tmx\t\\\\?\\DISPLAY#AAA#{guid}\tC:\\Apps\\Game.exe\tgame\n");

    const auto read = LoadLayout(file);
    CHECK(read.has_value());
    if (!read.has_value())
    {
        return;
    }

    CHECK_EQ(read->windows.size(), size_t{ 2 });

    // No dock column in version 1: free position is the only honest interpretation.
    CHECK_EQ(static_cast<int>(read->windows[0].dock), static_cast<int>(DockMode::Position));
    CHECK(read->windows[0].enabled);
    CHECK(read->windows[0].exePath == L"D:\\Apps\\Code.exe");
    CHECK(read->windows[0].title == L"main.cpp");

    CHECK_EQ(static_cast<int>(read->windows[1].dock), static_cast<int>(DockMode::Position));
    CHECK(read->windows[1].maximized);
    CHECK(read->windows[1].minimized);
}

// A dock value written by a newer build, or by hand as a typo, must degrade to "position" rather
// than producing a mode that ComputeDockRect treats as somewhere random.
WINTOOLS_TEST(LayoutStore_FallsBackOnUnknownDockValues)
{
    const std::filesystem::path file = TempFile(L"dock-garbage.txt");

    WriteRaw(file,
        "# WinTools layout v2\n"
        "window\t100\t80\t900\t700\t0\t99\t\\\\?\\DISPLAY#AAA#{guid}\tC:\\Apps\\A.exe\tone\n"
        "window\t100\t80\t900\t700\t0\t-3\t\\\\?\\DISPLAY#AAA#{guid}\tC:\\Apps\\B.exe\ttwo\n"
        "window\t100\t80\t900\t700\t0\toops\t\\\\?\\DISPLAY#AAA#{guid}\tC:\\Apps\\C.exe\tthree\n");

    const auto read = LoadLayout(file);
    CHECK(read.has_value());
    if (!read.has_value())
    {
        return;
    }

    CHECK_EQ(read->windows.size(), size_t{ 3 });

    for (const auto& window : read->windows)
    {
        CHECK_EQ(static_cast<int>(window.dock), static_cast<int>(DockMode::Position));
        CHECK(window.enabled);
    }
}

// The "remember me" flag rides in the flags column, which is also where minimized and maximized
// live; all three have to survive being written together.
WINTOOLS_TEST(LayoutStore_KeepsDisabledFlagAlongsideWindowState)
{
    const std::filesystem::path file = TempFile(L"disabled.txt");

    Layout layout = SampleLayout();
    layout.windows[0].enabled = false;
    layout.windows[0].dock = DockMode::Bottom;
    layout.windows[1].enabled = false;
    layout.windows[1].maximized = true;

    CHECK(SaveLayout(layout, file));

    const auto read = LoadLayout(file);
    CHECK(read.has_value());
    if (!read.has_value())
    {
        return;
    }

    CHECK(!read->windows[0].enabled);
    CHECK_EQ(static_cast<int>(read->windows[0].dock), static_cast<int>(DockMode::Bottom));
    CHECK(!read->windows[0].minimized);

    CHECK(!read->windows[1].enabled);
    CHECK(read->windows[1].maximized);
}
