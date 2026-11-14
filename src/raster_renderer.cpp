#include "planet_sandbox/renderer.h"
#include "planet_sandbox/sculpt.h"
#include "planet_sandbox/texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace planet_sandbox {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double half_fov_tangent = 0.41421356237309503;
constexpr double near_plane = 0.05;

int sampling_count(SamplingMode mode) {
    return mode == SamplingMode::Single ? 1 : mode == SamplingMode::SSAA16 ? 16 : 4;
}

Vec2 sample_offset(int sample_count, int sample) {
    if (sample_count == 1) return {0.5, 0.5};
    const int side = sample_count == 16 ? 4 : 2;
    return {(sample % side + 0.5) / side, (sample / side + 0.5) / side};
}

TraceSurface geometry_surface(TraceSurface surface) {
    return surface == TraceSurface::Cloud ? TraceSurface::Body : surface;
}

struct Vec3 { double x, y, z; };
struct Color { double r, g, b; };
struct Vertex { Vec3 position, normal; double u, v; };
struct ScreenVertex { double x, y, depth, u, v; Vec3 normal, position; };
struct Sample {
    Color color;
    double depth = std::numeric_limits<double>::infinity();
    int body = -1;
    Vec2 surface_uv{-1.0, -1.0};
    TraceSurface surface = TraceSurface::Background;
};
struct CloudBase {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    std::uint32_t layer = 0;
};

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, double scale) { return {a.x * scale, a.y * scale, a.z * scale}; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize(Vec3 value) {
    const double length = std::sqrt(dot(value, value));
    return length > 0.0 ? value * (1.0 / length) : Vec3{0.0, 0.0, 1.0};
}
Color mix(Color a, Color b, double fraction) {
    return {a.r + (b.r - a.r) * fraction, a.g + (b.g - a.g) * fraction,
            a.b + (b.b - a.b) * fraction};
}
Color scale(Color a, double amount) { return {a.r * amount, a.g * amount, a.b * amount}; }

Color warm_tint(BodyKind kind) {
    switch (kind) {
    case BodyKind::Sun: return {0.92, 0.35, 0.04};
    case BodyKind::Mercury: return {0.48, 0.38, 0.30};
    case BodyKind::Venus: return {0.78, 0.55, 0.24};
    case BodyKind::Earth: return {0.20, 0.62, 0.44};
    case BodyKind::Mars: return {0.78, 0.31, 0.12};
    case BodyKind::Jupiter: return {0.66, 0.37, 0.17};
    case BodyKind::Saturn: return {0.72, 0.55, 0.30};
    case BodyKind::Uranus: return {0.18, 0.67, 0.62};
    case BodyKind::Neptune: return {0.10, 0.34, 0.82};
    default: return {0.5, 0.5, 0.5};
    }
}

int hex_digit(char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    return digit - 'A' + 10;
}

Color parse_color(const std::string& hex) {
    const auto channel = [&](int offset) {
        const double srgb = (hex_digit(hex[offset]) * 16 + hex_digit(hex[offset + 1])) / 255.0;
        return std::pow(srgb, 2.2);
    };
    return {channel(1), channel(3), channel(5)};
}

std::uint8_t to_byte(double linear) {
    return static_cast<std::uint8_t>(std::round(std::pow(std::clamp(linear, 0.0, 1.0), 1.0 / 2.2) * 255.0));
}

double smoothstep(double low, double high, double value) {
    const double t = std::clamp((value - low) / (high - low), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double hash(int x, int y, std::uint32_t seed) {
    std::uint32_t value = static_cast<std::uint32_t>(x) * 374761393u +
                          static_cast<std::uint32_t>(y) * 668265263u + seed * 2246822519u;
    value = (value ^ (value >> 13)) * 1274126177u;
    return static_cast<double>(value ^ (value >> 16)) / 4294967295.0;
}

double noise(double x, double y, std::uint32_t seed) {
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const double tx = smoothstep(0.0, 1.0, x - ix);
    const double ty = smoothstep(0.0, 1.0, y - iy);
    const double a = hash(ix, iy, seed) * (1.0 - tx) + hash(ix + 1, iy, seed) * tx;
    const double b = hash(ix, iy + 1, seed) * (1.0 - tx) + hash(ix + 1, iy + 1, seed) * tx;
    return a * (1.0 - ty) + b * ty;
}

struct Material {
    const Appearance& appearance;
    Color primary;
    Color secondary;
    bool emissive;
    BodyKind kind = BodyKind::Custom;
    const DesignOverrides* design = nullptr;
    const Texture* color_map = nullptr;
    const Texture* cloud_map = nullptr;
    const Texture* ring_map = nullptr;
    bool cloud_pass = false;
    bool venus_surface_pass = false;
    bool venus_cloud_pass = false;
    double time = 0.0;
};

Color surface_color(const Material& material, double u, double v, Vec3 normal) {
    const auto& a = material.appearance;
    if (material.color_map) {
        const auto texel = sample_texture(*material.color_map, u, v);
        Color color{std::pow(texel[0], 2.2), std::pow(texel[1], 2.2),
                    std::pow(texel[2], 2.2)};
        const DesignOverrides& design = *material.design;
        if (material.kind == BodyKind::Sun) {
            const double luma = (color.r + color.g + color.b) / 3.0;
            color = mix({0.65, 0.30, 0.055}, color, design.granulation);
            const double grains = noise(u * 150.0 * design.granule_scale,
                                        v * 75.0 * design.granule_scale, 19u);
            color = scale(color, 0.93 + 0.14 * grains);
            const double spot = 1.0 - smoothstep(0.10, 0.38, luma);
            color = scale(color, 1.0 - spot * (design.sunspot_strength - 1.0) * 0.25);
        }
        if (material.kind == BodyKind::Mercury) {
            color = mix({0.31, 0.30, 0.29}, color, design.band_contrast);
        }
        if (material.kind == BodyKind::Jupiter || material.kind == BodyKind::Saturn ||
            material.kind == BodyKind::Uranus || material.kind == BodyKind::Neptune) {
            color = mix({0.34, 0.32, 0.30}, color, design.band_contrast);
        }
        const double wrap_distance = std::min(std::abs(u - 0.37), 1.0 - std::abs(u - 0.37));
        if (material.kind == BodyKind::Jupiter) {
            const double spot = std::exp(-0.5 * (std::pow(wrap_distance / 0.05, 2) +
                                                 std::pow((v - 0.61) / 0.045, 2)));
            const Color target = design.storm_scale < 1.0 ? Color{0.42, 0.35, 0.27} :
                                                            Color{0.67, 0.26, 0.095};
            color = mix(color, target,
                        std::clamp(std::abs(design.storm_scale - 1.0) * spot * 0.50, 0.0, 0.8));
        }
        if (material.kind == BodyKind::Neptune) {
            const double du = std::min(std::abs(u - 0.19), 1.0 - std::abs(u - 0.19));
            const double storm = std::exp(-0.5 * (std::pow(du / 0.075, 2) +
                                                  std::pow((v - 0.16) / 0.055, 2)));
            const Color target = design.storm_scale < 1.0 ? Color{0.09, 0.16, 0.45} :
                                                            Color{0.025, 0.035, 0.16};
            color = mix(color, target,
                        std::clamp(std::abs(design.storm_scale - 1.0) * storm * 0.55, 0.0, 0.8));
        }
        if (design.color_shift > 0.0) {
            color = mix(color, warm_tint(material.kind), design.color_shift * 0.42);
        } else if (design.color_shift < 0.0) {
            color = mix(color, {0.18, 0.36, 0.72}, -design.color_shift * 0.42);
        }
        if (material.kind == BodyKind::Mars) {
            color = mix(color, {0.68, 0.30, 0.13}, design.dust_opacity * 0.78);
        }
        if (material.kind == BodyKind::Earth && design.ice_extent > 0.0) {
            const double polar = std::abs(0.5 - v) * 2.0;
            color = mix(color, {0.82, 0.88, 0.91},
                        design.ice_extent * smoothstep(0.72, 0.96, polar));
        }
        const Vec3 light = normalize({-0.4, 0.55, 0.75});
        const double diffuse = std::max(0.0, dot(normal, light));
        return scale(color, material.emissive ? 0.90 + 0.10 * diffuse : 0.24 + 0.76 * diffuse);
    }
    const double latitude = 0.5 - v;
    const double terrain = noise(u * 12.0 + 20.0, v * 12.0 + 20.0, a.seed);
    const double detail = noise(u * 27.0 + 50.0, v * 27.0 + 50.0, a.seed + 17u);
    const double warp = (terrain - 0.5) * a.terrain * 2.3;
    const double bands = 0.5 + 0.5 * std::sin((latitude + warp) * a.bands * 2.0 * pi);
    Color color = mix(material.primary, material.secondary, smoothstep(0.25, 0.75, bands));
    color = mix(color, {0.72, 0.82, 0.8}, a.clouds * smoothstep(0.63, 0.84, detail));
    const Vec3 light = normalize({-0.4, 0.55, 0.75});
    const double diffuse = std::max(0.0, dot(normal, light));
    return scale(color, material.emissive ? 0.92 + 0.08 * diffuse : 0.22 + 0.78 * diffuse);
}

double edge(double ax, double ay, double bx, double by, double px, double py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

enum class FragmentBlend { Opaque, Alpha, Cloud };

struct Fragment {
    Color color{};
    double alpha = 1.0;
    FragmentBlend blend = FragmentBlend::Opaque;
};

Fragment shade_fragment(const Material& material, double u, double v, Vec3 normal, bool ring) {
    if (material.venus_cloud_pass) {
        return {surface_color(material, u, v, normal), material.design->cloud_opacity,
                FragmentBlend::Cloud};
    }
    if (material.cloud_pass) {
        const auto cloud = sample_texture(*material.cloud_map,
                                          u + material.design->cloud_motion * material.time * 0.006, v);
        const double mask = (cloud[0] + cloud[1] + cloud[2]) / 3.0;
        const double alpha = material.design->cloud_opacity * mask * 0.9;
        return {{0.78, 0.85, 0.92}, alpha < 0.01 ? 0.0 : alpha, FragmentBlend::Cloud};
    }
    if (ring && material.ring_map) {
        const auto texel = sample_texture(*material.ring_map, u, v, false);
        const double gap_distance = std::abs(u - 0.68);
        const double gap = smoothstep(material.design->cassini_gap * 0.008,
                                      material.design->cassini_gap * 0.013 + 0.001,
                                      gap_distance);
        const double alpha = texel[3] * gap *
                             std::clamp(material.design->ring_width, 0.0, 1.0);
        return {{std::pow(texel[0], 2.2), std::pow(texel[1], 2.2),
                 std::pow(texel[2], 2.2)}, alpha < 0.03 ? 0.0 : alpha,
                FragmentBlend::Alpha};
    }
    if (ring && material.kind == BodyKind::Uranus) {
        const double alpha = material.design->ring_width *
                             (0.14 + 0.06 * std::sin(u * 8.0 * pi));
        return {{0.14, 0.32, 0.36}, alpha < 0.01 ? 0.0 : alpha, FragmentBlend::Alpha};
    }
    const double stripe = 0.5 + 0.5 * std::sin(u * 26.0 * pi);
    return {ring ? scale(mix(material.secondary, material.primary, stripe), 0.72)
                 : surface_color(material, u, v, normal), 1.0, FragmentBlend::Opaque};
}

class Rasterizer {
public:
    Rasterizer(const Project& project, const RenderConfig& config)
        : config_(config), sample_count_(sampling_count(config.sampling)),
          samples_(static_cast<std::size_t>(config.width) * config.height * sample_count_) {
        const double yaw = config.camera.yaw, pitch = config.camera.pitch;
        view_direction_ = {std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                           std::cos(yaw) * std::cos(pitch)};
        right_ = {std::cos(yaw), 0.0, -std::sin(yaw)};
        up_ = {-std::sin(yaw) * std::sin(pitch), std::cos(pitch),
               -std::cos(yaw) * std::sin(pitch)};
        distance_ = project.view_extent * 1.35 / half_fov_tangent * config.camera.zoom;
        camera_position_ = Vec3{config.camera.target.x, config.camera.target.y, 0.0} +
                           view_direction_ * distance_;
        image_scale_ = config.height / (2.0 * half_fov_tangent);
        for (int y = 0; y < config.height; ++y) {
            for (int x = 0; x < config.width; ++x) {
                const double vx = (x + 0.5 - config.width * 0.5) / config.width;
                const double vy = (y + 0.5 - config.height * 0.5) / config.height;
                const double vignette = 1.0 - 0.38 * std::min(1.0, vx * vx + vy * vy);
                Color background = scale({0.0036, 0.0054, 0.008}, vignette);
                if (hash(x, y, 97u) > 0.99988) background = {0.26, 0.29, 0.34};
                const std::size_t pixel = static_cast<std::size_t>(y) * config.width + x;
                for (int sample = 0; sample < sample_count_; ++sample) {
                    samples_[pixel * sample_count_ + sample].color = background;
                }
            }
        }
    }

    double camera_depth(Vec3 world) const {
        const Vec3 relative = world - Vec3{config_.camera.target.x, config_.camera.target.y, 0.0};
        return distance_ - dot(relative, view_direction_);
    }

    bool project(Vec3 world, ScreenVertex& screen) const {
        const Vec3 relative = world - Vec3{config_.camera.target.x, config_.camera.target.y, 0.0};
        const double depth = camera_depth(world);
        if (depth < near_plane - 1e-9 || !std::isfinite(depth)) return false;
        const double projected_depth = std::max(depth, near_plane);
        screen.x = config_.width * 0.5 + dot(relative, right_) * image_scale_ / projected_depth;
        screen.y = config_.height * 0.5 - dot(relative, up_) * image_scale_ / projected_depth;
        screen.depth = projected_depth;
        return std::isfinite(screen.x) && std::isfinite(screen.y);
    }

    void begin_cloud_layer() {
        if (cloud_bases_.empty()) cloud_bases_.resize(samples_.size());
        ++cloud_layer_;
    }

    void triangle(const std::array<Vertex, 3>& vertices, const Material& material,
                  int body_index, bool ring) {
        std::array<double, 3> depths{};
        int inside_count = 0;
        for (int i = 0; i < 3; ++i) {
            depths[i] = camera_depth(vertices[i].position);
            if (!std::isfinite(depths[i])) return;
            inside_count += depths[i] >= near_plane;
        }
        if (inside_count == 0) return;
        if (inside_count == 3) {
            rasterize_triangle(vertices, material, body_index, ring);
            return;
        }
        std::array<Vertex, 4> clipped{};
        int count = 0;
        for (int i = 0; i < 3; ++i) {
            const int previous = (i + 2) % 3;
            const bool current_inside = depths[i] >= near_plane;
            const bool previous_inside = depths[previous] >= near_plane;
            if (current_inside != previous_inside) {
                const double t = std::clamp((near_plane - depths[previous]) /
                                                (depths[i] - depths[previous]), 0.0, 1.0);
                const Vertex& a = vertices[previous];
                const Vertex& b = vertices[i];
                clipped[count++] = {a.position + (b.position - a.position) * t,
                                    a.normal + (b.normal - a.normal) * t,
                                    a.u + (b.u - a.u) * t,
                                    a.v + (b.v - a.v) * t};
            }
            if (current_inside) clipped[count++] = vertices[i];
        }
        for (int i = 1; i + 1 < count; ++i) {
            rasterize_triangle({clipped[0], clipped[i], clipped[i + 1]},
                               material, body_index, ring);
        }
    }

private:
    void rasterize_triangle(const std::array<Vertex, 3>& vertices, const Material& material,
                            int body_index, bool ring) {
        std::array<ScreenVertex, 3> p{};
        for (int i = 0; i < 3; ++i) {
            if (!project(vertices[i].position, p[i])) return;
            p[i].normal = vertices[i].normal;
            p[i].position = vertices[i].position;
            p[i].u = vertices[i].u;
            p[i].v = vertices[i].v;
        }
        const double area = edge(p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y);
        if (!std::isfinite(area) || std::abs(area) < 1e-10) return;
        const double left = std::min({p[0].x, p[1].x, p[2].x});
        const double right = std::max({p[0].x, p[1].x, p[2].x});
        const double top = std::min({p[0].y, p[1].y, p[2].y});
        const double bottom = std::max({p[0].y, p[1].y, p[2].y});
        if (right < 0.0 || bottom < 0.0 || left >= config_.width || top >= config_.height) return;
        const int min_x = static_cast<int>(std::floor(std::clamp(left, 0.0, double(config_.width - 1))));
        const int max_x = static_cast<int>(std::ceil(std::clamp(right, 0.0, double(config_.width - 1))));
        const int min_y = static_cast<int>(std::floor(std::clamp(top, 0.0, double(config_.height - 1))));
        const int max_y = static_cast<int>(std::ceil(std::clamp(bottom, 0.0, double(config_.height - 1))));
        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                Fragment cached_fragment;
                bool cached = false;
                for (int s = 0; s < sample_count_; ++s) {
                    const Vec2 offset = sample_offset(sample_count_, s);
                    const double sx = x + offset.x;
                    const double sy = y + offset.y;
                    const double w0 = edge(p[1].x, p[1].y, p[2].x, p[2].y, sx, sy) / area;
                    const double w1 = edge(p[2].x, p[2].y, p[0].x, p[0].y, sx, sy) / area;
                    const double w2 = 1.0 - w0 - w1;
                    if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
                    const double inverse_depth = w0 / p[0].depth + w1 / p[1].depth + w2 / p[2].depth;
                    const double depth = 1.0 / inverse_depth;
                    const std::size_t sample_index =
                        (static_cast<std::size_t>(y) * config_.width + x) * sample_count_ + s;
                    Sample& sample = samples_[sample_index];
                    if (depth >= sample.depth) continue;
                    const auto interpolate = [&](double a, double b, double c) {
                        return (w0 * a / p[0].depth + w1 * b / p[1].depth +
                                w2 * c / p[2].depth) * depth;
                    };
                    const double u = interpolate(p[0].u, p[1].u, p[2].u);
                    const double v = interpolate(p[0].v, p[1].v, p[2].v);
                    const Vec3 normal = normalize({interpolate(p[0].normal.x, p[1].normal.x, p[2].normal.x),
                                                   interpolate(p[0].normal.y, p[1].normal.y, p[2].normal.y),
                                                   interpolate(p[0].normal.z, p[1].normal.z, p[2].normal.z)});
                    if (material.cloud_pass || material.venus_cloud_pass) {
                        const Vec3 world{interpolate(p[0].position.x, p[1].position.x, p[2].position.x),
                                         interpolate(p[0].position.y, p[1].position.y, p[2].position.y),
                                         interpolate(p[0].position.z, p[1].position.z, p[2].position.z)};
                        if (dot(normal, camera_position_ - world) <= 0.0) continue;
                    }
                    if (!cached || config_.sampling != SamplingMode::MSAA4) {
                        cached_fragment = shade_fragment(material, u, v, normal, ring);
                        ++shade_evaluations_;
                        cached = true;
                    }
                    if (cached_fragment.alpha <= 0.0) continue;
                    if (cached_fragment.blend == FragmentBlend::Cloud) {
                        blend_cloud(sample_index, sample, cached_fragment.color, cached_fragment.alpha);
                    } else if (cached_fragment.blend == FragmentBlend::Alpha) {
                        sample.color = mix(sample.color, cached_fragment.color, cached_fragment.alpha);
                    } else {
                        sample.color = cached_fragment.color;
                    }
                    sample.depth = depth;
                    sample.body = body_index;
                    sample.surface_uv = ring ? Vec2{-1.0, -1.0} : Vec2{u, v};
                    sample.surface = ring ? TraceSurface::Ring :
                        (material.cloud_pass || material.venus_cloud_pass)
                            ? TraceSurface::Cloud : TraceSurface::Body;
                }
            }
        }
    }

public:
    void trail(const BodyTrail& trail, Color color, int body_index,
               TraceSurface surface = TraceSurface::Trail) {
        for (std::size_t segment = 1; segment < trail.points.size(); ++segment) {
            const double plane = surface == TraceSurface::ReferenceTrail ? -0.07 : -0.05;
            Vec3 world_a{trail.points[segment - 1].x, trail.points[segment - 1].y, plane};
            Vec3 world_b{trail.points[segment].x, trail.points[segment].y, plane};
            const double depth_a = camera_depth(world_a);
            const double depth_b = camera_depth(world_b);
            if (!std::isfinite(depth_a) || !std::isfinite(depth_b) ||
                (depth_a < near_plane && depth_b < near_plane)) continue;
            if ((depth_a < near_plane) != (depth_b < near_plane)) {
                const double t = std::clamp((near_plane - depth_a) / (depth_b - depth_a),
                                            0.0, 1.0);
                const Vec3 intersection = world_a + (world_b - world_a) * t;
                if (depth_a < near_plane) world_a = intersection;
                else world_b = intersection;
            }
            ScreenVertex a{}, b{};
            if (!project(world_a, a) || !project(world_b, b)) continue;
            const double dx = b.x - a.x, dy = b.y - a.y;
            double start = 0.0, end = 1.0;
            const auto clip = [&](double p, double q) {
                if (p == 0.0) return q >= 0.0;
                const double t = q / p;
                if (p < 0.0) start = std::max(start, t);
                else end = std::min(end, t);
                return start <= end;
            };
            if (!clip(-dx, a.x + 1.0) || !clip(dx, config_.width + 1.0 - a.x) ||
                !clip(-dy, a.y + 1.0) || !clip(dy, config_.height + 1.0 - a.y)) continue;

            const double x0 = a.x + dx * start, x1 = a.x + dx * end;
            const double y0 = a.y + dy * start, y1 = a.y + dy * end;
            const int min_x = static_cast<int>(std::floor(std::clamp(std::min(x0, x1) - 1.0,
                                                                      0.0, double(config_.width - 1))));
            const int max_x = static_cast<int>(std::ceil(std::clamp(std::max(x0, x1) + 1.0,
                                                                     0.0, double(config_.width - 1))));
            const int min_y = static_cast<int>(std::floor(std::clamp(std::min(y0, y1) - 1.0,
                                                                      0.0, double(config_.height - 1))));
            const int max_y = static_cast<int>(std::ceil(std::clamp(std::max(y0, y1) + 1.0,
                                                                     0.0, double(config_.height - 1))));
            const double length_squared = dx * dx + dy * dy;
            for (int y = min_y; y <= max_y; ++y) {
                for (int x = min_x; x <= max_x; ++x) {
                    const std::size_t pixel = static_cast<std::size_t>(y) * config_.width + x;
                    for (int s = 0; s < sample_count_; ++s) {
                        const Vec2 offset = sample_offset(sample_count_, s);
                        const double sx = x + offset.x;
                        const double sy = y + offset.y;
                        const double t = length_squared > 0.0
                            ? std::clamp(((sx - a.x) * dx + (sy - a.y) * dy) / length_squared, start, end)
                            : start;
                        const double ox = sx - (a.x + dx * t), oy = sy - (a.y + dy * t);
                        if (ox * ox + oy * oy > 0.36) continue;
                        const double depth = 1.0 / ((1.0 - t) / a.depth + t / b.depth);
                        Sample& sample = samples_[pixel * sample_count_ + s];
                        if (depth >= sample.depth) continue;
                        sample.color = mix(sample.color, color,
                                           surface == TraceSurface::ReferenceTrail ? 0.5 : 0.65);
                        sample.depth = depth;
                        sample.body = body_index;
                        sample.surface_uv = {-1.0, -1.0};
                        sample.surface = surface;
                    }
                }
            }
        }
    }

    RenderFrame resolve() const {
        RenderFrame frame;
        frame.shade_evaluations = shade_evaluations_;
        frame.image = {config_.width, config_.height,
                       std::vector<std::uint8_t>(static_cast<std::size_t>(config_.width) * config_.height * 3)};
        frame.body_index.resize(static_cast<std::size_t>(config_.width) * config_.height, -1);
        frame.surface_uv.resize(frame.body_index.size(), {-1.0, -1.0});
        frame.sample_surface_variation.resize(frame.body_index.size(), 0);
        for (std::size_t pixel = 0; pixel < frame.body_index.size(); ++pixel) {
            Color resolved{0.0, 0.0, 0.0};
            double nearest = std::numeric_limits<double>::infinity();
            const Sample& first = samples_[pixel * sample_count_];
            for (int s = 0; s < sample_count_; ++s) {
                const Sample& sample = samples_[pixel * sample_count_ + s];
                resolved = mix(resolved, sample.color, 1.0 / (s + 1));
                if (sample.body != first.body ||
                    geometry_surface(sample.surface) != geometry_surface(first.surface)) {
                    frame.sample_surface_variation[pixel] = 1;
                }
                if (sample.depth < nearest) {
                    nearest = sample.depth;
                    frame.body_index[pixel] = sample.body;
                    frame.surface_uv[pixel] = sample.surface_uv;
                }
            }
            frame.image.rgb[pixel * 3] = to_byte(resolved.r);
            frame.image.rgb[pixel * 3 + 1] = to_byte(resolved.g);
            frame.image.rgb[pixel * 3 + 2] = to_byte(resolved.b);
        }
        if (config_.trace_x >= 0 && config_.trace_y >= 0) {
            PixelTrace trace;
            trace.x = config_.trace_x;
            trace.y = config_.trace_y;
            const std::size_t pixel = static_cast<std::size_t>(trace.y) * config_.width + trace.x;
            for (int s = 0; s < sample_count_; ++s) {
                const Sample& sample = samples_[pixel * sample_count_ + s];
                trace.samples.push_back({sample_offset(sample_count_, s),
                                         sample.body, sample.depth,
                                         {sample.color.r, sample.color.g, sample.color.b},
                                         sample.surface});
            }
            trace.display_rgb = {frame.image.rgb[pixel * 3], frame.image.rgb[pixel * 3 + 1],
                                 frame.image.rgb[pixel * 3 + 2]};
            frame.trace = std::move(trace);
        }
        return frame;
    }

private:
    void blend_cloud(std::size_t index, Sample& sample, Color color, double alpha) {
        CloudBase& base = cloud_bases_[index];
        if (base.layer != cloud_layer_) {
            base = {static_cast<float>(sample.color.r), static_cast<float>(sample.color.g),
                    static_cast<float>(sample.color.b), cloud_layer_};
        }
        sample.color = mix({base.r, base.g, base.b}, color, alpha);
    }

    const RenderConfig& config_;
    int sample_count_;
    std::vector<Sample> samples_;
    std::vector<CloudBase> cloud_bases_;
    std::uint32_t cloud_layer_ = 0;
    std::uint64_t shade_evaluations_ = 0;
    Vec3 view_direction_{}, right_{}, up_{};
    Vec3 camera_position_{};
    double distance_ = 0.0;
    double image_scale_ = 0.0;
};

void draw_sphere(Rasterizer& rasterizer, const BodyState& body, const Material& material,
                 int body_index, double scale_factor, double time) {
    const int latitude_steps = material.color_map ? 48 : 16;
    const int longitude_steps = material.color_map ? 96 : 32;
    const double spin = time * 0.45 +
                        (material.kind == BodyKind::Custom ? material.appearance.seed * 0.017 : 0.0);
    const double c = std::cos(spin), s = std::sin(spin);
    const double tilt = material.kind == BodyKind::Uranus
        ? (98.0 + material.design->axis_tilt_offset) * pi / 180.0 : 0.0;
    const double tilt_c = std::cos(tilt), tilt_s = std::sin(tilt);
    const double radius = body.radius * scale_factor;
    const Vec3 center{body.position.x, body.position.y, 0.0};
    std::vector<Vertex> grid((latitude_steps + 1) * (longitude_steps + 1));
    for (int row = 0; row <= latitude_steps; ++row) {
        const double theta = pi * row / latitude_steps;
        for (int col = 0; col <= longitude_steps; ++col) {
            const double phi = 2.0 * pi * col / longitude_steps;
            const Vec3 local{std::sin(theta) * std::cos(phi), std::cos(theta),
                             std::sin(theta) * std::sin(phi)};
            const Vec3 spun{local.x * c + local.z * s, local.y,
                            -local.x * s + local.z * c};
            const Vec3 tilted{spun.x * tilt_c - spun.y * tilt_s,
                              spun.x * tilt_s + spun.y * tilt_c, spun.z};
            const Vec3 normal = material.kind == BodyKind::Uranus
                ? Vec3{tilted.x * std::cos(0.35) + tilted.z * std::sin(0.35),
                       tilted.y,
                       -tilted.x * std::sin(0.35) + tilted.z * std::cos(0.35)}
                : tilted;
            double relief = 1.0;
            if (material.color_map && !material.cloud_pass) {
                relief = solar_shape_multiplier(
                    material.kind, static_cast<double>(col) / longitude_steps,
                    static_cast<double>(row) / latitude_steps, *material.design,
                    material.venus_surface_pass ? SolarShapeLayer::VenusRadar :
                                                  SolarShapeLayer::Visible);
            }
            if (material.cloud_pass) {
                relief *= 1.018;
                relief += sculpt_offset(*material.design, SolarShapeLayer::Visible,
                                        static_cast<double>(col) / longitude_steps,
                                        static_cast<double>(row) / latitude_steps);
            }
            grid[row * (longitude_steps + 1) + col] =
                {center + normal * (radius * relief), normal,
                 static_cast<double>(col) / longitude_steps,
                 static_cast<double>(row) / latitude_steps};
        }
    }
    if (material.color_map && !material.cloud_pass) {
        for (int row = 1; row < latitude_steps; ++row) {
            for (int col = 0; col <= longitude_steps; ++col) {
                const int left = col == 0 ? longitude_steps - 1 : col - 1;
                const int right = col == longitude_steps ? 1 : col + 1;
                const int at = row * (longitude_steps + 1) + col;
                const Vec3 along = grid[row * (longitude_steps + 1) + right].position -
                                   grid[row * (longitude_steps + 1) + left].position;
                const Vec3 down = grid[(row + 1) * (longitude_steps + 1) + col].position -
                                  grid[(row - 1) * (longitude_steps + 1) + col].position;
                grid[at].normal = normalize(cross(along, down));
            }
        }
    }
    for (int row = 0; row < latitude_steps; ++row) {
        for (int col = 0; col < longitude_steps; ++col) {
            const int first = row * (longitude_steps + 1) + col;
            const int second = first + longitude_steps + 1;
            rasterizer.triangle({grid[first], grid[second], grid[second + 1]}, material, body_index, false);
            rasterizer.triangle({grid[first], grid[second + 1], grid[first + 1]}, material, body_index, false);
        }
    }
}

void draw_ring(Rasterizer& rasterizer, const BodyState& body, const Material& material,
               int body_index, double scale_factor) {
    const double ring_width = material.ring_map || material.kind == BodyKind::Uranus
        ? material.design->ring_width : material.appearance.ring_width;
    if (ring_width <= 0.0) return;
    constexpr int segments = 72;
    const double radius = body.radius * scale_factor;
    const double inner = radius * (material.kind == BodyKind::Uranus ? 1.32 : 1.18);
    const double outer = inner + radius * (material.kind == BodyKind::Uranus ? 0.12 :
                                           material.ring_map ? 1.25 : ring_width);
    const double ring_tilt = material.ring_map ? material.design->ring_tilt_offset * pi / 180.0 : 0.0;
    const Vec3 center{body.position.x, body.position.y, 0.0};
    const auto vertex = [&](double angle, double distance, double radial) {
        if (material.kind == BodyKind::Uranus) {
            const double tilt = (98.0 + material.design->axis_tilt_offset) * pi / 180.0;
            const Vec3 local{std::cos(angle) * distance, 0.0,
                             std::sin(angle) * distance};
            const Vec3 tipped{local.x * std::cos(tilt), local.x * std::sin(tilt), local.z};
            return Vertex{center + Vec3{tipped.x * std::cos(0.35) + tipped.z * std::sin(0.35),
                                        tipped.y,
                                        -tipped.x * std::sin(0.35) + tipped.z * std::cos(0.35)},
                          {-std::sin(tilt), std::cos(tilt), 0.0}, radial, angle / (2.0 * pi)};
        }
        return Vertex{center + Vec3{std::cos(angle) * distance,
                                    std::sin(angle) * distance * (0.46 * std::cos(ring_tilt) -
                                                                  0.89 * std::sin(ring_tilt)),
                                    std::sin(angle) * distance * (0.46 * std::sin(ring_tilt) +
                                                                  0.89 * std::cos(ring_tilt))},
                      {0.0, 0.89, -0.46}, radial, angle / (2.0 * pi)};
    };
    for (int segment = 0; segment < segments; ++segment) {
        const double a = 2.0 * pi * segment / segments;
        const double b = 2.0 * pi * (segment + 1) / segments;
        const Vertex ai = vertex(a, inner, 0.0), ao = vertex(a, outer, 1.0);
        const Vertex bi = vertex(b, inner, 0.0), bo = vertex(b, outer, 1.0);
        rasterizer.triangle({ai, ao, bo}, material, body_index, true);
        rasterizer.triangle({ai, bo, bi}, material, body_index, true);
    }
}

} // namespace

RenderFrame render_scene(const Project& project, const SceneSnapshot& snapshot,
                         const RenderConfig& config) {
    validate_project(project);
    if (config.width <= 0 || config.height <= 0 || config.width > 8192 || config.height > 8192 ||
        !std::isfinite(config.camera.yaw) || !std::isfinite(config.camera.pitch) ||
        !std::isfinite(config.camera.zoom) || config.camera.zoom <= 0.0 ||
        !std::isfinite(config.camera.target.x) || !std::isfinite(config.camera.target.y) ||
        (config.sampling != SamplingMode::Single && config.sampling != SamplingMode::SSAA4 &&
         config.sampling != SamplingMode::MSAA4 && config.sampling != SamplingMode::SSAA16) ||
        ((config.trace_x != -1 || config.trace_y != -1) &&
         (config.trace_x < 0 || config.trace_x >= config.width ||
          config.trace_y < 0 || config.trace_y >= config.height))) {
        throw std::invalid_argument("Invalid render configuration");
    }
    const std::size_t sample_count = static_cast<std::size_t>(sampling_count(config.sampling));
    if (static_cast<std::size_t>(config.width) * config.height * sample_count > 8000000u ||
        !std::isfinite(project.view_extent * config.camera.zoom * 1.35 / half_fov_tangent)) {
        throw std::invalid_argument("Render size or camera distance is too large");
    }
    Rasterizer rasterizer(project, config);
    struct CloudDraw {
        const BodyState* body;
        const BodySpec* spec;
        int index;
        double scale_factor;
        double view_position;
    };
    struct RingDraw {
        const BodyState* body;
        const BodySpec* spec;
        int index;
        double scale_factor;
        double view_position;
    };
    std::vector<CloudDraw> clouds;
    std::vector<RingDraw> rings;
    for (const auto& body : snapshot.bodies) {
        const auto found = std::find_if(project.bodies.begin(), project.bodies.end(),
                                        [&](const BodySpec& item) { return item.id == body.id; });
        if (found == project.bodies.end()) continue;
        const int index = static_cast<int>(found - project.bodies.begin());
        Material material{found->appearance, parse_color(found->appearance.primary),
                          parse_color(found->appearance.secondary), index == 0};
        material.time = snapshot.time;
        double scale_factor = project.display_scale;
        if (project.solar_system) {
            material.kind = found->kind;
            material.design = &found->design;
            if (found->kind == BodyKind::Venus) {
                material.color_map = &solar_texture(found->kind, AssetLayer::VenusSurface);
                material.venus_surface_pass = true;
            } else {
                material.color_map = &solar_texture(found->kind, AssetLayer::Color);
            }
            if (found->kind == BodyKind::Saturn) {
                material.ring_map = &solar_texture(found->kind, AssetLayer::Ring);
            }
            scale_factor = found->display_radius * project.display_scale / found->radius;
        }
        draw_sphere(rasterizer, body, material, index, scale_factor, snapshot.time);
        const double view_position = body.position.x * std::sin(config.camera.yaw) *
                                         std::cos(config.camera.pitch) +
                                     body.position.y * std::sin(config.camera.pitch);
        rings.push_back({&body, &*found, index, scale_factor, view_position});
        if (project.solar_system && found->design.cloud_opacity > 0.0 &&
            (found->kind == BodyKind::Venus || found->kind == BodyKind::Earth)) {
            clouds.push_back({&body, &*found, index, scale_factor, view_position});
        }
    }
    for (const auto& trail : snapshot.reference_trails) {
        const auto found = std::find_if(project.bodies.begin(), project.bodies.end(),
                                        [&](const BodySpec& body) { return body.id == trail.id; });
        if (found != project.bodies.end()) {
            rasterizer.trail(trail, {0.95, 0.64, 0.24},
                             static_cast<int>(found - project.bodies.begin()),
                             TraceSurface::ReferenceTrail);
        }
    }
    for (const auto& trail : snapshot.trails) {
        const auto found = std::find_if(project.bodies.begin(), project.bodies.end(),
                                        [&](const BodySpec& body) { return body.id == trail.id; });
        if (found != project.bodies.end()) {
            rasterizer.trail(trail, parse_color(found->appearance.primary),
                             static_cast<int>(found - project.bodies.begin()));
        }
    }
    std::stable_sort(rings.begin(), rings.end(),
                     [](const RingDraw& a, const RingDraw& b) {
                         return a.view_position < b.view_position;
                     });
    for (const RingDraw& ring : rings) {
        const BodySpec& spec = *ring.spec;
        Material material{spec.appearance, parse_color(spec.appearance.primary),
                          parse_color(spec.appearance.secondary), ring.index == 0};
        if (project.solar_system) {
            material.kind = spec.kind;
            material.design = &spec.design;
            if (spec.kind == BodyKind::Saturn) {
                material.ring_map = &solar_texture(spec.kind, AssetLayer::Ring);
            }
        }
        draw_ring(rasterizer, *ring.body, material, ring.index, ring.scale_factor);
    }
    std::stable_sort(clouds.begin(), clouds.end(),
                     [](const CloudDraw& a, const CloudDraw& b) {
                         return a.view_position < b.view_position;
                     });
    for (const CloudDraw& cloud : clouds) {
        const BodySpec& spec = *cloud.spec;
        Material material{spec.appearance, parse_color(spec.appearance.primary),
                          parse_color(spec.appearance.secondary), cloud.index == 0};
        material.kind = spec.kind;
        material.design = &spec.design;
        material.color_map = &solar_texture(spec.kind, AssetLayer::Color);
        material.time = snapshot.time;
        if (spec.kind == BodyKind::Venus) material.venus_cloud_pass = true;
        else {
            material.cloud_pass = true;
            material.cloud_map = &solar_texture(spec.kind, AssetLayer::Clouds);
        }
        rasterizer.begin_cloud_layer();
        draw_sphere(rasterizer, *cloud.body, material, cloud.index,
                    cloud.scale_factor, snapshot.time);
    }
    return rasterizer.resolve();
}

} // namespace planet_sandbox
