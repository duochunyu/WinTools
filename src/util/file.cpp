#include "util/file.h"

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "util/log.h"
#include "util/text.h"

namespace wintools::util
{
namespace
{

// A static counter makes concurrent writes to *different* files use different temp names too,
// which keeps a failed write from deleting another thread's temp file. Same file from two
// threads is a caller bug; the process id keeps two processes from colliding.
std::atomic<unsigned long> g_tempSequence{ 0 };

std::filesystem::path TempPathFor(const std::filesystem::path& file)
{
    const auto sequence = g_tempSequence.fetch_add(1, std::memory_order_relaxed);
    const auto name = std::format(L"{}.{:08x}.{:08x}.tmp",
        file.filename().wstring(), ::GetCurrentProcessId(), sequence);
    return file.parent_path() / name;
}

} // namespace

bool AtomicWriteBytes(const std::filesystem::path& file, std::string_view bytes)
{
    std::error_code ec;
    if (file.has_parent_path() && !file.parent_path().empty())
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec)
        {
            LogError(L"cannot create '{}': {}", file.parent_path().wstring(),
                FromUtf8(ec.message()));
            return false;
        }
    }

    const std::filesystem::path temp = TempPathFor(file);
    {
        // Truncate is harmless here: temp is a fresh, unique name. The target is never opened
        // for writing, which is the whole point.
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            LogError(L"cannot open temp '{}' for writing", temp.filename().wstring());
            return false;
        }

        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();

        // Flush before the rename: a successful MoveFileEx must never publish a file whose
        // bytes are still in the CRT buffer.
        if (!out)
        {
            LogError(L"cannot write temp '{}'", temp.filename().wstring());
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
        out.close();
    }

    // WRITE_THROUGH asks the OS to flush its own cache before returning. It costs a little on
    // each save, but a settings file that survives a power cut is worth more than a fast save.
    if (!::MoveFileExW(temp.c_str(), file.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const DWORD error = ::GetLastError();
        LogError(L"cannot replace '{}': {}", file.filename().wstring(),
            FormatHresult(static_cast<long>(error)));
        std::filesystem::remove(temp, ec);
        return false;
    }

    return true;
}

bool AtomicWrite(const std::filesystem::path& file, std::wstring_view text)
{
    // One conversion here, at the boundary where text really is UTF-16. Stores pass wide text;
    // RTF bodies call AtomicWriteBytes directly so their bytes are never round-tripped.
    return AtomicWriteBytes(file, ToUtf8(text));
}

} // namespace wintools::util
