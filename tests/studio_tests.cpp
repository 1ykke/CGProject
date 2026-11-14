#include "planet_sandbox/studio_ui.h"
#ifdef NDEBUG
#error Studio tests require enabled assertions
#endif
#include "planet_sandbox/solar_system.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

using namespace planet_sandbox;

static void test_design_camera_flight() {
    StudioState state(make_solar_system_project(), "camera-flight.json");
    state.render.width = 128;
    state.render.height = 96;
    state.render.sampling = SamplingMode::SSAA4;
    const double initial_zoom = state.render.camera.zoom;
    begin_studio_design_focus(state, 3);
    assert(state.selected == 3 && !state.design_view);
    assert(state.camera_transition.has_value());
    assert(state.render.camera.zoom == initial_zoom);
    const StudioRenderInput departure = make_studio_render_input(state);
    assert(departure.snapshot.bodies.size() == 9);
    assert(departure.config.sampling == SamplingMode::Single);
    const RenderCamera world_close = state.camera_transition->to;
    assert(std::abs(world_close.target.x - state.session.project().bodies[3].position.x) < 1e-12);
    assert(world_close.zoom < initial_zoom);
    const Vec2 earth_position = state.session.project().bodies[3].position;
    const auto screen_offset = [&](const RenderCamera& camera) {
        return std::hypot(earth_position.x - camera.target.x,
                          earth_position.y - camera.target.y) / camera.zoom;
    };
    const double initial_offset = screen_offset(state.render.camera);
    advance_studio(state, 0.1);
    assert(screen_offset(state.render.camera) < initial_offset);

    StudioRenderInput matched_world = departure;
    const BodySpec& earth = state.session.project().bodies[3];
    matched_world.snapshot.bodies = {{earth.id, earth.position, earth.velocity, earth.mass, earth.radius}};
    matched_world.snapshot.trails.clear();
    matched_world.config.camera = world_close;
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(!state.camera_transition && state.design_view);
    assert(state.render.sampling == SamplingMode::SSAA4);
    StudioRenderInput detail = make_studio_render_input(state);
    assert(detail.snapshot.bodies.size() == 1 && detail.snapshot.bodies[0].id == "earth");
    detail.config.sampling = SamplingMode::Single;
    const RenderFrame world_frame = render_scene(matched_world.project, matched_world.snapshot,
                                                  matched_world.config);
    const RenderFrame detail_frame = render_scene(detail.project, detail.snapshot, detail.config);
    assert(world_frame.body_index == detail_frame.body_index);

    const double earth_x = earth.position.x;
    const double earth_y = earth.position.y;
    state.render.camera.yaw = 0.31;
    state.render.camera.zoom = 0.64;
    begin_studio_design_focus(state, 6);
    assert(state.selected == 6 && !state.design_view);
    assert(std::abs(state.render.camera.target.x - earth_x) < 1e-12);
    assert(std::abs(state.render.camera.target.y - earth_y) < 1e-12);
    assert(std::abs(state.render.camera.yaw - 0.31) < 1e-12);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(!state.camera_transition && state.design_view && state.selected == 6);
    return_studio_overview(state);
    assert(!state.design_view && state.camera_transition);
    assert(std::abs(state.render.camera.target.x -
                    state.session.project().bodies[6].position.x) < 1e-12);
    const double return_start_zoom = state.render.camera.zoom;
    advance_studio(state, 0.1);
    assert(state.render.camera.zoom > return_start_zoom);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(!state.camera_transition);
    assert(std::abs(state.render.camera.target.x) < 1e-12);
    assert(std::abs(state.render.camera.target.y) < 1e-12);
    assert(std::abs(state.render.camera.zoom - 0.55) < 1e-12);
}

