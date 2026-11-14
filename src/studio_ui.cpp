#include "planet_sandbox/studio_ui.h"
#include "planet_sandbox/solar_system.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace planet_sandbox {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double design_zoom = 1.15;

double detail_extent(const Project& project, const BodySpec& body) {
    const double framing = body.kind == BodyKind::Saturn ? 2.0 :
                           body.kind == BodyKind::Uranus ? 1.4 : 0.95;
    return body.display_radius * project.display_scale * framing;
}

RenderCamera world_camera(const StudioState& state) {
    RenderCamera camera = state.render.camera;
    if (state.design_view && state.session.project().solar_system) {
        const Project& project = state.session.project();
        const BodySpec& body = project.bodies[state.selected];
        camera.target = body.position;
        camera.zoom *= detail_extent(project, body) / project.view_extent;
    }
    return camera;
}

RenderCamera overview_camera(const StudioState& state) {
    RenderCamera camera;
    camera.zoom = state.session.project().solar_system ? 0.55 : 1.0;
    return camera;
}

double smooth_fraction(double fraction) {
    return fraction * fraction * fraction *
           (fraction * (fraction * 6.0 - 15.0) + 10.0);
}

RenderCamera interpolate_camera(const RenderCamera& from, const RenderCamera& to,
                                const std::optional<Vec2>& anchor, double fraction) {
    const double t = smooth_fraction(fraction);
    RenderCamera camera;
    camera.yaw = from.yaw + std::remainder(to.yaw - from.yaw, 2.0 * pi) * t;
    camera.pitch = from.pitch + (to.pitch - from.pitch) * t;
    camera.target = {from.target.x + (to.target.x - from.target.x) * t,
                     from.target.y + (to.target.y - from.target.y) * t};
    camera.zoom = std::exp(std::log(from.zoom) +
                           (std::log(to.zoom) - std::log(from.zoom)) * t);
    if (anchor) {
        const double start_x = (anchor->x - from.target.x) / from.zoom;
        const double start_y = (anchor->y - from.target.y) / from.zoom;
        const double end_x = (anchor->x - to.target.x) / to.zoom;
        const double end_y = (anchor->y - to.target.y) / to.zoom;
        camera.target = {anchor->x - camera.zoom * (start_x + (end_x - start_x) * t),
                         anchor->y - camera.zoom * (start_y + (end_y - start_y) * t)};
    }
    if (fraction >= 1.0) return to;
    return camera;
}

RenderCamera camera_between(const StudioCameraTransition& flight, double fraction) {
    if (flight.via) {
        if (fraction < 0.5) {
            return interpolate_camera(flight.from, *flight.via,
                                      flight.departure_anchor, fraction * 2.0);
        }
        return interpolate_camera(*flight.via, flight.to,
                                  flight.screen_anchor, (fraction - 0.5) * 2.0);
    }
    return interpolate_camera(flight.from, flight.to, flight.screen_anchor, fraction);
}

void start_camera_transition(StudioState& state, RenderCamera from, RenderCamera to,
                             StudioCameraDestination destination,
                             RenderCamera detail = {}, bool resume = false,
                             std::optional<Vec2> anchor = std::nullopt) {
    state.sculpt_stroke.end();
    state.selected_pixel_x = state.selected_pixel_y = -1;
    state.compare_modes = false;
    state.inspect_pixel = false;
    state.render.camera = from;
    state.camera_transition = StudioCameraTransition{from, to, detail, anchor,
                                                      std::nullopt, std::nullopt, 0.0, 0.9,
                                                      destination, resume};
}

void select_body(StudioState& state, int index) {
    const int selected = std::clamp(index, 0, static_cast<int>(state.session.project().bodies.size()) - 1);
    if (selected != state.selected) {
        if (state.design_view && state.session.project().solar_system) state.render.camera.zoom = 0.55;
        state.design_view = false;
        state.compare_baseline = false;
        state.sculpt_stroke.end();
        state.sculpt_layer = SolarShapeLayer::Visible;
    }
    state.selected = selected;
    std::snprintf(state.name.data(), state.name.size(), "%s",
                  state.session.project().bodies[state.selected].name.c_str());
}

void replace_project(StudioState& state, Project project) {
    try {
        state.session.replace_project(std::move(project));
        state.camera_transition.reset();
        select_body(state, state.selected);
        state.notice.clear();
    } catch (const std::exception& error) {
        state.notice = error.what();
    }
}

void save_project(StudioState& state) {
    try {
        std::ofstream output(state.path.data());
        if (!output) throw std::runtime_error("Cannot open project path");
        write_project(output, state.session.project());
        output.close();
        if (!output) throw std::runtime_error("Could not finish saving project");
        state.notice = "Saved";
    } catch (const std::exception& error) {
        state.notice = error.what();
    }
}

void load_project(StudioState& state) {
    try {
        if (state.session.mode() != SessionMode::Design) {
            throw std::logic_error("Return to Design before loading");
        }
        std::ifstream input(state.path.data());
        if (!input) throw std::runtime_error("Cannot open project path");
        Project project = read_project(input);
        if (state.demos.active() != DemoPreset::None) exit_studio_preset(state);
        state.session.replace_project(std::move(project));
        state.camera_transition.reset();
        state.design_view = false;
        state.compare_baseline = false;
        state.render.camera = {};
        state.sculpt_history.clear();
        state.sculpt_stroke.end();
        if (state.session.project().solar_system) state.render.camera.zoom = 0.55;
        select_body(state, 0);
        state.notice = "Loaded";
    } catch (const std::exception& error) {
        state.notice = error.what();
    }
}

float hex_channel(const std::string& hex, int offset) {
    return static_cast<float>(std::stoi(hex.substr(offset, 2), nullptr, 16)) / 255.0f;
}

std::string to_hex(const float color[3]) {
    const int r = static_cast<int>(std::round(std::clamp(color[0], 0.0f, 1.0f) * 255.0f));
    const int g = static_cast<int>(std::round(std::clamp(color[1], 0.0f, 1.0f) * 255.0f));
    const int b = static_cast<int>(std::round(std::clamp(color[2], 0.0f, 1.0f) * 255.0f));
    char text[8]{};
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", r, g, b);
    return text;
}

const char* sampling_name(SamplingMode mode) {
    switch (mode) {
    case SamplingMode::Single: return "1x";
    case SamplingMode::SSAA4: return "4x SSAA";
    case SamplingMode::MSAA4: return "4x MSAA";
    }
    return "Unknown";
}

