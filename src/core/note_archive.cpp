#include "core/note_archive.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>

#include "core/note_store.h"
#include "core/note_theme.h"
#include "util/log.h"
#include "util/path.h"
#include "util/text.h"

namespace wintools::core
{

namespace
{

constexpr wchar_t kHeader[] = L"# WinTools note archive\n";
constexpr wchar_t kKey[] = L"closed";

std::wstring Trim(std::wstring_view text)
{
    const auto isSpace = [](wchar_t ch)
    { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; };

    std::size_t first = 0;
    while (first < text.size() && isSpace(text[first]))
    {
        ++first;
    }

    std::size_t last = text.size();
    while (last > first && isSpace(text[last - 1]))
    {
        --last;
    }

    return std::wstring{ text.substr(first, last - first) };
}

bool IsSafeId(std::wstring_view id)
{
    if (id.empty() || id.size() > 32)
    {
        return false;
    }

    return std::all_of(id.begin(), id.end(), [](wchar_t ch)
        {
            return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z')
                || (ch >= L'A' && ch <= L'Z') || ch == L'_' || ch == L'-';
        });
}

bool ToInt(std::wstring_view text, int& out)
{
    try
    {
        out = std::stoi(std::wstring{ text });
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::filesystem::path g_folderOverride;

} // namespace

std::filesystem::path NoteArchive::FilePath()
{
    const std::filesystem::path folder =
        g_folderOverride.empty() ? NoteStore::Folder() : g_folderOverride;
    return folder / L"archive.txt";
}

void NoteArchive::SetFolder(std::filesystem::path folder)
{
    g_folderOverride = std::move(folder);
}

NoteArchive NoteArchive::Load()
{
    const std::filesystem::path file = FilePath();
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return NoteArchive{};
    }

    const std::string utf8{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    return ParseNoteArchive(util::FromUtf8(utf8));
}

NoteArchive ParseNoteArchive(std::wstring_view text)
{
    NoteArchive archive;

    std::size_t offset = 0;
    while (offset <= text.size())
    {
        const std::size_t end = text.find(L'\n', offset);
        const std::wstring line = Trim(text.substr(
            offset, end == std::wstring_view::npos ? std::wstring_view::npos : end - offset));
        offset = (end == std::wstring_view::npos) ? text.size() + 1 : end + 1;

        if (line.empty() || line.starts_with(L"#"))
        {
            continue;
        }

        // closed<TAB>id<TAB>x<TAB>y<TAB>w<TAB>h<TAB>theme<TAB>preview<TAB>closedAt<TAB>starred
        // The last three fields are free text or optional and may be absent in older files.
        std::wistringstream fields(line);
        std::wstring kind;
        std::wstring id;
        std::wstring x;
        std::wstring y;
        std::wstring w;
        std::wstring h;
        std::wstring theme;
        if (!std::getline(fields, kind, L'\t') || kind != kKey
            || !std::getline(fields, id, L'\t') || !IsSafeId(id))
        {
            util::LogWarn(L"note archive: skipping a row with an unusable id");
            continue;
        }

        ArchiveRecord record;
        record.id = id;
        if (!std::getline(fields, x, L'\t') || !std::getline(fields, y, L'\t')
            || !std::getline(fields, w, L'\t') || !std::getline(fields, h, L'\t')
            || !std::getline(fields, theme, L'\t')
            || !ToInt(x, record.rect.left) || !ToInt(y, record.rect.top)
            || !ToInt(w, record.rect.width) || !ToInt(h, record.rect.height))
        {
            util::LogWarn(L"note archive: skipping a row with unreadable geometry");
            continue;
        }

        int storedTheme = kNoteThemeDefault;
        if (ToInt(theme, storedTheme))
        {
            record.theme = ClampNoteTheme(storedTheme);
        }

        std::getline(fields, record.preview, L'\t');
        std::getline(fields, record.closedAt, L'\t');

        // Optional trailing field; an archive written before the star existed simply stops here.
        std::wstring starred;
        if (std::getline(fields, starred, L'\t'))
        {
            record.starred = starred == L"1";
        }

        archive.records.push_back(std::move(record));
    }

    return archive;
}

std::wstring FormatNoteArchive(const NoteArchive& archive)
{
    // Tabs and newlines inside a free-text field would split the row on the way back in, so they
    // are replaced before writing. Trimming keeps a hand-edited value from growing without bound.
    const auto flatten = [](std::wstring_view text)
    {
        std::wstring out;
        for (wchar_t ch : text)
        {
            out.push_back(ch == L'\t' || ch == L'\n' || ch == L'\r' ? L' ' : ch);
        }
        return out;
    };

    std::wstring text = kHeader;
    for (const ArchiveRecord& record : archive.records)
    {
        text += std::format(L"{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", kKey, record.id,
            record.rect.left, record.rect.top, record.rect.width, record.rect.height,
            ClampNoteTheme(record.theme), flatten(record.preview), flatten(record.closedAt),
            record.starred ? 1 : 0);
    }

    return text;
}

bool NoteArchive::Save() const
{
    const std::filesystem::path file = FilePath();
    std::error_code ec;
    if (file.has_parent_path())
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec)
        {
            util::LogError(L"cannot create '{}': {}", file.parent_path().wstring(),
                util::FromUtf8(ec.message()));
            return false;
        }
    }

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        util::LogError(L"cannot open '{}' for writing", file.wstring());
        return false;
    }

    const std::string utf8 = util::ToUtf8(FormatNoteArchive(*this));
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return static_cast<bool>(out);
}

void NoteArchive::Add(ArchiveRecord record)
{
    const auto match = std::find_if(records.begin(), records.end(),
        [&record](const ArchiveRecord& item) { return item.id == record.id; });
    if (match != records.end())
    {
        records.erase(match);
    }

    records.insert(records.begin(), std::move(record));
}

const ArchiveRecord* NoteArchive::Find(std::wstring_view id) const
{
    const auto match = std::find_if(records.begin(), records.end(),
        [id](const ArchiveRecord& item) { return item.id == id; });
    return match == records.end() ? nullptr : &*match;
}

bool NoteArchive::Remove(std::wstring_view id)
{
    const auto match = std::find_if(records.begin(), records.end(),
        [id](const ArchiveRecord& item) { return item.id == id; });
    if (match == records.end())
    {
        return false;
    }

    records.erase(match);
    return true;
}

} // namespace wintools::core
