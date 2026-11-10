#include "core/group.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

#include "util/log.h"
#include "util/path.h"
#include "util/text.h"

namespace wintools::core
{

namespace
{

constexpr wchar_t kHeader[] = L"# WinTools groups v1";

/// Tabs and newlines would break the record format; neither belongs in a file path.
std::wstring Sanitize(const std::wstring& value)
{
    std::wstring result = value;
    for (wchar_t& character : result)
    {
        if (character == L'\t' || character == L'\r' || character == L'\n')
        {
            character = L' ';
        }
    }
    return result;
}

/// Splits "a;b;c" into its parts, dropping empty ones.
std::vector<std::wstring> SplitMembers(const std::wstring& text)
{
    std::vector<std::wstring> members;
    size_t offset = 0;

    while (offset <= text.size())
    {
        const size_t end = text.find(L';', offset);
        std::wstring part = text.substr(
            offset, end == std::wstring::npos ? std::wstring::npos : end - offset);

        if (!part.empty())
        {
            members.push_back(std::move(part));
        }

        if (end == std::wstring::npos)
        {
            break;
        }

        offset = end + 1;
    }

    return members;
}

/// Membership is a set, so the stored order carries no meaning; the label and the canvas both
/// sort members themselves. Removing by value is therefore enough.
void RemoveMember(std::vector<std::wstring>& members, std::wstring_view devicePath)
{
    std::erase_if(members, [devicePath](const std::wstring& member)
        { return Monitor::IEquals(member, devicePath); });
}

void Renumber(std::vector<Group>& groups)
{
    for (size_t index = 0; index < groups.size(); ++index)
    {
        groups[index].slot = static_cast<int>(index);
    }
}

} // namespace

std::vector<size_t> ReadingOrder(std::span<const MonitorSnapshot> monitors)
{
    // Rows first, then left to right inside a row.
    //
    // Sorting by (left, top) - the obvious thing - numbers a 2x2 desk down the columns: 3 and 1
    // on the left, 4 and 2 on the right. Reading order is what the number is for, so displays are
    // bucketed into rows by vertical overlap and the rows are then walked top to bottom.
    std::vector<size_t> byTop(monitors.size());
    for (size_t index = 0; index < byTop.size(); ++index)
    {
        byTop[index] = index;
    }

    std::sort(byTop.begin(), byTop.end(), [monitors](size_t a, size_t b)
        {
            return (monitors[a].rect.top != monitors[b].rect.top) ? monitors[a].rect.top
                                                                  < monitors[b].rect.top
                                                                  : monitors[a].rect.left
                    < monitors[b].rect.left;
        });

    std::vector<size_t> order;
    order.reserve(byTop.size());
    std::vector<bool> placed(monitors.size(), false);

    for (const size_t anchor : byTop)
    {
        if (placed[anchor])
        {
            continue;
        }

        // A row is seeded by the topmost display that is left over; anything whose height overlaps
        // that seed by more than half of the shorter of the two belongs to the same row. Half,
        // not "any overlap": a 1200-high screen beside a 2160-high one shares 55% of the shorter,
        // while a stacked pair barely grazes.
        std::vector<size_t> row{ anchor };
        placed[anchor] = true;

        const Rect& seed = monitors[anchor].rect;
        for (const size_t candidate : byTop)
        {
            if (placed[candidate])
            {
                continue;
            }

            const Rect& other = monitors[candidate].rect;
            const int overlap = std::min(seed.Bottom(), other.Bottom()) - std::max(seed.top, other.top);
            const int shorter = std::min(seed.height, other.height);

            if (shorter > 0 && overlap * 2 >= shorter)
            {
                row.push_back(candidate);
                placed[candidate] = true;
            }
        }

        std::sort(row.begin(), row.end(), [monitors](size_t a, size_t b)
            {
                return (monitors[a].rect.left != monitors[b].rect.left) ? monitors[a].rect.left
                                                                        < monitors[b].rect.left
                                                                        : monitors[a].rect.top
                        < monitors[b].rect.top;
            });

        order.insert(order.end(), row.begin(), row.end());
    }

    return order;
}

int ScreenNumber(std::wstring_view devicePath, std::span<const MonitorSnapshot> monitors)
{
    if (devicePath.empty())
    {
        return 0;
    }

    const std::vector<size_t> order = ReadingOrder(monitors);
    for (size_t position = 0; position < order.size(); ++position)
    {
        if (Monitor::IEquals(monitors[order[position]].devicePath, devicePath))
        {
            return static_cast<int>(position) + 1;
        }
    }

    return 0;
}

std::vector<size_t> AttachedMembers(const Group& group, std::span<const MonitorSnapshot> monitors)
{
    std::vector<size_t> found;

    for (const size_t index : ReadingOrder(monitors))
    {
        for (const auto& member : group.members)
        {
            if (Monitor::IEquals(member, monitors[index].devicePath))
            {
                found.push_back(index);
                break;
            }
        }
    }

    return found;
}

std::wstring GroupLabel(const Group& group, std::span<const MonitorSnapshot> monitors)
{
    std::vector<int> numbers;
    numbers.reserve(group.members.size());

    for (const size_t index : AttachedMembers(group, monitors))
    {
        numbers.push_back(ScreenNumber(monitors[index].devicePath, monitors));
    }

    if (numbers.empty())
    {
        return {};
    }

    std::sort(numbers.begin(), numbers.end());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());

