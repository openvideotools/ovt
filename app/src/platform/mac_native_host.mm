// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The macOS NATIVE host (chapter 10 §10.4.1, chapter 11 §11.2.1; WI-1.463): the child QWindow of NativeSurfaceHost is
// a MetalSurface window, for which Qt's QNSView is layer-backed by a CAMetalLayer. That layer is the
// DisplayTarget{Kind::MetalLayer, native = CAMetalLayer*} of render/src/platform/macos_metal.mm (WI-1.406), which sets
// its EDR properties, creates the VkSurfaceKHR with vkCreateMetalSurfaceEXT and keeps its own strong reference.
// SPEC-ASSUMPTION(#702): §10.4.1 and WI-1.406 call this file app/src/platform/mac_layer.mm; WI-1.463's Produces
// line names it as here.
// Compiled with ARC.

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <QGuiApplication>
#include <QLatin1String>
#include <QString>
#include <QSurface>
#include <QWindow>

#include <memory>

#include "native_surface_host.h"

#if !__has_feature(objc_arc)
#error "mac_native_host.mm must be compiled with -fobjc-arc"
#endif

namespace ovt::app {

namespace {

// render::DisplayTarget::Kind::MetalLayer (chapter 10 §10.2.2); the app module does not include the render headers
// here, and NativeSurfaceHost passes the value through.
constexpr int kKindMetalLayer = 1;

class MacNativeBackend final : public NativeWindowBackend {
 public:
  [[nodiscard]] QSurface::SurfaceType surface_type() const override { return QSurface::MetalSurface; }
  [[nodiscard]] int target_kind() const override { return kKindMetalLayer; }

  [[nodiscard]] void* native_handle(QWindow& window, QString& error) override {
    // winId() is an NSView* only on the cocoa platform (the offscreen platform of the tests hands out counters).
    if (QGuiApplication::platformName() != QLatin1String("cocoa")) {
      error = QStringLiteral("HDR output needs the cocoa platform plugin (running on \"%1\")")
                  .arg(QGuiApplication::platformName());
      return nullptr;
    }
    // The QNSView of the window (created by NativeSurfaceHost after the parent was exposed).
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window.winId());
    if (view == nil) {
      error = QStringLiteral("the HDR window has no NSView");
      return nullptr;
    }
    CALayer* layer = view.layer;
    if (![layer isKindOfClass:[CAMetalLayer class]]) {
      error = QStringLiteral("the HDR window's view is not backed by a CAMetalLayer");
      return nullptr;
    }
    // Implicit animations would fade the layer's bounds during a splitter drag (§11.2.1 geometry sync).
    layer.actions = @{@"bounds" : [NSNull null], @"position" : [NSNull null], @"contents" : [NSNull null]};
    return (__bridge void*)layer;
  }
};

}  // namespace

std::unique_ptr<NativeWindowBackend> make_platform_native_backend() {
  return std::make_unique<MacNativeBackend>();
}

}  // namespace ovt::app
