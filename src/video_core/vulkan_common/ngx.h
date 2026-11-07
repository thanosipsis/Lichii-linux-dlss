// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::Ngx {

// All NGX calls, including evaluation on the scheduler worker, share this lock.
std::mutex& ApiMutex();
[[nodiscard]] std::filesystem::path UserFolder();
[[nodiscard]] std::filesystem::path FindModel();
bool Start();
[[nodiscard]] bool WantsDevice();
bool QueryDeviceExtensions(VkInstance instance, VkPhysicalDevice physical);
[[nodiscard]] std::vector<std::string> InstanceExtensions();
[[nodiscard]] std::vector<std::string> DeviceExtensions();
bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, vk::DeviceDispatch& dld);
[[nodiscard]] bool IsDlssAvailable();
void Stop();

} // namespace Vulkan::Ngx
