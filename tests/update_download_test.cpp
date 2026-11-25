#include "test_framework.h"

#include "core/update_download.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace
{

constexpr const char* kHelloHash =
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
constexpr const wchar_t* kHelloHashWide =
    L"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

wintools::core::UpdateFeed Feed()
{
    wintools::core::UpdateFeed feed;
    feed.version = *wintools::core::AppVersion::Parse(L"0.3.0");
    feed.size = 5;
    feed.sha256 = kHelloHashWide;
    feed.url = L"http://mirror-a/update.exe";
    feed.urlGithub = L"http://mirror-b/update.exe";
    return feed;
}

fs::path Scratch()
{
    const fs::path root = fs::temp_directory_path() / L"WinTools-download-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

bool Write(const fs::path& target, const std::string& bytes)
{
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::string Read(const fs::path& target)
{
    std::ifstream in(target, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

wintools::core::HashFile CorrectHash()
{
    return [](const fs::path&, std::string& hex)
    {
        hex = kHelloHash;
        return true;
    };
}

} // namespace

WINTOOLS_TEST(Download_MirrorsKeepFeedOrderAndDropDuplicates)
{
    const auto feed = Feed();
    CHECK_EQ(wintools::core::DownloadUrls(feed).size(), 2u);

    auto duplicate = feed;
    duplicate.urlGithub = duplicate.url;
    CHECK_EQ(wintools::core::DownloadUrls(duplicate).size(), 1u);
}

WINTOOLS_TEST(Download_FilePathCarriesTheVersion)
{
    const fs::path path = wintools::core::UpdateFilePath(Feed());
    CHECK_EQ(path.filename().wstring(), std::wstring(L"WinTools-0.3.0.exe.new"));
    CHECK_EQ(path.parent_path().filename().wstring(), std::wstring(L"update"));
}

WINTOOLS_TEST(Download_FallsBackAfterAFailedMirror)
{
    const fs::path target = Scratch() / L"fallback.exe.new";
    int calls = 0;

    const auto fetch = [&calls](std::wstring_view url, const fs::path& path, std::string& error)
    {
        ++calls;
        if (url.find(L"mirror-a") != std::wstring_view::npos)
        {
            Write(path, "partial");
            error = "mirror down";
            return false;
        }
        return Write(path, "hello");
    };

    const auto result = wintools::core::DownloadUpdate(fetch, CorrectHash(), Feed(), target);
    CHECK(result.ok);
    CHECK_EQ(calls, 2);
    CHECK(fs::exists(target));
    CHECK_EQ(Read(target), std::string("hello"));
}

WINTOOLS_TEST(Download_RejectsWrongSizeAndLeavesNoFile)
{
    const fs::path target = Scratch() / L"wrong-size.exe.new";
    auto feed = Feed();
    feed.size = 4;

    const auto fetch = [](std::wstring_view, const fs::path& path, std::string&)
    { return Write(path, "hello"); };

    const auto result = wintools::core::DownloadUpdate(fetch, CorrectHash(), feed, target);
    CHECK(!result.ok);
    CHECK(!fs::exists(target));
    CHECK(result.message.find(L"size") != std::wstring::npos);
}

WINTOOLS_TEST(Download_RejectsWrongHashAndLeavesNoFile)
{
    const fs::path target = Scratch() / L"wrong-hash.exe.new";
    const auto fetch = [](std::wstring_view, const fs::path& path, std::string&)
    { return Write(path, "hello"); };
    const auto badHash = [](const fs::path&, std::string& hex)
    {
        hex = std::string(64, '0');
        return true;
    };

    const auto result = wintools::core::DownloadUpdate(fetch, badHash, Feed(), target);
    CHECK(!result.ok);
    CHECK(!fs::exists(target));
    CHECK(result.message.find(L"SHA-256") != std::wstring::npos);
}

WINTOOLS_TEST(Download_NoMirrorLeavesNoFile)
{
    const fs::path target = Scratch() / L"offline.exe.new";
    const auto fetch = [](std::wstring_view, const fs::path& path, std::string& error)
    {
        Write(path, "partial");
        error = "offline";
        return false;
    };

    const auto result = wintools::core::DownloadUpdate(fetch, CorrectHash(), Feed(), target);
    CHECK(!result.ok);
    CHECK(!fs::exists(target));
}
