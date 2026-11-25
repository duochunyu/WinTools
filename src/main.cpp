#include <Windows.h>
#include <shellapi.h>

#include <string>
#include <string_view>

#include "app/cli.h"
#include "app/commands.h"
#include "app/settings.h"
#include "app/updater.h"
#include "core/update_check.h"
#include "platform/win/com.h"
#include "platform/win/http.h"
#include "util/i18n.h"
#include "util/log.h"
#include "util/version.h"

namespace
{

/// Borrows the console of whatever started us, when there is one.
///
/// The executable is built for the GUI subsystem on purpose: a console binary flashes a black
/// window every time it is started from Explorer, and this app is normally started from
/// Explorer. The cost is that a GUI binary gets no std handles even when the parent is cmd, so
/// they are wired up here; when there is no parent console (the Explorer case) AttachConsole
/// fails and log output is simply discarded.
void AttachParentConsole()
{
    if (!::AttachConsole(ATTACH_PARENT_PROCESS))
    {
        return;
    }

    const HANDLE console = ::CreateFileW(L"CONOUT$", GENERIC_WRITE | GENERIC_READ,
        FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);

    if (console == INVALID_HANDLE_VALUE)
    {
        return;
    }

    ::SetStdHandle(STD_OUTPUT_HANDLE, console);
    ::SetStdHandle(STD_ERROR_HANDLE, console);
}

void ReportStartupFailure(std::wstring_view message)
{
    ::MessageBoxW(nullptr, std::wstring{ wintools::util::i18n::Text(message) }.c_str(),
        L"WinTools", MB_ICONERROR | MB_OK);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    AttachParentConsole();

    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr)
    {
        return wintools::app::kBadArguments;
    }

    // The updater runs from a copy of this exe in %TEMP%. It must be recognised before settings,
    // COM and the single-instance mutex, or it would just bring the old process to the front and
    // exit without ever replacing the file.
    if (argc >= 4 && std::wstring_view{ argv[1] } == L"--apply-update")
    {
        const int result = wintools::app::RunUpdater(argv[2], argv[3], argv[4]);
        ::LocalFree(argv);
        return result;
    }

    // Before the first log line and before any window is created: CLI help, the tray menu and
    // every text measurement in the panel read the language as soon as they start.
    const wintools::app::Settings settings = wintools::app::Settings::Load();
    wintools::util::i18n::SetLanguage(
        static_cast<wintools::util::i18n::Language>(settings.language));

    const auto parsed = wintools::app::ParseCommandLine(argc, argv);
    ::LocalFree(argv);

    if (!parsed.options.has_value())
    {
        wintools::util::LogError(L"{}", parsed.error);
        wintools::app::PrintUsage();
        return wintools::app::kBadArguments;
    }

    const wintools::app::Options& options = *parsed.options;

    if (options.command == wintools::app::Command::Help)
    {
        wintools::app::PrintUsage();
        return wintools::app::kSuccess;
    }

    // Before COM: printing a number needs no desktop, and update checks will use this value.
    if (options.command == wintools::app::Command::Version)
    {
        // wformat_string's checker rejects a raw wchar_t[] argument on MSVC; a view is unambiguous.
        wintools::util::LogInfo(L"{}", std::wstring_view{ wintools::kVersion });
        return wintools::app::kSuccess;
    }

    if (options.command == wintools::app::Command::CheckUpdate)
    {
        const auto current = wintools::core::AppVersion::Parse(wintools::kVersion);
        if (!current)
        {
            wintools::util::LogError(L"built-in version is malformed: {}", wintools::kVersion);
            return wintools::app::kEnvironmentProblem;
        }

        const auto check = wintools::core::CheckForUpdate(
            [](std::wstring_view url, std::string& body)
            {
                const auto response = wintools::platform::win::HttpGetText(url);
                if (!response.ok)
                {
                    wintools::util::LogWarn(L"update source failed: {}", response.error);
                    return false;
                }
                body = response.body;
                return true;
            },
            options.source, *current);

        wintools::util::LogInfo(L"{}", check.message);
        if (check.error)
        {
            return wintools::app::kRuntimeFailure;
        }
        if (check.available && check.feed)
        {
            wintools::util::LogInfo(L"from {} to {}", current->Text(), check.feed->version.Text());
            wintools::util::LogInfo(L"size={} sha256={}", check.feed->size, check.feed->sha256);
        }
        return wintools::app::kSuccess;
    }

    const wintools::platform::ComApartment com;
    if (!com.IsValid())
    {
        wintools::util::LogError(L"COM 初始化失败，无法访问 shell 接口");

        if (options.command == wintools::app::Command::Tray)
        {
            ReportStartupFailure(L"无法初始化 COM，壁纸和窗口功能都无法使用。");
        }

        return wintools::app::kEnvironmentProblem;
    }

    switch (options.command)
    {
        case wintools::app::Command::Monitors:
            return wintools::app::RunMonitors();

        case wintools::app::Command::Dump:
            return wintools::app::RunDump(options.image);

        case wintools::app::Command::Span:
            return wintools::app::RunSpan(options.image, options.dryRun);

        case wintools::app::Command::Restore:
            return wintools::app::RunRestore();

        case wintools::app::Command::Windows:
            return wintools::app::RunWindows();

        case wintools::app::Command::Capture:
            return wintools::app::RunCapture();

        case wintools::app::Command::Apply:
            return wintools::app::RunApply();

        case wintools::app::Command::Show:
            return wintools::app::RunShow();

        case wintools::app::Command::Tray:
            return wintools::app::RunTray(options.showPanel);

        case wintools::app::Command::Help:
            break;
    }

    wintools::app::PrintUsage();
    return wintools::app::kSuccess;
}
