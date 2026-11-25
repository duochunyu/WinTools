#include "core/update_version.h"

#include <format>
#include <limits>

namespace wintools::core
{
namespace
{

/// Parses one numeric component without std::stoul: no exceptions and no partial-prefix
/// acceptance. A component with more digits than fit in uint32 is rejected, not clamped.
bool ParseComponent(std::wstring_view field, std::uint32_t& value)
{
    if (field.empty())
    {
        return false;
    }

    std::uint64_t accumulated = 0;
    for (const wchar_t character : field)
    {
        if (character < L'0' || character > L'9')
        {
            return false;
        }
        accumulated = accumulated * 10 + static_cast<std::uint32_t>(character - L'0');
        if (accumulated > std::numeric_limits<std::uint32_t>::max())
        {
            return false;
        }
    }

    value = static_cast<std::uint32_t>(accumulated);
    return true;
}

} // namespace

std::optional<AppVersion> AppVersion::Parse(std::wstring_view text)
{
    AppVersion result;
    std::uint32_t* fields[] = { &result.major, &result.minor, &result.patch };

    size_t start = 0;
    for (size_t index = 0; index < 3; ++index)
    {
        const size_t dot = text.find(L'.', start);
        const std::wstring_view field =
            text.substr(start, dot == std::wstring_view::npos ? std::wstring_view::npos : dot - start);
        if (!ParseComponent(field, *fields[index]))
        {
            return std::nullopt;
        }
        if (dot == std::wstring_view::npos)
        {
            // Fewer than three components: the third field was the last one, so this is only
            // valid when index is 2. Anything earlier cannot fill the remaining fields.
            return index == 2 ? std::optional<AppVersion>{ result } : std::nullopt;
        }
        start = dot + 1;
    }

    // A trailing dot or any extra component ("1.2.3.4", "1.2.3-beta") leaves text unconsumed.
    if (start != text.size())
    {
        return std::nullopt;
    }
    return result;
}

std::wstring AppVersion::Text() const
{
    return std::format(L"{}.{}.{}", major, minor, patch);
}

int Compare(const AppVersion& left, const AppVersion& right)
{
    if (left.major != right.major)
    {
        return left.major < right.major ? -1 : 1;
    }
    if (left.minor != right.minor)
    {
        return left.minor < right.minor ? -1 : 1;
    }
    if (left.patch != right.patch)
    {
        return left.patch < right.patch ? -1 : 1;
    }
    return 0;
}

} // namespace wintools::core
