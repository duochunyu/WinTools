#pragma once

#include <Windows.h>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "app/note.h"
#include "core/note_archive.h"
#include "core/note_store.h"
#include "core/window_layout.h"

namespace wintools::app
{

/// Owns every sticky note and the index that remembers them.
///
/// The note windows know how to edit and save themselves; this class owns the lifecycle - which
/// notes exist, where they sit between runs, and what happens when one is closed. Keeping it out
/// of Service matters because Service is already the place where every unrelated decision is made.
class NoteManager
{
public:
    NoteManager() = default;
    ~NoteManager();

    NoteManager(const NoteManager&) = delete;
    NoteManager& operator=(const NoteManager&) = delete;
    NoteManager(NoteManager&&) = delete;
    NoteManager& operator=(NoteManager&&) = delete;

    /// Reads the index, then re-creates its notes when `restore` is true. With it false the text
    /// stays on disk and no window opens; ShowAll materialises them later. Notes whose window
    /// cannot be created are dropped from memory but left on disk either way.
    void Start(HINSTANCE instance, bool restore);

    /// Opens one more note, stacked slightly away from the previous one.
    void NewNote();

    /// Brings every note back to the top of the z-order. Used from the tray, because a note can be
    /// buried by a full-screen application and a toolwindow has no taskbar button to click.
    void ShowAll();

    /// Closed notes, newest first, for the panel's history block.
    [[nodiscard]] const std::vector<core::ArchiveRecord>& Archived() const
    {
        return m_archive.records;
    }

    /// Reopens one archived note with its old text, geometry and paper. Returns false when the id
    /// is not in the archive or its body file has gone missing.
    bool Reopen(std::wstring_view id);

    /// Permanently drops one archived note, including its body. Refuses an unknown id.
    bool Forget(std::wstring_view id);

    /// Raised whenever the archived list changes, so the panel can refresh if it is open.
    std::function<void()> archiveChanged;

    /// Re-clamps every note after the display arrangement changed.
    void SyncMonitors(std::span<const core::MonitorSnapshot> monitors);

    /// Re-reads the paper palette after the panel edited it. `removed` is the palette index that
    /// went away, or -1 when the palette only changed colour: notes on and after a removed paper
    /// shift down one slot, so a note keeps the paper it was wearing instead of the index it had.
    void ApplyPalette(int removed = -1);

    /// The note the user is currently in: the open note that owns the foreground window.
    ///
    /// Null when the focus is somewhere else, which is what the capture hotkey uses to decide
    /// between inserting into a note and saving a file. Deliberately not "the last note that had
    /// the focus": a screenshot taken while working in another application belongs in a file, not
    /// in a note the user left half an hour ago.
    [[nodiscard]] Note* NoteInForeground();

    /// Saves every note body. Called on quit.
    void SaveAll();

    [[nodiscard]] std::size_t Count() const
    {
        return m_notes.size();
    }

private:
    /// Creates a window for every indexed note that does not have one yet. Called at startup when
    /// restore is on, and lazily by ShowAll when it is off.
    void RestoreWindows();

    void Persist();
    void OnMoved(Note& note);
    void OnThemeChanged(Note& note);
    void OnDeleted(Note& note);
    void OnStarChanged(Note& note);
    void OnClosed(Note& note);

    HINSTANCE m_instance{};
    core::NoteStore m_store;
    core::NoteArchive m_archive;
    std::vector<std::unique_ptr<Note>> m_notes;
    bool m_restored{};
};

} // namespace wintools::app
