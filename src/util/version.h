#pragma once

// Typed access to the version baked into this binary. The macros come from the generated
// version_macros.h (from project(VERSION)); keeping the C++ wrapper here means rc.exe only ever
// sees the macro-only generated header and never has to parse a namespace.
//
// The generated file is deliberately NOT called version.h: quotes-include from a file of that
// name would resolve to this wrapper itself, and the macros would never be seen.

#include "version_macros.h"

namespace wintools
{

/// The version of the running exe, as "1.2.3" - whatever project(VERSION) says, never repeated
/// here. A compile-time fact, never a setting or a file: auto-update compares this against the
/// feed's version to decide whether to offer an upgrade.
inline constexpr wchar_t kVersion[] = WINTOOLS_VERSION_STRING;

} // namespace wintools
