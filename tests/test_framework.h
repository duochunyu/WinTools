#pragma once

// Minimal test harness. No third-party dependency on purpose: a 60-line header is enough for
// pure geometry assertions, and keeping the tree dependency-free is a project goal.

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace wintools::test
{

using TestFn = void (*)();

struct TestCase
{
    const char* name;
    TestFn fn;
};

inline std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> registry;
    return registry;
}

inline const char*& CurrentTest()
{
    static const char* current = "";
    return current;
}

inline int& FailureCount()
{
    static int count = 0;
    return count;
}

inline bool Register(const char* name, TestFn fn)
{
    Registry().push_back(TestCase{ name, fn });
    return true;
}

inline void Fail(const char* file, int line, const std::string& message)
{
    std::printf("    FAIL %s:%d  %s\n", file, line, message.c_str());
    ++FailureCount();
}

template <typename T>
std::string ToText(const T& value)
{
    if constexpr (std::is_same_v<T, bool>)
    {
        return value ? "true" : "false";
    }
    else if constexpr (std::is_integral_v<T>)
    {
        return std::to_string(value);
    }
    else
    {
        return "<value>";
    }
}

inline int RunAll()
{
    std::printf("running %zu test(s)\n\n", Registry().size());

    int passed = 0;
    for (const auto& test : Registry())
    {
        const int before = FailureCount();
        CurrentTest() = test.name;
        std::printf("  %s\n", test.name);
        test.fn();

        if (FailureCount() == before)
        {
            ++passed;
        }
    }

    const int failed = static_cast<int>(Registry().size()) - passed;
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace wintools::test

#define WINTOOLS_TEST(name)                                                                        \
    static void name();                                                                            \
    [[maybe_unused]] static const bool name##_registered = ::wintools::test::Register(#name, name); \
    static void name()

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            ::wintools::test::Fail(__FILE__, __LINE__, "CHECK failed: " #condition);                \
        }                                                                                          \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                 \
    do                                                                                             \
    {                                                                                              \
        const auto actualValue__ = (actual);                                                       \
        const auto expectedValue__ = (expected);                                                   \
        if (!(actualValue__ == expectedValue__))                                                   \
        {                                                                                          \
            ::wintools::test::Fail(                                                                \
                __FILE__,                                                                          \
                __LINE__,                                                                          \
                std::string("CHECK_EQ failed: " #actual " == " #expected "  (actual=")             \
                    + ::wintools::test::ToText(actualValue__)                                      \
                    + ", expected=" + ::wintools::test::ToText(expectedValue__) + ")");             \
        }                                                                                          \
    } while (false)
