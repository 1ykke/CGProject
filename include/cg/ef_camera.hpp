#pragma once
#include "cg/ef.hpp"
#include <cmath>
#include <stdexcept>

namespace cg {

// Build a world-space picking ray from a top-left-origin framebuffer pixel.
// pixel_x/pixel_y are integer pixel coordinates; the ray passes through the
// pixel center. vertical_fov_radians is the vertical field of view.
inline Vec3 screen_pixel_ray(
    const CameraController& controller,
    int pixel_x,
    int pixel_y,
    int width,
    int height,
    double vertical_fov_radians) {
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("framebuffer dimensions must be positive");
    if (pixel_x < 0 || pixel_x >= width || pixel_y < 0 || pixel_y >= height)
        throw std::out_of_range("pixel is outside the framebuffer");
    if (!std::isfinite(vertical_fov_radians) ||
        vertical_fov_radians <= 0.0 ||
        vertical_fov_radians >= 3.14159265358979323846)
        throw std::invalid_argument("vertical FOV must be between 0 and pi");

    const Vec3 eye = controller.eye();
    const Vec3 forward = normalized(controller.camera.target - eye);
    const Vec3 world_up{0.0, 1.0, 0.0};
    Vec3 right{
        forward.y * world_up.z - forward.z * world_up.y,
        forward.z * world_up.x - forward.x * world_up.z,
        forward.x * world_up.y - forward.y * world_up.x
    };
    // Avoid a degenerate cross product when looking almost parallel to Y.
    if (length(right) < 1e-10) {
        right = {1.0, 0.0, 0.0};
    } else {
        right = normalized(right);
    }
    const Vec3 up{
        right.y * forward.z - right.z * forward.y,
        right.z * forward.x - right.x * forward.z,
        right.x * forward.y - right.y * forward.x
    };

    const double aspect = static_cast<double>(width) / height;
    const double half_height = std::tan(vertical_fov_radians * 0.5);
    const double ndc_x = 2.0 * (static_cast<double>(pixel_x) + 0.5) / width - 1.0;
    const double ndc_y = 1.0 - 2.0 * (static_cast<double>(pixel_y) + 0.5) / height;
    const Vec3 direction = forward
        + right * (ndc_x * aspect * half_height)
        + up * (ndc_y * half_height);
    return normalized(direction);
}

} // namespace cg