static void test_simulation_waits_for_return_flight() {
    StudioState state(make_solar_system_project(), "simulation-flight.json");
    state.render.sampling = SamplingMode::MSAA4;
    begin_studio_design_focus(state, 3);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(state.design_view);
    begin_studio_simulation(state);
    assert(state.session.mode() == SessionMode::Simulate);
    assert(!state.session.playing());
    assert(!state.design_view && state.camera_transition);
    assert(make_studio_render_input(state).config.sampling == SamplingMode::Single);
    assert(make_studio_render_input(state).snapshot.bodies.size() == 9);
    advance_studio(state, 0.25);
    assert(state.session.snapshot().time == 0.0);
    assert(!state.session.playing());
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(!state.camera_transition && state.session.playing());
    assert(state.render.sampling == SamplingMode::MSAA4);
    assert(make_studio_render_input(state).config.sampling == SamplingMode::MSAA4);
    advance_studio(state, 0.05);
    assert(state.session.snapshot().time > 0.0);

    const double frozen_time = state.session.snapshot().time;
    return_studio_design(state);
    assert(state.session.mode() == SessionMode::Simulate && state.camera_transition);
    assert(!state.session.playing());
    advance_studio(state, 0.2);
    assert(state.session.snapshot().time == frozen_time);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(!state.camera_transition && !state.design_view);
    assert(state.session.mode() == SessionMode::Design);
    assert(std::abs(state.render.camera.zoom - 0.55) < 1e-12);
}

static void test_focus_and_reset_animate_without_losing_playback() {
    StudioState state(make_solar_system_project(), "focus-flight.json");
    state.selected = 3;
    state.session.begin_simulation();
    const double start_zoom = state.render.camera.zoom;
    focus_studio_selected(state);
    assert(state.camera_transition && !state.session.playing());
    advance_studio(state, 0.1);
    assert(state.render.camera.zoom < start_zoom);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(state.session.playing());
    assert(std::abs(state.render.camera.target.x -
                    state.session.project().bodies[3].position.x) < 1e-12);

    state.render.camera.yaw = 6.2;
    reset_studio_view(state);
    assert(state.camera_transition && !state.session.playing());
    advance_studio(state, 0.45);
    assert(state.render.camera.yaw > 6.2);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(state.session.playing());
    assert(std::abs(state.render.camera.yaw) < 1e-12);
    assert(std::abs(state.render.camera.zoom - 0.55) < 1e-12);
}

static void test_collision_demo_exits_to_saved_camera_before_simulation() {
    StudioState state(make_solar_system_project(), "demo-flight.json");
    state.render.camera.yaw = 0.42;
    state.render.camera.zoom = 0.69;
    const RenderCamera saved = state.render.camera;
    activate_studio_preset(state, DemoPreset::Collision);
    assert(state.render.camera.target.x == 4.6);
    begin_studio_simulation(state);
    assert(state.demos.active() == DemoPreset::None);
    assert(state.camera_transition);
    assert(state.camera_transition->from.yaw == saved.yaw);
    assert(state.camera_transition->from.zoom == saved.zoom);
    assert(state.camera_transition->from.target.x == saved.target.x);
}

static void test_demo_return_keeps_departing_planet_in_view() {
    StudioState state(make_solar_system_project(), "demo-back.json");
    assert(state.selected == 1);
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    const Vec2 earth = state.session.project().bodies[3].position;
    return_studio_overview(state);
    assert(state.demos.active() == DemoPreset::None);
    assert(state.selected == 1 && state.camera_transition);
    assert(state.camera_transition->screen_anchor);
    assert(std::abs(state.camera_transition->screen_anchor->x - earth.x) < 1e-12);
    assert(std::abs(state.camera_transition->screen_anchor->y - earth.y) < 1e-12);
    advance_studio(state, 0.45);
    assert(std::abs(state.render.camera.target.x) < 10.0);
    assert(std::abs(state.render.camera.target.y) < 10.0);
}

static void test_demo_reset_returns_to_preset_pose() {
    StudioState state(make_solar_system_project(), "demo-reset.json");
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    state.render.camera.yaw = 0.3;
    state.render.camera.pitch = 0.0;
    state.render.camera.zoom = 1.8;
    reset_studio_view(state);
    assert(state.camera_transition);
    assert(std::abs(state.camera_transition->to.yaw + 0.5) < 1e-12);
    assert(std::abs(state.camera_transition->to.pitch - 0.23) < 1e-12);
    assert(std::abs(state.camera_transition->to.zoom - 0.62) < 1e-12);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    activate_studio_preset(state, DemoPreset::SaturnRing);
    reset_studio_view(state);
    assert(std::abs(state.camera_transition->to.pitch - 0.36) < 1e-12);
    assert(std::abs(state.camera_transition->to.zoom - 0.72) < 1e-12);
}