const char* trace_surface_name(TraceSurface surface) {
    switch (surface) {
    case TraceSurface::Background: return "Background";
    case TraceSurface::Body: return "Surface";
    case TraceSurface::Ring: return "Ring";
    case TraceSurface::Cloud: return "Cloud";
    case TraceSurface::Trail: return "Trail";
    case TraceSurface::ReferenceTrail: return "Reference orbit";
    }
    return "Unknown";
}

void sampling_combo(const char* label, SamplingMode& selected) {
    ImGui::SetNextItemWidth(120.0f);
    if (!ImGui::BeginCombo(label, sampling_name(selected))) return;
    for (SamplingMode mode : {SamplingMode::Single, SamplingMode::SSAA4, SamplingMode::MSAA4}) {
        if (ImGui::Selectable(sampling_name(mode), selected == mode)) selected = mode;
    }
    ImGui::EndCombo();
}

ImVec4 display_color(const std::array<double, 3>& linear) {
    const auto channel = [](double value) {
        return static_cast<float>(std::pow(std::clamp(value, 0.0, 1.0), 1.0 / 2.2));
    };
    return {channel(linear[0]), channel(linear[1]), channel(linear[2]), 1.0f};
}

void draw_pixel_inspector(const StudioState& state, const RenderFrame& frame) {
    if (state.compare_modes) {
        ImGui::Text("Left shades: %llu",
                    static_cast<unsigned long long>(frame.panel_shade_evaluations[0]));
        ImGui::Text("Right shades: %llu",
                    static_cast<unsigned long long>(frame.panel_shade_evaluations[1]));
    } else {
        ImGui::Text("Shade evaluations: %llu",
                    static_cast<unsigned long long>(frame.shade_evaluations));
    }
    if (!frame.trace) {
        ImGui::TextDisabled("No pixel selected");
        return;
    }
    const PixelTrace& trace = *frame.trace;
    ImGui::Text("Pixel (%d, %d)", trace.x, trace.y);
    ImGui::SameLine();
    ImGui::Text("RGB %u, %u, %u", trace.display_rgb[0], trace.display_rgb[1],
                trace.display_rgb[2]);

    constexpr int cells = 9;
    constexpr float cell_size = 16.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(cells * cell_size, cells * cell_size));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int row = 0; row < cells; ++row) {
        for (int col = 0; col < cells; ++col) {
            const int x = std::clamp(trace.x + col - cells / 2, 0, frame.image.width - 1);
            const int y = std::clamp(trace.y + row - cells / 2, 0, frame.image.height - 1);
            const std::size_t pixel = (static_cast<std::size_t>(y) * frame.image.width + x) * 3;
            const ImU32 color = IM_COL32(frame.image.rgb[pixel], frame.image.rgb[pixel + 1],
                                         frame.image.rgb[pixel + 2], 255);
            const ImVec2 a(origin.x + col * cell_size, origin.y + row * cell_size);
            const ImVec2 b(a.x + cell_size, a.y + cell_size);
            draw->AddRectFilled(a, b, color);
        }
    }
    draw->AddRect({origin.x + 4 * cell_size, origin.y + 4 * cell_size},
                  {origin.x + 5 * cell_size, origin.y + 5 * cell_size},
                  IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
    ImGui::Spacing();
    ImGui::SeparatorText("Samples");
    const ImVec2 diagram = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(112.0f, 112.0f));
    draw->AddRectFilled(diagram, {diagram.x + 112.0f, diagram.y + 112.0f},
                        IM_COL32(27, 32, 40, 255));
    for (std::size_t index = 0; index < trace.samples.size(); ++index) {
        const auto& sample = trace.samples[index];
        const ImVec4 color = display_color(sample.linear_rgb);
        const ImVec2 center(diagram.x + static_cast<float>(sample.position.x) * 112.0f,
                            diagram.y + static_cast<float>(sample.position.y) * 112.0f);
        draw->AddCircleFilled(center, 14.0f, ImGui::ColorConvertFloat4ToU32(color));
        draw->AddCircle(center, 14.0f, IM_COL32(240, 245, 250, 255), 0, 2.0f);
        draw->AddText({center.x - 4.0f, center.y - 7.0f}, IM_COL32(0, 0, 0, 255),
                      std::to_string(index + 1).c_str());
    }
    for (std::size_t index = 0; index < trace.samples.size(); ++index) {
        const auto& sample = trace.samples[index];
        const char* body = sample.body_index >= 0 &&
                           sample.body_index < static_cast<int>(state.session.project().bodies.size())
            ? state.session.project().bodies[sample.body_index].name.c_str() : "Background";
        ImGui::Text("%zu  %s / %s", index + 1, body,
                    trace_surface_name(sample.surface));
        ImGui::SameLine(0, 8);
        if (sample.body_index < 0) ImGui::TextDisabled("depth -");
        else ImGui::TextDisabled("depth %.3f", sample.depth);
        ImGui::TextDisabled("    linear %.3f  %.3f  %.3f", sample.linear_rgb[0],
                            sample.linear_rgb[1], sample.linear_rgb[2]);
    }
}

void edit_appearance(StudioState& state) {
    const Appearance current = state.session.project().bodies[state.selected].appearance;
    auto update = [&](auto edit) {
        Project next = state.session.project();
        edit(next.bodies[state.selected].appearance);
        replace_project(state, std::move(next));
    };

    float primary[3] = {hex_channel(current.primary, 1), hex_channel(current.primary, 3),
                        hex_channel(current.primary, 5)};
    if (ImGui::ColorEdit3("Primary", primary)) update([&](Appearance& a) { a.primary = to_hex(primary); });
    float secondary[3] = {hex_channel(current.secondary, 1), hex_channel(current.secondary, 3),
                          hex_channel(current.secondary, 5)};
    if (ImGui::ColorEdit3("Secondary", secondary)) update([&](Appearance& a) { a.secondary = to_hex(secondary); });
    std::uint32_t seed = current.seed;
    if (ImGui::InputScalar("Seed", ImGuiDataType_U32, &seed)) {
        update([&](Appearance& a) { a.seed = seed; });
    }
    float bands = static_cast<float>(current.bands);
    if (ImGui::SliderFloat("Bands", &bands, 0.0f, 40.0f)) update([&](Appearance& a) { a.bands = bands; });
    float terrain = static_cast<float>(current.terrain);
    if (ImGui::SliderFloat("Terrain", &terrain, 0.0f, 1.0f)) update([&](Appearance& a) { a.terrain = terrain; });
    float clouds = static_cast<float>(current.clouds);
    if (ImGui::SliderFloat("Clouds", &clouds, 0.0f, 1.0f)) update([&](Appearance& a) { a.clouds = clouds; });
    float ring = static_cast<float>(current.ring_width);
    if (ImGui::SliderFloat("Ring width", &ring, 0.0f, 2.0f)) update([&](Appearance& a) { a.ring_width = ring; });
}

