#pragma once

#include <format>
#include <string>
#include <string_view>

namespace wintools::util::i18n
{

/// 0 = follow the system UI language, 1 = Simplified Chinese, 2 = English.
enum class Language
{
    Auto = 0,
    Chinese = 1,
    English = 2,
};

void SetLanguage(Language language);
[[nodiscard]] Language CurrentLanguage();

/// Resolves Auto against GetUserDefaultUILanguage(): Chinese on a zh-* system,
/// English everywhere else. Every lookup goes through this, so a translated
/// string is never measured with the Chinese width and drawn with the English one.
[[nodiscard]] bool IsEnglish();

/// Returns the English counterpart of a Simplified Chinese UI string, or the
/// original string when no entry exists. Missing entries degrade to Chinese
/// rather than to a blank label, so coverage can grow incrementally.
[[nodiscard]] std::wstring_view Text(std::wstring_view chinese);

/// Same as Text, but guaranteed null-terminated for Win32 APIs such as
/// AppendMenuW. All callers pass wide string literals, and every table value
/// is a wide string literal, so the view always points into a terminated string.
[[nodiscard]] const wchar_t* Cstr(std::wstring_view chinese);

/// std::format equivalent that translates the format string first. The Chinese
/// source doubles as the lookup key, so no separate string IDs are needed.
template <typename... Args>
[[nodiscard]] std::wstring Format(std::wstring_view chinese, Args&&... args)
{
    return std::vformat(Text(chinese), std::make_wformat_args(args...));
}

/// Text for the language row, already in the active language.
[[nodiscard]] std::wstring_view LanguageLabel();

/// Compact caption for the title-bar language button: the language actually in use, not the
/// mode used to choose it. Auto resolves against the system UI language exactly as Text() does.
[[nodiscard]] inline std::wstring_view LanguageShortLabel()
{
    return IsEnglish() ? L"EN" : L"CN";
}

} // namespace wintools::util::i18n