static void test_camera_flight_respects_sustained_frame_time() {
    StudioState state(make_solar_system_project(), "slow-frame.json");
    begin_studio_design_focus(state, 3);
    advance_studio(state, 0.5);
    assert(state.camera_transition);
    assert(std::abs(state.camera_transition->elapsed - 0.5) < 1e-12);
    advance_studio(state, 0.5);
    assert(!state.camera_transition && state.design_view);
}

static void test_switching_planet_closeups_keeps_bodies_visible() {
    StudioState state(make_solar_system_project(), "cross-planet-flight.json");
    state.render.width = 160;
    state.render.height = 120;
    begin_studio_design_focus(state, 3);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    begin_studio_design_focus(state, 6);
    advance_studio(state, 0.63);
    const StudioRenderInput midpoint = make_studio_render_input(state);
    const RenderFrame frame = render_studio_frame(state, midpoint);
    assert(std::any_of(frame.body_index.begin(), frame.body_index.end(),
                       [](int body) { return body >= 0; }));
}

static void test_demo_sidebar_switch_uses_overview_waypoint() {
    StudioState state(make_solar_system_project(), "demo-switch.json");
    state.render.width = 160;
    state.render.height = 120;
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    select_studio_body(state, 6);
    assert(state.demos.active() == DemoPreset::None);
    assert(state.selected == 6 && state.camera_transition);
    advance_studio(state, 0.63);
    const StudioRenderInput midpoint = make_studio_render_input(state);
    const RenderFrame frame = render_studio_frame(state, midpoint);
    assert(std::any_of(frame.body_index.begin(), frame.body_index.end(),
                       [](int body) { return body >= 0; }));
}

static void test_collision_scene_selection_keeps_demo_running() {
    StudioState state(make_solar_system_project(), "collision-scene-click.json");
    activate_studio_preset(state, DemoPreset::Collision);
    assert(state.selected == 3 && state.demos.playing());

    select_studio_scene_body(state, 3);
    assert(state.demos.active() == DemoPreset::Collision);
    assert(state.demos.playing() && state.selected == 3);
    const double before = make_studio_render_input(state).snapshot.time;
    advance_studio(state, 0.1);
    assert(make_studio_render_input(state).snapshot.time > before);

    select_studio_scene_body(state, 4);
    assert(state.demos.active() == DemoPreset::Collision);
    assert(state.demos.playing() && state.selected == 4);

    exit_studio_preset(state);
    select_studio_scene_body(state, 3);
    assert(state.camera_transition && state.selected == 3);
}

