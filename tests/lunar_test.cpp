#include "test_framework.h"

#include <string>
#include <vector>

#include "core/lunar.h"

using namespace wintools::core;

namespace
{

/// One row of the cross-check table: a Gregorian date and the lunisolar date it must decode to.
struct Vector
{
    int year;
    int month;
    int day;
    int lunarYear;
    int lunarMonth;
    int lunarDay;
    bool leap;
};

/// 274 dates: the 15th of the 8th lunar month of every year 1900..2099, plus the first day of
/// the intercalary month in every year that has one.
///
/// Generated from a separate implementation of the same almanac, so this asserts the traversal -
/// epoch, year lengths, leap insertion - rather than restating the packed table.
const std::vector<Vector> kAlmanac = {
    { 1900, 9, 8, 1900, 8, 15, false }, { 1900, 9, 24, 1900, 8, 1, true },
    { 1901, 9, 27, 1901, 8, 15, false }, { 1902, 9, 16, 1902, 8, 15, false },
    { 1903, 10, 5, 1903, 8, 15, false }, { 1903, 6, 25, 1903, 5, 1, true },
    { 1904, 9, 24, 1904, 8, 15, false }, { 1905, 9, 13, 1905, 8, 15, false },
    { 1906, 10, 2, 1906, 8, 15, false }, { 1906, 5, 23, 1906, 4, 1, true },
    { 1907, 9, 22, 1907, 8, 15, false }, { 1908, 9, 10, 1908, 8, 15, false },
    { 1909, 9, 28, 1909, 8, 15, false }, { 1909, 3, 22, 1909, 2, 1, true },
    { 1910, 9, 18, 1910, 8, 15, false }, { 1911, 10, 6, 1911, 8, 15, false },
    { 1911, 7, 26, 1911, 6, 1, true }, { 1912, 9, 25, 1912, 8, 15, false },
    { 1913, 9, 15, 1913, 8, 15, false }, { 1914, 10, 4, 1914, 8, 15, false },
    { 1914, 6, 23, 1914, 5, 1, true }, { 1915, 9, 23, 1915, 8, 15, false },
    { 1916, 9, 12, 1916, 8, 15, false }, { 1917, 9, 30, 1917, 8, 15, false },
    { 1917, 3, 23, 1917, 2, 1, true }, { 1918, 9, 19, 1918, 8, 15, false },
    { 1919, 10, 8, 1919, 8, 15, false }, { 1919, 8, 25, 1919, 7, 1, true },
    { 1920, 9, 26, 1920, 8, 15, false }, { 1921, 9, 16, 1921, 8, 15, false },
    { 1922, 10, 5, 1922, 8, 15, false }, { 1922, 6, 25, 1922, 5, 1, true },
    { 1923, 9, 25, 1923, 8, 15, false }, { 1924, 9, 13, 1924, 8, 15, false },
    { 1925, 10, 2, 1925, 8, 15, false }, { 1925, 5, 22, 1925, 4, 1, true },
    { 1926, 9, 21, 1926, 8, 15, false }, { 1927, 9, 10, 1927, 8, 15, false },
    { 1928, 9, 28, 1928, 8, 15, false }, { 1928, 3, 22, 1928, 2, 1, true },
    { 1929, 9, 17, 1929, 8, 15, false }, { 1930, 10, 6, 1930, 8, 15, false },
    { 1930, 7, 26, 1930, 6, 1, true }, { 1931, 9, 26, 1931, 8, 15, false },
    { 1932, 9, 15, 1932, 8, 15, false }, { 1933, 10, 4, 1933, 8, 15, false },
    { 1933, 6, 23, 1933, 5, 1, true }, { 1934, 9, 23, 1934, 8, 15, false },
    { 1935, 9, 12, 1935, 8, 15, false }, { 1936, 9, 30, 1936, 8, 15, false },
    { 1936, 4, 21, 1936, 3, 1, true }, { 1937, 9, 19, 1937, 8, 15, false },
    { 1938, 10, 8, 1938, 8, 15, false }, { 1938, 8, 25, 1938, 7, 1, true },
    { 1939, 9, 27, 1939, 8, 15, false }, { 1940, 9, 16, 1940, 8, 15, false },
    { 1941, 10, 5, 1941, 8, 15, false }, { 1941, 7, 24, 1941, 6, 1, true },
    { 1942, 9, 24, 1942, 8, 15, false }, { 1943, 9, 14, 1943, 8, 15, false },
    { 1944, 10, 1, 1944, 8, 15, false }, { 1944, 5, 22, 1944, 4, 1, true },
    { 1945, 9, 20, 1945, 8, 15, false }, { 1946, 9, 10, 1946, 8, 15, false },
    { 1947, 9, 29, 1947, 8, 15, false }, { 1947, 3, 23, 1947, 2, 1, true },
    { 1948, 9, 17, 1948, 8, 15, false }, { 1949, 10, 6, 1949, 8, 15, false },
    { 1949, 8, 24, 1949, 7, 1, true }, { 1950, 9, 26, 1950, 8, 15, false },
    { 1951, 9, 15, 1951, 8, 15, false }, { 1952, 10, 3, 1952, 8, 15, false },
    { 1952, 6, 22, 1952, 5, 1, true }, { 1953, 9, 22, 1953, 8, 15, false },
    { 1954, 9, 11, 1954, 8, 15, false }, { 1955, 9, 30, 1955, 8, 15, false },
    { 1955, 4, 22, 1955, 3, 1, true }, { 1956, 9, 19, 1956, 8, 15, false },
    { 1957, 9, 8, 1957, 8, 15, false }, { 1957, 9, 24, 1957, 8, 1, true },
    { 1958, 9, 27, 1958, 8, 15, false }, { 1959, 9, 17, 1959, 8, 15, false },
    { 1960, 10, 5, 1960, 8, 15, false }, { 1960, 7, 24, 1960, 6, 1, true },
    { 1961, 9, 24, 1961, 8, 15, false }, { 1962, 9, 13, 1962, 8, 15, false },
    { 1963, 10, 2, 1963, 8, 15, false }, { 1963, 5, 23, 1963, 4, 1, true },
    { 1964, 9, 20, 1964, 8, 15, false }, { 1965, 9, 10, 1965, 8, 15, false },
    { 1966, 9, 29, 1966, 8, 15, false }, { 1966, 4, 21, 1966, 3, 1, true },
    { 1967, 9, 18, 1967, 8, 15, false }, { 1968, 10, 6, 1968, 8, 15, false },
    { 1968, 8, 24, 1968, 7, 1, true }, { 1969, 9, 26, 1969, 8, 15, false },
    { 1970, 9, 15, 1970, 8, 15, false }, { 1971, 10, 3, 1971, 8, 15, false },
    { 1971, 6, 23, 1971, 5, 1, true }, { 1972, 9, 22, 1972, 8, 15, false },
    { 1973, 9, 11, 1973, 8, 15, false }, { 1974, 9, 30, 1974, 8, 15, false },
    { 1974, 5, 22, 1974, 4, 1, true }, { 1975, 9, 20, 1975, 8, 15, false },
    { 1976, 9, 8, 1976, 8, 15, false }, { 1976, 9, 24, 1976, 8, 1, true },
    { 1977, 9, 27, 1977, 8, 15, false }, { 1978, 9, 16, 1978, 8, 15, false },
    { 1979, 10, 5, 1979, 8, 15, false }, { 1979, 7, 24, 1979, 6, 1, true },
    { 1980, 9, 23, 1980, 8, 15, false }, { 1981, 9, 12, 1981, 8, 15, false },
    { 1982, 10, 1, 1982, 8, 15, false }, { 1982, 5, 23, 1982, 4, 1, true },
    { 1983, 9, 21, 1983, 8, 15, false }, { 1984, 9, 10, 1984, 8, 15, false },
    { 1984, 11, 23, 1984, 10, 1, true }, { 1985, 9, 29, 1985, 8, 15, false },
    { 1986, 9, 18, 1986, 8, 15, false }, { 1987, 10, 7, 1987, 8, 15, false },
    { 1987, 7, 26, 1987, 6, 1, true }, { 1988, 9, 25, 1988, 8, 15, false },
    { 1989, 9, 14, 1989, 8, 15, false }, { 1990, 10, 3, 1990, 8, 15, false },
    { 1990, 6, 23, 1990, 5, 1, true }, { 1991, 9, 22, 1991, 8, 15, false },
    { 1992, 9, 11, 1992, 8, 15, false }, { 1993, 9, 30, 1993, 8, 15, false },
    { 1993, 4, 22, 1993, 3, 1, true }, { 1994, 9, 20, 1994, 8, 15, false },
    { 1995, 9, 9, 1995, 8, 15, false }, { 1995, 9, 25, 1995, 8, 1, true },
    { 1996, 9, 27, 1996, 8, 15, false }, { 1997, 9, 16, 1997, 8, 15, false },
    { 1998, 10, 5, 1998, 8, 15, false }, { 1998, 6, 24, 1998, 5, 1, true },
    { 1999, 9, 24, 1999, 8, 15, false }, { 2000, 9, 12, 2000, 8, 15, false },
    { 2001, 10, 1, 2001, 8, 15, false }, { 2001, 5, 23, 2001, 4, 1, true },
    { 2002, 9, 21, 2002, 8, 15, false }, { 2003, 9, 11, 2003, 8, 15, false },
    { 2004, 9, 28, 2004, 8, 15, false }, { 2004, 3, 21, 2004, 2, 1, true },
    { 2005, 9, 18, 2005, 8, 15, false }, { 2006, 10, 6, 2006, 8, 15, false },
    { 2006, 8, 24, 2006, 7, 1, true }, { 2007, 9, 25, 2007, 8, 15, false },
    { 2008, 9, 14, 2008, 8, 15, false }, { 2009, 10, 3, 2009, 8, 15, false },
    { 2009, 6, 23, 2009, 5, 1, true }, { 2010, 9, 22, 2010, 8, 15, false },
    { 2011, 9, 12, 2011, 8, 15, false }, { 2012, 9, 30, 2012, 8, 15, false },
    { 2012, 5, 21, 2012, 4, 1, true }, { 2013, 9, 19, 2013, 8, 15, false },
    { 2014, 9, 8, 2014, 8, 15, false }, { 2014, 10, 24, 2014, 9, 1, true },
    { 2015, 9, 27, 2015, 8, 15, false }, { 2016, 9, 15, 2016, 8, 15, false },
    { 2017, 10, 4, 2017, 8, 15, false }, { 2017, 7, 23, 2017, 6, 1, true },
    { 2018, 9, 24, 2018, 8, 15, false }, { 2019, 9, 13, 2019, 8, 15, false },
    { 2020, 10, 1, 2020, 8, 15, false }, { 2020, 5, 23, 2020, 4, 1, true },
    { 2021, 9, 21, 2021, 8, 15, false }, { 2022, 9, 10, 2022, 8, 15, false },
    { 2023, 9, 29, 2023, 8, 15, false }, { 2023, 3, 22, 2023, 2, 1, true },
    { 2024, 9, 17, 2024, 8, 15, false }, { 2025, 10, 6, 2025, 8, 15, false },
    { 2025, 7, 25, 2025, 6, 1, true }, { 2026, 9, 25, 2026, 8, 15, false },
    { 2027, 9, 15, 2027, 8, 15, false }, { 2028, 10, 3, 2028, 8, 15, false },
    { 2028, 6, 23, 2028, 5, 1, true }, { 2029, 9, 22, 2029, 8, 15, false },
    { 2030, 9, 12, 2030, 8, 15, false }, { 2031, 10, 1, 2031, 8, 15, false },
    { 2031, 4, 22, 2031, 3, 1, true }, { 2032, 9, 19, 2032, 8, 15, false },
    { 2033, 9, 8, 2033, 8, 15, false }, { 2033, 12, 22, 2033, 11, 1, true },
    { 2034, 9, 27, 2034, 8, 15, false }, { 2035, 9, 16, 2035, 8, 15, false },
    { 2036, 10, 4, 2036, 8, 15, false }, { 2036, 7, 23, 2036, 6, 1, true },
    { 2037, 9, 24, 2037, 8, 15, false }, { 2038, 9, 13, 2038, 8, 15, false },
    { 2039, 10, 2, 2039, 8, 15, false }, { 2039, 6, 22, 2039, 5, 1, true },
    { 2040, 9, 20, 2040, 8, 15, false }, { 2041, 9, 10, 2041, 8, 15, false },
    { 2042, 9, 28, 2042, 8, 15, false }, { 2042, 3, 22, 2042, 2, 1, true },
    { 2043, 9, 17, 2043, 8, 15, false }, { 2044, 10, 5, 2044, 8, 15, false },
    { 2044, 8, 23, 2044, 7, 1, true }, { 2045, 9, 25, 2045, 8, 15, false },
    { 2046, 9, 15, 2046, 8, 15, false }, { 2047, 10, 4, 2047, 8, 15, false },
    { 2047, 6, 23, 2047, 5, 1, true }, { 2048, 9, 22, 2048, 8, 15, false },
    { 2049, 9, 11, 2049, 8, 15, false }, { 2050, 9, 30, 2050, 8, 15, false },
    { 2050, 4, 21, 2050, 3, 1, true }, { 2051, 9, 19, 2051, 8, 15, false },
    { 2052, 9, 7, 2052, 8, 15, false }, { 2052, 9, 23, 2052, 8, 1, true },
    { 2053, 9, 26, 2053, 8, 15, false }, { 2054, 9, 16, 2054, 8, 15, false },
    { 2055, 10, 5, 2055, 8, 15, false }, { 2055, 7, 24, 2055, 6, 1, true },
    { 2056, 9, 24, 2056, 8, 15, false }, { 2057, 9, 13, 2057, 8, 15, false },
    { 2058, 10, 2, 2058, 8, 15, false }, { 2058, 5, 22, 2058, 4, 1, true },
    { 2059, 9, 21, 2059, 8, 15, false }, { 2060, 9, 9, 2060, 8, 15, false },
    { 2061, 9, 28, 2061, 8, 15, false }, { 2061, 4, 20, 2061, 3, 1, true },
    { 2062, 9, 17, 2062, 8, 15, false }, { 2063, 10, 6, 2063, 8, 15, false },
    { 2063, 8, 24, 2063, 7, 1, true }, { 2064, 9, 25, 2064, 8, 15, false },
    { 2065, 9, 15, 2065, 8, 15, false }, { 2066, 10, 3, 2066, 8, 15, false },
    { 2066, 6, 23, 2066, 5, 1, true }, { 2067, 9, 23, 2067, 8, 15, false },
    { 2068, 9, 11, 2068, 8, 15, false }, { 2069, 9, 29, 2069, 8, 15, false },
    { 2069, 5, 21, 2069, 4, 1, true }, { 2070, 9, 19, 2070, 8, 15, false },
    { 2071, 9, 8, 2071, 8, 15, false }, { 2071, 9, 24, 2071, 8, 1, true },
    { 2072, 9, 26, 2072, 8, 15, false }, { 2073, 9, 16, 2073, 8, 15, false },
    { 2074, 10, 5, 2074, 8, 15, false }, { 2074, 7, 24, 2074, 6, 1, true },
    { 2075, 9, 24, 2075, 8, 15, false }, { 2076, 9, 12, 2076, 8, 15, false },
    { 2077, 10, 1, 2077, 8, 15, false }, { 2077, 5, 22, 2077, 4, 1, true },
    { 2078, 9, 20, 2078, 8, 15, false }, { 2079, 9, 10, 2079, 8, 15, false },
    { 2080, 9, 28, 2080, 8, 15, false }, { 2080, 4, 20, 2080, 3, 1, true },
    { 2081, 9, 17, 2081, 8, 15, false }, { 2082, 10, 6, 2082, 8, 15, false },
    { 2082, 8, 24, 2082, 7, 1, true }, { 2083, 9, 26, 2083, 8, 15, false },
    { 2084, 9, 14, 2084, 8, 15, false }, { 2085, 10, 3, 2085, 8, 15, false },
    { 2085, 6, 22, 2085, 5, 1, true }, { 2086, 9, 22, 2086, 8, 15, false },
    { 2087, 9, 11, 2087, 8, 15, false }, { 2088, 9, 29, 2088, 8, 15, false },
    { 2088, 5, 21, 2088, 4, 1, true }, { 2089, 9, 18, 2089, 8, 15, false },
    { 2090, 9, 8, 2090, 8, 15, false }, { 2090, 9, 24, 2090, 8, 1, true },
    { 2091, 9, 27, 2091, 8, 15, false }, { 2092, 9, 16, 2092, 8, 15, false },
    { 2093, 10, 5, 2093, 8, 15, false }, { 2093, 7, 23, 2093, 6, 1, true },
    { 2094, 9, 24, 2094, 8, 15, false }, { 2095, 9, 13, 2095, 8, 15, false },
    { 2096, 9, 30, 2096, 8, 15, false }, { 2096, 5, 22, 2096, 4, 1, true },
    { 2097, 9, 20, 2097, 8, 15, false }, { 2098, 9, 9, 2098, 8, 15, false },
    { 2099, 9, 29, 2099, 8, 15, false }, { 2099, 3, 22, 2099, 2, 1, true },
};

} // namespace

