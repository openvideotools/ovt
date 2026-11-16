// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// display_probe() on macOS (chapter 10 §10.4.5; WI-1.407): NSScreen EDR queries, the probe-surface format check on a
// 1x1 hidden CAMetalLayer, the platform_detail string, and the re-probe watcher. The rules that turn the queries into
// a DisplayCaps are portable (display_caps_rules.cpp). Compiled with ARC.

#define VK_USE_PLATFORM_METAL_EXT 1

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "display_caps_rules.h"
#include "native_target.h"
#include "ovt/render/display_caps.h"

#if !__has_feature(objc_arc)
#error "display_caps_macos.mm must be compiled with -fobjc-arc"
#endif

namespace ovt::render::detail {
namespace {

// The screen of the target's layer (the layer's delegate is the NSView Qt created for the MetalSurface window), else
// the main screen. Main thread only.
NSScreen* screen_of(const DisplayTarget& where) {
  NSScreen* screen = nil;
  if (where.kind == DisplayTarget::Kind::MetalLayer && where.native != nullptr) {
    id obj = (__bridge id)where.native;
    if ([obj isKindOfClass:[CAMetalLayer class]]) {
      id delegate = ((CAMetalLayer*)obj).delegate;
      if ([delegate isKindOfClass:[NSView class]]) {
        screen = ((NSView*)delegate).window.screen;
      }
    }
  }
  return screen != nil ? screen : NSScreen.mainScreen;
}

std::string primaries_of(NSScreen* screen) {
  CGColorSpaceRef space = screen.colorSpace.CGColorSpace;
  if (space == nullptr) {
    return "unknown";
  }
  CFStringRef cf_name = CGColorSpaceCopyName(space);
  std::string name = cf_name != nullptr ? std::string([(__bridge NSString*)cf_name UTF8String]) : std::string();
  if (cf_name != nullptr) {
    CFRelease(cf_name);
  }
  if (name.find("2020") != std::string::npos || name.find("2100") != std::string::npos) {
    return "BT.2020";
  }
  if (name.find("P3") != std::string::npos) {
    return "P3";
  }
  if (name.find("sRGB") != std::string::npos || name.find("709") != std::string::npos) {
    return "BT.709";
  }
  return CGColorSpaceIsWideGamutRGB(space) ? "P3" : "unknown";
}

std::string os_name() {
  const NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
  return "macOS " + std::to_string(v.majorVersion) + "." + std::to_string(v.minorVersion);
}

// §10.4.5 step 3: creates a VkSurfaceKHR on a hidden 1x1 CAMetalLayer of a throw-away instance and reads the formats
// MoltenVK offers for it, then destroys everything. Fails soft: the result says why.
void probe_surface(ScreenInfo& info) {
  info.surface_check = ScreenInfo::SurfaceCheck::Failed;
  const NativeProvider& provider = native_provider();
  VkApplicationInfo app = {};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici = {};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = static_cast<uint32_t>(provider.instance_extensions.size());
  ici.ppEnabledExtensionNames = provider.instance_extensions.data();
  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
    info.surface_error = "vkCreateInstance failed";
    return;
  }
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(1, 1);
  VkMetalSurfaceCreateInfoEXT sci = {};
  sci.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
  sci.pLayer = layer;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (vkCreateMetalSurfaceEXT(instance, &sci, nullptr, &surface) != VK_SUCCESS) {
    info.surface_error = "vkCreateMetalSurfaceEXT failed";
    vkDestroyInstance(instance, nullptr);
    return;
  }
  uint32_t n_dev = 0;
  VkPhysicalDevice dev = VK_NULL_HANDLE;
  if (vkEnumeratePhysicalDevices(instance, &n_dev, nullptr) == VK_SUCCESS && n_dev > 0) {
    n_dev = 1;
    static_cast<void>(vkEnumeratePhysicalDevices(instance, &n_dev, &dev));  // VK_INCOMPLETE is fine: the first device
  }
  if (dev == VK_NULL_HANDLE) {
    info.surface_error = "no Vulkan device";
  } else {
    uint32_t n = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &n, nullptr) == VK_SUCCESS) {
      info.surface_formats.resize(n);
      if (vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &n, info.surface_formats.data()) == VK_SUCCESS) {
        info.surface_formats.resize(n);
        info.surface_check = ScreenInfo::SurfaceCheck::Done;
      } else {
        info.surface_formats.clear();
        info.surface_error = "vkGetPhysicalDeviceSurfaceFormatsKHR failed";
      }
    } else {
      info.surface_error = "vkGetPhysicalDeviceSurfaceFormatsKHR failed";
    }
  }
  vkDestroySurfaceKHR(instance, surface, nullptr);
  vkDestroyInstance(instance, nullptr);
}

