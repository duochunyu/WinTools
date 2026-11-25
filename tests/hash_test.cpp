#include "test_framework.h"

#include "platform/win/hash.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace
{

fs::path Scratch()
{
    const fs::path root = fs::temp_directory_path() / L"WinTools-hash-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

} // namespace

WINTOOLS_TEST(Hash_Sha256OfAbcMatchesVector)
{
    std::string hex;
    CHECK(wintools::platform::win::Sha256Bytes("abc", hex));
    CHECK_EQ(hex,
        std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

WINTOOLS_TEST(Hash_Sha256OfEmptyMatchesVector)
{
    std::string hex;
    CHECK(wintools::platform::win::Sha256Bytes("", hex));
    CHECK_EQ(hex,
        std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

WINTOOLS_TEST(Hash_Sha256FileStreamsTheWholeFile)
{
    const fs::path file = Scratch() / L"blob.bin";
    {
        std::ofstream out(file, std::ios::binary);
        out.write("hello", 5);
    }

    std::string hex;
    CHECK(wintools::platform::win::Sha256File(file, hex));
    CHECK_EQ(hex,
        std::string("2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"));
}

WINTOOLS_TEST(Hash_Sha256FileFailsForMissingFile)
{
    std::string hex = "unchanged";
    CHECK(!wintools::platform::win::Sha256File(Scratch() / L"missing.bin", hex));
}