void edit_solar_design(StudioState& state) {
    const auto& body = state.session.project().bodies[state.selected];
    const DesignOverrides current = body.design;
    const auto update = [&](auto edit) {
        Project next = state.session.project();
        edit(next.bodies[state.selected].design);
        replace_project(state, std::move(next));
    };
    auto slider = [&](const char* label, double value, float low, float high, auto edit) {
        ImGui::TextUnformatted(label);
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(-1.0f);
        float candidate = static_cast<float>(value);
        if (ImGui::SliderFloat("##value", &candidate, low, high)) {
            update([&](DesignOverrides& d) { edit(d, candidate); });
        }
        ImGui::PopID();
    };
    switch (body.kind) {
    case BodyKind::Sun:
        ImGui::SeparatorText("Photosphere");
        slider("Artistic photosphere relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Color temperature", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Granulation", current.granulation, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.granulation = v; });
        slider("Granule scale", current.granule_scale, 0.2f, 3.0f,
               [](DesignOverrides& d, float v) { d.granule_scale = v; });
        slider("Sunspot intensity", current.sunspot_strength, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.sunspot_strength = v; });
        break;
    case BodyKind::Mercury:
        ImGui::SeparatorText("Cratered surface");
        slider("Image-derived crater relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Crater depth", current.crater_depth, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.crater_depth = v; });
        slider("Crater contrast", current.band_contrast, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.band_contrast = v; });
        slider("Surface tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        break;
    case BodyKind::Venus:
        ImGui::SeparatorText("Cloud deck");
        slider("Artistic cloud-top relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Cloud opacity", current.cloud_opacity, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.cloud_opacity = v; });
        slider("Warmth", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        ImGui::SeparatorText("Radar surface");
        slider("Surface relief", current.radar_relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.radar_relief = v; });
        if (current.cloud_opacity < 0.99) ImGui::TextDisabled("Exposed surface is radar-derived");
        break;
    case BodyKind::Earth:
        ImGui::SeparatorText("Surface");
        slider("Relative terrain relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Land / ocean tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Polar ice", current.ice_extent, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.ice_extent = v; });
        ImGui::SeparatorText("Clouds");
        slider("Cloud cover", current.cloud_opacity, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.cloud_opacity = v; });
        slider("Cloud drift", current.cloud_motion, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.cloud_motion = v; });
        break;
    case BodyKind::Mars:
        ImGui::SeparatorText("Terrain");
        slider("Relative elevation relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Rust tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Dust", current.dust_opacity, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.dust_opacity = v; });
        break;
    case BodyKind::Saturn:
        ImGui::SeparatorText("Cloud belts");
        slider("Artistic cloud-top relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Cloud tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Band contrast", current.band_contrast, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.band_contrast = v; });
        slider("Ring density", current.ring_width, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.ring_width = v; });
        slider("Cassini gap", current.cassini_gap, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.cassini_gap = v; });
        slider("Ring tilt offset", current.ring_tilt_offset, -30.0f, 30.0f,
               [](DesignOverrides& d, float v) { d.ring_tilt_offset = v; });
        break;
    case BodyKind::Jupiter:
        ImGui::SeparatorText("Cloud belts");
        slider("Artistic cloud-top relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Cloud-belt relief", current.belt_relief, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.belt_relief = v; });
        slider("Band contrast", current.band_contrast, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.band_contrast = v; });
        slider("Great Red Spot", current.storm_scale, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.storm_scale = v; });
        slider("Spot relief", current.storm_relief, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.storm_relief = v; });
        slider("Cloud tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        break;
    case BodyKind::Uranus:
        ImGui::SeparatorText("Ice giant");
        slider("Artistic cloud-top relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Cyan tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Cloud contrast", current.band_contrast, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.band_contrast = v; });
        slider("Axis tilt offset", current.axis_tilt_offset, -30.0f, 30.0f,
               [](DesignOverrides& d, float v) { d.axis_tilt_offset = v; });
        slider("Faint rings", current.ring_width, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.ring_width = v; });
        break;
    case BodyKind::Neptune:
        ImGui::SeparatorText("Cloud deck");
        slider("Artistic cloud-top relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Blue tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Cloud contrast", current.band_contrast, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.band_contrast = v; });
        slider("Storm intensity", current.storm_scale, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.storm_scale = v; });
        break;
    default:
        break;
    }
    if (state.design_view) {
        ImGui::SeparatorText("Sculpt");
        if (ImGui::RadioButton("Raise", state.sculpt_mode == SculptMode::Raise)) {
            state.sculpt_mode = SculptMode::Raise;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Lower", state.sculpt_mode == SculptMode::Lower)) {
            state.sculpt_mode = SculptMode::Lower;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Smooth", state.sculpt_mode == SculptMode::Smooth)) {
            state.sculpt_mode = SculptMode::Smooth;
        }
        if (body.kind == BodyKind::Venus) {
            if (ImGui::RadioButton("Cloud top", state.sculpt_layer == SolarShapeLayer::Visible)) {
                state.sculpt_layer = SolarShapeLayer::Visible;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Radar surface",
                                   state.sculpt_layer == SolarShapeLayer::VenusRadar)) {
                state.sculpt_layer = SolarShapeLayer::VenusRadar;
            }
        }
        ImGui::SliderFloat("Brush size", &state.sculpt_radius, 0.12f, 0.8f);
        ImGui::SliderFloat("Strength", &state.sculpt_strength, 0.1f, 1.0f);
        if (ImGui::Button("Undo sculpt", ImVec2(-1, 0))) {
            Project next = state.session.project();
            if (state.sculpt_history.undo(body.kind, next.bodies[state.selected].design)) {
                replace_project(state, std::move(next));
            }
            state.sculpt_stroke.end();
        }
    }
    if (ImGui::Button("Reset this body", ImVec2(-1, 0))) {
        try {
            state.session.reset_body_design(body.kind);
            state.sculpt_history.clear_body(body.kind);
            state.sculpt_stroke.end();
            state.notice = "Body reset";
        }
        catch (const std::exception& error) { state.notice = error.what(); }
    }
}

