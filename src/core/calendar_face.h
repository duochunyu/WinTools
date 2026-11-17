#pragma once

#include <string>

namespace wintools::core
{

/// Everything one day of the calendar widget shows, already resolved to text.
///
/// Split out of the painting code so the wording - not just the arithmetic - can be asserted:
/// "十月初三" versus "10-3" is a product decision, and a product decision that is only visible
/// in pixels is one that nobody can test.
struct CalendarFace
{
    int year{};
    int month{};
    int day{};

    /// 0 = Sunday, matching SYSTEMTIME::wDayOfWeek.
    int weekday{};

    int hour{};
    int minute{};

    /// "丙午" and "马".
    std::wstring ganzhi;
    std::wstring zodiac;

    /// "十月" and "初三"; empty when the lunisolar date is outside the table.
    std::wstring lunarMonth;
    std::wstring lunarDay;

    [[nodiscard]] bool HasLunar() const
    {
        return !lunarMonth.empty() && !lunarDay.empty();
    }
};

/// Resolves one moment. `weekday` is the Gregorian weekday of the date, 0 = Sunday.
[[nodiscard]] CalendarFace MakeFace(
    int year, int month, int day, int weekday, int hour, int minute);

/// "丙午 · 马" - the top line.
[[nodiscard]] std::wstring FaceHeadline(const CalendarFace& face);

/// "十月初三" - the lunar date, or an em dash when the table does not reach that year.
[[nodiscard]] std::wstring FaceLunarLine(const CalendarFace& face);

/// "2026.11 · 周三 · 13:45" - the footer.
[[nodiscard]] std::wstring FaceFootline(const CalendarFace& face);

} // namespace wintools::core
