#pragma once

#include "planet_sandbox/renderer.h"
#include "planet_sandbox/demo_presets.h"
#include "planet_sandbox/sculpt.h"
#include "planet_sandbox/session.h"

#include <imgui.h>

#include <array>
#include <optional>
#include <string>

namespace planet_sandbox {

enum class StudioCameraDestination { Design, Overview, Simulation, DesignOverview, Camera };

struct StudioCameraTransition {
    RenderCamera from;
    RenderCamera to;
    RenderCamera detail_camera;
    std::optional<Vec2> screen_anchor;
    std::optional<RenderCamera> via;
    std::optional<Vec2> departure_anchor;
    double elapsed = 0.0;
    double duration = 0.9;
    StudioCameraDestination destination = StudioCameraDestination::Camera;
    bool resume_playback = false;
};

struct StudioRenderMeasurement {
    static constexpr int warmup_runs = 3;
    static constexpr int sample_runs = 11;
    static constexpr double settle_seconds = 0.25;
    bool waiting = false;
    bool running = false;
    bool ready = false;
    int completed_runs = 0;
    double stable_since = 0.0;
    std::string input_key;
    std::array<std::vector<double>, 2> samples;
    std::array<double, 2> median_ms{};
};

struct StudioFrameCache {
    std::optional<RenderFrame> frame;
    std::string input_key;
    int trace_x = -1;
    int trace_y = -1;
    // Also used by frontends to upload only newly rendered images.
    std::uint64_t render_count = 0;
};

struct StudioState {
    StudioState(Project project, const std::string& project_path);

    SandboxSession session;
    DemoPresets demos;
    RenderConfig render;
    std::array<char, 512> path{};
    std::array<char, 96> name{};
    std::string notice;
    int selected = 0;
    bool design_view = false;
    bool compare_baseline = false;
    bool show_reference_orbit = true;
    double timeline_horizon = 80.0;
    bool compare_modes = false;
    bool inspect_pixel = false;
    SamplingMode compare_left = SamplingMode::Single;
    SamplingMode compare_right = SamplingMode::MSAA4;
    int selected_pixel_x = -1;
    int selected_pixel_y = -1;
    int pixel_region_size = 9;
    bool select_pixel_tab = false;
    bool pixel_tab_active = false;
    StudioRenderMeasurement measurement;
    StudioFrameCache frame_cache;
    bool request_close = false;
    std::optional<StudioCameraTransition> camera_transition;
    SculptMode sculpt_mode = SculptMode::Raise;
    SolarShapeLayer sculpt_layer = SolarShapeLayer::Visible;
    float sculpt_radius = 0.3f;
    float sculpt_strength = 0.6f;
    SculptStroke sculpt_stroke;
    SculptHistory sculpt_history;
    struct SavedView {
        int selected = 0;
        RenderCamera camera;
        bool design_view = false;
        bool compare_baseline = false;
        bool compare_modes = false;
        bool inspect_pixel = false;
        int pixel_x = -1;
        int pixel_y = -1;
    };
    std::optional<SavedView> saved_view;
};

struct StudioRenderInput {
    Project project;
    SceneSnapshot snapshot;
    RenderConfig config;
};

// A crop in panel-local coordinates; it never crosses a comparison divider.
struct StudioPixelRegion {
    int x = 0;
    int y = 0;
    int size = 0;
};
StudioPixelRegion studio_pixel_region(const RenderFrame& frame, int pixel_x, int pixel_y,
                                      int cells);

StudioRenderInput make_studio_render_input(const StudioState& state);
RenderFrame render_studio_frame(const StudioState& state, const StudioRenderInput& input);
// Reuses the displayed frame for timing, then caches it once the measurement is complete.
const RenderFrame& update_studio_frame(StudioState& state, const StudioRenderInput& input,
                                       double now_seconds);
void activate_studio_preset(StudioState& state, DemoPreset preset);
void exit_studio_preset(StudioState& state);
void advance_studio(StudioState& state, double real_seconds);
void select_studio_body(StudioState& state, int body_index);
void select_studio_scene_body(StudioState& state, int body_index);
void begin_studio_design_focus(StudioState& state, int body_index);
void return_studio_overview(StudioState& state);
void begin_studio_simulation(StudioState& state);
void return_studio_design(StudioState& state);
void focus_studio_selected(StudioState& state);
void reset_studio_view(StudioState& state);
double studio_timeline_limit(const StudioState& state);
void seek_studio_time(StudioState& state, double time);

void draw_studio(StudioState& state, ImTextureID texture, const StudioRenderInput& input,
                 const RenderFrame& frame,
                 bool flip_image_y);

} // namespace planet_sandbox
