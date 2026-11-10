#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <vector>

#include "core/composer.h"

using namespace wintools::core;

namespace
{

/// A folder that deletes itself, plus the handful of file helpers these cases need.
///
/// Real files on disk on purpose: what is being tested is which names get unlinked, and a fake
/// filesystem would be testing the fake instead.
class TempFolder
{
public:
    explicit TempFolder(const wchar_t* name)
        : m_path(std::filesystem::temp_directory_path() / name)
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);
    }

    ~TempFolder()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const
    {
        return m_path;
    }

    void Touch(const std::wstring& name) const
    {
        std::ofstream out(m_path / name, std::ios::binary);
        out << "x";
    }

    [[nodiscard]] bool Has(const std::wstring& name) const
    {
        std::error_code ec;
        return std::filesystem::exists(m_path / name, ec);
    }

    [[nodiscard]] int Count() const
    {
        int total = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(m_path, ec))
        {
            (void)entry;
            ++total;
        }

        return total;
    }

private:
    std::filesystem::path m_path;
};

std::vector<RenderedPiece> Keep(const TempFolder& folder, std::initializer_list<const wchar_t*> names)
{
    std::vector<RenderedPiece> pieces;
    for (const wchar_t* name : names)
    {
        pieces.push_back(RenderedPiece{ folder.Path() / name, Slice{}, 0 });
    }

    return pieces;
}

} // namespace

// The whole point: a display that changed resolution or moved leaves its old slice behind, and
// nothing ever references it again.
WINTOOLS_TEST(PieceCache_RemovesSlicesThisRoundDidNotProduce)
{
    TempFolder folder(L"wintools-pieces-stale");
    folder.Touch(L"m22ea05b2_1920_0_1920x1200.png");
    folder.Touch(L"m331166c7_0_0_1920x1080.png"); // previous resolution of the same display
    folder.Touch(L"m39f20861_181_-1440_3440x1440.png");

    const auto keep = Keep(folder, { L"m22ea05b2_1920_0_1920x1200.png",
        L"m39f20861_181_-1440_3440x1440.png" });

    const int removed = RemoveStalePieces(folder.Path(), keep);

    CHECK(removed == 1);
    CHECK(folder.Has(L"m22ea05b2_1920_0_1920x1200.png"));
    CHECK(folder.Has(L"m39f20861_181_-1440_3440x1440.png"));
    CHECK(!folder.Has(L"m331166c7_0_0_1920x1080.png"));
}

// The folder is under the user's profile. Anything that is not a slice name has to survive, even
// if it happens to be a PNG.
WINTOOLS_TEST(PieceCache_LeavesForeignFilesAlone)
{
    TempFolder folder(L"wintools-pieces-foreign");
    folder.Touch(L"m22ea05b2_1920_0_1920x1200.png"); // this round's slice
    folder.Touch(L"holiday.png");
    folder.Touch(L"m22ea05b2_1920_0_1920x1200.png.bak");
    folder.Touch(L"mZZZZZZZZ_0_0_100x100.png");      // not hex
    folder.Touch(L"m22ea05b_0_0_100x100.png");       // seven digits
    folder.Touch(L"m22ea05b2_0_0_100.png");          // no x<height>
    folder.Touch(L"notes.txt");

    const auto keep = Keep(folder, { L"m22ea05b2_1920_0_1920x1200.png" });

    const int removed = RemoveStalePieces(folder.Path(), keep);

    CHECK(removed == 0);
    CHECK(folder.Count() == 7);
    CHECK(folder.Has(L"holiday.png"));
    CHECK(folder.Has(L"mZZZZZZZZ_0_0_100x100.png"));
    CHECK(folder.Has(L"m22ea05b_0_0_100x100.png"));
    CHECK(folder.Has(L"m22ea05b2_0_0_100.png"));
    CHECK(folder.Has(L"notes.txt"));
}

// Negative coordinates are normal - a display left of or above the primary one has them - and the
// name parser must not treat the minus sign as "not ours".
WINTOOLS_TEST(PieceCache_UnderstandsNegativeCoordinates)
{
    TempFolder folder(L"wintools-pieces-negative");
    folder.Touch(L"m39f20861_-181_-1440_3440x1440.png");
    folder.Touch(L"m39f20861_181_-1440_3440x1440.png"); // same display, old position

    const auto keep = Keep(folder, { L"m39f20861_-181_-1440_3440x1440.png" });

    const int removed = RemoveStalePieces(folder.Path(), keep);

    CHECK(removed == 1);
    CHECK(folder.Has(L"m39f20861_-181_-1440_3440x1440.png"));
    CHECK(!folder.Has(L"m39f20861_181_-1440_3440x1440.png"));
}

// A missing folder is the normal state on a fresh profile, and must not be reported as a failure.
WINTOOLS_TEST(PieceCache_MissingFolderIsNotAnError)
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / L"wintools-pieces-does-not-exist";
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);

    const std::vector<RenderedPiece> keep;

    CHECK(RemoveStalePieces(folder, keep) == 0);
}

// Nothing is deleted when the round produced nothing: an empty keep list would otherwise wipe the
// folder that the desktop is still pointing at.
WINTOOLS_TEST(PieceCache_EmptyKeepListWouldStillOnlyTouchSliceNames)
{
    TempFolder folder(L"wintools-pieces-empty-keep");
    folder.Touch(L"m22ea05b2_1920_0_1920x1200.png");
    folder.Touch(L"readme.txt");

    const std::vector<RenderedPiece> keep;

    const int removed = RemoveStalePieces(folder.Path(), keep);

    CHECK(removed == 1);
    CHECK(!folder.Has(L"m22ea05b2_1920_0_1920x1200.png"));
    CHECK(folder.Has(L"readme.txt"));
}
