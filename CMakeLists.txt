cmake_minimum_required(VERSION 3.25)

project(WinTools
    VERSION 0.3.0
    DESCRIPTION "Multi-monitor utilities for Windows: spanning wallpaper, window layout, sticky notes"
    LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR
        "WinTools depends on Windows-only APIs (IDesktopWallpaper, WIC) and the MSVC "
        "toolchain. Configure with a Visual Studio developer prompt.")
endif()

# Single source of truth for the version. Both the C++ code and the VERSIONINFO resource read
# the generated header; nothing else in the tree spells out a version number. A release tag
# vX.Y.Z is checked against this value in CI before anything is built.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/version_macros.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version_macros.h"
    @ONLY)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.manifest.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/app.manifest"
    @ONLY)
set(WINTOOLS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only the icon and the manifest are compiled; RC is enabled for those two.
enable_language(RC)

# Static CRT: the point of this project is a single self-contained .exe that runs on a
# machine with no redistributables installed. Must be set before any target is created.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Shared compile settings
# ---------------------------------------------------------------------------
add_library(wintools_flags INTERFACE)
target_compile_options(wintools_flags INTERFACE
    /W4                 # the useful warning set; /Wall is noise on Windows headers
    /permissive-        # standards conformance, catches template/overload surprises
    /utf-8              # source and execution charset both UTF-8
    /Zc:__cplusplus     # make __cplusplus report the real standard
    /Zc:preprocessor    # conforming preprocessor
    /EHsc
    /WX                 # warnings are errors: this is a small codebase, keep it clean
)
target_compile_definitions(wintools_flags INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

# ---------------------------------------------------------------------------
# Core library: everything that can be tested without a desktop
# ---------------------------------------------------------------------------
add_library(wintools_core STATIC
    src/core/layout_math.cpp
    src/core/composer.cpp
    src/core/desktop_wallpaper.cpp
    src/core/state.cpp
    src/core/window_enum.cpp
    src/core/window_layout.cpp
    src/core/layout_store.cpp
    src/core/wallpaper_store.cpp
    src/core/group.cpp
    src/core/hotkeys.cpp
    src/core/display_watcher.cpp
    src/core/lunar.cpp
    src/core/canvas.cpp
    src/core/calendar_face.cpp
    src/core/note_layout.cpp
    src/core/note_archive.cpp
    src/core/note_store.cpp
    src/core/note_theme.cpp
    src/core/rtf.cpp
    src/core/update_version.cpp
    src/core/update_feed.cpp
    src/core/update_check.cpp
    src/core/update_download.cpp
    src/core/update_snapshot.cpp
    src/platform/win/http.cpp
    src/platform/win/hash.cpp
    src/platform/win/com.cpp
    src/platform/win/wic.cpp
    src/util/i18n.cpp
    src/util/log.cpp
    src/util/file.cpp
    src/util/path.cpp
    src/util/text.cpp
)
target_include_directories(wintools_core PUBLIC src)
target_link_libraries(wintools_core
    PUBLIC  wintools_flags
    PRIVATE bcrypt windowscodecs ole32 oleaut32 shell32 user32 winhttp
)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
# WIN32_EXECUTABLE: no console window flashing on every Explorer launch. main.cpp puts the
# console back when the parent process actually has one, so the CLI still prints.
add_executable(WinTools WIN32
    src/main.cpp
    src/app/cli.cpp
    src/app/commands.cpp
    src/app/actions.cpp
    src/app/settings.cpp
    src/app/startup.cpp
    src/app/tray.cpp
    src/app/panel.cpp
    src/app/service.cpp
    src/app/desktop_canvas.cpp
    src/app/capture.cpp
    src/app/note.cpp
    src/app/note_palette.cpp
    src/app/note_manager.cpp
    src/app/updater.cpp
    assets/app.rc
    # Listed as a source so CMake hands it to mt.exe; the linker already emits a manifest of its
    # own, and passing this one through /MANIFESTINPUT produces a duplicate resource instead.
    "${WINTOOLS_GENERATED_DIR}/app.manifest"
)
target_include_directories(WinTools PRIVATE assets "${WINTOOLS_GENERATED_DIR}")
# cli.cpp is compiled only into WinTools, so the generated header only has to reach that target;
# the version test below reads it through the same include path.
target_link_libraries(WinTools PRIVATE
    wintools_core
    advapi32    # RegOpenKeyEx / RegSetValueEx for the autostart entry
    comdlg32    # ChooseColorW, the system picker behind the note palette's rows
    dwmapi      # rounded window corners on Windows 11
    gdiplus     # anti-aliased sticky-note chrome (icons, page-edge rule, save time)
)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(wintools_tests
    tests/test_main.cpp
    tests/layout_math_test.cpp
    tests/window_layout_test.cpp
    tests/layout_store_test.cpp
    tests/wallpaper_store_test.cpp
    tests/group_test.cpp
    tests/hotkeys_test.cpp
    tests/i18n_test.cpp
    tests/file_test.cpp
    tests/version_test.cpp
    tests/update_version_test.cpp
    tests/update_feed_test.cpp
    tests/update_check_test.cpp
    tests/update_download_test.cpp
    tests/update_snapshot_test.cpp
    tests/hash_test.cpp
    tests/piece_cache_test.cpp
    tests/preview_test.cpp
    tests/panel_view_test.cpp
    tests/settings_test.cpp
    tests/lunar_test.cpp
    tests/canvas_test.cpp
    tests/note_layout_test.cpp
    tests/note_archive_test.cpp
    tests/note_store_test.cpp
    tests/note_theme_test.cpp
    tests/rtf_test.cpp
    # Compiled into the test binary instead of a library of its own: these are the only UI-side
    # translation units with arithmetic worth asserting, and one-file libraries to hold them would
    # cost more than they explain. panel.cpp comes in for the pan/zoom model, which is static and
    # therefore linkable without ever creating a window. settings.cpp is here for the v1/v2
    # header compatibility tests; its SetFileForTests hook keeps those away from real user data.
    src/app/panel.cpp
    src/app/settings.cpp
)
target_include_directories(wintools_tests PRIVATE "${WINTOOLS_GENERATED_DIR}")
target_link_libraries(wintools_tests PRIVATE wintools_core comdlg32 dwmapi gdiplus)
add_test(NAME unit COMMAND wintools_tests)

# Put every binary in one place so scripts and packaging do not have to guess.
set_target_properties(WinTools wintools_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
