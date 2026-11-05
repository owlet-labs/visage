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

#include "visage/app.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <thread>

#if VISAGE_LINUX
#include <X11/Xlib.h>
#include <X11/Xutil.h>

using namespace visage;

namespace {
  bool hasDisplay() {
    const char* display_name = std::getenv("DISPLAY");
    return display_name && display_name[0];
  }

  int ignoreXErrors(Display*, XErrorEvent*) {
    return 0;
  }

  class ScopedErrorHandler {
  public:
    explicit ScopedErrorHandler(XErrorHandler handler) : previous_(XSetErrorHandler(handler)) { }
    ~ScopedErrorHandler() { XSetErrorHandler(previous_); }

  private:
    XErrorHandler previous_;
  };

  class ColorFrame : public Frame {
  public:
    void draw(Canvas& canvas) override {
      canvas.setColor(color);
      canvas.fill(0, 0, width(), height());
    }

    unsigned int color = 0;
  };

  // Feathers: one channel of a pixel, scaled to 8 bits through the image's own mask, so the compare
  // holds on a 30-bit (10 bits per channel) visual as well as a 24-bit one.
  unsigned int channel8(unsigned long pixel, unsigned long mask) {
    if (mask == 0)
      return 0;
    int shift = __builtin_ctzl(mask);
    unsigned long max = mask >> shift;
    return static_cast<unsigned int>((((pixel & mask) >> shift) * 255 + max / 2) / max);
  }

  // Feathers: as 0xRRGGBB, and within 2 per channel of the expected colour (a 10-bit round trip of
  // 0x80 can land one step away).
  unsigned int rgb8(const XImage* image, unsigned long pixel) {
    return (channel8(pixel, image->red_mask) << 16) | (channel8(pixel, image->green_mask) << 8) |
           channel8(pixel, image->blue_mask);
  }

  bool near(unsigned int got, unsigned int want) {
    for (int shift = 0; shift <= 16; shift += 8) {
      int a = static_cast<int>((got >> shift) & 0xff);
      int b = static_cast<int>((want >> shift) & 0xff);
      if (a - b > 2 || b - a > 2)
        return false;
    }
    return true;
  }

  struct Readback {
    unsigned int left = 0;
    unsigned int right = 0;
    int depth = 0;
    bool read = false;
  };

  Readback readWindow(Display* display, ::Window window, int width, int height) {
    Readback result;
    XImage* image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    if (image == nullptr)
      return result;
    result.left = rgb8(image, XGetPixel(image, width / 4, height / 2));
    result.right = rgb8(image, XGetPixel(image, 3 * width / 4, height / 2));
    result.depth = image->depth;
    result.read = true;
    XDestroyImage(image);
    return result;
  }

  bool windowShows(Display* display, ::Window window, int width, int height, unsigned int left,
                   unsigned int right) {
    Readback seen = readWindow(display, window, width, height);
    return seen.read && near(seen.left, left & 0xffffff) && near(seen.right, right & 0xffffff);
  }

  // Feathers: on a real desktop the window manager maps a new window ASYNCHRONOUSLY, so a draw made
  // straight after show() can be presented to a window that is not yet on screen and be lost — and
  // this test draws each step once. MEASURED on a composited 1.75x desktop (2026-11-05): the good
  // build failed at step 0 every run. Xvfb maps at once, which is why upstream never saw it.
  bool waitForViewable(Display* display, ::Window window) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < end) {
      XWindowAttributes attributes {};
      if (XGetWindowAttributes(display, window, &attributes) && attributes.map_state == IsViewable)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  bool waitForWindow(Display* display, ::Window window, int width, int height, unsigned int left,
                     unsigned int right) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
      if (windowShows(display, window, width, height, left, right))
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }
}

TEST_CASE("X11 windows show every change when only part of the window redraws", "[integration]") {
  if (!hasDisplay())
    SKIP("No X display");

  ScopedErrorHandler ignore_errors(ignoreXErrors);
  Display* display = XOpenDisplay(nullptr);
  REQUIRE(display);

  ApplicationEditor app;
  ColorFrame left;
  ColorFrame right;
  app.addChild(left);
  app.addChild(right);

  std::unique_ptr<visage::Window> window = createWindow(Dimension::nativePixels(40),
                                                        Dimension::nativePixels(40));
  app.addToWindow(window.get());
  left.setBounds(0, 0, app.width() / 2, app.height());
  right.setBounds(app.width() / 2, 0, app.width() - app.width() / 2, app.height());
  window->show();

  ::Window handle = static_cast<::Window>(reinterpret_cast<uintptr_t>(window->nativeHandle()));
  int width = app.nativeWidth();
  int height = app.nativeHeight();

  // Feathers: step 0 is drawn only once the window is on screen (see waitForViewable). The steps
  // themselves are NOT redrawn while polling: a step redraws only the frames it changed, and a whole-
  // window redraw per poll would turn this into a full-redraw test that no damage fault could fail.
  REQUIRE(waitForViewable(display, handle));

  struct Colors {
    unsigned int left = 0;
    unsigned int right = 0;
  };
  Colors steps[] = { { 0xffff0000, 0xff0000ff }, { 0xff00ff00, 0xff0000ff },
                     { 0xff00ff00, 0xffffff00 }, { 0xffffffff, 0xffffff00 },
                     { 0xffffffff, 0xffff00ff }, { 0xff00ffff, 0xffff00ff },
                     { 0xff00ffff, 0xff808080 }, { 0xff404040, 0xff808080 } };

  int failed_step = -1;
  for (int i = 0; i < static_cast<int>(std::size(steps)) && failed_step < 0; ++i) {
    const Colors& colors = steps[i];
    if (left.color != colors.left) {
      left.color = colors.left;
      left.redraw();
    }
    if (right.color != colors.right) {
      right.color = colors.right;
      right.redraw();
    }
    app.drawWindow();
    if (!waitForWindow(display, handle, width, height, colors.left, colors.right)) {
      failed_step = i;
      // Feathers: say what was READ, so a red names the venue's fault or the renderer's.
      Readback seen = readWindow(display, handle, width, height);
      std::fprintf(stderr,
                   "partial-present: step %d wanted left %06x right %06x, read %s left %06x right %06x "
                   "(depth %d, %dx%d native)\n",
                   i, colors.left & 0xffffff, colors.right & 0xffffff, seen.read ? "" : "NOTHING",
                   seen.left, seen.right, seen.depth, width, height);
    }
  }

  app.removeFromWindow();
  XCloseDisplay(display);
  REQUIRE(failed_step == -1);
}

#endif
