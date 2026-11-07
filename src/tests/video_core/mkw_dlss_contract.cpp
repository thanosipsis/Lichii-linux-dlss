// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include "video_core/mkw_dlss_contract.h"

namespace {

namespace MKW = VideoCommon::MarioKartWorldDlss;
using MKW::Extent;
using MKW::Matrix;
using CameraBytes = std::array<u8, MKW::CameraBufferSize>;

constexpr Extent Render{800, 450};
constexpr Extent Output{1200, 675};
constexpr Matrix Identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
constexpr Matrix Projection{
    {{1.2, 0, 0.08, 0}, {0, 2, -0.18, 0}, {0, 0, -1.25, -2.25}, {0, 0, -1, 0}}};
constexpr MKW::Viewport TopUp{MKW::NdcY::PositiveAtTop, MKW::ClipDepth::MinusOneToOne, 0, 1};

Matrix Product(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (size_t i = 0; i < 4; ++i) {
        for (size_t j = 0; j < 4; ++j) {
            for (size_t k = 0; k < 4; ++k) {
                out[i][j] += a[i][k] * b[k][j];
            }
        }
    }
    return out;
}

MKW::Camera Pose(double x, double yaw, const Matrix& projection = Projection) {
    const double c = std::cos(yaw);
    const double s = std::sin(yaw);
    const Matrix view{{{c, 0, -s, -x * c}, {0, 1, 0, 0}, {s, 0, c, -x * s}, {0, 0, 0, 1}}};
    const Matrix inverse{{{c, 0, s, x}, {0, 1, 0, 0}, {-s, 0, c, 0}, {0, 0, 0, 1}}};
    return {view, Product(projection, view), projection, inverse};
}

void PutFloat(CameraBytes& bytes, size_t offset, double value) {
    const float_le little = static_cast<float>(value);
    std::memcpy(bytes.data() + offset, &little, sizeof(little));
}

void PutMatrix(CameraBytes& bytes, size_t offset, const Matrix& matrix, size_t rows) {
    for (size_t i = 0; i < rows; ++i) {
        for (size_t j = 0; j < 4; ++j) {
            PutFloat(bytes, offset + (4 * i + j) * sizeof(float), matrix[i][j]);
        }
    }
}

CameraBytes Buffer(const MKW::Camera& current, const MKW::Camera& previous,
                   Extent render = Render) {
    CameraBytes bytes{};
    const auto put_camera = [&](size_t base, const MKW::Camera& camera) {
        PutMatrix(bytes, base, camera.view, 3);
        PutMatrix(bytes, base + 0x30, camera.view_projection, 4);
        PutMatrix(bytes, base + 0x70, camera.projection, 4);
        PutMatrix(bytes, base + 0xb0, camera.inverse_view, 3);
    };
    put_camera(0, current);
    put_camera(0x150, previous);
    for (size_t i = 0; i < 2; ++i) {
        PutFloat(bytes, 0x120 + 4 * i, render[i]);
        PutFloat(bytes, 0x128 + 4 * i, 1.0 / render[i]);
        PutFloat(bytes, 0x130 + 4 * i, Output[i]);
        PutFloat(bytes, 0x138 + 4 * i, 1.0 / Output[i]);
    }
    return bytes;
}

struct Sample {
    MKW::Pixel pixel;
    double depth;
};

// Forward projection of a known world point, independent of the reprojection implementation.
Sample Project(const Matrix& vp, std::array<double, 4> point, const MKW::Viewport& viewport) {
    std::array<double, 4> clip{};
    for (size_t i = 0; i < 4; ++i) {
        for (size_t j = 0; j < 4; ++j) {
            clip[i] += vp[i][j] * point[j];
        }
    }
    const double y = clip[1] / clip[3];
    const double z = clip[2] / clip[3];
    const double fraction = viewport.clip_depth == MKW::ClipDepth::MinusOneToOne ? (1 + z) / 2 : z;
    return {{Render[0] * (1 + clip[0] / clip[3]) / 2,
             Render[1] * (1 + (viewport.y == MKW::NdcY::PositiveAtTop ? -y : y)) / 2},
            viewport.depth_at_min + fraction * (viewport.depth_at_max - viewport.depth_at_min)};
}

TEST_CASE("MKW recognizes the captured camera layout and rejects invalid blocks",
          "[video_core][mkw_dlss_contract]") {
    const auto bytes = Buffer(Pose(4430, 0.049), Pose(4429.5, 0.047));
    const auto scene = MKW::ParseCameraBuffer(bytes, Render);
    REQUIRE(scene);
    REQUIRE(scene->render_size == Render);
    REQUIRE(scene->nominal_size == Output);
    REQUIRE_FALSE(MKW::ParseCameraBuffer(std::span{bytes}.first(bytes.size() - 1), Render));
    REQUIRE_FALSE(MKW::ParseCameraBuffer(bytes, {Render[0] + 1, Render[1]}));
    for (const auto offset :
         {0x00, 0x30, 0x70, 0xb0, 0x120, 0x128, 0x130, 0x138, 0x150, 0x180, 0x1c0, 0x200}) {
        auto invalid = bytes;
        PutFloat(invalid, offset, std::numeric_limits<double>::quiet_NaN());
        REQUIRE_FALSE(MKW::ParseCameraBuffer(invalid, Render));
    }
    auto invalid = bytes;
    PutFloat(invalid, 0x30 + 3 * sizeof(float), 0);
    REQUIRE_FALSE(MKW::ParseCameraBuffer(invalid, Render));
    invalid = bytes;
    PutFloat(invalid, 0x200, 0);
    REQUIRE_FALSE(MKW::ParseCameraBuffer(invalid, Render));
    invalid = bytes;
    PutFloat(invalid, 0x130, 1200.125);
    REQUIRE_FALSE(MKW::ParseCameraBuffer(invalid, Render));
    invalid = bytes;
    PutFloat(invalid, 0x128, 0.5);
    REQUIRE_FALSE(MKW::ParseCameraBuffer(invalid, Render));
    Matrix singular = Projection;
    singular[2][3] = 0;
    REQUIRE_FALSE(MKW::ParseCameraBuffer(Buffer(Pose(0, 0, singular), Pose(0, 0)), Render));
    REQUIRE_FALSE(MKW::ParseCameraBuffer(Buffer(Pose(0, 0, Identity), Pose(0, 0)), Render));
}

TEST_CASE("MKW camera reprojection follows known points under explicit viewport conventions",
          "[video_core][mkw_dlss_contract]") {
    const auto current = Pose(0.1, 0.11);
    for (const auto& previous : {current, Pose(0, 0.11), Pose(0.1, -0.03)}) {
        const auto scene = MKW::ParseCameraBuffer(Buffer(current, previous), Render);
        REQUIRE(scene);
        for (const auto y : {MKW::NdcY::PositiveAtTop, MKW::NdcY::NegativeAtTop}) {
            for (const auto clip : {MKW::ClipDepth::ZeroToOne, MKW::ClipDepth::MinusOneToOne}) {
                for (const auto depths : {std::array{0.1, 0.95}, std::array{0.95, 0.1}}) {
                    const MKW::Viewport v{y, clip, depths[0], depths[1]};
                    for (const auto point :
                         {std::array{0.4, 0.2, -4.0, 1.0}, std::array{-0.3, -0.6, -5.0, 1.0}}) {
                        const auto now = Project(current.view_projection, point, v);
                        const auto then = Project(previous.view_projection, point, v);
                        const MKW::Pixel jitter{-0.25, 0.375};
                        const MKW::Pixel sample{now.pixel[0] + jitter[0], now.pixel[1] + jitter[1]};
                        const auto motion = MKW::Reproject(*scene, v, sample, now.depth, jitter);
                        REQUIRE(motion);
                        REQUIRE(motion->previous_on_screen);
                        for (size_t i = 0; i < 2; ++i) {
                            REQUIRE(std::abs(motion->previous_minus_current[i] -
                                             (then.pixel[i] - now.pixel[i])) < 0.001);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("MKW distinguishes offscreen, behind-camera, and invalid depth reprojection",
          "[video_core][mkw_dlss_contract]") {
    const auto current = Pose(0, 0);
    const auto sample = Project(current.view_projection, {0, 0, -3, 1}, TopUp);
    auto scene = MKW::ParseCameraBuffer(Buffer(current, Pose(12, 0)), Render);
    REQUIRE(scene);
    const auto offscreen = MKW::Reproject(*scene, TopUp, sample.pixel, sample.depth, {});
    REQUIRE(offscreen);
    REQUIRE_FALSE(offscreen->previous_on_screen);
    REQUIRE(offscreen->previous_minus_current[0] < -static_cast<double>(Render[0]));
    scene = MKW::ParseCameraBuffer(Buffer(current, Pose(0, 3.14)), Render);
    REQUIRE(scene);
    REQUIRE_FALSE(MKW::Reproject(*scene, TopUp, sample.pixel, sample.depth, {}));
    REQUIRE_FALSE(MKW::Reproject(*scene, TopUp, sample.pixel, 1.1, {}));
    REQUIRE_FALSE(MKW::Reproject(*scene, TopUp, {-1, 20}, sample.depth, {}));
    REQUIRE_FALSE(
        MKW::Reproject(*scene, TopUp, sample.pixel, std::numeric_limits<double>::infinity(), {}));
    const MKW::Viewport zero_depth_range{TopUp.y, TopUp.clip_depth, 0.5, 0.5};
    REQUIRE_FALSE(MKW::Reproject(*scene, zero_depth_range, sample.pixel, sample.depth, {}));

    auto infinite = Projection;
    infinite[2][2] = -1;
    scene = MKW::ParseCameraBuffer(Buffer(Pose(0, 0, infinite), Pose(0, 0, infinite)), Render);
    REQUIRE(scene);
    REQUIRE_FALSE(MKW::Reproject(*scene, TopUp, sample.pixel, 1, {}));
}

TEST_CASE("MKW shifts scene color and depth by the same subpixel displacement",
          "[video_core][mkw_dlss_contract]") {
    const auto pose = Pose(1, 0.2);
    const MKW::Pixel jitter{0.375, -0.25};
    const std::array point{1.0, 0.5, -4.0, 1.0};
    for (const auto y : {MKW::NdcY::PositiveAtTop, MKW::NdcY::NegativeAtTop}) {
        const MKW::Viewport v{y, TopUp.clip_depth, 0, 1};
        const auto base = Project(pose.view_projection, point, v);
        const auto jittered_p = MKW::JitteredMatrix(pose.projection, jitter, Render, y);
        const auto jittered_vp = MKW::JitteredMatrix(pose.view_projection, jitter, Render, y);
        REQUIRE(jittered_p);
        REQUIRE(jittered_vp);
        for (const Matrix& transformed : {*jittered_vp, Product(*jittered_p, pose.view)}) {
            const auto shifted = Project(transformed, point, v);
            REQUIRE(std::abs(shifted.pixel[0] - base.pixel[0] - jitter[0]) < 1e-9);
            REQUIRE(std::abs(shifted.pixel[1] - base.pixel[1] - jitter[1]) < 1e-9);
            REQUIRE(shifted.depth == base.depth);
        }
        REQUIRE(*MKW::JitteredMatrix(pose.projection, {}, Render, y) == Projection);
    }
    REQUIRE_FALSE(MKW::JitteredMatrix(Projection, jitter, {}, TopUp.y));
    REQUIRE_FALSE(MKW::JitteredMatrix(Projection, {std::nan(""), 0}, Render, TopUp.y));
}

TEST_CASE("MKW temporal history advances only for consecutive evaluations of one camera",
          "[video_core][mkw_dlss_contract]") {
    using Action = MKW::HistoryAction;
    MKW::History history;
    const auto a = Pose(0, 0);
    const auto b = Pose(1, 0.01);
    const auto c = Pose(2, 0.02);
    const MKW::Scene first{a, a, Render, Output};
    const MKW::Scene second{b, a, Render, Output};
    const MKW::Scene third{c, b, Render, Output};
    REQUIRE(history.Observe(1, first, Output, false) == Action::Reset);
    REQUIRE(history.Observe(2, second, Output, false) == Action::Accumulate);
    REQUIRE(history.Observe(2, second, Output, false) == Action::SkipDuplicate);
    REQUIRE(history.Observe(2, second, Output, true) == Action::Reset);
    REQUIRE(history.Observe(3, third, Output, false) == Action::Accumulate);
    REQUIRE(history.Observe(3, third, Output, true) == Action::Reset);
    REQUIRE(history.Observe(4, first, Output, false) == Action::Reset);  // previous mismatch
    REQUIRE(history.Observe(6, second, Output, false) == Action::Reset); // gap
    REQUIRE(history.Observe(7, third, {1920, 1080}, false) == Action::Reset);
    auto resized = third;
    resized.render_size = {640, 360};
    resized.previous = third.current;
    REQUIRE(history.Observe(8, resized, {1920, 1080}, false) == Action::Reset);
    REQUIRE(history.Observe(7, resized, {1920, 1080}, false) == Action::Reset); // rewound frame
    history.Clear();
    REQUIRE(history.Observe(8, resized, {1920, 1080}, false) == Action::Reset);
}

} // namespace
