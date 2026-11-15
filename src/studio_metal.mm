#include "planet_sandbox/renderer.h"
#include "planet_sandbox/studio_ui.h"
#include "planet_sandbox/solar_system.h"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_metal.h>

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void glfw_error(int code, const char* message) {
    std::fprintf(stderr, "GLFW %d: %s\n", code, message);
}

planet_sandbox::Project open_project(const char* path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error(std::string("Cannot open project: ") + path);
    return planet_sandbox::read_project(input);
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            const char* path = argc > 1 ? argv[1] : "my-solar-system.json";
            planet_sandbox::StudioState state(argc > 1 ? open_project(path) :
                                              planet_sandbox::make_solar_system_project(), path);

            glfwSetErrorCallback(glfw_error);
            if (!glfwInit()) throw std::runtime_error("Could not initialize GLFW");
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            GLFWwindow* window = glfwCreateWindow(1280, 800, "Planet Studio", nullptr, nullptr);
            if (!window) {
                glfwTerminate();
                throw std::runtime_error("Could not create the editor window");
            }
            glfwSetWindowSizeLimits(window, 1100, 700, GLFW_DONT_CARE, GLFW_DONT_CARE);

            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) {
                glfwDestroyWindow(window);
                glfwTerminate();
                throw std::runtime_error("Metal is unavailable");
            }
            id<MTLCommandQueue> queue = [device newCommandQueue];
            NSWindow* native_window = glfwGetCocoaWindow(window);
            CAMetalLayer* layer = [CAMetalLayer layer];
            layer.device = device;
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            native_window.contentView.layer = layer;
            native_window.contentView.wantsLayer = YES;

            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                width:state.render.width height:state.render.height mipmapped:NO];
            descriptor.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> preview = [device newTextureWithDescriptor:descriptor];
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            ImGui::StyleColorsDark();
            ImGui::GetStyle().WindowRounding = 0.0f;
            ImGui_ImplGlfw_InitForOther(window, true);
            ImGui_ImplMetal_Init(device);

            std::vector<std::uint8_t> rgba(state.render.width * state.render.height * 4);
            std::uint64_t uploaded_frame = 0;
            double previous = glfwGetTime();
            while (!glfwWindowShouldClose(window) && !state.request_close) {
                @autoreleasepool {
                    glfwPollEvents();
                    const double now = glfwGetTime();
                    planet_sandbox::advance_studio(state, std::max(0.0, now - previous));
                    previous = now;

                    const auto input = planet_sandbox::make_studio_render_input(state);
                    const auto& frame = planet_sandbox::update_studio_frame(state, input, now);
                    const auto& image = frame.image;
                    if (uploaded_frame != state.frame_cache.render_count) {
                        if (preview.width != image.width || preview.height != image.height) {
                            descriptor.width = image.width;
                            descriptor.height = image.height;
                            preview = [device newTextureWithDescriptor:descriptor];
                        }
                        rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
                        for (std::size_t i = 0; i < image.rgb.size() / 3; ++i) {
                            rgba[4 * i] = image.rgb[3 * i];
                            rgba[4 * i + 1] = image.rgb[3 * i + 1];
                            rgba[4 * i + 2] = image.rgb[3 * i + 2];
                            rgba[4 * i + 3] = 255;
                        }
                        [preview replaceRegion:MTLRegionMake2D(0, 0, image.width, image.height)
                                 mipmapLevel:0 withBytes:rgba.data() bytesPerRow:image.width * 4];
                        uploaded_frame = state.frame_cache.render_count;
                    }

                    int width = 0, height = 0;
                    glfwGetFramebufferSize(window, &width, &height);
                    if (width <= 0 || height <= 0) continue;
                    layer.drawableSize = CGSizeMake(width, height);
                    id<CAMetalDrawable> drawable = [layer nextDrawable];
                    if (!drawable) continue;
                    pass.colorAttachments[0].texture = drawable.texture;
                    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.045, 0.063, 0.085, 1.0);

                    ImGui_ImplMetal_NewFrame(pass);
                    ImGui_ImplGlfw_NewFrame();
                    ImGui::NewFrame();
                    planet_sandbox::draw_studio(state, (ImTextureID)(intptr_t)(__bridge void*)preview,
                                                input, frame, false);
                    ImGui::Render();

                    id<MTLCommandBuffer> commands = [queue commandBuffer];
                    id<MTLRenderCommandEncoder> encoder =
                        [commands renderCommandEncoderWithDescriptor:pass];
                    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commands, encoder);
                    [encoder endEncoding];
                    [commands presentDrawable:drawable];
                    [commands commit];
                    [commands waitUntilCompleted];
                }
            }

            ImGui_ImplMetal_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Planet Studio: %s\n", error.what());
            return 1;
        }
    }
}