void edit_motion(StudioState& state) {
    const BodySpec current = state.session.project().bodies[state.selected];
    auto update = [&](auto edit) {
        Project next = state.session.project();
        edit(next.bodies[state.selected]);
        replace_project(state, std::move(next));
    };
    double mass = current.mass;
    if (ImGui::InputDouble("Mass", &mass,
                           state.session.project().solar_system ? 0.0 : 0.1,
                           state.session.project().solar_system ? 0.0 : 1.0,
                           state.session.project().solar_system ? "%.8g" : "%.4f")) {
        update([&](BodySpec& b) { b.mass = mass; });
    }
    double radius = current.radius;
    if (ImGui::InputDouble(state.session.project().solar_system ? "Collision radius" : "Radius",
                           &radius, state.session.project().solar_system ? 0.0 : 0.01,
                           state.session.project().solar_system ? 0.0 : 0.1,
                           state.session.project().solar_system ? "%.8g" : "%.4f")) {
        update([&](BodySpec& b) { b.radius = radius; });
    }
    ImGui::SeparatorText("Initial position");
    double x = current.position.x, y = current.position.y;
    if (ImGui::InputDouble("X##position", &x, 0.1, 1.0, "%.3f")) update([&](BodySpec& b) { b.position.x = x; });
    if (ImGui::InputDouble("Y##position", &y, 0.1, 1.0, "%.3f")) update([&](BodySpec& b) { b.position.y = y; });
    ImGui::SeparatorText("Initial velocity");
    double vx = current.velocity.x, vy = current.velocity.y;
    if (ImGui::InputDouble("X##velocity", &vx, 0.1, 1.0, "%.3f")) update([&](BodySpec& b) { b.velocity.x = vx; });
    if (ImGui::InputDouble("Y##velocity", &vy, 0.1, 1.0, "%.3f")) update([&](BodySpec& b) { b.velocity.y = vy; });
}

