#include "planet_sandbox/studio_ui.h"

#include <algorithm>
#include <type_traits>

namespace planet_sandbox {
namespace {

// Exact in-process identity, with no struct padding or lossy numeric formatting.
// Project edits use the session revision; snapshots/cameras are checked directly.
std::string measurement_key(const StudioState& state, const StudioRenderInput& input) {
    std::string key;
    auto add = [&](auto value) {
        static_assert(std::is_arithmetic_v<decltype(value)> || std::is_enum_v<decltype(value)>);
        key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    auto text = [&](const std::string& value) { add(value.size()); key.append(value); };
    auto point = [&](Vec2 p) { add(p.x); add(p.y); };
    add(state.session.project_revision()); add(state.session.mode());
    add(state.demos.active()); add(state.selected); add(state.design_view);
    add(state.compare_baseline); add(state.compare_modes);
    add(input.project.view_extent); add(input.project.display_scale);
    const auto& config = input.config;
    add(config.width); add(config.height);
    add(config.camera.yaw); add(config.camera.pitch); add(config.camera.zoom);
    point(config.camera.target);
    add(state.compare_modes ? state.compare_left : config.sampling);
    add(state.compare_modes ? state.compare_right : config.sampling);
    const auto& snapshot = input.snapshot;
    add(snapshot.time); add(snapshot.bodies.size());
    for (const auto& body : snapshot.bodies) {
        text(body.id); point(body.position); point(body.velocity);
        add(body.mass); add(body.radius);
    }
    auto trails = [&](const std::vector<BodyTrail>& values) {
        add(values.size());
        for (const auto& trail : values) {
            text(trail.id); add(trail.points.size());
            for (const auto& p : trail.points) point(p);
        }
    };
    trails(snapshot.trails); trails(snapshot.reference_trails);
    // Pixel selection/Region are excluded: trace extraction is outside the render timer.
    return key;
}

} // namespace

const RenderFrame& update_studio_frame(StudioState& state, const StudioRenderInput& input,
                                       double now_seconds) {
    auto& result = state.measurement;
    auto& cache = state.frame_cache;
    const auto key = measurement_key(state, input);
    const bool playing = state.demos.active() == DemoPreset::Collision
        ? state.demos.playing() : state.session.playing();
    const bool eligible = !playing && !state.camera_transition &&
        (state.compare_modes || state.inspect_pixel || state.pixel_tab_active);
    if (!eligible) {
        result = {};
    } else if (result.input_key != key) {
        result = {};
        result.input_key = key;
        result.waiting = true;
        result.stable_since = now_seconds;
    }
    if (result.waiting && now_seconds - result.stable_since >=
                           StudioRenderMeasurement::settle_seconds) {
        result.waiting = false;
        result.running = true;
    }

    const bool needs_render = !cache.frame || cache.input_key != key || result.running ||
        cache.trace_x != input.config.trace_x || cache.trace_y != input.config.trace_y;
    if (needs_render) {
        // The timed image is the displayed image: no extra benchmark render.
        // Alternating sides avoids always giving one method the same execution order.
        cache.frame = state.compare_modes
            ? render_comparison_scene(input.project, input.snapshot, input.config,
                state.compare_left, state.compare_right, result.completed_runs % 2 != 0)
            : render_scene(input.project, input.snapshot, input.config);
        cache.input_key = key;
        cache.trace_x = input.config.trace_x;
        cache.trace_y = input.config.trace_y;
        ++cache.render_count;
        if (result.running) {
            const auto& frame = *cache.frame;
            if (result.completed_runs >= StudioRenderMeasurement::warmup_runs) {
                for (int panel = 0; panel < frame.panel_count; ++panel) {
                    result.samples[panel].push_back(frame.panel_render_ms[panel]);
                }
            }
            ++result.completed_runs;
            if (result.completed_runs == StudioRenderMeasurement::warmup_runs +
                                         StudioRenderMeasurement::sample_runs) {
                for (int panel = 0; panel < frame.panel_count; ++panel) {
                    auto sorted = result.samples[panel];
                    std::sort(sorted.begin(), sorted.end());
                    result.median_ms[panel] = sorted[sorted.size() / 2];
                }
                result.running = false;
                result.ready = true;
            }
        }
    }
    return *cache.frame;
}

} // namespace planet_sandbox
