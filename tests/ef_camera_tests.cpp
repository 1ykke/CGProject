#include "cg/ef_camera.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

int main() {
    using namespace cg;
    CameraController camera;
    const Vec3 center = screen_pixel_ray(camera, 319, 179, 640, 360, 1.0471975512);
    const Vec3 expected = normalized(camera.camera.target - camera.eye());
    CHECK(length(center - expected) < 0.01);

    const Vec3 left = screen_pixel_ray(camera, 0, 179, 640, 360, 1.0471975512);
    const Vec3 right = screen_pixel_ray(camera, 639, 179, 640, 360, 1.0471975512);
    CHECK(left.x < right.x);

    bool threw = false;
    try { (void)screen_pixel_ray(camera, -1, 0, 640, 360, 1.0); }
    catch (const std::out_of_range&) { threw = true; }
    CHECK(threw);

    threw = false;
    try { (void)screen_pixel_ray(camera, 0, 0, 0, 360, 1.0); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    threw = false;
    try { (void)screen_pixel_ray(camera, 0, 0, 640, 360, 0.0); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    std::cout << "E camera ray tests passed.\\n";
}
