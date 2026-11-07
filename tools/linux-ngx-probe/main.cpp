// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include <vulkan/vulkan.hpp>
// NGX expects its caller to provide the Vulkan types.
#include <nvsdk_ngx_helpers_vk.h>

namespace {
std::atomic_uint validation_errors{};

VKAPI_ATTR VkBool32 VKAPI_CALL
OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
             VkDebugUtilsMessageTypeFlagsEXT,
             const VkDebugUtilsMessengerCallbackDataEXT *message, void *) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    ++validation_errors;
  }
  std::cerr << message->pMessage << '\n';
  return VK_FALSE;
}

struct ValidationLog {
  VkInstance instance;
  VkDebugUtilsMessengerEXT messenger{};

  void Enable() {
    const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerCreateInfoEXT info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = OnValidation;
    if (!create || create(instance, &info, nullptr, &messenger) != VK_SUCCESS) {
      throw std::runtime_error("failed to install Vulkan validation callback");
    }
  }

  ~ValidationLog() {
    if (messenger) {
      const auto destroy =
          reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
              vkGetInstanceProcAddr(instance,
                                    "vkDestroyDebugUtilsMessengerEXT"));
      destroy(instance, messenger, nullptr);
    }
  }
};

constexpr char ProjectId[] = "83845793-fae6-46cc-a72d-1ba2ec45150a";

void Check(std::string_view operation, NVSDK_NGX_Result result) {
  if (NVSDK_NGX_FAILED(result)) {
    std::cerr << operation << " failed: 0x" << std::hex
              << static_cast<unsigned>(result) << std::dec << '\n';
    throw std::runtime_error("NGX operation failed");
  }
}

std::vector<const char *> Extensions(uint32_t count,
                                     const VkExtensionProperties *properties) {
  std::vector<const char *> names;
  for (uint32_t i = 0; i < count; ++i) {
    std::cout << "  " << properties[i].extensionName << '\n';
    names.push_back(properties[i].extensionName);
  }
  return names;
}

uint32_t MemoryType(vk::PhysicalDevice gpu, uint32_t allowed,
                    vk::MemoryPropertyFlags flags) {
  const auto properties = gpu.getMemoryProperties();
  for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
    if ((allowed & (1U << i)) &&
        (properties.memoryTypes[i].propertyFlags & flags) == flags) {
      return i;
    }
  }
  throw std::runtime_error("no suitable Vulkan memory type");
}

struct Image {
  vk::UniqueDeviceMemory memory;
  vk::UniqueImage image;
  vk::UniqueImageView view;
  vk::ImageSubresourceRange range;
  NVSDK_NGX_Resource_VK resource;
};

Image MakeImage(vk::PhysicalDevice gpu, vk::Device device, vk::Extent2D size,
                vk::Format format, bool output = false) {
  const bool depth = format == vk::Format::eD32Sfloat;
  Image result;
  result.range = {depth ? vk::ImageAspectFlagBits::eDepth
                        : vk::ImageAspectFlagBits::eColor,
                  0, 1, 0, 1};
  auto usage =
      vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
  if (output) {
    usage |=
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage;
  }
  result.image = device.createImageUnique({{},
                                           vk::ImageType::e2D,
                                           format,
                                           {size.width, size.height, 1},
                                           1,
                                           1,
                                           vk::SampleCountFlagBits::e1,
                                           vk::ImageTiling::eOptimal,
                                           usage,
                                           vk::SharingMode::eExclusive});
  const auto req = device.getImageMemoryRequirements(*result.image);
  result.memory = device.allocateMemoryUnique(
      {req.size, MemoryType(gpu, req.memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eDeviceLocal)});
  device.bindImageMemory(*result.image, *result.memory, 0);
  result.view = device.createImageViewUnique(
      {{}, *result.image, vk::ImageViewType::e2D, format, {}, result.range});
  result.resource = NVSDK_NGX_Create_ImageView_Resource_VK(
      *result.view, *result.image, result.range, static_cast<VkFormat>(format),
      size.width, size.height, output);
  return result;
}

