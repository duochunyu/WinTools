#include "app/cli.h"

#include <string_view>

#include "util/log.h"

namespace wintools::app
{

using util::LogInfo;

namespace
{

std::wstring UnknownCommand(std::wstring_view command)
{
    return L"unknown command: " + std::wstring{ command };
}

} // namespace

ParseResult ParseCommandLine(int argc, wchar_t** argv)
{
    // No arguments means "run". Double-clicking the exe is the most common way to start a tray
    // app, and printing help instead would look like a crash to anyone who does it.
    if (argc < 2)
    {
        return ParseResult{ Options{ Command::Tray }, {} };
    }

    const std::wstring_view first = argv[1];

    if (first == L"--tray" || first == L"tray")
    {
        Options options;
        options.command = Command::Tray;
        options.showPanel = false;
        return ParseResult{ options, {} };
    }

    if (first == L"-h" || first == L"--help" || first == L"help")
    {
        return ParseResult{ Options{ Command::Help }, {} };
    }

    if (first == L"monitors")
    {
        return ParseResult{ Options{ Command::Monitors }, {} };
    }

    if (first == L"restore")
    {
        return ParseResult{ Options{ Command::Restore }, {} };
    }

    if (first == L"windows")
    {
        return ParseResult{ Options{ Command::Windows }, {} };
    }

    if (first == L"capture")
    {
        return ParseResult{ Options{ Command::Capture }, {} };
    }

    if (first == L"apply")
    {
        return ParseResult{ Options{ Command::Apply }, {} };
    }

    if (first == L"show")
    {
        return ParseResult{ Options{ Command::Show }, {} };
    }

    if (first == L"span" || first == L"dump")
    {
        Options options;
        options.command = (first == L"span") ? Command::Span : Command::Dump;

        for (int index = 2; index < argc; ++index)
        {
            const std::wstring_view argument = argv[index];

            if (argument == L"--dry-run")
            {
                options.dryRun = true;
            }
            else if (argument.starts_with(L"--"))
            {
                return ParseResult{ std::nullopt, L"unknown option: " + std::wstring{ argument } };
            }
            else if (options.image.empty())
            {
                options.image = std::filesystem::path{ std::wstring{ argument } };
            }
            else
            {
                return ParseResult{ std::nullopt, L"unexpected argument: " + std::wstring{ argument } };
            }
        }

        if (options.image.empty())
        {
            return ParseResult{ std::nullopt, L"missing image path" };
        }

        return ParseResult{ options, {} };
    }

    return ParseResult{ std::nullopt, UnknownCommand(first) };
}

void PrintUsage()
{
    LogInfo(L"WinTools - 多显示器壁纸与窗口布局工具");
    LogInfo(L"");
    LogInfo(L"用法:");
    LogInfo(L"  WinTools.exe monitors                    列出显示器及其虚拟桌面矩形");
    LogInfo(L"  WinTools.exe windows                     列出所有可管理的窗口");
    LogInfo(L"");
    LogInfo(L"  壁纸:");
    LogInfo(L"  WinTools.exe dump <图片>                  只计算切片参数，不写文件不改壁纸");
    LogInfo(L"  WinTools.exe span <图片> [--dry-run]      把一张图拼接铺满所有显示器");
    LogInfo(L"  WinTools.exe restore                      恢复应用前保存的壁纸状态");
    LogInfo(L"");
    LogInfo(L"  窗口布局:");
    LogInfo(L"  WinTools.exe capture                      把当前所有窗口位置存成一份布局");
    LogInfo(L"  WinTools.exe show                         查看已保存的布局");
    LogInfo(L"  WinTools.exe apply                        把窗口按保存的布局摆回去");
    LogInfo(L"");
    LogInfo(L"  不带参数启动则常驻托盘并打开设置面板；--tray 只进托盘，不弹面板（开机启动用）。");
    LogInfo(L"");
    LogInfo(L"说明:");
    LogInfo(L"  span 会在 %LOCALAPPDATA%\\WinTools\\wallpaper\\pieces 生成每台显示器一张切片，");
    LogInfo(L"  并把壁纸位置模式设为“居中”（切片尺寸已等于显示器分辨率，任何缩放都会二次重采样）。");
    LogInfo(L"  apply 只移动当前已经在运行的窗口，不启动任何程序；某台显示器不在线时，");
    LogInfo(L"  属于它的窗口保持原位，等那台显示器回来再摆。");
}

} // namespace wintools::app
