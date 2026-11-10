#pragma once

#include <wincodec.h>

#include "platform/win/com.h"

namespace wintools::platform
{

/// Process-wide WIC factory.
///
/// Creating a factory is cheap but not free, and it is thread-safe to share, so one instance is
/// created lazily and kept alive until the process exits. Returns nullptr when WIC is unavailable
/// (only possible on a broken Windows install).
///
/// The returned pointer is borrowed: the caller must not release it. See wic.cpp for why the
/// lifetime is the whole process rather than an RAII object.
[[nodiscard]] IWICImagingFactory* GetWicFactory();

} // namespace wintools::platform