void draw_sidebar(StudioState& state, float width) {
    ImGui::BeginChild("##sidebar", ImVec2(width, 0), ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(state.camera_transition.has_value());
    ImGui::TextUnformatted(state.session.project().solar_system ? "SOLAR SYSTEM" : "BODIES");
    ImGui::Separator();
    for (std::size_t i = 0; i < state.session.project().bodies.size(); ++i) {
        const auto& body = state.session.project().bodies[i];
        if (ImGui::Selectable((body.name + "##" + body.id).c_str(), state.selected == static_cast<int>(i))) {
            select_studio_body(state, static_cast<int>(i));
        }
    }
    const bool can_edit = state.session.mode() == SessionMode::Design &&
                          state.demos.active() != DemoPreset::Collision;
    if (!state.session.project().solar_system) {
      ImGui::BeginDisabled(!can_edit || state.session.project().bodies.size() >= 32);
      if (ImGui::Button("Add planet", ImVec2(-1, 0))) {
        try {
            state.session.add_planet();
            select_body(state, static_cast<int>(state.session.project().bodies.size()) - 1);
            state.notice.clear();
        } catch (const std::exception& error) {
            state.notice = error.what();
        }
    }
      ImGui::EndDisabled();
      ImGui::BeginDisabled(!can_edit || state.selected == 0);
      if (ImGui::Button("Remove", ImVec2(-1, 0))) {
        Project next = state.session.project();
        next.bodies.erase(next.bodies.begin() + state.selected);
        replace_project(state, std::move(next));
    }
      ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Project");
    ImGui::InputText("##path", state.path.data(), state.path.size());
    if (ImGui::Button("Save", ImVec2((width - 28) * 0.5f, 0))) save_project(state);
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_edit);
    if (ImGui::Button("Load", ImVec2(-1, 0))) load_project(state);
    ImGui::EndDisabled();
    if (!state.notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, state.notice == "Saved" || state.notice == "Loaded" ||
                                              state.notice == "Body reset"
                                              ? ImVec4(0.45f, 0.8f, 0.66f, 1.0f)
                                              : ImVec4(0.94f, 0.45f, 0.41f, 1.0f));
        ImGui::TextWrapped("%s", state.notice.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("View");
    ImGui::BeginDisabled(!can_edit);
    float extent = static_cast<float>(state.session.project().view_extent);
    if (ImGui::SliderFloat("Extent", &extent, 1.0f, 20.0f)) {
        Project next = state.session.project();
        next.view_extent = extent;
        replace_project(state, std::move(next));
    }
    float scale = static_cast<float>(state.session.project().display_scale);
    if (ImGui::SliderFloat("Scale", &scale, 0.5f, 8.0f)) {
        Project next = state.session.project();
        next.display_scale = scale;
        replace_project(state, std::move(next));
    }
    ImGui::EndDisabled();
    if (state.session.project().solar_system) {
        ImGui::Spacing();
        ImGui::SeparatorText("Demos");
        ImGui::BeginDisabled(state.session.mode() != SessionMode::Design);
        if (ImGui::Button("Earth terrain", ImVec2(-1, 0))) {
            activate_studio_preset(state, DemoPreset::EarthTerrain);
        }
        if (ImGui::Button("Saturn ring edge", ImVec2(-1, 0))) {
            activate_studio_preset(state, DemoPreset::SaturnRing);
        }
        if (ImGui::Button("Planet collision", ImVec2(-1, 0))) {
            activate_studio_preset(state, DemoPreset::Collision);
        }
        ImGui::EndDisabled();
        if (state.demos.active() != DemoPreset::None &&
            ImGui::Button("Exit demo", ImVec2(-1, 0))) exit_studio_preset(state);
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
}

void draw_inspector(StudioState& state, const RenderFrame& frame, float width) {
    ImGui::BeginChild("##inspector", ImVec2(width, 0), ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(state.camera_transition.has_value());
    ImGui::TextUnformatted("INSPECTOR");
    ImGui::Separator();
    const auto& body = state.session.project().bodies[state.selected];
    ImGui::Text("%s", body.id.c_str());
    if (!state.session.project().solar_system) {
      ImGui::BeginDisabled(state.session.mode() != SessionMode::Design);
      ImGui::InputText("Name", state.name.data(), state.name.size());
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        Project next = state.session.project();
        next.bodies[state.selected].name = state.name.data();
        replace_project(state, std::move(next));
      }
      ImGui::EndDisabled();
    }
    if (state.session.project().solar_system && state.design_view) {
        ImGui::Checkbox("Show baseline", &state.compare_baseline);
    }
    if (ImGui::BeginTabBar("##inspector-tabs")) {
        if (ImGui::BeginTabItem(state.session.project().solar_system ? "Design" : "Appearance")) {
            ImGui::BeginDisabled(state.session.mode() != SessionMode::Design ||
                                 state.demos.active() == DemoPreset::Collision);
            if (state.session.project().solar_system) edit_solar_design(state);
            else edit_appearance(state);
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Orbit / Physics")) {
            ImGui::BeginDisabled(state.session.mode() != SessionMode::Design ||
                                 state.demos.active() == DemoPreset::Collision);
            edit_motion(state);
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pixel", nullptr,
                                state.select_pixel_tab ? ImGuiTabItemFlags_SetSelected : 0)) {
            draw_pixel_inspector(state, frame);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    state.select_pixel_tab = false;
    ImGui::EndDisabled();
    ImGui::EndChild();
}

void draw_scene(StudioState& state, ImTextureID texture, const StudioRenderInput& input,
                const RenderFrame& frame,
                bool flip_image_y, float width) {
    ImGui::BeginChild("##scene", ImVec2(width, 0));
    const SceneSnapshot& snapshot = input.snapshot;
    ImGui::BeginDisabled(state.camera_transition.has_value());
    if (ImGui::Button("Reset view")) {
        reset_studio_view(state);
    }
    ImGui::SameLine();
    if (state.session.project().solar_system && state.session.mode() == SessionMode::Design) {
        const bool leaving = state.design_view || state.demos.active() != DemoPreset::None;
        if (ImGui::Button(leaving ? "Back to system" : "Design selected")) {
            if (leaving) {
                return_studio_overview(state);
            } else {
                begin_studio_design_focus(state, state.selected);
            }
        }
    } else if (ImGui::Button("Focus selected")) {
        focus_studio_selected(state);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Compare", &state.compare_modes)) {
        state.selected_pixel_x = state.selected_pixel_y = -1;
        state.sculpt_stroke.end();
        if (state.compare_modes) {
            state.inspect_pixel = true;
            if (state.session.mode() == SessionMode::Simulate) state.session.set_playing(false);
            if (state.demos.active() == DemoPreset::Collision) state.demos.set_playing(false);
        }
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Inspect", &state.inspect_pixel)) {
        state.selected_pixel_x = state.selected_pixel_y = -1;
        state.sculpt_stroke.end();
        if (!state.inspect_pixel) state.compare_modes = false;
    }
    if (state.compare_modes) {
        sampling_combo("Left", state.compare_left);
        ImGui::SameLine();
        sampling_combo("Right", state.compare_right);
    } else {
        for (SamplingMode mode : {SamplingMode::Single, SamplingMode::SSAA4,
                                  SamplingMode::MSAA4}) {
            if (mode != SamplingMode::Single) ImGui::SameLine();
            if (ImGui::RadioButton(sampling_name(mode), state.render.sampling == mode)) {
                state.render.sampling = mode;
            }
        }
    }
    const bool orbit_lab = state.session.project().solar_system &&
                           state.session.mode() == SessionMode::Simulate &&
                           state.demos.active() == DemoPreset::None;
    if (orbit_lab) {
        const double current_time = snapshot.time;
        float time = static_cast<float>(current_time);
        const float max_time = static_cast<float>(studio_timeline_limit(state));
        ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 65.0f));
        if (ImGui::SliderFloat("Time", &time, 0.0f, max_time, "%.2f",
                               ImGuiSliderFlags_AlwaysClamp)) {
            seek_studio_time(state, time);
        }
        ImGui::Checkbox("Reference orbit", &state.show_reference_orbit);
        if (state.show_reference_orbit) {
            ImGui::SameLine();
            ImGui::TextColored({0.95f, 0.64f, 0.24f, 1.0f}, "Catalog");
            ImGui::SameLine();
            ImGui::TextColored({0.43f, 0.75f, 0.75f, 1.0f}, "DIY");
        }
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    const int image_width = frame.image.width;
    const int image_height = frame.image.height;
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float image_space_height = std::max(100.0f, available.y - 100.0f);
    const float fit = std::min(available.x / image_width, image_space_height / image_height);
    const ImVec2 size(image_width * fit, image_height * fit);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (available.x - size.x) * 0.5f));
    const ImVec2 top_left = ImGui::GetCursorScreenPos();
    ImGui::Image(texture, size, flip_image_y ? ImVec2(0, 1) : ImVec2(0, 0),
                 flip_image_y ? ImVec2(1, 0) : ImVec2(1, 1));
    if (state.compare_modes) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float middle = top_left.x + size.x * 0.5f;
        draw->AddLine({middle, top_left.y}, {middle, top_left.y + size.y},
                      IM_COL32(245, 245, 245, 220), 1.0f);
        draw->AddText({top_left.x + 8.0f, top_left.y + 8.0f}, IM_COL32(230, 235, 240, 255),
                      sampling_name(state.compare_left));
        draw->AddText({middle + 8.0f, top_left.y + 8.0f}, IM_COL32(230, 235, 240, 255),
                      sampling_name(state.compare_right));
    }
    if (state.inspect_pixel && state.selected_pixel_x >= 0 && state.selected_pixel_y >= 0 &&
        state.selected_pixel_x < image_width && state.selected_pixel_y < image_height) {
        const ImVec2 point(top_left.x + (state.selected_pixel_x + 0.5f) * size.x / image_width,
                           top_left.y + (state.selected_pixel_y + 0.5f) * size.y / image_height);
        ImGui::GetWindowDrawList()->AddRect({point.x - 4, point.y - 4},
                                            {point.x + 4, point.y + 4},
                                            IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.5f);
    }
    ImGui::SetCursorScreenPos(top_left);
    ImGui::InvisibleButton("##scene-interaction", size);
    if (!state.camera_transition && ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const double minimum_zoom = state.design_view && state.session.project().solar_system
                ? 0.4 : 0.08;
            state.render.camera.zoom = std::clamp(
                state.render.camera.zoom * std::pow(0.88, wheel), minimum_zoom, 3.0);
        }
    }
    const bool design_canvas = state.design_view && state.session.project().solar_system &&
                               state.session.mode() == SessionMode::Design;
    const bool sculpting = design_canvas && !state.inspect_pixel && !state.compare_modes &&
                           !state.camera_transition;
    const ImGuiMouseButton rotate_button = design_canvas ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;
    if (!state.camera_transition && ImGui::IsItemHovered() &&
        ImGui::IsMouseDragging(rotate_button, 2.0f)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        state.render.camera.yaw += delta.x * 0.006;
        state.render.camera.pitch = std::clamp(state.render.camera.pitch + delta.y * 0.006,
                                               -1.3, 1.3);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) state.sculpt_stroke.end();
    if (sculpting && !state.compare_baseline && ImGui::IsItemHovered() &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const int x = std::clamp(static_cast<int>((mouse.x - top_left.x) / size.x * image_width),
                                 0, image_width - 1);
        int y = std::clamp(static_cast<int>((mouse.y - top_left.y) / size.y * image_height),
                           0, image_height - 1);
        if (flip_image_y) y = image_height - 1 - y;
        const BodyKind kind = state.session.project().bodies[state.selected].kind;
        std::vector<Vec2> dabs;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            state.sculpt_stroke.begin(frame, state.selected, x, y)) {
            state.sculpt_history.begin_stroke(kind, state.sculpt_layer,
                                               state.session.project().bodies[state.selected].design);
            dabs.push_back(frame.surface_uv[static_cast<std::size_t>(y) * image_width + x]);
        } else {
            dabs = state.sculpt_stroke.move(frame, state.selected, x, y);
        }
        if (!dabs.empty()) {
            Project next = state.session.project();
            for (const Vec2 uv : dabs) {
                apply_sculpt_brush(next.bodies[state.selected].design, kind, state.sculpt_layer,
                                   state.sculpt_mode, uv.x, uv.y, state.sculpt_radius,
                                   state.sculpt_strength);
            }
            replace_project(state, std::move(next));
        }
    }
    if (!state.camera_transition && ImGui::IsItemClicked()) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const int x = std::clamp(static_cast<int>((mouse.x - top_left.x) / size.x * image_width),
                                 0, image_width - 1);
        int y = std::clamp(static_cast<int>((mouse.y - top_left.y) / size.y * image_height),
                           0, image_height - 1);
        if (flip_image_y) y = image_height - 1 - y;
        if (state.inspect_pixel) {
            state.selected_pixel_x = x;
            state.selected_pixel_y = y;
            state.select_pixel_tab = true;
        } else {
            const int body = frame.body_index[static_cast<std::size_t>(y) * image_width + x];
            if (body >= 0 && !state.design_view) select_studio_scene_body(state, body);
        }
    }
    ImGui::BeginDisabled(state.camera_transition.has_value());
    ImGui::SeparatorText("Events");
    const auto& events = orbit_lab ? state.session.observed_events() : snapshot.events;
    if (events.empty()) ImGui::TextDisabled("No collisions");
    std::optional<double> pending_seek;
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (orbit_lab) {
            char label[192]{};
            std::snprintf(label, sizeof(label), "%.3f   %s + %s", it->time,
                          it->survivor.c_str(), it->absorbed.c_str());
            if (ImGui::Selectable(label)) {
                pending_seek = std::min(100000.0, it->time + 0.05);
            }
        } else {
            ImGui::Text("%.3f   %s + %s", it->time, it->survivor.c_str(),
                        it->absorbed.c_str());
        }
    }
    if (pending_seek) seek_studio_time(state, *pending_seek);
    ImGui::EndDisabled();
    ImGui::EndChild();
}

} // namespace