WINTOOLS_TEST(Lunar_MatchesTheAlmanac)
{
    int converted = 0;

    for (const Vector& entry : kAlmanac)
    {
        const LunarDate lunar = ToLunar(entry.year, entry.month, entry.day);
        CHECK(lunar.valid);
        CHECK_EQ(lunar.year, entry.lunarYear);
        CHECK_EQ(lunar.month, entry.lunarMonth);
        CHECK_EQ(lunar.day, entry.lunarDay);
        CHECK_EQ(lunar.leap, entry.leap);
        ++converted;
    }

    CHECK_EQ(converted, static_cast<int>(kAlmanac.size()));
}

WINTOOLS_TEST(Lunar_SpringFestival)
{
    // 春节 is 正月初一, the one date every almanac agrees on.
    struct Case
    {
        int year;
        int month;
        int day;
    };

    const Case cases[] = {
        { 2024, 2, 10 },
        { 2025, 1, 29 },
        { 2026, 2, 17 },
        { 2027, 2, 6 },
    };

    for (const Case& item : cases)
    {
        const LunarDate lunar = ToLunar(item.year, item.month, item.day);
        CHECK(lunar.valid);
        CHECK_EQ(lunar.month, 1);
        CHECK_EQ(lunar.day, 1);
        CHECK(!lunar.leap);
    }
}

