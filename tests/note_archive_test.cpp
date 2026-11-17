#include "test_framework.h"

#include <filesystem>

#include "core/note_archive.h"
#include "core/note_store.h"

using namespace wintools::core;

namespace
{

ArchiveRecord MakeRecord(const wchar_t* id, const wchar_t* preview)
{
    ArchiveRecord record;
    record.id = id;
    record.rect = Rect{ 96, 96, 360, 300 };
    record.theme = 2;
    record.preview = preview;
    record.closedAt = L"11-17 02:31";
    return record;
}

/// The archive lives beside the note index, so a test that writes it has to point both at a
/// scratch folder or it would overwrite the user's real history.
class ScratchFolder
{
public:
    ScratchFolder()
    {
        static int counter = 0;
        m_path = std::filesystem::temp_directory_path() / L"wintools-tests"
            / (L"archive-" + std::to_wstring(++counter));

        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);

        NoteStore::SetFolder(m_path);
        NoteArchive::SetFolder(m_path);
    }

    ~ScratchFolder()
    {
        NoteArchive::SetFolder({});
        NoteStore::SetFolder({});

        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;

private:
    std::filesystem::path m_path;
};

} // namespace

WINTOOLS_TEST(NoteArchive_RoundTripsEveryField)
{
    NoteArchive archive;
    archive.Add(MakeRecord(L"n1", L"下周要交的东西"));
    archive.Add(MakeRecord(L"n2", L"second note"));

    const NoteArchive parsed = ParseNoteArchive(FormatNoteArchive(archive));

    CHECK_EQ(parsed.records.size(), static_cast<std::size_t>(2));
    if (parsed.records.size() == 2)
    {
        CHECK(parsed.records[0].id == L"n2"); // newest first
        CHECK(parsed.records[1].id == L"n1");
        CHECK_EQ(parsed.records[1].theme, 2);
        CHECK(parsed.records[1].rect == (Rect{ 96, 96, 360, 300 }));
        CHECK(parsed.records[1].preview == L"下周要交的东西");
        CHECK(parsed.records[1].closedAt == L"11-17 02:31");
    }
}

WINTOOLS_TEST(NoteArchive_AddMovesAnExistingIdToTheFront)
{
    NoteArchive archive;
    archive.Add(MakeRecord(L"n1", L"first"));
    archive.Add(MakeRecord(L"n2", L"second"));
    archive.Add(MakeRecord(L"n1", L"first again"));

    CHECK_EQ(archive.records.size(), static_cast<std::size_t>(2));
    CHECK(archive.records[0].id == L"n1");
    CHECK(archive.records[0].preview == L"first again");
}

WINTOOLS_TEST(NoteArchive_RemoveReportsWhetherItFoundTheId)
{
    NoteArchive archive;
    archive.Add(MakeRecord(L"n1", L"one"));

    CHECK(archive.Remove(L"n1"));
    CHECK(!archive.Remove(L"n1"));
    CHECK(archive.records.empty());
}

WINTOOLS_TEST(NoteArchive_FlattensTabsAndNewlinesInThePreview)
{
    NoteArchive archive;
    archive.Add(MakeRecord(L"n1", L"line one\tline two\nline three"));

    const NoteArchive parsed = ParseNoteArchive(FormatNoteArchive(archive));

    CHECK_EQ(parsed.records.size(), static_cast<std::size_t>(1));
    if (!parsed.records.empty())
    {
        CHECK(parsed.records[0].preview.find(L'\t') == std::wstring::npos);
        CHECK(parsed.records[0].preview.find(L'\n') == std::wstring::npos);
    }
}

WINTOOLS_TEST(NoteArchive_SkipsBadRowsWithoutLosingTheGoodOnes)
{
    const NoteArchive archive = ParseNoteArchive(
        L"# comment\n"
        L"closed\tn1\t0\t0\t360\t300\t2\tok\t11-17 02:31\n"
        L"closed\t../escape\t0\t0\t360\t300\t2\tbad id\t11-17 02:31\n"
        L"closed\tn2\t0\t0\tjunk\t300\t2\tbad geometry\t11-17 02:31\n"
        L"closed\tn3\t10\t20\t360\t300\t9\tclamped\t11-17 02:31\n");

    CHECK_EQ(archive.records.size(), static_cast<std::size_t>(2));
    if (archive.records.size() == 2)
    {
        CHECK(archive.records[0].id == L"n1");
        CHECK(archive.records[1].id == L"n3");
        CHECK_EQ(archive.records[1].theme, 0); // 9 is outside the palette
    }
}

WINTOOLS_TEST(NoteArchive_LoadWithoutAFileIsEmpty)
{
    const ScratchFolder scratch;

    CHECK(NoteArchive::Load().records.empty());
}

WINTOOLS_TEST(NoteArchive_SavesToDiskAndLoadsBack)
{
    const ScratchFolder scratch;

    NoteArchive archive;
    archive.Add(MakeRecord(L"n7", L"saved to disk"));
    CHECK(archive.Save());

    const NoteArchive loaded = NoteArchive::Load();
    CHECK_EQ(loaded.records.size(), static_cast<std::size_t>(1));
    if (!loaded.records.empty())
    {
        CHECK(loaded.records[0].id == L"n7");
        CHECK(loaded.records[0].preview == L"saved to disk");
    }
}
