// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <vulkan/vulkan.h>

// NGX declarations require Vulkan types to be visible first.
#include <nvsdk_ngx_helpers_vk.h>
#include "common/logging.h"
#include "video_core/dlss_contract.h"
#include "video_core/dlss_evaluation.h"
#include "video_core/renderer_vulkan/vk_ngx_dlss.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/ngx.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
NVSDK_NGX_PerfQuality_Value ModeFor(VkExtent2D render, VkExtent2D output) {
    const float ratio = static_cast<float>(render.width) / output.width;
    if (ratio >= 0.99f)
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (ratio >= 0.65f)
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ratio >= 0.57f)
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (ratio >= 0.49f)
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}
bool ValidInputs(const NgxDlssUpscaler::Inputs& in, VkImage output, u32 max_dimension) {
    if (!VideoCommon::DlssContract::ValidGeometry({in.render_size.width, in.render_size.height},
                                                  {in.upscale_size.width, in.upscale_size.height},
                                                  in.jitter, in.motion_vector_scale) ||
        std::abs(in.jitter[0]) > 0.5f || std::abs(in.jitter[1]) > 0.5f ||
        in.motion_vector_scale[0] == 0.0f || in.motion_vector_scale[1] == 0.0f ||
        in.upscale_size.width > max_dimension || in.upscale_size.height > max_dimension) {
        return false;
    }
    for (const auto* image : {&in.color, &in.depth, &in.motion_vectors}) {
        if (!image->image || !image->view || image->image == output ||
            image->range.levelCount != 1 || image->range.layerCount != 1 ||
            image->extent.width > max_dimension || image->extent.height > max_dimension ||
            image->extent.width < in.render_size.width ||
            image->extent.height < in.render_size.height) {
            return false;
        }
    }
    if (in.color.image == in.depth.image || in.color.image == in.motion_vectors.image ||
        in.depth.image == in.motion_vectors.image)
        return false;
    const bool color = in.color.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
                       in.color.format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool depth =
        in.depth.format == VK_FORMAT_D32_SFLOAT || in.depth.format == VK_FORMAT_D24_UNORM_S8_UINT ||
        in.depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT || in.depth.format == VK_FORMAT_D16_UNORM;
    return color && depth && in.motion_vectors.format == VK_FORMAT_R16G16_SFLOAT &&
           in.color.range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
           in.motion_vectors.range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
           in.depth.range.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT &&
           in.color.barrier_aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
           in.motion_vectors.barrier_aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
           in.depth.barrier_aspect ==
               (VK_IMAGE_ASPECT_DEPTH_BIT | ((in.depth.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                                              in.depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT)
                                                 ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                 : 0));
}

NVSDK_NGX_DLSS_Hint_Render_Preset Preset() {
    const char* value = std::getenv("EDEN_NX2_DLSS_PRESET");
    if (value && std::string_view{value} == "default")
        return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    if (value && value[0] >= 'J' && value[0] <= 'M' && value[1] == '\0') {
        return static_cast<NVSDK_NGX_DLSS_Hint_Render_Preset>(10 + value[0] - 'J');
    }
    return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
}

VkImageMemoryBarrier Barrier(const NgxDlssUpscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = range,
    };
}

} // namespace

struct NgxDlssUpscaler::State {
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    u32 max_image_dimension{};
};

NgxDlssUpscaler::NgxDlssUpscaler(const Device& device_, Scheduler& scheduler_,
                                 MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      state{std::make_unique<State>()} {
    if (!Ngx::IsDlssAvailable())
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    state->max_image_dimension = device.GetPhysical().GetProperties().limits.maxImageDimension2D;
    point_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    });
    std::scoped_lock lock{Ngx::ApiMutex()};
    const auto result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&state->parameters);
    if (NVSDK_NGX_FAILED(result) || !state->parameters) {
        if (state->parameters)
            NVSDK_NGX_VULKAN_DestroyParameters(state->parameters);
        state->parameters = nullptr;
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
}

NgxDlssUpscaler::~NgxDlssUpscaler() {
    scheduler.WaitWorker();
    void(device.GetLogical().WaitIdle());
    std::scoped_lock lock{Ngx::ApiMutex()};
    if (state->feature)
        NVSDK_NGX_VULKAN_ReleaseFeature(state->feature);
    if (state->parameters)
        NVSDK_NGX_VULKAN_DestroyParameters(state->parameters);
}

void NgxDlssUpscaler::CreateOutput(VkExtent2D upscale_size) {
    if (output_image) {
        scheduler.Finish(); // the old output may still be in use
    }
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .components{},
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            });
    });
}

