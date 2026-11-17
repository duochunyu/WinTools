#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "core/note_store.h"

using namespace wintools::core;

namespace
{

NoteRecord MakeRecord(const wchar_t* id, int left, int top, int width, int height)
{
    NoteRecord record;
    record.id = id;
    record.rect = Rect{ left, top, width, height };
    return record;
}

/// Points the store at an empty scratch folder for the duration of one case, then puts the real
/// one back and deletes the scratch. Save() and Remove() touch real files, so a test that ran
/// against %LOCALAPPDATA%\WinTools\notes would be deleting the user's notes.
class ScratchFolder
{
public:
    ScratchFolder()
    {
        static int counter = 0;
        m_path = std::filesystem::temp_directory_path() / L"wintools-tests"
            / (L"notes-" + std::to_wstring(++counter));

        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);

        NoteStore::SetFolder(m_path);
    }

    ~ScratchFolder()
    {
        NoteStore::SetFolder({});

        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

} // namespace

WINTOOLS_TEST(NoteStore_RoundTripsThroughText)
{
    NoteStore store;
    store.nextId = 4;
    store.notes.push_back(MakeRecord(L"n1", 96, 96, 360, 300));
    store.notes.push_back(MakeRecord(L"n3", -40, 1200, 500, 220));

    const NoteStore parsed = ParseNoteIndex(FormatNoteIndex(store));

    CHECK_EQ(parsed.nextId, 4);
    CHECK_EQ(parsed.notes.size(), static_cast<std::size_t>(2));
    if (parsed.notes.size() == 2)
    {
        CHECK(parsed.notes[0].id == L"n1");
        CHECK(parsed.notes[0].rect == (Rect{ 96, 96, 360, 300 }));
        CHECK(parsed.notes[1].id == L"n3");
        CHECK(parsed.notes[1].rect == (Rect{ -40, 1200, 500, 220 }));
    }
}

WINTOOLS_TEST(NoteStore_MissingFileReadsAsEmpty)
{
    // The first run has no index at all; that must look like "no notes", never like an error.
    const NoteStore store = ParseNoteIndex(L"");

    CHECK_EQ(store.nextId, 1);
    CHECK(store.notes.empty());
}

WINTOOLS_TEST(NoteStore_SkipsRecordsItCannotTrust)
{
    // One bad id, one truncated geometry, one record type from a future version. The good record on
    // the last line must still survive: a corrupt index costs the user one note, not the list.
    const std::wstring text =
        L"# WinTools notes\n"
        L"next=7\n"
        L"note\t../../evil\t0\t0\t360\t300\n"
        L"note\tn2\t11\t22\t360\n"
        L"sticky\tn5\t0\t0\t360\t300\n"
        L"note\tn6\t11\t22\t360\t300\n";

    const NoteStore store = ParseNoteIndex(text);

    CHECK_EQ(store.nextId, 7);
    CHECK_EQ(store.notes.size(), static_cast<std::size_t>(1));
    if (!store.notes.empty())
    {
        CHECK(store.notes[0].id == L"n6");
    }
}

WINTOOLS_TEST(NoteStore_IgnoresBlankLinesCommentsAndSpacing)
{
    const std::wstring text =
        L"\n"
        L"   # a hand edit, indented\n"
        L"\n"
        L"  note\tn1\t10\t20\t360\t300  \r\n"
        L"next=2\r\n";

    const NoteStore store = ParseNoteIndex(text);

    CHECK_EQ(store.notes.size(), static_cast<std::size_t>(1));
    CHECK_EQ(store.nextId, 2);
    if (!store.notes.empty())
    {
        CHECK(store.notes[0].rect == (Rect{ 10, 20, 360, 300 }));
    }
}

WINTOOLS_TEST(NoteStore_KeepsTheCounterFromGoingBackwards)
{
    const NoteStore store = ParseNoteIndex(L"next=0\n");

    // A zero or absent counter would hand out "n0" and then collide with the next run's ids.
    CHECK_EQ(store.nextId, 1);
}

WINTOOLS_TEST(NoteStore_AllocatesSequentialIds)
{
    NoteStore store;
    store.nextId = 9;

    CHECK(store.AllocateId() == L"n9");
    CHECK(store.AllocateId() == L"n10");
    CHECK_EQ(store.nextId, 11);
}

WINTOOLS_TEST(NoteStore_RemoveForgetsTheRecordAndItsBody)
{
    const ScratchFolder scratch;

    NoteStore store;
    store.notes.push_back(MakeRecord(L"n1", 0, 0, 360, 300));
    store.notes.push_back(MakeRecord(L"n2", 0, 0, 360, 300));

    // A body file for each, so the file deletion is actually exercised.
    for (const wchar_t* id : { L"n1", L"n2" })
    {
        std::ofstream out(store.BodyPath(id), std::ios::binary);
        out << "{\\rtf1}";
    }

    CHECK(std::filesystem::exists(store.BodyPath(L"n1")));
    CHECK(store.BodyPath(L"n1").parent_path() == scratch.Path());

    store.Remove(L"n1");

    CHECK_EQ(store.notes.size(), static_cast<std::size_t>(1));
    CHECK(store.notes[0].id == L"n2");
    CHECK(!std::filesystem::exists(store.BodyPath(L"n1")));
    CHECK(std::filesystem::exists(store.BodyPath(L"n2")));

    // Removing something that is not there is a no-op, not a crash.
    store.Remove(L"n9");
    CHECK_EQ(store.notes.size(), static_cast<std::size_t>(1));
}

WINTOOLS_TEST(NoteStore_SavesAndLoadsThroughTheRealFiles)
{
    const ScratchFolder scratch;

    NoteStore store;
    store.nextId = 3;
    store.notes.push_back(MakeRecord(L"n1", 96, 96, 360, 300));
    store.notes.push_back(MakeRecord(L"n2", 1920, -40, 500, 220));

    CHECK(store.Save());
    CHECK(std::filesystem::exists(NoteStore::IndexFile()));

    const NoteStore loaded = NoteStore::Load();

    CHECK_EQ(loaded.nextId, 3);
    CHECK_EQ(loaded.notes.size(), static_cast<std::size_t>(2));
    if (loaded.notes.size() == 2)
    {
        CHECK(loaded.notes[1].id == L"n2");
        CHECK(loaded.notes[1].rect == (Rect{ 1920, -40, 500, 220 }));
    }

    // Save() creates the folder: the first note ever created has no notes directory to write into.
    std::error_code ec;
    std::filesystem::remove_all(NoteStore::Folder(), ec);
    CHECK(!std::filesystem::exists(NoteStore::Folder()));
    CHECK(store.Save());
    CHECK(std::filesystem::exists(NoteStore::IndexFile()));
}

WINTOOLS_TEST(NoteStore_KeepsThePaperColour)
{
    NoteStore store;
    store.notes.push_back(MakeRecord(L"n1", 0, 0, 360, 300));
    store.notes.push_back(MakeRecord(L"n2", 0, 0, 360, 300));
    store.notes[0].theme = 3;
    store.notes[1].theme = 5;

    const NoteStore parsed = ParseNoteIndex(FormatNoteIndex(store));

    CHECK_EQ(parsed.notes.size(), static_cast<std::size_t>(2));
    if (parsed.notes.size() == 2)
    {
        CHECK_EQ(parsed.notes[0].theme, 3);
        CHECK_EQ(parsed.notes[1].theme, 5);
    }
}

WINTOOLS_TEST(NoteStore_ReadsAnIndexWrittenBeforeThePaletteExisted)
{
    // Six fields, no colour: every note the user already has looks like this. It has to load, and
    // it has to load as the yellow all of them were.
    const NoteStore store = ParseNoteIndex(L"next=2\nnote\tn1\t10\t20\t360\t300\n");

    CHECK_EQ(store.notes.size(), static_cast<std::size_t>(1));
    if (!store.notes.empty())
    {
        CHECK_EQ(store.notes[0].theme, 0);
        CHECK(store.notes[0].rect == (Rect{ 10, 20, 360, 300 }));
    }
}

WINTOOLS_TEST(NoteStore_FallsBackWhenTheColourIsUnusable)
{
    const NoteStore store = ParseNoteIndex(L"note\tn1\t0\t0\t360\t300\tpurple\n"
                                           L"note\tn2\t0\t0\t360\t300\t77\n"
                                           L"note\tn3\t0\t0\t360\t300\t2\n");

    // A word, a number past the end of the palette, and one that is simply right: the first two
    // cost a colour, the third is honoured, and none of them costs the note.
    CHECK_EQ(store.notes.size(), static_cast<std::size_t>(3));
    if (store.notes.size() == 3)
    {
        CHECK_EQ(store.notes[0].theme, 0);
        CHECK_EQ(store.notes[1].theme, 0);
        CHECK_EQ(store.notes[2].theme, 2);
    }
}

WINTOOLS_TEST(NoteStore_LoadWithoutAnIndexIsAnEmptyStore)
{
    const ScratchFolder scratch;

    const NoteStore store = NoteStore::Load();

    CHECK_EQ(store.nextId, 1);
    CHECK(store.notes.empty());
}