WINTOOLS_TEST(Lunar_NewYearEveBelongsToTheOldYear)
{
    // 2026-01-01 is not "2026 年" in the lunisolar calendar: the year starts at 春节.
    const LunarDate lunar = ToLunar(2026, 1, 1);
    CHECK(lunar.valid);
    CHECK_EQ(lunar.year, 2025);
    CHECK_EQ(lunar.month, 11);
    CHECK_EQ(lunar.day, 13);
}

WINTOOLS_TEST(Lunar_OutsideTheTableIsInvalid)
{
    CHECK(!ToLunar(1899, 12, 31).valid);

    // The table starts at 1900-01-31; the first month of that January belongs to 1899.
    CHECK(!ToLunar(1900, 1, 30).valid);
    CHECK(ToLunar(1900, 1, 31).valid);

    CHECK(!ToLunar(2100, 1, 1).valid);
    CHECK(ToLunar(2099, 12, 31).valid);
}

WINTOOLS_TEST(Lunar_RejectsImpossibleDates)
{
    CHECK(!ToLunar(2026, 13, 1).valid);
    CHECK(!ToLunar(2026, 0, 1).valid);
    CHECK(!ToLunar(2026, 2, 30).valid);
    CHECK(!ToLunar(2025, 2, 29).valid);
    CHECK(ToLunar(2024, 2, 29).valid);
}

