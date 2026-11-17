#pragma once

#include <string>

namespace wintools::core
{

/// The lunisolar calendar only covers the range the year table covers.
///
/// 1900-01-31 is 农历 1900 年正月初一; the table ends with 农历 2099 年. Anything outside is
/// reported as invalid rather than silently clamped, because a calendar that quietly shows the
/// wrong month is worse than one that shows nothing.
inline constexpr int kLunarFirstYear = 1900;
inline constexpr int kLunarLastYear = 2099;

/// One date in the Chinese lunisolar calendar.
struct LunarDate
{
    int year{};
    int month{ 1 };

    /// 1..30.
    int day{ 1 };

    /// True when the month is the intercalary (闰) repeat of `month`.
    bool leap{};

    /// False when the input was outside kLunarFirstYear..kLunarLastYear or not a real date.
    bool valid{};
};

/// Converts a Gregorian date to the lunisolar one. `valid` is false when it cannot be converted.
[[nodiscard]] LunarDate ToLunar(int year, int month, int day);

/// "丙午" - the heavenly stem and earthly branch of a lunar year.
[[nodiscard]] std::wstring GanzhiOfYear(int lunarYear);

/// "马" - the zodiac animal of a lunar year.
[[nodiscard]] std::wstring ZodiacOfYear(int lunarYear);

/// "十月", "闰四月" - the lunar month, with the intercalary marker when there is one.
[[nodiscard]] std::wstring LunarMonthName(int month, bool leap);

/// "初三", "廿九", "三十".
[[nodiscard]] std::wstring LunarDayName(int day);

/// "周三". 0 is Sunday, matching both SYSTEMTIME and std::chrono's Sunday-based conversion.
[[nodiscard]] std::wstring WeekdayName(int weekday);

} // namespace wintools::core