bool NgxDlssUpscaler::Upscale(const Inputs& in) {
    if (failed.load() ||
        !ValidInputs(in, output_image ? *output_image : VkImage{}, state->max_image_dimension))
        return false;
    const bool changed = !output_image || in.render_size.width != render_extent.width ||
                         in.render_size.height != render_extent.height ||
                         in.upscale_size.width != upscale_extent.width ||
                         in.upscale_size.height != upscale_extent.height ||
                         in.depth_inverted != depth_inverted;
    if (changed) {
        // Finish retires evaluations that reference the old feature and output before replacing
        // them.
        CreateOutput(in.upscale_size);
        render_extent = in.render_size;
        upscale_extent = in.upscale_size;
        depth_inverted = in.depth_inverted;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, changed, output = *output_image,
                      view = *output_view](vk::CommandBuffer cmdbuf) {
        std::scoped_lock lock{Ngx::ApiMutex()};
        constexpr VkAccessFlags WRITES = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        const std::array read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
        const std::array restore{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | WRITES, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL)};
        auto resource = [](const Image& image) {
            return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, image.range,
                                                          image.format, image.extent.width,
                                                          image.extent.height, false);
        };
        auto color = resource(in.color);
        auto depth = resource(in.depth);
        auto motion = resource(in.motion_vectors);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        auto destination = NVSDK_NGX_Create_ImageView_Resource_VK(
            view, output, range, OUTPUT_FORMAT, in.upscale_size.width, in.upscale_size.height,
            true);
        NVSDK_NGX_VK_DLSS_Eval_Params evaluation{};
        evaluation.Feature.pInColor = &color;
        evaluation.Feature.pInOutput = &destination;
        evaluation.pInDepth = &depth;
        evaluation.pInMotionVectors = &motion;
        evaluation.InJitterOffsetX = in.jitter[0];
        evaluation.InJitterOffsetY = in.jitter[1];
        evaluation.InRenderSubrectDimensions = {in.render_size.width, in.render_size.height};
        evaluation.InMVScaleX = in.motion_vector_scale[0];
        evaluation.InMVScaleY = in.motion_vector_scale[1];
        evaluation.InReset = in.reset || changed;
        evaluation.InPreExposure = 1.0f;
        evaluation.InExposureScale = 1.0f;
        NVSDK_NGX_Result api_result = NVSDK_NGX_Result_Success;
        const auto result = VideoCommon::RecordDlssEvaluation(
            [&] {
                if (!changed)
                    return state->feature != nullptr && !failed.load();
                if (state->feature) {
                    NVSDK_NGX_VULKAN_ReleaseFeature(state->feature);
                    state->feature = nullptr;
                }
                for (const char* parameter :
                     {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                      NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                      NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                      NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                      NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                      NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
                    state->parameters->Set(parameter, static_cast<unsigned>(Preset()));
                }
                NVSDK_NGX_DLSS_Create_Params creation{};
                creation.Feature.InWidth = in.render_size.width;
                creation.Feature.InHeight = in.render_size.height;
                creation.Feature.InTargetWidth = in.upscale_size.width;
                creation.Feature.InTargetHeight = in.upscale_size.height;
                creation.Feature.InPerfQualityValue = ModeFor(in.render_size, in.upscale_size);
                creation.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                                NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                                NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                if (in.depth_inverted) {
                    creation.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
                }
                api_result =
                    NGX_VULKAN_CREATE_DLSS_EXT1(*device.GetLogical(), *cmdbuf, 1, 1,
                                                &state->feature, state->parameters, &creation);
                return NVSDK_NGX_SUCCEED(api_result);
            },
            [] { return true; },
            [&] {
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, read);
                const Image output_resource{view, output, OUTPUT_FORMAT, range,
                                            VK_IMAGE_ASPECT_COLOR_BIT};
                // NGX clears the output through the transfer stage as well as dispatching compute.
                cmdbuf.PipelineBarrier(
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                    Barrier(output_resource,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL));
            },
            [] { return true; },
            [&] {
                api_result = NGX_VULKAN_EVALUATE_DLSS_EXT(*cmdbuf, state->feature,
                                                          state->parameters, &evaluation);
                return NVSDK_NGX_SUCCEED(api_result);
            },
            [&] {
                const VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                              VK_ACCESS_SHADER_WRITE_BIT |
                                                  VK_ACCESS_TRANSFER_WRITE_BIT,
                                              VK_ACCESS_SHADER_READ_BIT};
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, written, {}, restore);
            });
        if (result != VideoCommon::DlssEvaluationResult::Succeeded) {
            if (!failed.exchange(true)) {
                LOG_ERROR(Render_Vulkan,
                          "Linux NGX DLSS failed at stage {} ({:#x}); falling back to FSR",
                          static_cast<int>(result), static_cast<u32>(api_result));
            }
            const Image output_resource{view, output, OUTPUT_FORMAT, range,
                                        VK_IMAGE_ASPECT_COLOR_BIT};
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                Barrier(output_resource, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_GENERAL));
            cmdbuf.ClearColorImage(output, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{}, range);
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                Barrier(output_resource, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL));
        } else if (frame_index++ == 0) {
            LOG_INFO(Render_Vulkan, "Linux NGX DLSS evaluated successfully: {}x{} -> {}x{}",
                     in.render_size.width, in.render_size.height, in.upscale_size.width,
                     in.upscale_size.height);
        }
    });
    scheduler.InvalidateState();
    // Obtain the recording result before the caller replaces the guest output. A failed
    // evaluation then falls back this frame instead of displaying an unwritten image.
    scheduler.WaitWorker();
    return !failed.load();
}
} // namespace Vulkan
