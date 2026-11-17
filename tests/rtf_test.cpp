#include "test_framework.h"

#include <string>

#include "core/rtf.h"

using namespace wintools::core;

namespace
{

/// The smallest thing RichEdit writes for a picture: the group word, the blip keyword and a couple
/// of characters of payload.
std::string NoteBody(std::string_view text, bool withBreak)
{
    std::string body = "{\\rtf1\\ansi\\deff0\\pard\\f0\\fs18 ";
    body += text;
    if (withBreak)
    {
        body += "\\line";
    }
    body += "{\\pict\\pngblip\\picw10\\pich10 0a0b0c}\\par}";
    return body;
}

} // namespace

WINTOOLS_TEST(NoteRtf_AddsBreakBeforeAPictureInTheText)
{
    const std::string body = NoteBody("hello", false);
    const std::string patched = BreakBeforePictures(body);

    CHECK(patched.find("hello\\line{\\pict") != std::string::npos);
    CHECK_EQ(patched.size(), body.size() + 5);
}

WINTOOLS_TEST(NoteRtf_LeavesABreakThatIsAlreadyThere)
{
    const std::string body = NoteBody("hello", true);

    CHECK(BreakBeforePictures(body) == body);
}

WINTOOLS_TEST(NoteRtf_LeavesABodyWithoutPicturesAlone)
{
    const std::string body = "{\\rtf1\\ansi\\deff0\\pard hello\\par}";

    CHECK(BreakBeforePictures(body) == body);
}

WINTOOLS_TEST(NoteRtf_BreaksEveryPicture)
{
    const std::string body =
        "{\\rtf1\\ansi a{\\pict\\pngblip 0a0b} b{\\pict\\pngblip 0c0d}\\par}";
    const std::string patched = BreakBeforePictures(body);

    CHECK(patched.find("a\\line{\\pict") != std::string::npos);
    CHECK(patched.find("b\\line{\\pict") != std::string::npos);
}

WINTOOLS_TEST(NoteRtf_KeepsThePictureGroupItselfUntouched)
{
    const std::string body = NoteBody("hello", false);
    const std::string patched = BreakBeforePictures(body);

    // The break goes in front of the group, not inside it: the payload and the size words have to
    // stay exactly as RichEdit wrote them or the picture comes back corrupted.
    CHECK(patched.find("{\\pict\\pngblip\\picw10\\pich10 0a0b0c}") != std::string::npos);
}
