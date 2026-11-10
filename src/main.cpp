#include <Windows.h>
#include <shellapi.h>

#include <string>
#include <string_view>

#include "app/cli.h"
#include "app/commands.h"
#include "app/settings.h"
#include "platform/win/com.h"
#include "util/i18n.h"
#include "util/log.h"

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

    // Before the first log line and before any window is created: CLI help, the tray menu and
    // every text measurement in the panel read the language as soon as they start.
    const wintools::app::Settings settings = wintools::app::Settings::Load();
    wintools::util::i18n::SetLanguage(
        static_cast<wintools::util::i18n::Language>(settings.language));

    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr)
    {
        return wintools::app::kBadArguments;
    }

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