void Transition(vk::CommandBuffer cb, const Image &image,
                vk::ImageLayout old_layout, vk::ImageLayout layout,
                vk::AccessFlags src, vk::AccessFlags dst) {
  const vk::ImageMemoryBarrier barrier(
      src, dst, old_layout, layout, VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED, *image.image, image.range);
  cb.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                     vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                     barrier);
}

struct Commands {
  vk::Queue queue;
  vk::CommandBuffer cb;

  void Begin() { cb.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit}); }

  void Submit() {
    cb.end();
    queue.submit(vk::SubmitInfo({}, {}, cb), {});
    queue.waitIdle();
    cb.reset();
  }
};

void Evaluate(vk::PhysicalDevice gpu, vk::Device device, Commands commands,
              NVSDK_NGX_Parameter *params, vk::Extent2D render,
              vk::Extent2D target, vk::Extent2D backing) {
  auto color = MakeImage(gpu, device, backing, vk::Format::eR16G16B16A16Sfloat);
  auto depth = MakeImage(gpu, device, backing, vk::Format::eD32Sfloat);
  auto motion = MakeImage(gpu, device, backing, vk::Format::eR16G16Sfloat);
  auto output =
      MakeImage(gpu, device, target, vk::Format::eR16G16B16A16Sfloat, true);
  const size_t bytes = size_t{target.width} * target.height * 8;
  vk::UniqueDeviceMemory staging_memory;
  auto staging =
      device.createBufferUnique({{},
                                 bytes,
                                 vk::BufferUsageFlagBits::eTransferDst |
                                     vk::BufferUsageFlagBits::eTransferSrc});
  const auto req = device.getBufferMemoryRequirements(*staging);
  staging_memory = device.allocateMemoryUnique(
      {req.size, MemoryType(gpu, req.memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eHostVisible |
                                vk::MemoryPropertyFlagBits::eHostCoherent)});
  device.bindBufferMemory(*staging, *staging_memory, 0);

  commands.Begin();
  NVSDK_NGX_DLSS_Create_Params create{};
  create.Feature = {render.width, render.height, target.width, target.height,
                    NVSDK_NGX_PerfQuality_Value_MaxQuality};
  create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
  NVSDK_NGX_Handle *raw_handle{};
  Check("create DLSS",
        NGX_VULKAN_CREATE_DLSS_EXT1(device, commands.cb, 1, 1, &raw_handle,
                                    params, &create));
  std::unique_ptr<NVSDK_NGX_Handle, decltype(&NVSDK_NGX_VULKAN_ReleaseFeature)>
      handle(raw_handle, NVSDK_NGX_VULKAN_ReleaseFeature);
  commands.Submit();
  std::cout << "Created " << render.width << 'x' << render.height << " -> "
            << target.width << 'x' << target.height << " backing "
            << backing.width << 'x' << backing.height << '\n';
  for (int frame = 0; frame < 3; ++frame) {
    auto cb = commands.cb;
    commands.Begin();
    const auto old_input = frame ? vk::ImageLayout::eShaderReadOnlyOptimal
                                 : vk::ImageLayout::eUndefined;
    const auto old_output = frame ? vk::ImageLayout::eTransferSrcOptimal
                                  : vk::ImageLayout::eUndefined;
    for (const auto *image : {&color, &depth, &motion}) {
      Transition(cb, *image, old_input, vk::ImageLayout::eTransferDstOptimal,
                 frame ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{},
                 vk::AccessFlagBits::eTransferWrite);
    }
    Transition(cb, output, old_output, vk::ImageLayout::eTransferDstOptimal,
               frame ? vk::AccessFlagBits::eTransferRead : vk::AccessFlags{},
               vk::AccessFlagBits::eTransferWrite);
    const std::array<float, 4> scene =
        frame == 2 ? std::array{0.75f, 0.25f, 0.1f, 1.f}
                   : std::array{0.1f, 0.25f, 0.75f, 1.f};
    cb.clearColorImage(*color.image, vk::ImageLayout::eTransferDstOptimal,
                       vk::ClearColorValue(scene), color.range);
    if (backing != render) {
      // Fill the unused right and bottom strips with RGBA16F (2, 2, 2, 2).
      // Sampling the full image instead of the render subrect spills this into
      // the output.
      cb.fillBuffer(*staging, 0, bytes, 0x40004000);
      const auto transfer = vk::PipelineStageFlagBits::eTransfer;
      const auto read = vk::AccessFlagBits::eTransferRead;
      const auto write = vk::AccessFlagBits::eTransferWrite;
      cb.pipelineBarrier(transfer, transfer, {},
                         vk::MemoryBarrier(write, read | write), {}, {});
      std::vector<vk::BufferImageCopy> strips;
      const auto add_strip = [&](int x, int y, uint32_t width,
                                 uint32_t height) {
        if (width && height) {
          if (uint64_t{width} * height * 8 > bytes) {
            throw std::runtime_error("padding exceeds staging allocation");
          }
          strips.emplace_back(0, 0, 0,
                              vk::ImageSubresourceLayers(
                                  vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                              vk::Offset3D(x, y, 0),
                              vk::Extent3D(width, height, 1));
        }
      };
      add_strip(render.width, 0, backing.width - render.width, backing.height);
      add_strip(0, render.height, render.width, backing.height - render.height);
      cb.copyBufferToImage(*staging, *color.image,
                           vk::ImageLayout::eTransferDstOptimal, strips);
      cb.pipelineBarrier(transfer, transfer, {}, vk::MemoryBarrier(read, write),
                         {}, {});
    }
    cb.clearDepthStencilImage(*depth.image,
                              vk::ImageLayout::eTransferDstOptimal,
                              vk::ClearDepthStencilValue(0.5f, 0), depth.range);
    cb.clearColorImage(*motion.image, vk::ImageLayout::eTransferDstOptimal,
                       vk::ClearColorValue(std::array<float, 4>{}),
                       motion.range);
    cb.clearColorImage(*output.image, vk::ImageLayout::eTransferDstOptimal,
                       vk::ClearColorValue(std::array<float, 4>{}),
                       output.range);
    for (const auto *image : {&color, &depth, &motion}) {
      Transition(cb, *image, vk::ImageLayout::eTransferDstOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::AccessFlagBits::eTransferWrite,
                 vk::AccessFlagBits::eShaderRead);
    }
    Transition(cb, output, vk::ImageLayout::eTransferDstOptimal,
               vk::ImageLayout::eGeneral, vk::AccessFlagBits::eTransferWrite,
               vk::AccessFlagBits::eShaderWrite |
                   vk::AccessFlagBits::eTransferWrite);
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color.resource;
    eval.Feature.pInOutput = &output.resource;
    eval.pInDepth = &depth.resource;
    eval.pInMotionVectors = &motion.resource;
    eval.InJitterOffsetX = frame == 1 ? 0.25f : -0.25f;
    eval.InRenderSubrectDimensions = {render.width, render.height};
    eval.InReset = frame != 1;
    eval.InMVScaleX = eval.InMVScaleY = 1.f;
    eval.InPreExposure = eval.InExposureScale = 1.f;
    eval.InFrameTimeDeltaInMsec = 16.67f;
    Check("evaluate DLSS",
          NGX_VULKAN_EVALUATE_DLSS_EXT(cb, handle.get(), params, &eval));
    Transition(cb, output, vk::ImageLayout::eGeneral,
               vk::ImageLayout::eTransferSrcOptimal,
               vk::AccessFlagBits::eShaderWrite |
                   vk::AccessFlagBits::eTransferWrite,
               vk::AccessFlagBits::eTransferRead);
    vk::BufferImageCopy copy{};
    copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.imageExtent = vk::Extent3D(target.width, target.height, 1);
    cb.copyImageToBuffer(*output.image, vk::ImageLayout::eTransferSrcOptimal,
                         *staging, copy);
    cb.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                       vk::PipelineStageFlagBits::eHost, {},
                       vk::MemoryBarrier(vk::AccessFlagBits::eTransferWrite,
                                         vk::AccessFlagBits::eHostRead),
                       {}, {});
    commands.Submit();
    const auto *data = static_cast<const uint16_t *>(
        device.mapMemory(*staging_memory, 0, bytes));
    std::array<double, 3> mean{};
    bool finite = true;
    size_t bright_pixels{};
    for (size_t i = 0; i < bytes / 2; i += 4) {
      bool bright = false;
      for (size_t c = 0; c < mean.size(); ++c) {
        const float value = std::bit_cast<_Float16>(data[i + c]);
        finite &= std::isfinite(value);
        mean[c] += value;
        bright |= value > 1.0f;
      }
      bright_pixels += bright;
    }
    device.unmapMemory(*staging_memory);
    for (auto &channel : mean) {
      channel /= static_cast<double>(target.width) * target.height;
    }
    std::cout << "Frame " << frame << " reset=" << eval.InReset << " mean RGB "
              << mean[0] << ' ' << mean[1] << ' ' << mean[2]
              << " bright_pixels=" << bright_pixels << '\n';
    if (!finite || bright_pixels || std::abs(mean[0] - scene[0]) > 0.01 ||
        std::abs(mean[1] - scene[1]) > 0.01 ||
        std::abs(mean[2] - scene[2]) > 0.01) {
      throw std::runtime_error("DLSS output differs from synthetic scene");
    }
  }
}

