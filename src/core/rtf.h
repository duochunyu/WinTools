#pragma once

#include <string>
#include <string_view>

namespace wintools::core
{

/// Puts a soft line break in front of every embedded picture that does not already start its line.
///
/// RichEdit reserves a picture's box in the line its character lands on and then draws the picture
/// on the line below, so a picture that arrives mid-line leaves a blank band as tall as itself
/// behind it and inflates the height of the paragraph it was pasted into. A \line before the \pict
/// group keeps the picture at the head of its own line, which is where a screenshot belongs anyway.
///
/// Bodies written before this existed carry no break, so they are normalised when they are read
/// back. Everything that is not a picture group is passed through byte for byte, including the
/// base64-ish payload, which cannot contain the marker anyway.
[[nodiscard]] std::string BreakBeforePictures(std::string_view rtf);

} // namespace wintools::core
