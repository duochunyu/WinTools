#include "app/note_manager.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>

#include "core/note_layout.h"
#include "core/note_theme.h"
#include "util/log.h"
#include "util/text.h"

namespace wintools::app
{

NoteManager::~NoteManager()
{
    // Windows are destroyed explicitly rather than by their own destructors, so the bodies are
    // flushed to disk while the message loop is still alive.
    for (const auto& note : m_notes)
    {
        if (note != nullptr)
        {
            note->Destroy();
        }
    }
}

void NoteManager::Start(HINSTANCE instance)
{
    m_instance = instance;
    m_store = core::NoteStore::Load();
    m_archive = core::NoteArchive::Load();

    const std::vector<core::MonitorSnapshot> monitors = core::CaptureMonitors();

    util::LogInfo(L"sticky notes: {} note(s) in the index", m_store.notes.size());

    for (const core::NoteRecord& record : m_store.notes)
    {
        auto note = std::make_unique<Note>();
        const core::Rect rect = core::ClampNote(record.rect, monitors);

        if (!note->Create(m_instance, record.id, rect, record.theme))
        {
            util::LogWarn(L"sticky notes: {} could not be reopened, keeping its text on disk",
                record.id);
            continue;
        }

        note->moved = [this](Note& subject) { OnMoved(subject); };
        note->closeRequested = [this](Note& subject) { OnClosed(subject); };
        note->themeChanged = [this](Note& subject) { OnThemeChanged(subject); };

        note->LoadBody();
        m_notes.push_back(std::move(note));
    }
}

void NoteManager::NewNote()
{
    const std::vector<core::MonitorSnapshot> monitors = core::CaptureMonitors();

    core::NoteRecord record;

    // New notes walk the palette instead of always being yellow: a desktop with four notes on it
    // is already a set of categories, and handing them out in order says so before the user has
    // touched anything. Taken from the counter rather than the note count, so deleting a note
    // never makes the next one repeat a colour that is still on screen.
    const int ordinal = m_store.nextId;
    record.id = m_store.AllocateId();
    record.rect = core::DefaultNoteRect(monitors, static_cast<int>(m_store.notes.size()));
    record.theme = (ordinal - 1) % core::kNoteThemeCount;

    auto note = std::make_unique<Note>();
    if (!note->Create(m_instance, record.id, record.rect, record.theme))
    {
        util::LogError(L"sticky notes: a new note could not be created");
        return;
    }

    note->moved = [this](Note& subject) { OnMoved(subject); };
    note->closeRequested = [this](Note& subject) { OnClosed(subject); };
    note->themeChanged = [this](Note& subject) { OnThemeChanged(subject); };

    m_store.notes.push_back(record);
    Persist();

    m_notes.push_back(std::move(note));
    m_notes.back()->Focus();
}

void NoteManager::ShowAll()
{
    for (const auto& note : m_notes)
    {
        if (note != nullptr)
        {
            note->Focus();
        }
    }
}

bool NoteManager::Reopen(std::wstring_view id)
{
    const core::ArchiveRecord* record = m_archive.Find(id);
    if (record == nullptr)
    {
        util::LogWarn(L"sticky notes: cannot reopen {}, not in the archive", id);
        return false;
    }

    const std::wstring noteId{ id };
    const std::filesystem::path retired = m_store.Folder() / (noteId + L".deleted.rtf");
    const std::filesystem::path body = m_store.BodyPath(noteId);
    std::error_code ec;
    if (std::filesystem::exists(retired, ec))
    {
        std::filesystem::rename(retired, body, ec);
        if (ec)
        {
            util::LogError(L"sticky notes: could not restore {}: {}", id,
                util::FromUtf8(ec.message()));
            return false;
        }
    }
    // An empty note has no retired body; reopening it simply starts with blank text.

    const std::vector<core::MonitorSnapshot> monitors = core::CaptureMonitors();
    const core::Rect rect = core::ClampNote(record->rect, monitors);

    auto note = std::make_unique<Note>();
    if (!note->Create(m_instance, noteId, rect, record->theme))
    {
        // Put the body back where it came from: a window that failed to create must not cost the
        // user the note they asked to reopen.
        std::error_code back;
        std::filesystem::rename(body, retired, back);
        util::LogError(L"sticky notes: {} could not be reopened", id);
        return false;
    }

    note->moved = [this](Note& subject) { OnMoved(subject); };
    note->closeRequested = [this](Note& subject) { OnClosed(subject); };
    note->themeChanged = [this](Note& subject) { OnThemeChanged(subject); };
    note->LoadBody();

    core::NoteRecord live;
    live.id = noteId;
    live.rect = rect;
    live.theme = record->theme;
    m_store.notes.push_back(std::move(live));
    Persist();

    m_archive.Remove(id);
    if (!m_archive.Save())
    {
        util::LogError(L"sticky notes: the archive could not be written after reopening");
    }
    if (archiveChanged)
    {
        archiveChanged();
    }

    m_notes.push_back(std::move(note));
    m_notes.back()->Focus();
    util::LogInfo(L"sticky note {} reopened from the archive", id);
    return true;
}

bool NoteManager::Forget(std::wstring_view id)
{
    if (!m_archive.Remove(id))
    {
        return false;
    }

    std::error_code ec;
    std::filesystem::remove(m_store.Folder() / (std::wstring{ id } + L".deleted.rtf"), ec);
    if (!m_archive.Save())
    {
        util::LogError(L"sticky notes: the archive could not be written after forgetting");
    }
    if (archiveChanged)
    {
        archiveChanged();
    }

    util::LogInfo(L"sticky note {} forgotten from the archive", id);
    return true;
}

void NoteManager::SyncMonitors(std::span<const core::MonitorSnapshot> monitors)
{
    bool changed = false;

    for (const auto& note : m_notes)
    {
        if (note == nullptr)
        {
            continue;
        }

        const core::Rect clamped = core::ClampNote(note->Rect(), monitors);
        if (clamped == note->Rect())
        {
            continue;
        }

        note->SetRect(clamped);
        changed = true;

        for (core::NoteRecord& record : m_store.notes)
        {
            if (record.id == note->Id())
            {
                record.rect = clamped;
                break;
            }
        }
    }

    // Written immediately: the alternative is an index that still points at the old arrangement if
    // the process dies before the next note is moved.
    if (changed)
    {
        Persist();
    }
}

void NoteManager::SaveAll()
{
    for (const auto& note : m_notes)
    {
        if (note != nullptr)
        {
            note->Flush();
        }
    }
}

void NoteManager::OnMoved(Note& note)
{
    for (core::NoteRecord& record : m_store.notes)
    {
        if (record.id == note.Id())
        {
            record.rect = note.Rect();
            Persist();
            return;
        }
    }
}

void NoteManager::OnThemeChanged(Note& note)
{
    for (core::NoteRecord& record : m_store.notes)
    {
        if (record.id == note.Id())
        {
            record.theme = note.Theme();
            Persist();
            return;
        }
    }
}

void NoteManager::OnClosed(Note& note)
{
    const std::wstring id = note.Id();

    // The text is the user's, not ours: closing a note retires its file instead of deleting it, so
    // a mis-click costs a directory listing to undo rather than the note itself.
    note.Flush();

    // Capture the preview before the window goes away. The record keeps enough to rebuild the note
    // in the same spot with the same paper when the user asks for it back from the panel.
    core::ArchiveRecord record;
    record.id = id;
    record.rect = note.Rect();
    record.theme = note.Theme();
    record.preview = note.Preview();

    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_s(&local, &now);
    record.closedAt = std::format(L"{:02}-{:02} {:02}:{:02}", local.tm_mon + 1, local.tm_mday,
        local.tm_hour, local.tm_min);

    const std::filesystem::path body = m_store.BodyPath(id);
    std::error_code ec;
    if (std::filesystem::exists(body, ec))
    {
        const std::filesystem::path retired = body.parent_path() / (id + L".deleted.rtf");
        std::filesystem::rename(body, retired, ec);
        if (!ec)
        {
            util::LogInfo(L"sticky note {} retired to {}", id, retired.wstring());
        }
        else
        {
            util::LogError(L"sticky note {}: could not retire its body: {}", id,
                util::FromUtf8(ec.message()));
        }
    }

    // Archive even an empty note: a blank sheet the user closed by mistake is still a note they
    // may want back, and there is no body file to retire in that case.
    m_archive.Add(std::move(record));
    if (!m_archive.Save())
    {
        util::LogError(L"sticky notes: the archive could not be written");
    }
    if (archiveChanged)
    {
        archiveChanged();
    }

    m_store.Remove(id);
    Persist();

    const auto match = std::find_if(m_notes.begin(), m_notes.end(),
        [&id](const std::unique_ptr<Note>& candidate)
        { return candidate != nullptr && candidate->Id() == id; });

    if (match == m_notes.end())
    {
        return;
    }

    // Destroying the window from inside its own click handler would tear down the object the
    // handler is running in, so the note is moved out of the vector first and released after the
    // call stack has unwound.
    std::unique_ptr<Note> dying = std::move(*match);
    m_notes.erase(match);
    dying->Destroy();
}

void NoteManager::Persist()
{
    if (!m_store.Save())
    {
        util::LogError(L"sticky notes: the index could not be written");
    }
}

} // namespace wintools::app