void Run(const std::filesystem::path &runtime,
         const std::filesystem::path &data) {
  const auto search = std::filesystem::absolute(runtime).wstring();
  const auto path = search.c_str();
  std::filesystem::create_directories(data);
  const auto data_path = std::filesystem::absolute(data).wstring();
  NVSDK_NGX_FeatureCommonInfo common{};
  common.PathListInfo = {&path, 1};
  NVSDK_NGX_FeatureDiscoveryInfo discovery{};
  discovery.SDKVersion = NVSDK_NGX_Version_API;
  discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
  discovery.Identifier.IdentifierType =
      NVSDK_NGX_Application_Identifier_Type_Project_Id;
  discovery.Identifier.v.ProjectDesc = {ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                        "1.0"};
  discovery.FeatureInfo = &common;
  discovery.ApplicationDataPath = data_path.c_str();

  uint32_t count{};
  VkExtensionProperties *properties{};
  Check("instance extension discovery",
        NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
            &discovery, &count, &properties));
  std::cout << "Required instance extensions (" << count << ")\n";
  auto instance_exts = Extensions(count, properties);
  const vk::ApplicationInfo app("Lichii Linux NGX probe", 1, "Lichii", 1,
                                VK_API_VERSION_1_3);
  std::vector<const char *> layers;
  for (const auto &layer : vk::enumerateInstanceLayerProperties()) {
    if (std::string_view(layer.layerName.data()) ==
        "VK_LAYER_KHRONOS_validation") {
      layers.push_back("VK_LAYER_KHRONOS_validation");
      instance_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
  }
  std::cout << "Khronos validation "
            << (layers.empty() ? "unavailable" : "enabled") << '\n';
  auto instance = vk::createInstanceUnique({{}, &app, layers, instance_exts});
  ValidationLog validation{*instance};
  if (!layers.empty()) {
    validation.Enable();
  }
  auto devices = instance->enumeratePhysicalDevices();
  const auto it = std::ranges::find_if(
      devices, [](auto gpu) { return gpu.getProperties().vendorID == 0x10de; });
  if (it == devices.end()) {
    throw std::runtime_error("NVIDIA Vulkan device unavailable");
  }
  const auto gpu = *it;
  std::cout << "Device " << gpu.getProperties().deviceName.data() << '\n';
  NVSDK_NGX_FeatureRequirement requirements{};
  Check("feature requirements", NVSDK_NGX_VULKAN_GetFeatureRequirements(
                                    *instance, gpu, &discovery, &requirements));
  if (requirements.FeatureSupported != 0) {
    throw std::runtime_error("feature support flags " +
                             std::to_string(requirements.FeatureSupported));
  }
  Check("device extension discovery",
        NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
            *instance, gpu, &discovery, &count, &properties));
  std::cout << "Required device extensions (" << count << ")\n";
  const auto device_exts = Extensions(count, properties);
  const auto families = gpu.getQueueFamilyProperties();
  const auto compute = std::ranges::find_if(families, [](const auto &props) {
    return bool(props.queueFlags & vk::QueueFlagBits::eCompute);
  });
  if (compute == families.end()) {
    throw std::runtime_error("no compute queue");
  }
  const auto family = static_cast<uint32_t>(compute - families.begin());
  const float priority = 1.f;
  const vk::DeviceQueueCreateInfo queue_info({}, family, 1, &priority);
  vk::PhysicalDeviceBufferDeviceAddressFeatures address{};
  vk::PhysicalDeviceFeatures2 supported{};
  supported.pNext = &address;
  gpu.getFeatures2(&supported);
  if (!address.bufferDeviceAddress) {
    throw std::runtime_error("buffer device address unavailable");
  }
  address.bufferDeviceAddressCaptureReplay = false;
  address.bufferDeviceAddressMultiDevice = false;
  auto device = gpu.createDeviceUnique(
      vk::DeviceCreateInfo({}, queue_info, {}, device_exts).setPNext(&address));
  auto queue = device->getQueue(family, 0);
  auto pool = device->createCommandPoolUnique(
      {vk::CommandPoolCreateFlagBits::eResetCommandBuffer, family});
  auto buffers = device->allocateCommandBuffersUnique(
      {*pool, vk::CommandBufferLevel::ePrimary, 1});
  Check("initialize NGX",
        NVSDK_NGX_VULKAN_Init_with_ProjectID(
            ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data_path.c_str(),
            *instance, gpu, *device, vkGetInstanceProcAddr, vkGetDeviceProcAddr,
            &common));
  struct Shutdown {
    VkDevice device;
    ~Shutdown() { NVSDK_NGX_VULKAN_Shutdown1(device); }
  } shutdown{*device};
  NVSDK_NGX_Parameter *raw_params{};
  Check("query capabilities",
        NVSDK_NGX_VULKAN_GetCapabilityParameters(&raw_params));
  std::unique_ptr<NVSDK_NGX_Parameter,
                  decltype(&NVSDK_NGX_VULKAN_DestroyParameters)>
      params(raw_params, NVSDK_NGX_VULKAN_DestroyParameters);
  int available{};
  Check("query SuperSampling.Available",
        params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available));
  std::cout << "NGX initialized, SuperSampling.Available=" << available << '\n';
  if (!available) {
    throw std::runtime_error("DLSS unavailable");
  }
  const Commands commands{queue, *buffers.front()};
  Evaluate(gpu, *device, commands, params.get(), {640, 360}, {960, 540},
           {640, 360});
  Evaluate(gpu, *device, commands, params.get(), {800, 450}, {1200, 675},
           {800, 450});
  Evaluate(gpu, *device, commands, params.get(), {800, 450}, {1200, 675},
           {832, 480});
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "Usage: linux-ngx-probe RUNTIME_DIR WRITABLE_DATA_DIR\n";
    return 1;
  }
  try {
    Run(argv[1], argv[2]);
    if (validation_errors.load()) {
      throw std::runtime_error("Vulkan validation reported errors");
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