StudioState::StudioState(Project project, const std::string& project_path)
    : session(std::move(project)) {
    std::snprintf(path.data(), path.size(), "%s", project_path.c_str());
    if (session.project().solar_system) render.camera.zoom = 0.55;
    select_body(*this, session.project().bodies.size() > 1 ? 1 : 0);
}

void begin_studio_design_focus(StudioState& state, int body_index) {
    if (!state.session.project().solar_system || state.session.mode() != SessionMode::Design ||
        state.demos.active() == DemoPreset::Collision) return;
    if (body_index < 0 || body_index >= static_cast<int>(state.session.project().bodies.size())) return;
    const bool transferring = state.design_view && state.selected != body_index;
    const Vec2 departing = state.session.project().bodies[state.selected].position;
    const RenderCamera from = world_camera(state);
    if (state.demos.active() != DemoPreset::None) {
        exit_studio_preset(state);
        state.design_view = false;
        state.render.camera = from;
    }
    select_body(state, body_index);
    state.design_view = false;
    state.compare_baseline = false;
    const Project& project = state.session.project();
    const BodySpec& body = project.bodies[body_index];
    RenderCamera detail;
    detail.yaw = from.yaw;
    detail.pitch = from.pitch;
    detail.zoom = design_zoom;
    RenderCamera to = detail;
    to.target = body.position;
    to.zoom *= detail_extent(project, body) / project.view_extent;
    start_camera_transition(state, from, to, StudioCameraDestination::Design,
                            detail, false, body.position);
    if (transferring) {
        RenderCamera via = overview_camera(state);
        via.yaw = from.yaw;
        via.pitch = from.pitch;
        state.camera_transition->via = via;
        state.camera_transition->departure_anchor = departing;
        state.camera_transition->duration = 1.5;
    }
}

void select_studio_body(StudioState& state, int body_index) {
    if (state.session.mode() == SessionMode::Design && state.session.project().solar_system &&
        state.demos.active() != DemoPreset::Collision) {
        begin_studio_design_focus(state, body_index);
    } else {
        if (state.demos.active() != DemoPreset::None) exit_studio_preset(state);
        select_body(state, body_index);
    }
}

void select_studio_scene_body(StudioState& state, int body_index) {
    if (state.demos.active() == DemoPreset::Collision) {
        select_body(state, body_index);
    } else {
        select_studio_body(state, body_index);
    }
}

void return_studio_overview(StudioState& state) {
    RenderCamera from = world_camera(state);
    std::optional<Vec2> anchor = state.session.project().solar_system
        ? std::optional<Vec2>(state.session.project().bodies[state.selected].position)
        : std::nullopt;
    const DemoPreset preset = state.demos.active();
    if (preset != DemoPreset::None) {
        exit_studio_preset(state);
        if (preset == DemoPreset::Collision) {
            from = world_camera(state);
            anchor = state.session.project().bodies[state.selected].position;
        }
    }
    state.design_view = false;
    state.compare_baseline = false;
    start_camera_transition(state, from, overview_camera(state),
                            StudioCameraDestination::Overview, {}, false, anchor);
}

