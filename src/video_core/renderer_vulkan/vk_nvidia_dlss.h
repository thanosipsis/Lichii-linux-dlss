// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef EGLANTINE_NGX
#include "video_core/renderer_vulkan/vk_ngx_dlss.h"
namespace Vulkan {
using NvidiaDlssUpscaler = NgxDlssUpscaler;
}
#elif defined(EGLANTINE_STREAMLINE)
#include "video_core/renderer_vulkan/vk_streamline_dlss.h"
namespace Vulkan {
using NvidiaDlssUpscaler = StreamlineDlssUpscaler;
}
#endif
