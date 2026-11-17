#include "core/note_store.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>

#include "core/note_theme.h"
#include "util/log.h"
#include "util/path.h"
#include "util/text.h"

namespace wintools::core
{

namespace
{

constexpr wchar_t kHeader[] = L"# WinTools notes\n";
constexpr wchar_t kKeyNext[] = L"next=";
constexpr wchar_t kKeyNote[] = L"note";

std::wstring Trim(std::wstring_view text)
{
    const auto isSpace = [](wchar_t ch)
    {
        return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
    };

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

/// Ids become file names, so anything that could escape the folder or confuse the shell is
/// refused. The generator only ever produces [A-Za-z0-9_], which is why a hand-edited index with
/// something else in it is treated as corruption rather than translated.
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

/// Empty in the app; the tests point it at a scratch folder. See NoteStore::SetFolder.
std::filesystem::path g_folderOverride;

} // namespace

std::filesystem::path NoteStore::Folder()
{
    if (!g_folderOverride.empty())
    {
        return g_folderOverride;
    }

    return util::AppDataFolder() / L"notes";
}

void NoteStore::SetFolder(std::filesystem::path folder)
{
    g_folderOverride = std::move(folder);
}

std::filesystem::path NoteStore::IndexFile()
{
    return Folder() / L"index.txt";
}

NoteStore NoteStore::Load()
{
    const std::filesystem::path file = IndexFile();
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return NoteStore{};
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    return ParseNoteIndex(util::FromUtf8(buffer.str()));
}

NoteStore ParseNoteIndex(std::wstring_view text)
{
    NoteStore store;

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

        if (line.starts_with(kKeyNext))
        {
            ToInt(std::wstring_view{ line }.substr(std::size(kKeyNext) - 1), store.nextId);
            continue;
        }

        // note<TAB>id<TAB>x<TAB>y<TAB>w<TAB>h<TAB>theme
        std::wistringstream fields(line);
        std::wstring kind;
        std::wstring id;
        std::wstring x;
        std::wstring y;
        std::wstring w;
        std::wstring h;
        std::wstring theme;
        if (!std::getline(fields, kind, L'\t') || kind != kKeyNote)
        {
            continue;
        }

        if (!std::getline(fields, id, L'\t') || !IsSafeId(id))
        {
            util::LogWarn(L"note index: skipping a record with an unusable id");
            continue;
        }

        NoteRecord record;
        record.id = id;

        if (!std::getline(fields, x, L'\t') || !std::getline(fields, y, L'\t')
            || !std::getline(fields, w, L'\t') || !std::getline(fields, h, L'\t')
            || !ToInt(x, record.rect.left) || !ToInt(y, record.rect.top)
            || !ToInt(w, record.rect.width) || !ToInt(h, record.rect.height))
        {
            util::LogWarn(L"note index: skipping a record with unreadable geometry");
            continue;
        }

        // Optional, and silently so: every index written before the palette existed stops after h,
        // and a note that is merely older than a feature is not corruption worth a warning.
        int storedTheme = kNoteThemeDefault;
        if (std::getline(fields, theme, L'\t') && ToInt(theme, storedTheme))
        {
            record.theme = ClampNoteTheme(storedTheme);
        }

        store.notes.push_back(std::move(record));
    }

    if (store.nextId < 1)
    {
        store.nextId = 1;
    }

    return store;
}

std::wstring FormatNoteIndex(const NoteStore& store)
{
    std::wstring text = kHeader;
    text += std::format(L"{}{}\n", kKeyNext, store.nextId);

    for (const NoteRecord& note : store.notes)
    {
        text += std::format(L"{}\t{}\t{}\t{}\t{}\t{}\t{}\n", kKeyNote, note.id, note.rect.left,
            note.rect.top, note.rect.width, note.rect.height, ClampNoteTheme(note.theme));
    }

    return text;
}

bool NoteStore::Save() const
{
    const std::filesystem::path file = IndexFile();

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

    const std::string utf8 = util::ToUtf8(FormatNoteIndex(*this));
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return static_cast<bool>(out);
}

std::filesystem::path NoteStore::BodyPath(std::wstring_view id) const
{
    return Folder() / (std::wstring{ id } + L".rtf");
}

std::wstring NoteStore::AllocateId()
{
    return L"n" + std::to_wstring(nextId++);
}

void NoteStore::Remove(std::wstring_view id)
{
    const auto match = std::find_if(notes.begin(), notes.end(),
        [id](const NoteRecord& note) { return note.id == id; });
    if (match == notes.end())
    {
        return;
    }

    std::error_code ec;
    std::filesystem::remove(BodyPath(id), ec);
    notes.erase(match);
}

} // namespace wintools::core
