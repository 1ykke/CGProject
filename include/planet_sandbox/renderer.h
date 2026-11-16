#pragma once

#include "planet_sandbox/project.h"
#include "planet_sandbox/simulation.h"

#include <cstdint>
#include <array>
#include <optional>
#include <ostream>
#include <vector>

namespace planet_sandbox {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

enum class SamplingMode { Single, SSAA4, MSAA4, SSAA16 };
enum class TraceSurface { Background, Body, Ring, Cloud, Trail, ReferenceTrail, Atmosphere };

struct TraceSample {
    Vec2 position;
    int body_index = -1;
    double depth = 0.0;
    std::array<double, 3> linear_rgb{};
    TraceSurface surface = TraceSurface::Background;
};

struct PixelTrace {
    int x = -1;
    int y = -1;
    std::vector<TraceSample> samples;
    std::array<std::uint8_t, 3> display_rgb{};
};

struct RenderCamera {
    double yaw = 0.0;
    double pitch = 0.0;
    double zoom = 1.0;
    Vec2 target;
};

struct RenderConfig {
    int width = 640;
    int height = 480;
    RenderCamera camera;
    SamplingMode sampling = SamplingMode::Single;
    int trace_x = -1;
    int trace_y = -1;
};

struct RenderFrame {
    Image image;
    std::vector<int> body_index;
    std::vector<Vec2> surface_uv;
    std::vector<std::uint8_t> sample_surface_variation;
    std::optional<PixelTrace> trace;
    std::uint64_t shade_evaluations = 0;
    std::array<std::uint64_t, 2> panel_shade_evaluations{};
    // CPU render time, excluding pixel-trace extraction, upload, UI and comparison stitching.
    double render_ms = 0.0;
    std::array<double, 2> panel_render_ms{};
    int panel_count = 1;
    std::array<SamplingMode, 2> panel_sampling{SamplingMode::Single, SamplingMode::Single};
};

RenderFrame render_scene(const Project& project, const SceneSnapshot& snapshot,
                         const RenderConfig& config);
RenderFrame render_comparison_scene(const Project& project, const SceneSnapshot& snapshot,
                                    const RenderConfig& config, SamplingMode left,
                                    SamplingMode right, bool right_first = false);

Image render_preview(const Project& project, const SceneSnapshot& snapshot, int width, int height);
void write_ppm(std::ostream& output, const Image& image);

} // namespace planet_sandbox
