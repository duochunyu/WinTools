#include "test_framework.h"

#include "core/update_snapshot.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace
{

fs::path Scratch()
{
    const fs::path root = fs::temp_directory_path() / L"WinTools-snapshot-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

void Put(const fs::path& file, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

WINTOOLS_TEST(Snapshot_CopiesLiveDataAndKeepsTheOriginals)
{
    const fs::path root = Scratch();
    Put(root / L"settings.txt", "theme=1\n");
    Put(root / L"groups.tsv", "group\t1\n");
    Put(root / L"notes" / L"index.txt", "1\t0\t0\t0\t0\n");
    Put(root / L"notes" / L"archive.txt", "1\ttext\n");
    Put(root / L"notes" / L"1.rtf", "{\\rtf1 hello}");

    std::wstring error;
    CHECK(wintools::core::SnapshotBeforeUpdate(root, L"0.2.0", error));

    const fs::path backup = root / L"backup" / L"0.2.0";
    CHECK(fs::exists(backup / L"settings.txt"));
    CHECK(fs::exists(backup / L"groups.tsv"));
    CHECK(fs::exists(backup / L"notes" / L"index.txt"));
    CHECK(fs::exists(backup / L"notes" / L"archive.txt"));
    CHECK(fs::exists(backup / L"notes" / L"1.rtf"));

    // Snapshot means copy: the live files must still be exactly where the running app left them.
    CHECK(fs::exists(root / L"settings.txt"));
    CHECK(fs::exists(root / L"notes" / L"index.txt"));
}

WINTOOLS_TEST(Snapshot_MissingFilesAreNotAnError)
{
    const fs::path root = Scratch();
    std::wstring error;
    CHECK(wintools::core::SnapshotBeforeUpdate(root, L"0.2.0", error));
    CHECK(fs::is_directory(root / L"backup" / L"0.2.0"));
}
