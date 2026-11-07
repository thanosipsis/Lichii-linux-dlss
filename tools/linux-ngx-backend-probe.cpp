// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises the production Vulkan/NGX adapter without a frontend, Core::System
// or game.
// Load engine declarations before the Xlib macros brought in by Vulkan platform headers.
#include "video_core/renderer_vulkan/vk_state_tracker.h"

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_ngx_dlss.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/ngx.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace Vulkan;
std::atomic_uint validation_errors{};
VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                          VkDebugUtilsMessageTypeFlagsEXT,
                                          const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        ++validation_errors;
    std::cerr << data->pMessage << '\n';
    return VK_FALSE;
}
void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct InputImage {
    vk::Image image;
    vk::ImageView view;
    VkFormat format;
    VkExtent2D extent;
    VkImageAspectFlags aspects;
    bool initialized{};

    InputImage(const Device& device, MemoryAllocator& allocator, VkFormat format_,
               VkExtent2D extent_, VkImageAspectFlags aspects_)
        : format{format_}, extent{extent_}, aspects{aspects_} {
        image = allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage =
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                       : 0u),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        const VkImageAspectFlags view_aspect = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
                                                   ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT}
                                                   : VkImageAspectFlags{VK_IMAGE_ASPECT_COLOR_BIT};
        view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = *image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {view_aspect, 0, 1, 0, 1},
        });
    }
    NgxDlssUpscaler::Image Resource() const {
        return {*view,
                *image,
                format,
                {(aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
                     ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT}
                     : VkImageAspectFlags{VK_IMAGE_ASPECT_COLOR_BIT},
                 0, 1, 0, 1},
                aspects,
                extent};
    }
    void Fill(const Device& device, Scheduler& scheduler, std::array<float, 4> value) {
        const VkImageSubresourceRange range{aspects, 0, 1, 0, 1};
        const VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = initialized ? VkAccessFlags{VK_ACCESS_SHADER_READ_BIT} : 0u,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *image,
            .subresourceRange = range,
        };
        const auto clear_depth = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
            device.GetDispatchLoader().vkGetDeviceProcAddr(*device.GetLogical(),
                                                           "vkCmdClearDepthStencilImage"));
        scheduler.Record([barrier, range, value, clear_depth](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0, barrier);
            if (range.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
                const VkClearDepthStencilValue depth{0.5f, 0};
                clear_depth(*cmd, barrier.image, VK_IMAGE_LAYOUT_GENERAL, &depth, 1, &range);
            } else {
                VkClearColorValue color{};
                std::copy(value.begin(), value.end(), color.float32);
                cmd.ClearColorImage(barrier.image, VK_IMAGE_LAYOUT_GENERAL, color, range);
            }
        });
        initialized = true;
    }
};

void CheckOutput(MemoryAllocator& allocator, Scheduler& scheduler, VkImage image, VkExtent2D extent,
                 std::array<float, 4> expected) {
    const size_t bytes = size_t{extent.width} * extent.height * 8;
    auto buffer = allocator.CreateBuffer(
        {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bytes,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        },
        MemoryUsage::Download);
    scheduler.Record([image, destination = *buffer, extent](vk::CommandBuffer cmd) {
        const VkImageMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            before);
        const VkBufferImageCopy copy{
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {extent.width, extent.height, 1},
        };
        cmd.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_GENERAL, destination, copy);
        const VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, host);
    });
    scheduler.Finish();
    buffer.Invalidate();
    const auto pixels = buffer.Mapped();
    std::array<double, 3> sum{};
    for (size_t offset = 0; offset < bytes; offset += 8) {
        for (size_t c = 0; c < 4; ++c) {
            std::uint16_t bits;
            std::memcpy(&bits, pixels.data() + offset + c * 2, 2);
            const float value = static_cast<float>(std::bit_cast<_Float16>(bits));
            Require(std::isfinite(value), "DLSS output contains nonfinite pixels");
            if (c < 3) {
                Require(std::abs(value - expected[c]) < 0.025f,
                        "DLSS output has a corrupt synthetic-scene pixel");
                sum[c] += value;
            }
        }
    }
    std::cout << " mean RGB";
    for (size_t c = 0; c < 3; ++c) {
        const double mean = sum[c] / (size_t{extent.width} * extent.height);
        std::cout << ' ' << mean;
        Require(std::abs(mean - expected[c]) < 0.015,
                "DLSS output does not match the synthetic scene");
    }
    std::cout << '\n';
}

