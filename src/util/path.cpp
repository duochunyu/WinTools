#include "util/path.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstdint>
#include <format>

namespace wintools::util
{

namespace
{

std::filesystem::path LocalAppData()
{
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)))
    {
        return {};
    }

    std::filesystem::path result{ raw };
    ::CoTaskMemFree(raw);
    return result;
}

} // namespace

std::filesystem::path AppDataFolder()
{
    return LocalAppData() / L"WinTools";
}

std::filesystem::path PicturesFolder()
{
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &raw)))
    {
        return {};
    }

    std::filesystem::path result{ raw };
    ::CoTaskMemFree(raw);
    return result;
}

std::filesystem::path PieceFolder()
{
    return AppDataFolder() / L"wallpaper" / L"pieces";
}

std::filesystem::path StateFile()
{
    return AppDataFolder() / L"wallpaper" / L"state.txt";
}

std::filesystem::path UpdateFolder()
{
    return AppDataFolder() / L"update";
}

std::filesystem::path LogFile()
{
    return AppDataFolder() / L"WinTools.log";
}

std::wstring BuildPieceName(const core::Monitor& monitor, const core::Slice& slice)
{
    // FNV-1a 32 over UTF-16 code units. Must match the C# original:
    //   foreach (char c in monitor.DevicePath) hash = (hash ^ c) * 16777619;
    uint32_t hash = 2166136261u;
    for (const wchar_t ch : monitor.devicePath)
    {
        hash = (hash ^ static_cast<uint32_t>(ch)) * 16777619u;
    }

    return std::format(
        L"m{:08x}_{}_{}_{}x{}.png",
        hash,
        monitor.left,
        monitor.top,
        slice.outputWidth,
        slice.outputHeight);
}

} // namespace wintools::util
