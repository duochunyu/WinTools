#include "test_framework.h"

#include "util/file.h"
#include "util/path.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace
{

// Tests must never touch the real %LOCALAPPDATA%\WinTools folder - in particular notes/index.txt
// and archive.txt, which hold the user's data. A per-process scratch folder under the system
// temp directory keeps every test self-contained.
fs::path Scratch()
{
    const fs::path root = fs::temp_directory_path() / L"WinTools-file-test";
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

std::string ReadAll(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

} // namespace

WINTOOLS_TEST(AtomicWrite_CreatesParentDirectories)
{
    const fs::path file = Scratch() / L"nested" / L"deep" / L"settings.txt";
    std::error_code ec;
    fs::remove_all(file.parent_path().parent_path(), ec);

    CHECK(wintools::util::AtomicWrite(file, L"key=value\n"));
    CHECK(fs::exists(file));
    CHECK_EQ(ReadAll(file), std::string("key=value\n"));
}

WINTOOLS_TEST(AtomicWrite_ReplacesExistingFile)
{
    const fs::path file = Scratch() / L"replace.txt";
    CHECK(wintools::util::AtomicWrite(file, L"old content"));
    CHECK(wintools::util::AtomicWrite(file, L"new content"));
    CHECK_EQ(ReadAll(file), std::string("new content"));
}

WINTOOLS_TEST(AtomicWrite_LeavesNoTempFiles)
{
    const fs::path dir = Scratch() / L"clean";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    const fs::path file = dir / L"data.txt";
    for (int i = 0; i < 5; ++i)
    {
        CHECK(wintools::util::AtomicWrite(file, L"value"));
    }

    int entries = 0;
    for (const auto& item : fs::directory_iterator(dir))
    {
        ++entries;
        CHECK_EQ(item.path().filename().wstring(), std::wstring(L"data.txt"));
    }
    CHECK_EQ(entries, 1);
}

WINTOOLS_TEST(AtomicWriteBytes_PreservesRawBytes)
{
    // RTF bodies are byte streams. A UTF-8 round trip through wstring would mangle these.
    const std::string bytes{ '\x00', 'a', '\\', 'p', 'i', 'c', 't', '\xff', '\xfe' };
    const fs::path file = Scratch() / L"note.rtf";

    CHECK(wintools::util::AtomicWriteBytes(file, bytes));
    CHECK_EQ(ReadAll(file), bytes);
}

WINTOOLS_TEST(AtomicWrite_FailsWithoutTouchingTargetWhenParentIsAFile)
{
    const fs::path blocker = Scratch() / L"blocker";
    std::error_code ec;
    fs::remove(blocker, ec);
    {
        std::ofstream out(blocker, std::ios::binary);
        out << "occupied";
    }

    // A parent path that is a regular file cannot hold children: create_directories fails and
    // the existing target must be left exactly as it was.
    const fs::path target = blocker / L"child.txt";
    CHECK(!wintools::util::AtomicWrite(target, L"should not land"));
    CHECK(!fs::exists(target));
    CHECK_EQ(ReadAll(blocker), std::string("occupied"));
}
