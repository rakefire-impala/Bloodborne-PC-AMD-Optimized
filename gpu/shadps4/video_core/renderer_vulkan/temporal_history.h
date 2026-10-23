// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace Vulkan::Motion {

struct CameraSample {
    std::array<float, 12> view{};     // world to view, 3x4 rows
    std::array<float, 12> inv_view{}; // view to world, 3x4 rows
    std::array<float, 4> proj{};      // x/y scale, depth scale/offset
    bool valid = false;
};

struct SceneCamera {
    CameraSample camera;
    std::array<uint32_t,2> size;
};
// Decode the exact scene-constant snapshot bound by this draw, never later guest data.
inline std::optional<SceneCamera> DecodeSceneCamera(std::span<const float> data) {
    if (data.size() < 216 || data[0] != 3000.0f || !std::isfinite(data[1]) ||
        !(data[4] >= 64.0f && data[4] <= 32768.0f && data[5] >= 64.0f && data[5] <= 32768.0f) ||
        std::abs(data[1]*data[0]-1.0f) > 1e-3f) return std::nullopt;
    SceneCamera result;
    std::copy_n(data.begin()+8,12,result.camera.view.begin());
    std::copy_n(data.begin()+180,12,result.camera.inv_view.begin());
    result.camera.proj = {data[52],data[57],data[62],data[63]};
    result.size = {uint32_t(data[4]),uint32_t(data[5])};
    return result;
}

inline bool ValidCamera(const CameraSample& sample) {
    const auto finite = [](const auto& values) {
        return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
    };
    if (!finite(sample.view) || !finite(sample.inv_view) || !finite(sample.proj) ||
        sample.proj[0] == 0 || sample.proj[1] == 0 || sample.proj[2] == 0) {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        float length = 0;
        for (int row = 0; row < 3; ++row) {
            const float v = sample.inv_view[row * 4 + axis];
            length += v * v;
        }
        if (length < 0.25f || length > 4.0f) return false;
    }
    return true;
}

// Conservative discontinuity detection: ordinary motion, jitter and gradual FOV changes
// keep history. Teleports (10 world units), turns over 100 degrees in one frame and abrupt
// projection changes discard it. Resolution/provider changes use Invalidate separately.
inline bool CameraDiscontinuity(const CameraSample& previous, const CameraSample& current) {
    if (!previous.valid) return true;
    float distance_squared = 0, trace = 0;
    for (int row = 0; row < 3; ++row) {
        const float delta = current.inv_view[row * 4 + 3] - previous.inv_view[row * 4 + 3];
        distance_squared += delta * delta;
    }
    if (distance_squared >= 100.0f) return true;
    for (int axis = 0; axis < 3; ++axis) {
        float dot = 0, old_length = 0, new_length = 0;
        for (int row = 0; row < 3; ++row) {
            const float a = previous.inv_view[row * 4 + axis];
            const float b = current.inv_view[row * 4 + axis];
            dot += a * b;
            old_length += a * a;
            new_length += b * b;
        }
        trace += dot / std::sqrt(old_length * new_length);
    }
    if ((trace - 1.0f) * 0.5f < -0.17364818f) return true; // cos(100 degrees)
    for (int axis = 0; axis < 4; ++axis) {
        const float a = previous.proj[axis], b = current.proj[axis];
        if (std::abs(a - b) > 0.2f * std::max({std::abs(a), std::abs(b), 1.0e-4f})) return true;
    }
    return false;
}

// A planar reflection pass (water, wet ground) renders the scene from the main camera mirrored
// about a horizontal plane: same x/z position and heading, mirrored height and pitch.
inline bool MirroredVertically(const CameraSample& a, const CameraSample& b) {
    const auto& p = a.inv_view;
    const auto& q = b.inv_view;
    return std::abs(p[3] - q[3]) < 0.05f && std::abs(p[11] - q[11]) < 0.05f &&
           std::abs(p[7] - q[7]) > 0.01f && std::abs(p[2] - q[2]) < 0.02f &&
           std::abs(p[10] - q[10]) < 0.02f && std::abs(p[6] + q[6]) < 0.02f;
}

// The reflection camera of the next frame: the previous main camera mirrored about the same
// plane (the camera moves a little between frames, the plane stays). Its height differs by more
// than a unit, which the main camera's own motion between two frames does not.
inline bool MirrorOfPrevious(const CameraSample& previous, const CameraSample& sample,
                             float plane) {
    const auto& p = previous.inv_view;
    const auto& q = sample.inv_view;
    return std::abs(q[3] - p[3]) < 1.0f && std::abs(q[11] - p[11]) < 1.0f &&
           std::abs(q[7] - (2.0f * plane - p[7])) < 1.0f && std::abs(q[7] - p[7]) > 1.0f &&
           std::abs(q[2] - p[2]) < 0.2f && std::abs(q[10] - p[10]) < 0.2f &&
           std::abs(q[6] + p[6]) < 0.2f;
}

class CameraHistory {
public:
    CameraSample current, previous;

    bool Capture(CameraSample sample, bool resized = false) {
        if (captured) return false;
        if (!ValidCamera(sample)) {
            Invalidate();
            return false;
        }
        sample.valid = true;
        before = current;
        reset_before = reset;
        previous = current;
        current = sample;
        reset = reset || resized || CameraDiscontinuity(previous, current);
        if (reset) previous = current; // zero velocity; do not reuse the preceding scene
        captured = true;
        return true;
    }
    /// A later camera of this frame replaces the captured one when it continues the previous
    /// frame's camera better (the main camera after a reflection pass drawn first): history and
    /// reset are decided again against it.
    bool Replace(CameraSample sample, bool resized = false) {
        if (!captured || !ValidCamera(sample)) return false;
        const auto distance = [](const CameraSample& a, const CameraSample& b) {
            float sum = 0;
            for (int row = 0; row < 3; ++row) {
                const float d = a.inv_view[row * 4 + 3] - b.inv_view[row * 4 + 3];
                sum += d * d;
            }
            return sum;
        };
        if (before.valid && distance(sample, before) >= distance(current, before)) return false;
        sample.valid = true;
        previous = before;
        current = sample;
        reset = reset_before || resized || CameraDiscontinuity(previous, current);
        if (reset) previous = current;
        return true;
    }
    void Invalidate() noexcept {
        current.valid = previous.valid = false;
        reset = true;
    }
    void EndFrame() noexcept {
        if (!captured) Invalidate();
        captured = false;
        reset = false; // a missing/invalid camera will request reset on its next capture
    }
    bool NeedsCamera() const noexcept { return !captured; }
    bool Reset() const noexcept { return reset; }

private:
    CameraSample before; ///< current before this frame's capture (for Replace)
    bool captured = false;
    bool reset = true;
    bool reset_before = true;
};

// Scene samplers ask for this bias on many draws; only changed dimensions need log2.
class MipBiasCache {
public:
    float Get(uint32_t render, uint32_t output) {
        if (render != render_width || output != output_width) {
            render_width = render;
            output_width = output;
            value = render && render < output ? std::log2(float(render) / float(output)) : 0.0f;
        }
        return value;
    }
private:
    uint32_t render_width = 0, output_width = 0;
    float value = 0;
};

} // namespace Vulkan::Motion
