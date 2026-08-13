/* Copyright Vital Audio, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "renderer.h"

#include "visage_utils/string_utils.h"

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

namespace visage {
  class GraphicsCallbackHandler : public bgfx::CallbackI {
    void fatal(const char* file_path, uint16_t line, bgfx::Fatal::Enum _code, const char* error) override {
      VISAGE_LOG(String(file_path) + String(" (") + line + String(") "));
      VISAGE_LOG(error);
      VISAGE_LOG("Graphics fatal error");
      VISAGE_ASSERT(false);
    }

    void traceVargs(const char* file_path, uint16_t line, const char* format, va_list arg_list) override {
#if VISAGE_GRAPHICS_DEBUG_LOGGING
      visage::debugLogArgs(file_path, line, format, arg_list);
#endif
    }

    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override { }
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override { }
    void profilerEnd() override { }
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override { }

    void screenShot(const char* file_path, uint32_t width, uint32_t height, uint32_t pitch,
                    const void* data, uint32_t size, bool y_flip) override {
      Renderer::instance().setScreenshotData(static_cast<const uint8_t*>(data), width, height, pitch, true);
    }

    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override { }
    void captureEnd() override { }
    void captureFrame(const void*, uint32_t) override { }
  };

  static constexpr uint32_t resetFlags() {
#if VISAGE_WINDOWS
    return BGFX_RESET_FLIP_AFTER_RENDER;
#elif VISAGE_MAC
    return BGFX_RESET_FLIP_AFTER_RENDER | BGFX_RESET_VSYNC;
#elif VISAGE_LINUX
    return BGFX_RESET_VSYNC;
#else
    return 0;
#endif
  }

  Renderer& Renderer::instance() {
    static Renderer renderer;
    return renderer;
  }

  Renderer::Renderer() : Thread("Renderer Thread") { }

  Renderer::~Renderer() {
    stop();
  }

  void Renderer::startRenderThread() {
#if VISAGE_BACKGROUND_GRAPHICS_THREAD
    start();
    while (!render_thread_started_.load())
      yield();
#endif
  }

  void Renderer::run() {
    render();
  }

  void Renderer::render() {
    static constexpr int kRenderTimeout = 100;
    render_thread_started_ = bgfx::renderFrame() == bgfx::RenderFrame::NoContext;

    while (shouldRun())
      bgfx::renderFrame(kRenderTimeout);
  }

  void Renderer::initialize(void* model_window, void* display) {
    if (initialized_)
      return;

    callback_handler_ = std::make_unique<GraphicsCallbackHandler>();
    initialized_ = true;
    startRenderThread();

    bgfx::Init bgfx_init;
    bgfx_init.resolution.numBackBuffers = 1;
    bgfx_init.resolution.width = 0;
    bgfx_init.resolution.height = 0;
    bgfx_init.callback = callback_handler_.get();

    // THE TRANSIENT VERTEX ARENA, SIZED FROM A CENSUS RATHER THAN LEFT AT THE DEFAULT.
    //
    // This pool is what every quad batch in a frame allocates from, and it is per FRAME and shared.
    // When it runs out, `allocTransientBuffers` fails and the batch that asked is skipped ENTIRELY:
    // every shape of that type, in every region. bgfx defaults it to 6 MB, which at 112 bytes per
    // ShapeVertex is 14043 quads for the whole frame — and that is BELOW the 16384 quads the uint16
    // index space can address, so on a default build a dense frame meets the arena first, silently,
    // with the only report compiled out under NDEBUG.
    //
    // 8 MB is 18724 quads, and the size is chosen for WHICH CEILING BINDS rather than for a
    // multiple.
    // Above 16384, the first limit a growing batch meets is the index space, and that one is
    // handled:
    // the batch is split into runs and the frame comes out right. Below it, the first limit is the
    // arena, and that one loses the batch. Given a choice between a failure mode that is corrected
    // and one that is silent, the arena should be the one that never binds.
    //
    // The headroom on top is large and cheap. tools/quad-census.cpp drives the Explore surface
    // through
    // the automation recipe that provoked the original fault and reports a worst frame of 230
    // quads,
    // so this is about eighty times what the surface asks for. It was 32 MB while the terrain drew
    // one
    // quad per texel and a frame reached 13067; the terrain is one textured quad now, so the same
    // recipe measures 230 and the pool comes down by a factor of four.
    //
    // The census measures ONE surface, not the assembled editor. That is why the number to reason
    // about is the eighty-times margin rather than the 230.
    //
    // The index buffer is left alone deliberately: 6 uint16 indices per quad is 12 bytes, so the
    // 2 MB default already covers 174762 quads, well past what the vertex side allows.
    bgfx_init.limits.transientVbSize = 8 << 20;

    bgfx_init.platformData.ndt = display;
    bgfx_init.platformData.nwh = model_window;
    bgfx_init.platformData.type = bgfx::NativeWindowHandleType::Default;

    bgfx::RendererType::Enum supported_renderers[bgfx::RendererType::Count];
    uint8_t num_supported = bgfx::getSupportedRenderers(bgfx::RendererType::Count, supported_renderers);

#if VISAGE_WINDOWS
    bgfx_init.type = bgfx::RendererType::Direct3D11;
#if USE_DIRECTX12
    for (int i = 0; i < num_supported; ++i) {
      if (supported_renderers[i] == bgfx::RendererType::Direct3D12)
        bgfx_init.type = bgfx::RendererType::Direct3D12;
    }
#endif
#elif VISAGE_MAC
    bgfx_init.type = bgfx::RendererType::Metal;
    bgfx_init.resolution.width = 1;
    bgfx_init.resolution.height = 1;
#elif VISAGE_LINUX
    bgfx_init.type = bgfx::RendererType::Vulkan;
#elif VISAGE_EMSCRIPTEN
    bgfx_init.type = bgfx::RendererType::OpenGLES;
#endif

    bgfx_init.resolution.reset = resetFlags();

    for (int i = 0; i < num_supported && !supported_; ++i)
      supported_ = supported_renderers[i] == bgfx_init.type;

    if (!supported_) {
      VISAGE_ASSERT(false);
      std::string renderer_name = bgfx::getRendererName(bgfx_init.type);
      error_message_ = renderer_name + " is required and not supported on this computer.";
    }

    bgfx::init(bgfx_init);
    VISAGE_ASSERT(bgfx::getRendererType() == bgfx_init.type);
    swap_chain_supported_ = bgfx::getCaps()->supported & BGFX_CAPS_SWAP_CHAIN;
  }

  void Renderer::resetResolution(int width, int height) {
#if VISAGE_MAC
    bgfx::reset(width, height, resetFlags());
#endif
  }

  void Renderer::setScreenshotData(const uint8_t* data, int width, int height, int pitch, bool blue_red) {
    screenshot_ = Screenshot(data, width, height, pitch, blue_red);
  }
}
