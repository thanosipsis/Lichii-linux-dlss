// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdlib>
#include <vulkan/vulkan.h>

// NGX declarations require Vulkan types to be visible first.
#include <nvsdk_ngx_vk.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/ngx.h"

namespace Vulkan::Ngx {
namespace {
std::mutex api_mutex;
bool requested{};
std::atomic<bool> available{};
VkDevice attached_device{};
NVSDK_NGX_Parameter* capabilities{};
std::vector<std::string> instance_extensions;
std::vector<std::string> device_extensions;
constexpr const char* ProjectId = "9c6137f3-5a59-45a7-99d9-7b50ed097e51";
struct DiscoveryContext {
    std::wstring folder;
    std::wstring cache;
    const wchar_t* path{};
    NVSDK_NGX_FeatureCommonInfo info{};
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    explicit DiscoveryContext(const std::filesystem::path& model)
        : folder{model.parent_path().wstring()},
          cache{(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "ngx").wstring()},
          path{folder.c_str()} {
        info.PathListInfo = {&path, 1};
        discovery.SDKVersion = NVSDK_NGX_Version_API;
        discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
        discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        discovery.Identifier.v.ProjectDesc = {ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                              "Lichii Linux DLSS"};
        discovery.ApplicationDataPath = cache.c_str();
        discovery.FeatureInfo = &info;
    }
};
} // namespace

std::mutex& ApiMutex() {
    return api_mutex;
}

std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "ngx";
}

std::filesystem::path FindModel() {
    const char* override_dir = std::getenv("EDEN_NGX_DIR");
    const auto directory = override_dir ? std::filesystem::path{override_dir} : UserFolder();
    std::error_code ec;
    const auto unversioned = directory / "libnvidia-ngx-dlss.so";
    if (std::filesystem::is_regular_file(unversioned, ec)) {
        return unversioned;
    }
    std::filesystem::path found;
    for (std::filesystem::directory_iterator it{directory, ec}, end; !ec && it != end;
         it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.starts_with("libnvidia-ngx-dlss.so.") && it->is_regular_file(ec) &&
            (found.empty() || name > found.filename().string())) {
            found = it->path();
        }
    }
    return found;
}

bool Start() {
    std::scoped_lock lock{api_mutex};
    requested = false;
    instance_extensions.clear();
    device_extensions.clear();
    if (!Settings::values.nvidia_dlss.GetValue()) {
        return false;
    }
    if (FindModel().empty()) {
        LOG_WARNING(Render_Vulkan,
                    "NVIDIA DLSS: no Linux runtime in {}; using the existing renderer",
                    UserFolder().string());
        return false;
    }
    std::error_code cache_error;
    std::filesystem::create_directories(
        Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "ngx", cache_error);
    if (cache_error) {
        LOG_WARNING(Render_Vulkan, "NVIDIA NGX cache directory is unavailable: {}",
                    cache_error.message());
        return false;
    }
    const DiscoveryContext context{FindModel()};
    unsigned count{};
    VkExtensionProperties* properties{};
    const auto result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
        &context.discovery, &count, &properties);
    if (NVSDK_NGX_FAILED(result)) {
        LOG_WARNING(Render_Vulkan, "NVIDIA NGX extension discovery failed ({:#x})",
                    static_cast<u32>(result));
        return false;
    }
    for (unsigned i = 0; i < count; ++i)
        instance_extensions.emplace_back(properties[i].extensionName);
    requested = true;
    return true;
}

bool QueryDeviceExtensions(VkInstance instance, VkPhysicalDevice physical) {
    std::scoped_lock lock{api_mutex};
    const DiscoveryContext context{FindModel()};
    unsigned count{};
    VkExtensionProperties* properties{};
    const auto result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        instance, physical, &context.discovery, &count, &properties);
    device_extensions.clear();
    if (NVSDK_NGX_FAILED(result)) {
        LOG_WARNING(Render_Vulkan, "NVIDIA NGX device extension discovery failed ({:#x})",
                    static_cast<u32>(result));
        requested = false;
        return false;
    }
    for (unsigned i = 0; i < count; ++i)
        device_extensions.emplace_back(properties[i].extensionName);
    return true;
}

bool WantsDevice() {
    return requested;
}
std::vector<std::string> InstanceExtensions() {
    return instance_extensions;
}
std::vector<std::string> DeviceExtensions() {
    return device_extensions;
}
bool IsDlssAvailable() {
    return available.load(std::memory_order_acquire);
}

bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device, u32,
                  vk::DeviceDispatch& dld) {
    std::scoped_lock lock{api_mutex};
    if (!requested)
        return false;
    const auto model = FindModel();
    if (model.empty())
        return false;
    const std::wstring folder = model.parent_path().wstring();
    const wchar_t* paths[] = {folder.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo = {paths, 1};
    const auto cache = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "ngx";
    std::error_code ec;
    std::filesystem::create_directories(cache, ec);
    if (ec)
        return false;
    const auto cache_wide = cache.wstring();
    auto result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "Lichii Linux DLSS", cache_wide.c_str(), instance,
        physical, device, dld.vkGetInstanceProcAddr, dld.vkGetDeviceProcAddr, &info);
    if (NVSDK_NGX_FAILED(result)) {
        LOG_WARNING(Render_Vulkan, "NVIDIA NGX initialization failed ({:#x}); keeping fallback",
                    static_cast<u32>(result));
        return false;
    }
    attached_device = device;
    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&capabilities);
    int supported{};
    if (NVSDK_NGX_SUCCEED(result) && capabilities) {
        capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
    }
    if (!supported) {
        if (capabilities)
            NVSDK_NGX_VULKAN_DestroyParameters(capabilities);
        capabilities = nullptr;
        NVSDK_NGX_VULKAN_Shutdown1(attached_device);
        attached_device = VK_NULL_HANDLE;
    }
    available.store(supported != 0, std::memory_order_release);
    LOG_INFO(Render_Vulkan,
             "NVIDIA NGX Linux DLSS capability: {} (runtime {}); game evaluations are reported "
             "separately",
             supported ? "available" : "unavailable", model.string());
    return supported != 0;
}

void Stop() {
    std::scoped_lock lock{api_mutex};
    available.store(false, std::memory_order_release);
    if (capabilities) {
        NVSDK_NGX_VULKAN_DestroyParameters(capabilities);
        capabilities = nullptr;
    }
    if (attached_device) {
        NVSDK_NGX_VULKAN_Shutdown1(attached_device);
        attached_device = VK_NULL_HANDLE;
    }
    requested = false;
}
} // namespace Vulkan::Ngx