void begin_studio_simulation(StudioState& state) {
    if (state.session.mode() != SessionMode::Design) return;
    const DemoPreset preset = state.demos.active();
    const int focused = state.selected;
    RenderCamera from = world_camera(state);
    std::optional<Vec2> anchor = state.session.project().solar_system
        ? std::optional<Vec2>(state.session.project().bodies[state.selected].position)
        : std::nullopt;
    if (preset != DemoPreset::None) {
        exit_studio_preset(state);
        if (preset != DemoPreset::Collision) select_body(state, focused);
        else {
            from = world_camera(state);
            anchor = state.session.project().bodies[state.selected].position;
        }
    }
    state.session.begin_simulation();
    state.session.set_playing(false);
    state.design_view = false;
    state.compare_baseline = false;
    state.timeline_horizon = 80.0;
    start_camera_transition(state, from, overview_camera(state),
                            StudioCameraDestination::Simulation, {}, true, anchor);
}

void return_studio_design(StudioState& state) {
    if (state.session.mode() != SessionMode::Simulate) return;
    const RenderCamera from = world_camera(state);
    state.session.set_playing(false);
    const std::optional<Vec2> anchor = state.session.project().solar_system
        ? std::optional<Vec2>(state.session.project().bodies[state.selected].position)
        : std::nullopt;
    state.design_view = false;
    state.timeline_horizon = 80.0;
    start_camera_transition(state, from, overview_camera(state),
                            StudioCameraDestination::DesignOverview, {}, false, anchor);
}

void focus_studio_selected(StudioState& state) {
    const Project& project = state.session.project();
    const BodySpec& selected = project.bodies[state.selected];
    const SceneSnapshot snapshot = state.demos.snapshot(state.session.snapshot());
    const auto found = std::find_if(snapshot.bodies.begin(), snapshot.bodies.end(),
                                    [&](const BodyState& body) { return body.id == selected.id; });
    const Vec2 position = found == snapshot.bodies.end() ? selected.position : found->position;
    const double radius = found == snapshot.bodies.end() ? selected.radius : found->radius;
    const double visible_radius = project.solar_system ? selected.display_radius : radius;
    const RenderCamera from = world_camera(state);
    RenderCamera to = from;
    to.target = position;
    to.zoom = std::clamp(visible_radius * project.display_scale /
                             (project.view_extent * 0.55), 0.08, 1.0);
    const bool resume = state.session.mode() == SessionMode::Simulate && state.session.playing();
    if (resume) state.session.set_playing(false);
    start_camera_transition(state, from, to, StudioCameraDestination::Camera, {}, resume, position);
}

void reset_studio_view(StudioState& state) {
    const RenderCamera from = state.render.camera;
    RenderCamera to = overview_camera(state);
    if (state.demos.active() == DemoPreset::Collision) {
        to.target = {4.6, 0.0};
        to.zoom = 0.45;
    } else if (state.demos.active() == DemoPreset::EarthTerrain) {
        to.yaw = -0.5;
        to.pitch = 0.23;
        to.zoom = 0.62;
    } else if (state.demos.active() == DemoPreset::SaturnRing) {
        to.pitch = 0.36;
        to.zoom = 0.72;
    } else if (state.design_view && state.session.project().solar_system) {
        to.zoom = design_zoom;
    }
    const bool resume = state.session.mode() == SessionMode::Simulate && state.session.playing();
    if (resume) state.session.set_playing(false);
    start_camera_transition(state, from, to, StudioCameraDestination::Camera, {}, resume);
}

void activate_studio_preset(StudioState& state, DemoPreset preset) {
    if (preset == DemoPreset::None) { exit_studio_preset(state); return; }
    if (state.session.mode() != SessionMode::Design) {
        throw std::logic_error("Return to Design before starting a demo");
    }
    state.demos.activate(preset, state.session.project());
    state.camera_transition.reset();
    if (!state.saved_view) {
        state.saved_view = StudioState::SavedView{state.selected, state.render.camera,
            state.design_view, state.compare_baseline, state.compare_modes,
            state.inspect_pixel, state.selected_pixel_x, state.selected_pixel_y};
    }
    state.sculpt_stroke.end();
    state.compare_baseline = false;
    state.compare_modes = false;
    state.inspect_pixel = false;
    state.selected_pixel_x = state.selected_pixel_y = -1;
    select_body(state, preset == DemoPreset::SaturnRing ? 6 : 3);
    state.render.camera = {};
    if (preset == DemoPreset::Collision) {
        state.design_view = false;
        state.render.camera.target = {4.6, 0.0};
        state.render.camera.zoom = 0.45;
    } else {
        state.design_view = true;
        state.render.camera.zoom = preset == DemoPreset::EarthTerrain ? 0.62 : 0.72;
        state.render.camera.yaw = preset == DemoPreset::EarthTerrain ? -0.5 : 0.0;
        state.render.camera.pitch = preset == DemoPreset::EarthTerrain ? 0.23 : 0.36;
    }
}

void exit_studio_preset(StudioState& state) {
    state.camera_transition.reset();
    state.demos.clear();
    if (!state.saved_view) return;
    const StudioState::SavedView saved = *state.saved_view;
    state.saved_view.reset();
    select_body(state, saved.selected);
    state.render.camera = saved.camera;
    state.design_view = saved.design_view;
    state.compare_baseline = saved.compare_baseline;
    state.compare_modes = saved.compare_modes;
    state.inspect_pixel = saved.inspect_pixel;
    state.selected_pixel_x = saved.pixel_x;
    state.selected_pixel_y = saved.pixel_y;
}

void advance_studio(StudioState& state, double real_seconds) {
    if (state.camera_transition) {
        StudioCameraTransition& flight = *state.camera_transition;
        if (std::isfinite(real_seconds) && real_seconds > 0.0) {
            flight.elapsed = std::min(flight.duration, flight.elapsed + std::min(real_seconds, 0.75));
        }
        const double fraction = flight.elapsed / flight.duration;
        state.render.camera = camera_between(flight, fraction);
        if (fraction >= 1.0) {
            if (flight.destination == StudioCameraDestination::Design) {
                state.design_view = true;
                state.render.camera = flight.detail_camera;
            } else if (flight.destination == StudioCameraDestination::DesignOverview) {
                state.session.return_to_design();
            } else if (flight.destination == StudioCameraDestination::Simulation &&
                       flight.resume_playback) {
                state.session.set_playing(true);
            } else if (flight.destination == StudioCameraDestination::Camera &&
                       flight.resume_playback && state.session.mode() == SessionMode::Simulate) {
                state.session.set_playing(true);
            }
            state.camera_transition.reset();
        }
        return;
    }
    if (state.compare_modes) return;
    if (state.demos.active() == DemoPreset::Collision) {
        state.demos.advance(real_seconds, state.session.speed());
    } else {
        state.session.advance(real_seconds);
        if (state.session.mode() == SessionMode::Simulate) {
            state.timeline_horizon = std::max(state.timeline_horizon,
                                               std::ceil(state.session.snapshot().time));
        }
    }
}