    std::wstring label;
    for (const int number : numbers)
    {
        if (!label.empty())
        {
            label += L'+';
        }
        label += std::to_wstring(number);
    }

    return label;
}

int GroupOf(const std::vector<Group>& groups, std::wstring_view devicePath)
{
    if (devicePath.empty())
    {
        return -1;
    }

    for (size_t index = 0; index < groups.size(); ++index)
    {
        for (const auto& member : groups[index].members)
        {
            if (Monitor::IEquals(member, devicePath))
            {
                return static_cast<int>(index);
            }
        }
    }

    return -1;
}

void NormaliseGroups(std::vector<Group>& groups, std::span<const MonitorSnapshot> monitors)
{
    // 1. One display, one home. The first group that claims a display keeps it; a later duplicate
    //    is dropped rather than being allowed to make the canvas ambiguous.
    std::vector<std::wstring> claimed;
    for (auto& group : groups)
    {
        std::erase_if(group.members, [&claimed](const std::wstring& member)
            {
                if (member.empty())
                {
                    return true;
                }

                for (const auto& owner : claimed)
                {
                    if (Monitor::IEquals(owner, member))
                    {
                        return true;
                    }
                }

                claimed.push_back(member);
                return false;
            });
    }

    // 2. A group with no members at all is not a group; drop it before newcomers are handed out,
    //    otherwise the first one would inherit an empty slot and the count would be one too high.
    std::erase_if(groups, [](const Group& group) { return group.members.empty(); });

    // 3. A display that is attached but in no group joins the first one. That is what makes a
    //    freshly plugged screen show up inside the arrangement the user already had, instead of
    //    sitting outside every frame with nothing to say about it.
    for (const auto& monitor : monitors)
    {
        if (monitor.devicePath.empty() || GroupOf(groups, monitor.devicePath) >= 0)
        {
            continue;
        }

        if (groups.empty())
        {
            groups.push_back(Group{});
        }

        groups.front().members.push_back(monitor.devicePath);
    }

    // 4. Slots follow list order.
    Renumber(groups);
}

int MoveMonitorToGroup(std::vector<Group>& groups, std::wstring_view devicePath, int targetSlot)
{
    if (devicePath.empty())
    {
        return -1;
    }

    const int from = GroupOf(groups, devicePath);

    if (targetSlot < 0)
    {
        if (from < 0)
        {
            return -1;
        }

        // Already alone: a split would create an identical group next to it.
        if (groups[static_cast<size_t>(from)].members.size() <= 1)
        {
            return from;
        }

        RemoveMember(groups[static_cast<size_t>(from)].members, devicePath);
        if (groups[static_cast<size_t>(from)].members.empty())
        {
            groups.erase(groups.begin() + from);
        }

        Group fresh;
        fresh.members.emplace_back(devicePath);
        groups.push_back(std::move(fresh));
        Renumber(groups);
        return static_cast<int>(groups.size()) - 1;
    }

    if (targetSlot >= static_cast<int>(groups.size()))
    {
        return -1;
    }

    // A display that belongs to no group has nothing to move: every attached display is placed by
    // NormaliseGroups before the panel can offer a menu for it, so this only happens on a stale
    // click, and adding a phantom member from a device path that no longer exists is worse than
    // doing nothing.
    if (from < 0)
    {
        return -1;
    }

    if (from == targetSlot)
    {
        return from;
    }

    RemoveMember(groups[static_cast<size_t>(from)].members, devicePath);

    // A source group that just lost its last member is removed here rather than by a later
    // NormaliseGroups call: the caller asked for one edit, and the slot it is handed back has to
    // describe the list that exists afterwards.
    if (groups[static_cast<size_t>(from)].members.empty())
    {
        groups.erase(groups.begin() + from);
        if (from < targetSlot)
        {
            --targetSlot;
        }
    }

    groups[static_cast<size_t>(targetSlot)].members.emplace_back(devicePath);
    Renumber(groups);
    return targetSlot;
}