void Run(bool validation) {
    Require(Ngx::Start(), "NGX did not start; set EDEN_NGX_DIR to the external "
                          "Linux runtime directory");
    const auto library = OpenLibrary();
    vk::InstanceDispatch dispatch;
    auto instance = CreateInstance(*library, dispatch, VK_API_VERSION_1_1,
                                   Core::Frontend::WindowSystemType::Headless, validation);
    if (validation) {
        const auto layers = vk::EnumerateInstanceLayerProperties(dispatch);
        Require(layers && std::ranges::any_of(*layers, [](const auto& layer) {
                    return std::string_view{layer.layerName} == "VK_LAYER_KHRONOS_validation";
                }),
                "--validation requires VK_LAYER_KHRONOS_validation");
    }
    auto messenger = validation
                         ? instance.CreateDebugUtilsMessenger({
                               .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                               .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                               .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                              VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                               .pfnUserCallback = Validation,
                           })
                         : vk::DebugUtilsMessenger{};
    const auto devices = instance.EnumeratePhysicalDevices();
    const auto selected = std::find_if(devices.begin(), devices.end(), [&](auto raw) {
        return vk::PhysicalDevice{raw, dispatch}.GetProperties().vendorID == 0x10de;
    });
    Require(selected != devices.end(), "NVIDIA Vulkan device required");
    Device device{*instance, vk::PhysicalDevice{*selected, dispatch}, {}, dispatch};
    Require(Ngx::IsDlssAvailable(), "Device does not support native NGX DLSS");
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    {
        NgxDlssUpscaler upscaler{device, scheduler, allocator};
        struct Case {
            VkExtent2D render, backing, output;
            VkFormat color, depth;
        };
        const std::array cases{
            Case{{640, 360},
                 {640, 360},
                 {960, 540},
                 VK_FORMAT_R16G16B16A16_SFLOAT,
                 VK_FORMAT_D32_SFLOAT},
            Case{{800, 450},
                 {832, 480},
                 {1200, 675},
                 VK_FORMAT_R16G16B16A16_SFLOAT,
                 VK_FORMAT_D32_SFLOAT},
            Case{{1248, 700},
                 {1248, 700},
                 {1920, 1080},
                 VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                 VK_FORMAT_D24_UNORM_S8_UINT},
            Case{{640, 360},
                 {640, 360},
                 {640, 360},
                 VK_FORMAT_R16G16B16A16_SFLOAT,
                 VK_FORMAT_D32_SFLOAT},
            Case{{640, 360},
                 {640, 360},
                 {641, 361},
                 VK_FORMAT_R16G16B16A16_SFLOAT,
                 VK_FORMAT_D32_SFLOAT},
        };
        for (const auto& test : cases) {
            InputImage color{device, allocator, test.color, test.backing,
                             VK_IMAGE_ASPECT_COLOR_BIT};
            InputImage motion{device, allocator, VK_FORMAT_R16G16_SFLOAT, test.backing,
                              VK_IMAGE_ASPECT_COLOR_BIT};
            const auto depth_aspects =
                VK_IMAGE_ASPECT_DEPTH_BIT |
                (test.depth == VK_FORMAT_D24_UNORM_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
            InputImage depth{device, allocator, test.depth, test.backing, depth_aspects};
            NgxDlssUpscaler::Inputs inputs{
                color.Resource(), depth.Resource(), motion.Resource(), test.render,
                test.output,      {0.0f, 0.0f},     {1.0f, 1.0f},      true,
            };
            auto invalid = inputs;
            invalid.render_size.width = 0;
            Require(!upscaler.Upscale(invalid), "Empty render size was accepted");
            invalid = inputs;
            invalid.depth.view = {};
            Require(!upscaler.Upscale(invalid), "Missing depth view was accepted");
            invalid = inputs;
            invalid.color.extent.width = test.render.width - 1;
            Require(!upscaler.Upscale(invalid), "Undersized input extent was accepted");
            invalid = inputs;
            invalid.jitter[0] = std::numeric_limits<float>::quiet_NaN();
            Require(!upscaler.Upscale(invalid), "Nonfinite jitter was accepted");
            invalid = inputs;
            invalid.motion_vectors = invalid.color;
            Require(!upscaler.Upscale(invalid), "Aliased resources were accepted");
            invalid = inputs;
            invalid.upscale_size.width = std::numeric_limits<u32>::max();
            Require(!upscaler.Upscale(invalid), "Impossible output extent was accepted");
            invalid = inputs;
            invalid.jitter[0] = 0.75f;
            Require(!upscaler.Upscale(invalid), "Out-of-range jitter was accepted");
            invalid = inputs;
            invalid.motion_vector_scale[0] = 0.0f;
            Require(!upscaler.Upscale(invalid), "Zero motion scale was accepted");
            for (unsigned frame = 0; frame < 4; ++frame) {
                const std::array<float, 4> scene = frame >= 2
                                                       ? std::array{0.75f, 0.25f, 0.1f, 1.0f}
                                                       : std::array{0.1f, 0.25f, 0.75f, 1.0f};
                color.Fill(device, scheduler, scene);
                motion.Fill(device, scheduler, {});
                depth.Fill(device, scheduler, {});
                inputs.reset = frame != 1;
                // Recreate a same-size feature when only the depth convention changes.
                inputs.depth_inverted =
                    test.depth == VK_FORMAT_D24_UNORM_S8_UINT ? frame == 3 : frame != 3;
                inputs.jitter =
                    frame == 1 ? std::array{0.25f, -0.125f} : std::array{-0.25f, 0.375f};
                Require(upscaler.Upscale(inputs), "Production NGX adapter rejected the evaluation");
                std::cout << "Production adapter " << test.render.width << 'x' << test.render.height
                          << " -> " << test.output.width << 'x' << test.output.height << " frame "
                          << frame
                          << (inputs.depth_inverted ? " reversed depth" : " forward depth");
                CheckOutput(allocator, scheduler, upscaler.OutputImage(), test.output, scene);
                invalid = inputs;
                invalid.color.image = upscaler.OutputImage();
                invalid.color.view = upscaler.OutputView();
                invalid.color.extent = test.output;
                Require(!upscaler.Upscale(invalid), "Output feedback alias was accepted");
            }
            scheduler.Finish();
        }
    }
    scheduler.Finish();
}
} // namespace

int main(int argc, char** argv) {
    Common::Log::Initialize();
    Common::Log::Start();
    bool validation{};
    bool unavailable{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--validation")
            validation = true;
        else if (argument == "--expect-unavailable")
            unavailable = true;
        else {
            std::cerr << "Unknown option: " << argument << '\n';
            return 2;
        }
    }
    Settings::values.nvidia_dlss.SetValue(true);
    int result{};
    try {
        if (unavailable) {
            Require(!Ngx::Start(), "Expected the missing runtime to be rejected");
            std::cout << "Missing runtime correctly rejected\n";
        } else {
            Run(validation);
            Run(validation); // Recreate all production device, scheduler and NGX
                             // state.
            Require(validation_errors == 0, "Vulkan validation reported errors");
            std::cout << "PASS: 40 production-adapter evaluations, invalid inputs, "
                         "resize, depth convention changes, DLAA and device recreation\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    Ngx::Stop();
    Common::Log::Stop();
    return result;
}