int main() {
    test_design_camera_flight();
    test_simulation_waits_for_return_flight();
    test_focus_and_reset_animate_without_losing_playback();
    test_collision_demo_exits_to_saved_camera_before_simulation();
    test_demo_return_keeps_departing_planet_in_view();
    test_demo_reset_returns_to_preset_pose();
    test_camera_flight_respects_sustained_frame_time();
    test_switching_planet_closeups_keeps_bodies_visible();
    test_demo_sidebar_switch_uses_overview_waypoint();
    test_collision_scene_selection_keeps_demo_running();
    Project project = make_solar_system_project();
    project.bodies[3].design.relief = 2.25;
    StudioState state(project, "test-solar-system.json");
    state.selected = 5;
    state.render.camera.yaw = 0.42;
    state.render.camera.zoom = 0.7;
    const RenderCamera original_camera = state.render.camera;

    activate_studio_preset(state, DemoPreset::EarthTerrain);
    assert(state.demos.active() == DemoPreset::EarthTerrain);
    assert(state.selected == 3 && state.design_view);
    assert(state.session.project().bodies[3].design.relief == 2.25);
    const StudioRenderInput earth = make_studio_render_input(state);
    assert(earth.snapshot.bodies.size() == 1 && earth.snapshot.bodies[0].id == "earth");
    assert(earth.project.bodies[3].design.relief == 2.25);

    activate_studio_preset(state, DemoPreset::SaturnRing);
    assert(state.selected == 6 && state.design_view);
    assert(std::abs(state.render.camera.pitch) > 0.3);
    const StudioRenderInput saturn = make_studio_render_input(state);
    assert(saturn.snapshot.bodies.size() == 1 && saturn.snapshot.bodies[0].id == "saturn");

    activate_studio_preset(state, DemoPreset::Collision);
    assert(state.selected == 3 && !state.design_view);
    assert(state.demos.playing());
    const StudioRenderInput before = make_studio_render_input(state);
    assert(before.snapshot.bodies.size() == 2);
    assert(before.project.bodies[3].position.x == 4.0);
    assert(state.session.project().bodies[3].position.x != 4.0);
    state.compare_modes = true;
    assert(state.demos.playing());
    const double paused = make_studio_render_input(state).snapshot.time;
    advance_studio(state, 0.1);
    assert(make_studio_render_input(state).snapshot.time == paused);
    state.compare_modes = false;
    for (int i = 0; i < 60 && make_studio_render_input(state).snapshot.events.empty(); ++i) {
        advance_studio(state, 0.05);
    }
    const StudioRenderInput impact = make_studio_render_input(state);
    assert(impact.snapshot.events.size() == 1);
    assert(impact.snapshot.bodies.size() == 1);

    exit_studio_preset(state);
    assert(state.demos.active() == DemoPreset::None);
    assert(state.selected == 5 && !state.design_view);
    assert(state.render.camera.yaw == original_camera.yaw);
    assert(state.render.camera.zoom == original_camera.zoom);
    assert(state.session.project().bodies[3].design.relief == 2.25);

    state.design_view = true;
    state.selected = 4;
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    exit_studio_preset(state);
    assert(state.design_view && state.selected == 4);

    Project changed = make_solar_system_project();
    changed.bodies[3].velocity.x += 0.2;
    StudioState experiment(changed, "orbit-lab.json");
    experiment.selected = 3;
    experiment.session.begin_simulation();
    experiment.session.seek(0.5);
    experiment.show_reference_orbit = true;
    const StudioRenderInput earth_orbit = make_studio_render_input(experiment);
    assert(earth_orbit.snapshot.reference_trails.size() == 1);
    assert(earth_orbit.snapshot.reference_trails[0].id == "earth");
    assert(earth_orbit.snapshot.trails.size() == 1);
    assert(earth_orbit.snapshot.trails[0].id == "earth");
    experiment.selected = 4;
    const StudioRenderInput mars_orbit = make_studio_render_input(experiment);
    assert(mars_orbit.snapshot.reference_trails[0].id == "mars");
    experiment.show_reference_orbit = false;
    assert(make_studio_render_input(experiment).snapshot.reference_trails.empty());

    StudioState timeline(make_solar_system_project(), "timeline.json");
    timeline.session.begin_simulation();
    seek_studio_time(timeline, 90.0);
    assert(std::abs(timeline.session.snapshot().time - 90.0) < 1e-12);
    assert(studio_timeline_limit(timeline) >= 90.0);
    seek_studio_time(timeline, 45.0);
    assert(std::abs(timeline.session.snapshot().time - 45.0) < 1e-12);
    assert(studio_timeline_limit(timeline) >= 90.0);
    seek_studio_time(timeline, -1.0);
    assert(timeline.session.snapshot().time == 0.0);
    seek_studio_time(timeline, 100001.0);
    assert(timeline.session.snapshot().time == 0.0);
    seek_studio_time(timeline, 90.0);
    assert(std::abs(timeline.session.snapshot().time - 90.0) < 1e-12);
    seek_studio_time(timeline, std::numeric_limits<double>::quiet_NaN());
    assert(std::abs(timeline.session.snapshot().time - 90.0) < 1e-12);
    assert(std::abs(timeline.session.reference_snapshot()->time - 90.0) < 1e-12);
}
