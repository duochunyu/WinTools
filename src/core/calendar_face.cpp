#include "core/calendar_face.h"

#include <format>

#include "core/lunar.h"

namespace wintools::core
{

CalendarFace MakeFace(int year, int month, int day, int weekday, int hour, int minute)
{
    CalendarFace face;
    face.year = year;
    face.month = month;
    face.day = day;
    face.weekday = weekday;
    face.hour = hour;
    face.minute = minute;

    const LunarDate lunar = ToLunar(year, month, day);
    if (lunar.valid)
    {
        face.ganzhi = GanzhiOfYear(lunar.year);
        face.zodiac = ZodiacOfYear(lunar.year);
        face.lunarMonth = LunarMonthName(lunar.month, lunar.leap);
        face.lunarDay = LunarDayName(lunar.day);
    }

    return face;
}

std::wstring FaceHeadline(const CalendarFace& face)
{
    if (face.ganzhi.empty())
    {
        return {};
    }

    return std::format(L"{} · {}", face.ganzhi, face.zodiac);
}

std::wstring FaceLunarLine(const CalendarFace& face)
{
    if (!face.HasLunar())
    {
        // The table stops at 农历 2099. Saying so beats showing a month that is off by a
        // cycle - and it is one character, not a dialog box.
        return L"—";
    }

    return face.lunarMonth + face.lunarDay;
}

std::wstring FaceFootline(const CalendarFace& face)
{
    return std::format(L"{}.{:02} · {} · {:02}:{:02}", face.year, face.month,
        WeekdayName(face.weekday), face.hour, face.minute);
}

} // namespace wintools::core
