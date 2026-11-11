#include "test_framework.h"

#include <string>
#include <string_view>

#include "util/i18n.h"

namespace
{

using wintools::util::i18n::Language;

/// Every test leaves the global language back at Auto so it cannot change how a later
/// test in the same binary resolves a string.
struct LanguageScope
{
    explicit LanguageScope(Language language)
    {
        wintools::util::i18n::SetLanguage(language);
    }

    ~LanguageScope()
    {
        wintools::util::i18n::SetLanguage(Language::Auto);
    }

    LanguageScope(const LanguageScope&) = delete;
    LanguageScope& operator=(const LanguageScope&) = delete;
};

} // namespace

WINTOOLS_TEST(I18n_ChineseModeKeepsSourceStrings)
{
    const LanguageScope scope(Language::Chinese);
    CHECK(wintools::util::i18n::Text(L"更换壁纸") == L"更换壁纸");
    CHECK(wintools::util::i18n::Text(L"{} 个窗口") == L"{} 个窗口");
    CHECK(!wintools::util::i18n::IsEnglish());
}

WINTOOLS_TEST(I18n_EnglishModeResolvesKnownStrings)
{
    const LanguageScope scope(Language::English);
    CHECK(wintools::util::i18n::Text(L"更换壁纸") == L"Change Wallpaper");
    CHECK(wintools::util::i18n::Text(L"● 未应用") == L"● Not applied");
    CHECK(wintools::util::i18n::IsEnglish());
    CHECK_EQ(wintools::util::i18n::CurrentLanguage(), Language::English);
}

WINTOOLS_TEST(I18n_UnknownStringsDegradeToChinese)
{
    const LanguageScope scope(Language::English);
    const std::wstring_view unknown = L"某个还没翻译的串";
    CHECK(wintools::util::i18n::Text(unknown) == unknown);
    CHECK(std::wstring_view{ wintools::util::i18n::Cstr(L"退出") } == L"Quit");
}

WINTOOLS_TEST(I18n_FormatTranslatesThenInterpolates)
{
    const LanguageScope scope(Language::English);
    CHECK(wintools::util::i18n::Format(L"{} 个窗口", 7) == L"7 windows");
    CHECK(wintools::util::i18n::Format(L"屏 {}", 3) == L"Display 3");
    CHECK(wintools::util::i18n::Format(L"快照 {} · 不是实时桌面", std::wstring{ L"11-07 16:26" })
        == L"Snapshot 11-07 16:26 - not a live desktop");
}

WINTOOLS_TEST(I18n_FormatKeepsChineseInChineseMode)
{
    const LanguageScope scope(Language::Chinese);
    CHECK(wintools::util::i18n::Format(L"{} 个窗口", 7) == L"7 个窗口");
    CHECK(wintools::util::i18n::Format(L"源图小于外接矩形，将被放大 {:.1f}%，边缘可能变糊", 12.0)
        == L"源图小于外接矩形，将被放大 12.0%，边缘可能变糊");
}

WINTOOLS_TEST(I18n_ShortLabelShowsTheLanguageInUse)
{
    {
        const LanguageScope scope(Language::Chinese);
        CHECK(wintools::util::i18n::LanguageShortLabel() == L"CN");
    }
    {
        const LanguageScope scope(Language::English);
        CHECK(wintools::util::i18n::LanguageShortLabel() == L"EN");
    }
    {
        // Auto reports whichever language the system resolves to, not a third caption.
        const LanguageScope scope(Language::Auto);
        CHECK(wintools::util::i18n::LanguageShortLabel()
            == (wintools::util::i18n::IsEnglish() ? L"EN" : L"CN"));
    }
}

WINTOOLS_TEST(I18n_LanguageLabelFollowsTheActiveLanguage)
{
    {
        const LanguageScope scope(Language::Chinese);
        CHECK(wintools::util::i18n::LanguageLabel() == L"界面语言：简体中文");
    }
    {
        const LanguageScope scope(Language::English);
        CHECK(wintools::util::i18n::LanguageLabel() == L"Language: English");
    }
    {
        const LanguageScope scope(Language::Auto);
        const std::wstring_view label = wintools::util::i18n::LanguageLabel();
        CHECK(label == L"界面语言：跟随系统" || label == L"Language: System");
    }
}