WINTOOLS_TEST(Lunar_GanzhiFollowsTheSexagenaryCycle)
{
    // 1900 庚子, 2024 甲辰, 2025 乙巳, 2026 丙午 - the cycle runs one step per lunar year.
    CHECK(GanzhiOfYear(1900) == L"庚子");
    CHECK(GanzhiOfYear(2024) == L"甲辰");
    CHECK(GanzhiOfYear(2025) == L"乙巳");
    CHECK(GanzhiOfYear(2026) == L"丙午");
    CHECK(ZodiacOfYear(2024) == L"龙");
    CHECK(ZodiacOfYear(2025) == L"蛇");
    CHECK(ZodiacOfYear(2026) == L"马");
}

WINTOOLS_TEST(Lunar_NamesUseTheAlmanacWording)
{
    CHECK(LunarMonthName(1, false) == L"正月");
    CHECK(LunarMonthName(11, false) == L"冬月");
    CHECK(LunarMonthName(12, false) == L"腊月");
    CHECK(LunarMonthName(4, true) == L"闰四月");

    CHECK(LunarDayName(1) == L"初一");
    CHECK(LunarDayName(10) == L"初十");
    CHECK(LunarDayName(20) == L"二十");
    CHECK(LunarDayName(21) == L"廿一");
    CHECK(LunarDayName(30) == L"三十");
    CHECK(LunarDayName(31).empty());

    CHECK(WeekdayName(0) == L"周日");
    CHECK(WeekdayName(3) == L"周三");
    CHECK(WeekdayName(7) == L"周日");
}