// Reads the NSScreen values on the main thread (§10.4.5 note: potential value for hdr_capable, current for headroom;
// `maximumExtendedDynamicRangeColorComponentValue` stays 1.0 until EDR content is on screen). The surface probe runs
// on the caller's thread afterwards, only when the display has headroom at all.
ScreenInfo query_screen(const DisplayTarget& where) {
  __block ScreenInfo info;
  void (^read)(void) = ^{
    NSScreen* screen = screen_of(where);
    info.os_name = os_name();
    if (screen == nil) {
      return;
    }
    info.have_screen = true;
    info.current_edr = static_cast<float>(screen.maximumExtendedDynamicRangeColorComponentValue);
    info.potential_edr = static_cast<float>(screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
    info.primaries_name = primaries_of(screen);
  };
  if (NSThread.isMainThread) {
    read();
  } else {
    dispatch_sync(dispatch_get_main_queue(), read);  // the host must not hold the main thread waiting on this thread
  }
  if (info.have_screen && info.potential_edr > 1.0F) {
    probe_surface(info);
  }
  return info;
}

// Re-probe triggers (§10.4.5): screen change notifications and a 500 ms timer; `on_change` fires when the caps differ
// in what the host acts on.
class MacDisplayWatcher final : public DisplayWatcher {
 public:
  MacDisplayWatcher(const DisplayTarget& where, std::function<void()> on_change) {
    auto state = std::make_shared<State>();
    state_ = state;
    state->where = where;
    state->on_change = std::move(on_change);
    void (^check)(void) = ^{
      if (state->stopped) {
        return;
      }
      NSScreen* screen = screen_of(state->where);
      if (screen == nil) {
        return;
      }
      const float cur = static_cast<float>(screen.maximumExtendedDynamicRangeColorComponentValue);
      const float pot = static_cast<float>(screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
      const bool first = !state->have_last;
      const bool changed = first || pot != state->last_potential || headroom_changed(state->last_current, cur);
      state->have_last = true;
      state->last_potential = pot;
      state->last_current = cur;
      if (changed && !first) {
        state->on_change();
      }
    };
    dispatch_async(dispatch_get_main_queue(), ^{
      check();
      NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
      void (^on_note)(NSNotification*) = ^(NSNotification*) {
        check();
      };
      state->tokens = [NSMutableArray array];
      [state->tokens addObject:[nc addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                               object:nil
                                                queue:NSOperationQueue.mainQueue
                                           usingBlock:on_note]];
      [state->tokens addObject:[nc addObserverForName:NSWindowDidChangeScreenNotification
                                               object:nil
                                                queue:NSOperationQueue.mainQueue
                                           usingBlock:on_note]];
      state->timer = [NSTimer timerWithTimeInterval:0.5
                                            repeats:YES
                                              block:^(NSTimer*) {
                                                check();
                                              }];
      [NSRunLoop.mainRunLoop addTimer:state->timer forMode:NSRunLoopCommonModes];
    });
  }
  MacDisplayWatcher(const MacDisplayWatcher&) = delete;
  MacDisplayWatcher& operator=(const MacDisplayWatcher&) = delete;
  MacDisplayWatcher(MacDisplayWatcher&&) = delete;
  MacDisplayWatcher& operator=(MacDisplayWatcher&&) = delete;
  ~MacDisplayWatcher() override {
    auto state = state_;
    state->stopped = true;
    dispatch_async(dispatch_get_main_queue(), ^{  // never wait for the main thread here
      [state->timer invalidate];
      state->timer = nil;
      for (id token in state->tokens) {
        [NSNotificationCenter.defaultCenter removeObserver:token];
      }
      [state->tokens removeAllObjects];
    });
  }

 private:
  struct State {
    DisplayTarget where;
    std::function<void()> on_change;
    NSTimer* timer = nil;
    NSMutableArray<id>* tokens = nil;
    bool stopped = false;
    bool have_last = false;
    float last_potential = 0.0F;
    float last_current = 0.0F;
  };
  std::shared_ptr<State> state_;
};

}  // namespace

std::unique_ptr<DisplayWatcher> watch_display(const DisplayTarget& where, std::function<void()> on_change) {
  return std::make_unique<MacDisplayWatcher>(where, std::move(on_change));
}

}  // namespace ovt::render::detail

namespace ovt::render {

DisplayCaps display_probe(const DisplayTarget& where) {
  const detail::ScreenQuery override_query = detail::screen_query_override();
  return detail::caps_from_screen(override_query != nullptr ? override_query(where) : detail::query_screen(where));
}

}  // namespace ovt::render
