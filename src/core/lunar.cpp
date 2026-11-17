#include "core/lunar.h"

#include <array>
#include <cstdint>

namespace wintools::core
{

namespace
{

/// One packed record per lunar year, 1900 .. 2099.
///
///   bits 0-3   the intercalary month number, 0 when the year has none
///   bits 4-15  one length bit per month, month 1 at bit 15: 1 = 30 days, 0 = 29
///   bit  16    the intercalary month's length, same encoding
///
/// The values are the ones every Chinese calendar implementation ships; they encode a
/// published almanac and cannot be derived, so they live here as data rather than as code.
/// tests/lunar_test.cpp checks 274 dates against an independent implementation that carries
/// the same table - which is the useful check: it asserts the traversal, not the table.
constexpr std::array<std::uint32_t, 200> kYearInfo = {
    // 1900 .. 2099
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950,
    0x16554, 0x056a0, 0x09ad0, 0x055d2, 0x04ae0, 0x0a5b6,
    0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2,
    0x095b0, 0x14977, 0x04970, 0x0a4b0, 0x0b4b5, 0x06a50,
    0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970,
    0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60,
    0x186e3, 0x092e0, 0x1c8d7, 0x0c950, 0x0d4a0, 0x1d8a6,
    0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2,
    0x0a950, 0x0b557, 0x06ca0, 0x0b550, 0x15355, 0x04da0,
    0x0a5d0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0,
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260,
    0x0f263, 0x0d950, 0x05b57, 0x056a0, 0x096d0, 0x04dd5,
    0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540,
    0x0b5a0, 0x195a6, 0x095b0, 0x049b0, 0x0a974, 0x0a4b0,
    0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570,
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58,
    0x05ac0, 0x0ab60, 0x096d5, 0x092e0, 0x0c960, 0x0d954,
    0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0,
    0x092d0, 0x0cab5, 0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50,
    0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6,
    0x0a4e0, 0x0d260, 0x0ea65, 0x0d530, 0x05aa0, 0x076a3,
    0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250,
    0x0d520, 0x0dd45, 0x0b5a0, 0x056d0, 0x055b2, 0x049b0,
    0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0,
    0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, 0x168a6,
    0x0ea50, 0x06aa0, 0x1a6c4, 0x0aae0, 0x092e0, 0x0d2e3,
    0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0,
    0x0a6d0, 0x055d4, 0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0,
    0x0b6a6, 0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0,
    0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50, 0x14b55,
    0x04b60, 0x0a570, 0x054e4, 0x0d160, 0x0e968, 0x0d520,
    0x0daa0, 0x16aa6, 0x056d0, 0x04ae0, 0x0a9d4, 0x0a2d0,
    0x0d150, 0x0f252,
};

constexpr auto kStems = L"甲乙丙丁戊己庚辛壬癸";
constexpr auto kBranches = L"子丑寅卯辰巳午未申酉戌亥";
constexpr auto kZodiac = L"鼠牛虎兔龙蛇马羊猴鸡狗猪";

/// Days in a Gregorian month; `month` must be 1..12.
constexpr int GregorianMonthDays(int year, int month)
{
    constexpr std::array<int, 12> days = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    const bool leapYear = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return days[static_cast<size_t>(month - 1)] + ((month == 2 && leapYear) ? 1 : 0);
}

/// Days since 1970-01-01 for a Gregorian date (Howard Hinnant's civil-from-days inverse).
///
/// Written out rather than pulled from <chrono> because the conversion needs to stay a constexpr
/// integer function that both the calendar and its tests can call with a plain int triple.
constexpr long long DaysFromCivil(int year, int month, int day)
{
    year -= month <= 2;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned>(year - static_cast<int>(era) * 400);
    const auto dayOfYear = static_cast<unsigned>(
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned dayOfEra =
        yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<long long>(dayOfEra) - 719468;
}

/// 1900-01-31, the first day the table describes.
constexpr long long kEpoch = DaysFromCivil(1900, 1, 31);

std::uint32_t InfoOf(int lunarYear)
{
    return kYearInfo[static_cast<size_t>(lunarYear - kLunarFirstYear)];
}

/// The intercalary month of a lunar year, 0 when there is none.
int LeapMonthOf(int lunarYear)
{
    return static_cast<int>(InfoOf(lunarYear) & 0xFu);
}

int LeapMonthDays(int lunarYear)
{
    if (LeapMonthOf(lunarYear) == 0)
    {
        return 0;
    }

    return static_cast<int>(((InfoOf(lunarYear) >> 16) & 1u) + 29u);
}

int MonthDaysOf(int lunarYear, int month)
{
    return static_cast<int>(((InfoOf(lunarYear) >> (16 - month)) & 1u) + 29u);
}

int YearDaysOf(int lunarYear)
{
    int total = 0;
    for (int month = 1; month <= 12; ++month)
    {
        total += MonthDaysOf(lunarYear, month);
    }

    return total + LeapMonthDays(lunarYear);
}

} // namespace

LunarDate ToLunar(int year, int month, int day)
{
    LunarDate result;

    if (year < kLunarFirstYear || year > kLunarLastYear || month < 1 || month > 12)
    {
        return result;
    }

    if (day < 1 || day > GregorianMonthDays(year, month))
    {
        return result;
    }

    long long offset = DaysFromCivil(year, month, day) - kEpoch;
    if (offset < 0)
    {
        // Before the table starts: 1900-01-01 .. 1900-01-30 belong to 农历 1899 年, which the
        // packed records do not describe.
        return result;
    }

    int lunarYear = kLunarFirstYear;
    while (lunarYear <= kLunarLastYear && offset >= YearDaysOf(lunarYear))
    {
        offset -= YearDaysOf(lunarYear);
        ++lunarYear;
    }

    if (lunarYear > kLunarLastYear)
    {
        return result;
    }

    const int leapMonth = LeapMonthOf(lunarYear);
    int lunarMonth = 1;
    bool isLeap = false;

    for (; lunarMonth <= 12; ++lunarMonth)
    {
        const int days = MonthDaysOf(lunarYear, lunarMonth);
        if (offset < days)
        {
            break;
        }

        offset -= days;

        // The intercalary month is inserted *after* the month it repeats, so it is tested here
        // rather than in a pass of its own.
        if (lunarMonth == leapMonth)
        {
            const int leapDays = LeapMonthDays(lunarYear);
            if (offset < leapDays)
            {
                isLeap = true;
                break;
            }

            offset -= leapDays;
        }
    }

    if (lunarMonth > 12)
    {
        return result;
    }

    result.year = lunarYear;
    result.month = lunarMonth;
    result.day = static_cast<int>(offset) + 1;
    result.leap = isLeap;
    result.valid = true;
    return result;
}

std::wstring GanzhiOfYear(int lunarYear)
{
    // 公元 4 年是甲子年, which is why the cycle is indexed from year - 4.
    const int stem = ((lunarYear - 4) % 10 + 10) % 10;
    const int branch = ((lunarYear - 4) % 12 + 12) % 12;

    std::wstring text;
    text.push_back(kStems[stem]);
    text.push_back(kBranches[branch]);
    return text;
}

std::wstring ZodiacOfYear(int lunarYear)
{
    const int index = ((lunarYear - 4) % 12 + 12) % 12;
    return std::wstring(1, kZodiac[index]);
}

std::wstring LunarMonthName(int month, bool leap)
{
    static const std::array<const wchar_t*, 12> names = { L"正月", L"二月", L"三月", L"四月",
        L"五月", L"六月", L"七月", L"八月", L"九月", L"十月", L"冬月", L"腊月" };

    if (month < 1 || month > 12)
    {
        return {};
    }

    return (leap ? L"闰" : L"") + std::wstring(names[static_cast<size_t>(month - 1)]);
}

std::wstring LunarDayName(int day)
{
    static const std::array<const wchar_t*, 30> names = { L"初一", L"初二", L"初三", L"初四",
        L"初五", L"初六", L"初七", L"初八", L"初九", L"初十", L"十一", L"十二", L"十三", L"十四",
        L"十五", L"十六", L"十七", L"十八", L"十九", L"二十", L"廿一", L"廿二", L"廿三", L"廿四",
        L"廿五", L"廿六", L"廿七", L"廿八", L"廿九", L"三十" };

    if (day < 1 || day > 30)
    {
        return {};
    }

    return names[static_cast<size_t>(day - 1)];
}

std::wstring WeekdayName(int weekday)
{
    static const std::array<const wchar_t*, 7> names = { L"周日", L"周一", L"周二", L"周三",
        L"周四", L"周五", L"周六" };

    return names[static_cast<size_t>(((weekday % 7) + 7) % 7)];
}

} // namespace wintools::core