std::vector<Group> GroupsFromLegacy(std::span<const MonitorSnapshot> monitors,
    const std::wstring& spanImage, const WallpaperView& spanView,
    const MonitorWallpapers& overrides)
{
    std::vector<Group> groups;

    Group span;
    span.image = spanImage;
    span.view = ClampView(spanView);

    std::vector<Group> singles;

    for (const size_t index : ReadingOrder(monitors))
    {
        const std::wstring& devicePath = monitors[index].devicePath;
        if (devicePath.empty())
        {
            continue;
        }

        const std::wstring_view own = FindMonitorImage(overrides, devicePath);
        if (!own.empty())
        {
            Group single;
            single.image = std::wstring{ own };
            single.members.emplace_back(devicePath);
            singles.push_back(std::move(single));
            continue;
        }

        span.members.emplace_back(devicePath);
    }

    if (!span.members.empty())
    {
        groups.push_back(std::move(span));
    }

    for (auto& single : singles)
    {
        groups.push_back(std::move(single));
    }

    Renumber(groups);
    return groups;
}

std::filesystem::path DefaultGroupFile()
{
    return util::AppDataFolder() / L"groups.tsv";
}

bool SaveGroups(const std::vector<Group>& groups, const std::filesystem::path& file)
{
    std::error_code ec;
    if (file.has_parent_path())
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec)
        {
            util::LogError(L"cannot create '{}': {}", file.parent_path().wstring(),
                util::FromUtf8(ec.message()));
            return false;
        }
    }

    std::wstring text = kHeader;
    text += L'\n';

    for (const auto& group : groups)
    {
        if (group.members.empty())
        {
            continue;
        }

        std::wstring members;
        for (const auto& member : group.members)
        {
            if (!members.empty())
            {
                members += L';';
            }
            members += Sanitize(member);
        }

        // One line per group, fixed column order. The framing is written with four decimals for
        // the same reason settings.txt does: a float that round-trips through a text file should
        // come back the same number, and a crop the user spent time on should not drift.
        text += std::format(L"group\t{:.4f}\t{:.4f}\t{:.4f}\t{}\t{}\t{}\n", group.view.zoom,
            group.view.focusX, group.view.focusY, group.dirty ? 1 : 0, Sanitize(group.image),
            members);
    }

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        util::LogError(L"cannot open '{}' for writing", file.wstring());
        return false;
    }

    const std::string utf8 = util::ToUtf8(text);
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    out.flush();

    if (!out)
    {
        util::LogError(L"cannot write '{}'", file.wstring());
        return false;
    }

    return true;
}

std::optional<std::vector<Group>> LoadGroups(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }

    const std::string utf8{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    const std::wstring text = util::FromUtf8(utf8);

    std::vector<Group> groups;
    size_t offset = 0;

    const auto number = [](const std::wstring& value, double fallback) -> double
    {
        try
        {
            return std::stod(value);
        }
        catch (...)
        {
            return fallback;
        }
    };

    while (offset < text.size())
    {
        const size_t end = text.find(L'\n', offset);
        std::wstring line =
            text.substr(offset, end == std::wstring::npos ? std::wstring::npos : end - offset);
        offset = (end == std::wstring::npos) ? text.size() : end + 1;

        while (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }

        if (line.empty() || line.starts_with(L"#"))
        {
            continue;
        }

        // tag, zoom, focusX, focusY, dirty, image, members
        std::vector<std::wstring> fields;
        size_t start = 0;
        while (true)
        {
            const size_t tab = line.find(L'\t', start);
            fields.push_back(
                line.substr(start, tab == std::wstring::npos ? std::wstring::npos : tab - start));
            if (tab == std::wstring::npos)
            {
                break;
            }
            start = tab + 1;
        }

        if (fields.size() < 7 || fields[0] != L"group")
        {
            continue;
        }

        Group group;
        group.view.zoom = number(fields[1], 1.0);
        group.view.focusX = number(fields[2], 0.5);
        group.view.focusY = number(fields[3], 0.5);
        group.view = ClampView(group.view);
        group.dirty = fields[4] == L"1" || fields[4] == L"true";
        group.image = fields[5];
        group.members = SplitMembers(fields[6]);

        if (group.members.empty())
        {
            continue;
        }

        groups.push_back(std::move(group));
    }

    if (groups.empty())
    {
        return std::nullopt;
    }

    Renumber(groups);
    return groups;
}

} // namespace wintools::core