double studio_timeline_limit(const StudioState& state) {
    return std::min(100000.0, std::max({80.0, state.timeline_horizon,
                                        std::ceil(state.session.snapshot().time)}));
}

void seek_studio_time(StudioState& state, double time) {
    if (state.session.mode() != SessionMode::Simulate ||
        !std::isfinite(time) || time > 100000.0) return;
    const double target = std::max(0.0, time);
    state.session.seek(target);
    state.timeline_horizon = std::max(state.timeline_horizon, std::ceil(target));
}

StudioRenderInput make_studio_render_input(const StudioState& state) {
    StudioRenderInput result{state.demos.project(state.session.project()),
                             state.demos.snapshot(state.session.snapshot()), state.render};
    if (state.show_reference_orbit && state.session.mode() == SessionMode::Simulate &&
        state.session.project().solar_system) {
        const std::string& id = state.session.project().bodies[state.selected].id;
        const auto reference = state.session.reference_snapshot();
        if (reference) {
            for (const BodyTrail& trail : reference->trails) {
                if (trail.id == id) result.snapshot.reference_trails.push_back(trail);
            }
        }
        result.snapshot.trails.erase(
            std::remove_if(result.snapshot.trails.begin(), result.snapshot.trails.end(),
                           [&](const BodyTrail& trail) { return trail.id != id; }),
            result.snapshot.trails.end());
    }
    if (state.design_view && result.project.solar_system &&
        state.session.mode() == SessionMode::Design) {
        const BodySpec& selected = result.project.bodies[state.selected];
        if (state.compare_baseline) {
            result.project.bodies[state.selected].design = baseline_design(selected.kind);
        }
        result.snapshot.bodies = {{selected.id, {0, 0}, {}, selected.mass, selected.radius}};
        result.snapshot.trails.clear();
        result.snapshot.events.clear();
        const double framing = selected.kind == BodyKind::Saturn ? 2.0 :
                               selected.kind == BodyKind::Uranus ? 1.4 : 0.95;
        result.project.view_extent = selected.display_radius * result.project.display_scale * framing;
        result.config.camera.target = {};
    }
    if (state.compare_modes) {
        result.config.width = std::max(1, result.config.width / 2);
        result.config.camera.zoom *= 2.0;
    }
    if (state.inspect_pixel && state.selected_pixel_x >= 0 && state.selected_pixel_y >= 0) {
        result.config.trace_x = state.selected_pixel_x;
        result.config.trace_y = state.selected_pixel_y;
    }
    if (state.camera_transition) result.config.sampling = SamplingMode::Single;
    return result;
}

RenderFrame render_studio_frame(const StudioState& state, const StudioRenderInput& input) {
    if (state.compare_modes) {
        return render_comparison_scene(input.project, input.snapshot, input.config,
                                       state.compare_left, state.compare_right);
    }
    return render_scene(input.project, input.snapshot, input.config);
}

void draw_studio(StudioState& state, ImTextureID texture, const StudioRenderInput& input,
                 const RenderFrame& frame,
                 bool flip_image_y) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Planet Studio", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted("PLANET STUDIO");
    ImGui::SameLine();
    ImGui::SetCursorPosX(210.0f);
    ImGui::BeginDisabled(state.camera_transition.has_value());
    if (state.demos.active() == DemoPreset::Collision) {
        ImGui::TextDisabled("COLLISION DEMO");
        ImGui::SameLine();
        if (ImGui::Button("Exit demo")) exit_studio_preset(state);
        ImGui::SameLine();
        if (ImGui::Button(state.demos.playing() ? "Pause" : "Play")) {
            state.demos.set_playing(!state.demos.playing());
        }
        ImGui::SameLine();
        if (ImGui::Button("Step")) state.demos.step(0.025);
        ImGui::SameLine();
        if (ImGui::Button("Reset")) state.demos.reset();
        ImGui::SameLine();
        float speed = static_cast<float>(state.session.speed());
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::SliderFloat("Speed", &speed, 0.1f, 20.0f, "%.1fx")) state.session.set_speed(speed);
        ImGui::SameLine();
        ImGui::Text("t=%.3f", input.snapshot.time);
    } else if (state.session.mode() == SessionMode::Design) {
        ImGui::TextDisabled("DESIGN");
        ImGui::SameLine();
        if (ImGui::Button("Run simulation")) begin_studio_simulation(state);
    } else {
        if (ImGui::Button("Design")) return_studio_design(state);
        ImGui::SameLine();
        if (ImGui::Button(state.session.playing() ? "Pause" : "Play")) {
            state.session.set_playing(!state.session.playing());
        }
        ImGui::SameLine();
        if (ImGui::Button("Step")) {
            state.session.step(0.025);
            state.timeline_horizon = std::max(state.timeline_horizon,
                                               std::ceil(state.session.snapshot().time));
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            state.session.reset_simulation();
            state.timeline_horizon = 80.0;
        }
        ImGui::SameLine();
        float speed = static_cast<float>(state.session.speed());
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::SliderFloat("Speed", &speed, 0.1f, 20.0f, "%.1fx")) state.session.set_speed(speed);
        ImGui::SameLine();
        ImGui::Text("t=%.3f", input.snapshot.time);
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    const float total = ImGui::GetContentRegionAvail().x;
    const float sidebar = std::clamp(total * 0.19f, 190.0f, 235.0f);
    const float inspector = std::clamp(total * 0.25f, 275.0f, 335.0f);
    draw_sidebar(state, sidebar);
    ImGui::SameLine();
    draw_scene(state, texture, input, frame, flip_image_y,
               std::max(300.0f, total - sidebar - inspector - 16.0f));
    ImGui::SameLine();
    draw_inspector(state, frame, inspector);
    ImGui::End();
}

} // namespace planet_sandbox
