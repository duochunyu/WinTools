#include "core/rtf.h"

namespace wintools::core
{

namespace
{

/// The word that starts a picture group, and the break that has to come before it.
constexpr std::string_view kPicture = "{\\pict";
constexpr std::string_view kBreak = "\\line";

} // namespace

std::string BreakBeforePictures(std::string_view rtf)
{
    std::string patched{ rtf };

    std::size_t at = patched.find(kPicture);
    while (at != std::string::npos)
    {
        const bool already = at >= kBreak.size()
            && std::string_view{ patched }.substr(at - kBreak.size(), kBreak.size()) == kBreak;

        if (!already)
        {
            patched.insert(at, kBreak);
            at += kBreak.size();
        }

        at = patched.find(kPicture, at + kPicture.size());
    }

    return patched;
}

} // namespace wintools::core
