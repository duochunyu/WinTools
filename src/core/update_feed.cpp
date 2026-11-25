#include "core/update_feed.h"

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>

#include "util/log.h"

namespace wintools::core
{
namespace
{

constexpr std::size_t kMaxFieldChars = 4096;
constexpr std::size_t kMaxNotesChars = 4096;

std::wstring_view Trim(std::wstring_view text)
{
    const auto isSpace = [](wchar_t ch)
    { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; };

    size_t first = 0;
    while (first < text.size() && isSpace(text[first]))
    {
        ++first;
    }
    size_t last = text.size();
    while (last > first && isSpace(text[last - 1]))
    {
        --last;
    }
    return text.substr(first, last - first);
}

bool IsHex64(std::wstring_view text)
{
    if (text.size() != 64)
    {
        return false;
    }
    return std::ranges::all_of(text, [](wchar_t ch)
        {
            return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f')
                || (ch >= L'A' && ch <= L'F');
        });
}

/// Decimal-only parse, no signs and no partial consumption. Returns false on overflow.
bool ParseSize(std::wstring_view text, std::uint64_t& value)
{
    if (text.empty() || text.size() > 20)
    {
        return false;
    }
    // Parse manually rather than through from_chars: the view is wide and size fields come
    // from the network, where a partial prefix like "12abc" must be rejected outright.
    std::uint64_t result = 0;
    for (const wchar_t ch : text)
    {
        if (ch < L'0' || ch > L'9')
        {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - L'0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
        {
            return false;
        }
        result = result * 10 + digit;
    }
    value = result;
    return true;
}

} // namespace

std::optional<UpdateFeed> ParseUpdateFeed(std::wstring_view text)
{
    // Required fields must appear, not merely default to zero. A feed missing its hash is a
    // broken or hostile feed and must never be treated as "an update with no checksum".
    bool hasSchema = false;
    bool hasVersion = false;
    bool hasSize = false;
    bool hasSha = false;

    UpdateFeed feed;
    size_t offset = 0;
    while (offset <= text.size())
    {
        const size_t newline = text.find(L'\n', offset);
        const std::wstring_view line = Trim(text.substr(
            offset, newline == std::wstring_view::npos ? std::wstring_view::npos : newline - offset));
        offset = (newline == std::wstring_view::npos) ? text.size() + 1 : newline + 1;

        if (line.empty() || line.starts_with(L"#"))
        {
            continue;
        }
        const size_t equals = line.find(L'=');
        if (equals == std::wstring_view::npos)
        {
            continue;
        }

        const std::wstring_view key = Trim(line.substr(0, equals));
        std::wstring_view value = Trim(line.substr(equals + 1));

        // Notes carry an arbitrary-length changelog, so they get their own (larger) budget and
        // are truncated rather than dropped. Every other field is bounded by kMaxFieldChars;
        // a hostile feed cannot make us allocate an unbounded string for a URL or hash.
        if (key == L"notes.zh" || key == L"notes.en")
        {
            if (value.size() > kMaxNotesChars)
            {
                util::LogWarn(L"update feed: '{}' truncated to {} chars", key, kMaxNotesChars);
                value = value.substr(0, kMaxNotesChars);
            }
        }
        else if (value.size() > kMaxFieldChars)
        {
            util::LogWarn(L"update feed: '{}' is too long, ignoring", key);
            continue;
        }

        if (key == L"schema")
        {
            // A future incompatible format must opt in with a new number; an unknown schema is
            // rejected rather than parsed with today's rules.
            if (value != L"1")
            {
                return std::nullopt;
            }
            hasSchema = true;
        }
        else if (key == L"version")
        {
            const auto parsed = AppVersion::Parse(value);
            if (!parsed)
            {
                return std::nullopt;
            }
            feed.version = *parsed;
            hasVersion = true;
        }
        else if (key == L"size")
        {
            if (!ParseSize(value, feed.size))
            {
                return std::nullopt;
            }
            hasSize = true;
        }
        else if (key == L"sha256")
        {
            if (!IsHex64(value))
            {
                return std::nullopt;
            }
            feed.sha256 = value;
            hasSha = true;
        }
        else if (key == L"url")
        {
            feed.url = value;
        }
        else if (key == L"urlGithub" || key == L"url.github")
        {
            // urlGithub is what the release workflow writes and what the struct calls it. The
            // dotted spelling was read here first and never appeared in a generated feed, which
            // meant the GitHub fallback was silently always empty; it is still accepted so a
            // hand-written or older feed does not lose its mirror.
            feed.urlGithub = value;
        }
        else if (key == L"notes.zh")
        {
            feed.notesZh = value.substr(0, kMaxNotesChars);
        }
        else if (key == L"notes.en")
        {
            feed.notesEn = value.substr(0, kMaxNotesChars);
        }
        // Unknown keys are intentionally ignored: additive metadata from a newer publisher
        // must not break an older client.
    }

    if (!hasSchema || !hasVersion || !hasSize || !hasSha)
    {
        return std::nullopt;
    }
    return feed;
}

} // namespace wintools::core
