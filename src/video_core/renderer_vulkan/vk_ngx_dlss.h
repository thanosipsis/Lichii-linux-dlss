// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Native Linux NVIDIA DLSS through NGX. The runtime is supplied by the user.

#pragma once

#include <array>
#include <atomic>
#include <memory>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

class NgxDlssUpscaler {
public:
    struct Image {
        VkImageView view;
        VkImage image;
        VkFormat format;
        VkImageSubresourceRange range;     ///< The view's level and layer, with the view's aspect.
        VkImageAspectFlags barrier_aspect; ///< Every aspect of the image's format.
        VkExtent2D extent; ///< Actual bound image extent, independent of the active subrect.
    };

    struct Inputs {
        Image color;          ///< Jittered linear colour at the render size.
        Image depth;          ///< Device depth at the render size.
        Image motion_vectors; ///< Motion to the previous frame at the render size.
        VkExtent2D render_size;
        VkExtent2D upscale_size;
        /// This frame's sample offset in render pixels, as the game hands its own DLSS.
        std::array<f32, 2> jitter;
        /// Turns a motion vector into render pixels (the render size for UV offsets).
        std::array<f32, 2> motion_vector_scale;
        bool reset;
        bool depth_inverted = true; ///< True when near depth is greater than far depth.
    };

    /// Throws vk::Exception when NGX has no DLSS for this device.
    explicit NgxDlssUpscaler(const Device& device, Scheduler& scheduler,
                             MemoryAllocator& memory_allocator);
    ~NgxDlssUpscaler();

    /// Records one evaluation into the output image (RGBA16F at the upscale size, GENERAL
    /// layout). Returns false when DLSS can't run, so the caller can fall back to FSR 3.1.
    bool Upscale(const Inputs& inputs);

    [[nodiscard]] VkImage OutputImage() const noexcept {
        return *output_image;
    }
    [[nodiscard]] VkImageView OutputView() const noexcept {
        return *output_view;
    }
    /// Nearest-neighbour clamp sampler (normalized coordinates), for reading the output.
    [[nodiscard]] VkSampler PointSampler() const noexcept {
        return *point_sampler;
    }

private:
    void CreateOutput(VkExtent2D upscale_size);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    struct State;
    std::unique_ptr<State> state;
    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    bool depth_inverted = true;
    u32 frame_index{};
    vk::Image output_image;
    vk::ImageView output_view;
    vk::Sampler point_sampler;
    std::atomic<bool> failed{};
};

} // namespace Vulkan
