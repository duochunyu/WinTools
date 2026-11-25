#include "core/update_snapshot.h"

#include <format>
#include <fstream>
#include <string>
#include <system_error>

#include "util/log.h"

namespace wintools::core
{
namespace
{

bool CopyOne(const std::filesystem::path& source, const std::filesystem::path& destination,
    std::wstring& error)
{
    std::error_code ec;
    if (!std::filesystem::exists(source, ec))
    {
        // A first-run install can legitimately have no saved layout yet. Nothing to snapshot.
        return true;
    }

    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec)
    {
        error = std::format(L"cannot create {}", destination.parent_path().wstring());
        return false;
    }

    std::filesystem::copy_file(source, destination,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        const std::string reason = ec.message();
        error = std::format(L"cannot copy {}: {}", source.wstring(),
            std::wstring{ reason.begin(), reason.end() });
        return false;
    }
    return true;
}

} // namespace

bool SnapshotBeforeUpdate(
    const std::filesystem::path& appData, std::wstring_view fromVersion, std::wstring& error)
{
    const std::filesystem::path backup = appData / L"backup" / std::wstring{ fromVersion };
    std::error_code directoryError;
    std::filesystem::create_directories(backup, directoryError);
    if (directoryError)
    {
        error = std::format(L"cannot create {}", backup.wstring());
        return false;
    }

    const std::filesystem::path files[] = {
        appData / L"settings.txt",
        appData / L"layout.txt",
        appData / L"groups.tsv",
        appData / L"wallpaper" / L"state.txt",
        appData / L"notes" / L"index.txt",
        appData / L"notes" / L"archive.txt",
    };
    for (const auto& file : files)
    {
        if (!CopyOne(file, backup / file.filename(), error))
        {
            return false;
        }
    }

    // Note bodies live beside the index as individual .rtf files. Copy the whole folder so a
    // rollback does not produce an index pointing at missing text.
    const std::filesystem::path notes = appData / L"notes";
    std::error_code ec;
    if (std::filesystem::exists(notes, ec))
    {
        for (const auto& entry : std::filesystem::directory_iterator(notes, ec))
        {
            if (!entry.is_regular_file(ec))
            {
                continue;
            }
            if (!CopyOne(entry.path(), backup / L"notes" / entry.path().filename(), error))
            {
                return false;
            }
        }
    }

    util::LogInfo(L"update snapshot written to {}", backup.wstring());
    return true;
}

} // namespace wintools::core
