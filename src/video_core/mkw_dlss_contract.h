// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

#include "common/common_types.h"
#include "common/swap.h"

namespace VideoCommon::MarioKartWorldDlss {

using Matrix = std::array<std::array<double, 4>, 4>;
using Extent = std::array<u32, 2>;
using Pixel = std::array<double, 2>;

constexpr u32 CameraBufferSlot = 4;
constexpr size_t CameraBufferSize = 864;

struct Camera {
    Matrix view;
    Matrix view_projection;
    Matrix projection;
    Matrix inverse_view;
};

struct Scene {
    Camera current;
    Camera previous;
    Extent render_size;
    Extent nominal_size;
};

// Pixel coordinates are relative to the top-left of the active subrect; centers are at i + 0.5.
enum class NdcY { PositiveAtTop, NegativeAtTop };
enum class ClipDepth { MinusOneToOne, ZeroToOne };

struct Viewport {
    NdcY y;
    ClipDepth clip_depth;
    // Device depths at the minimum and maximum clip Z. Reversed viewport ranges are supported.
    double depth_at_min;
    double depth_at_max;
};

namespace Detail {

inline double ReadFloat(std::span<const u8> bytes, size_t offset) {
    float_le value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

inline Matrix ReadMatrix(std::span<const u8> bytes, size_t offset, size_t rows) {
    Matrix result{};
    result[3][3] = 1.0;
    for (size_t row = 0; row < rows; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            result[row][col] = ReadFloat(bytes, offset + (row * 4 + col) * sizeof(float));
        }
    }
    return result;
}

inline bool ProductMatches(const Matrix& left, const Matrix& right, const Matrix& expected) {
    for (size_t row = 0; row < 4; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            double value = 0.0;
            double scale = 0.0;
            for (size_t k = 0; k < 4; ++k) {
                const double term = left[row][k] * right[k][col];
                value += term;
                scale += std::abs(term);
            }
            const double tolerance =
                16 * std::numeric_limits<float>::epsilon() * std::max(1.0, scale);
            if (!std::isfinite(value) || !std::isfinite(expected[row][col]) ||
                std::abs(value - expected[row][col]) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

inline std::optional<Camera> ReadCamera(std::span<const u8> bytes, size_t offset) {
    Camera camera{ReadMatrix(bytes, offset, 3), ReadMatrix(bytes, offset + 0x30, 4),
                  ReadMatrix(bytes, offset + 0x70, 4), ReadMatrix(bytes, offset + 0xb0, 3)};
    const auto& p = camera.projection;
    // The captured perspective projection permits off-center optics in p[0][2] and p[1][2].
    if (!(p[0][0] > 0 && p[1][1] > 0) || p[0][1] != 0 || p[0][3] != 0 || p[1][0] != 0 ||
        p[1][3] != 0 || p[2][0] != 0 || p[2][1] != 0 || p[2][3] == 0 ||
        p[3] != std::array{0.0, 0.0, -1.0, 0.0}) {
        return std::nullopt;
    }
    // A scene camera is a rigid transform. Check the rotation separately from translation,
    // whose large world-space magnitude can dominate the product's rounding tolerance.
    for (size_t i = 0; i < 3; ++i) {
        for (size_t j = 0; j <= i; ++j) {
            double dot = 0.0;
            for (size_t k = 0; k < 3; ++k) {
                dot += camera.view[i][k] * camera.view[j][k];
            }
            if (!std::isfinite(dot) || std::abs(dot - (i == j ? 1.0 : 0.0)) > 1e-4) {
                return std::nullopt;
            }
        }
    }
    constexpr Matrix identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    if (!ProductMatches(p, camera.view, camera.view_projection) ||
        !ProductMatches(camera.view, camera.inverse_view, identity) ||
        !ProductMatches(camera.inverse_view, camera.view, identity)) {
        return std::nullopt;
    }
    return camera;
}

inline std::optional<Extent> ReadExtent(std::span<const u8> bytes, size_t offset) {
    Extent extent{};
    for (size_t i = 0; i < extent.size(); ++i) {
        const double size = ReadFloat(bytes, offset + i * sizeof(float));
        const double reciprocal = ReadFloat(bytes, offset + (2 + i) * sizeof(float));
        if (!std::isfinite(size) || size < 1 || size > std::numeric_limits<u32>::max() ||
            std::trunc(size) != size || !std::isfinite(reciprocal) ||
            std::abs(reciprocal * size - 1.0) > 2 * std::numeric_limits<float>::epsilon()) {
            return std::nullopt;
        }
        extent[i] = static_cast<u32>(size);
    }
    return extent;
}

inline std::array<double, 4> Transform(const Matrix& matrix, const std::array<double, 4>& v) {
    std::array<double, 4> result{};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            result[row] += matrix[row][col] * v[col];
        }
    }
    return result;
}

inline double YSign(NdcY y) {
    return y == NdcY::PositiveAtTop ? -1.0 : 1.0;
}

} // namespace Detail

inline std::optional<Scene> ParseCameraBuffer(std::span<const u8> bytes, Extent world_extent) {
    if (bytes.size() < CameraBufferSize) {
        return std::nullopt;
    }
    const auto render = Detail::ReadExtent(bytes, 0x120);
    const auto nominal = Detail::ReadExtent(bytes, 0x130);
    if (!render || *render != world_extent || !nominal) {
        return std::nullopt;
    }
    const auto current = Detail::ReadCamera(bytes, 0);
    const auto previous = Detail::ReadCamera(bytes, 0x150);
    if (!current || !previous) {
        return std::nullopt;
    }
    return Scene{*current, *previous, *render, *nominal};
}

// Apply the same pixel displacement to projection and VP for all participating scene and depth
// draws. Only clip X and Y change. The game's own off-center projection is part of the camera.
inline std::optional<Matrix> JitteredMatrix(Matrix matrix, Pixel jitter, Extent extent, NdcY y) {
    if (!extent[0] || !extent[1] || !std::isfinite(jitter[0]) || !std::isfinite(jitter[1])) {
        return std::nullopt;
    }
    const Pixel clip_delta{2 * jitter[0] / extent[0], Detail::YSign(y) * 2 * jitter[1] / extent[1]};
    for (size_t row = 0; row < 2; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            matrix[row][col] += clip_delta[row] * matrix[3][col];
        }
    }
    for (const auto& row : matrix) {
        if (!std::ranges::all_of(row, [](double value) { return std::isfinite(value); })) {
            return std::nullopt;
        }
    }
    return matrix;
}

struct CameraMotion {
    Pixel previous_minus_current;
    bool previous_on_screen;
};

// Motion of a fixed world point, in unjittered render pixels. An independently moving object
// needs its own velocity. current_jitter is the displacement actually applied to this depth pass.
inline std::optional<CameraMotion> Reproject(const Scene& scene, const Viewport& viewport,
                                             Pixel pixel, double depth, Pixel current_jitter) {
    const auto [width, height] = scene.render_size;
    if (!width || !height || !std::isfinite(pixel[0]) || !std::isfinite(pixel[1]) || pixel[0] < 0 ||
        pixel[0] >= width || pixel[1] < 0 || pixel[1] >= height ||
        !std::isfinite(current_jitter[0]) || !std::isfinite(current_jitter[1]) ||
        !std::isfinite(depth) || !std::isfinite(viewport.depth_at_min) ||
        !std::isfinite(viewport.depth_at_max) || viewport.depth_at_min == viewport.depth_at_max ||
        viewport.depth_at_min < 0 || viewport.depth_at_min > 1 || viewport.depth_at_max < 0 ||
        viewport.depth_at_max > 1) {
        return std::nullopt;
    }
    const double fraction =
        (depth - viewport.depth_at_min) / (viewport.depth_at_max - viewport.depth_at_min);
    if (fraction < 0 || fraction > 1) {
        return std::nullopt;
    }
    const double y_sign = Detail::YSign(viewport.y);
    const Pixel current{pixel[0] - current_jitter[0], pixel[1] - current_jitter[1]};
    const double x = 2 * current[0] / width - 1;
    const double y = y_sign * (2 * current[1] / height - 1);
    const double z = viewport.clip_depth == ClipDepth::MinusOneToOne ? 2 * fraction - 1 : fraction;
    const Matrix& p = scene.current.projection;
    const std::array view_point{(x + p[0][2]) / p[0][0], (y + p[1][2]) / p[1][1], -1.0,
                                (z + p[2][2]) / p[2][3]};
    if (!(view_point[3] > 0)) {
        return std::nullopt; // No finite point in front of the current camera.
    }
    const auto world = Detail::Transform(scene.current.inverse_view, view_point);
    const auto old = Detail::Transform(scene.previous.view_projection, world);
    if (!(old[3] > 0)) {
        return std::nullopt;
    }
    const Pixel prior{width * (1 + old[0] / old[3]) * 0.5,
                      height * (1 + y_sign * old[1] / old[3]) * 0.5};
    if (!std::isfinite(prior[0]) || !std::isfinite(prior[1])) {
        return std::nullopt;
    }
    const double prior_z = old[2] / old[3];
    const double min_z = viewport.clip_depth == ClipDepth::MinusOneToOne ? -1.0 : 0.0;
    const bool in_view = prior_z >= min_z && prior_z <= 1 && prior[0] >= 0 && prior[0] < width &&
                         prior[1] >= 0 && prior[1] < height;
    return CameraMotion{{prior[0] - current[0], prior[1] - current[1]}, in_view};
}

enum class HistoryAction { Reset, Accumulate, SkipDuplicate };

// Keep one instance per camera/viewport. The caller identifies scene cuts and calls Clear
// if an evaluation fails after observing a frame.
class History {
public:
    HistoryAction Observe(u64 frame, const Scene& scene, Extent output, bool cut) {
        const auto& vp = scene.current.view_projection;
        if (last && !cut && frame == last->frame && scene.render_size == last->render &&
            output == last->output && vp == last->vp) {
            return HistoryAction::SkipDuplicate;
        }
        const bool continuous = last && !cut && frame > last->frame && frame - last->frame == 1 &&
                                scene.render_size == last->render && output == last->output &&
                                scene.previous.view_projection == last->vp;
        last = Evaluation{frame, scene.render_size, output, vp};
        return continuous ? HistoryAction::Accumulate : HistoryAction::Reset;
    }

    void Clear() {
        last.reset();
    }

private:
    struct Evaluation {
        u64 frame;
        Extent render;
        Extent output;
        Matrix vp;
    };
    std::optional<Evaluation> last;
};

} // namespace VideoCommon::MarioKartWorldDlss
