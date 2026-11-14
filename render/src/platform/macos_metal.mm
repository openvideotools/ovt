// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// macOS EDR native target (chapter 10 §10.4.1, WI-1.406): DisplayTarget{Kind::MetalLayer, native = CAMetalLayer*}
// from the app's child QWindow(MetalSurface) (app/src/platform/mac_layer.mm, WI-1.463). This file sets the layer's EDR
// properties before the first present, creates the VkSurfaceKHR with vkCreateMetalSurfaceEXT (MoltenVK) and polls the
// screen's EDR headroom. Compiled with ARC.

#define VK_USE_PLATFORM_METAL_EXT 1

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "native_target.h"

#if !__has_feature(objc_arc)
#error "macos_metal.mm must be compiled with -fobjc-arc"
#endif

using ovt::render::DisplayTarget;
using ovt::render::detail::MetalLayerConfig;

// Polls -[NSScreen maximumExtendedDynamicRangeColorComponentValue] of the layer's screen on the main thread: on
// NSApplicationDidChangeScreenParametersNotification, on NSWindowDidChangeScreenNotification and every 500 ms while
// the target exists (§10.4.1). The value is published to the render thread through a shared atomic.
@interface OVTEdrHeadroomObserver : NSObject
- (instancetype)initWithLayer:(CAMetalLayer*)layer value:(std::shared_ptr<std::atomic<float>>)value;
- (void)start;
- (void)stop;
@end

@implementation OVTEdrHeadroomObserver {
  __weak CAMetalLayer* _layer;
  std::shared_ptr<std::atomic<float>> _value;
  NSTimer* _timer;
  NSMutableArray<id>* _tokens;
}

- (instancetype)initWithLayer:(CAMetalLayer*)layer value:(std::shared_ptr<std::atomic<float>>)value {
  if ((self = [super init])) {
    _layer = layer;
    _value = std::move(value);
    _tokens = [NSMutableArray array];
  }
  return self;
}

// Main thread only. The layer's delegate is the NSView Qt created for the MetalSurface window.
- (void)refresh {
  CAMetalLayer* layer = _layer;
  NSScreen* screen = nil;
  if (layer != nil && [layer.delegate isKindOfClass:[NSView class]]) {
    screen = ((NSView*)layer.delegate).window.screen;
  }
  if (screen == nil) {
    screen = NSScreen.mainScreen;
  }
  _value->store(screen != nil ? static_cast<float>(screen.maximumExtendedDynamicRangeColorComponentValue) : 0.0F);
}

- (void)start {
  dispatch_async(dispatch_get_main_queue(), ^{
    [self refresh];
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    __weak OVTEdrHeadroomObserver* weakSelf = self;
    void (^onChange)(NSNotification*) = ^(NSNotification*) {
      [weakSelf refresh];
    };
    [self->_tokens addObject:[nc addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                             object:nil
                                              queue:NSOperationQueue.mainQueue
                                         usingBlock:onChange]];
    [self->_tokens addObject:[nc addObserverForName:NSWindowDidChangeScreenNotification
                                             object:nil
                                              queue:NSOperationQueue.mainQueue
                                         usingBlock:onChange]];
    self->_timer = [NSTimer timerWithTimeInterval:0.5
                                          repeats:YES
                                            block:^(NSTimer*) {
                                              [weakSelf refresh];
                                            }];
    [NSRunLoop.mainRunLoop addTimer:self->_timer forMode:NSRunLoopCommonModes];
  });
}

- (void)stop {
  // Asynchronous: the render thread never waits for the main thread (which may be waiting for the render thread).
  dispatch_async(dispatch_get_main_queue(), ^{
    [self->_timer invalidate];
    self->_timer = nil;
    for (id token in self->_tokens) {
      [NSNotificationCenter.defaultCenter removeObserver:token];
    }
    [self->_tokens removeAllObjects];
  });
}
@end

namespace ovt::render::detail {
namespace {

// Indexed by the MetalLayerConfig enums (declaration order).
MTLPixelFormat mtl_pixel_format(MetalLayerConfig::PixelFormat f) {
  const MTLPixelFormat formats[] = {MTLPixelFormatBGRA8Unorm, MTLPixelFormatRGBA16Float, MTLPixelFormatBGR10A2Unorm};
  return formats[static_cast<size_t>(f)];
}

CFStringRef cg_color_space_name(MetalLayerConfig::ColorSpace c) {
  const CFStringRef names[] = {kCGColorSpaceSRGB, kCGColorSpaceExtendedLinearITUR_2020, kCGColorSpaceITUR_2100_PQ,
                               kCGColorSpaceITUR_2100_HLG};
  return names[static_cast<size_t>(c)];
}

class MetalLayerOutput final : public NativeOutput {
 public:
  explicit MetalLayerOutput(CAMetalLayer* layer)
      : layer_(layer), headroom_(std::make_shared<std::atomic<float>>(0.0F)) {
    observer_ = [[OVTEdrHeadroomObserver alloc] initWithLayer:layer value:headroom_];
    [observer_ start];
  }
  MetalLayerOutput(const MetalLayerOutput&) = delete;
  MetalLayerOutput& operator=(const MetalLayerOutput&) = delete;
  MetalLayerOutput(MetalLayerOutput&&) = delete;
  MetalLayerOutput& operator=(MetalLayerOutput&&) = delete;
  ~MetalLayerOutput() override { [observer_ stop]; }

  VkSurfaceKHR create_surface(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, std::string& error) override {
    const auto create = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(gipa(instance, "vkCreateMetalSurfaceEXT"));
    if (create == nullptr) {
      error = "VK_EXT_metal_surface is not available (MoltenVK missing)";
      return VK_NULL_HANDLE;
    }
    VkMetalSurfaceCreateInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    info.pLayer = layer_;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    const VkResult rc = create(instance, &info, nullptr, &surf);
    if (rc != VK_SUCCESS) {
      error = "vkCreateMetalSurfaceEXT failed (VkResult " + std::to_string(static_cast<int>(rc)) + ")";
      return VK_NULL_HANDLE;
    }
    return surf;
  }

  // §10.4.1. The colour space goes in before the first present of the swapchain that follows: changing it (or the
  // EDR flag) after presenting flashes. MoltenVK sets the same colour space and pixel format again from the pair the
  // swapchain is created with. Implicit layer animations are off so nothing fades.
  void configure(DisplayTarget::OutputEncoding encoding, int width, int height) override {
    const MetalLayerConfig c = metal_layer_config(encoding);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer_.wantsExtendedDynamicRangeContent = c.wants_edr ? YES : NO;
    layer_.pixelFormat = mtl_pixel_format(c.pixel_format);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(cg_color_space_name(c.color_space));
    layer_.colorspace = space;
    CGColorSpaceRelease(space);
    switch (c.edr_metadata) {
      case MetalLayerConfig::EdrMetadata::HDR10:
        // Placeholder mastering range until WI-1.411 passes the content's MDCV (§10.3.6); 10000 = PQ code 1.0 in nits.
        layer_.EDRMetadata = [CAEDRMetadata HDR10MetadataWithMinLuminance:0.005F
                                                             maxLuminance:1000.0F
                                                       opticalOutputScale:10000.0F];
        break;
      case MetalLayerConfig::EdrMetadata::HLG:
        layer_.EDRMetadata = [CAEDRMetadata HLGMetadata];
        break;
      case MetalLayerConfig::EdrMetadata::None:
        layer_.EDRMetadata = nil;  // scRGB: libplacebo tone-maps to the headroom itself
        break;
    }
    layer_.displaySyncEnabled = YES;
    layer_.drawableSize = CGSizeMake(width, height);
    [CATransaction commit];
  }

  [[nodiscard]] float polled_headroom() const override { return headroom_->load(); }

 private:
  CAMetalLayer* layer_;  // strong (ARC): the layer outlives the surface made from it
  std::shared_ptr<std::atomic<float>> headroom_;
  OVTEdrHeadroomObserver* observer_;
};

std::unique_ptr<NativeOutput> create_metal_output(const DisplayTarget& target, std::string& error) {
  if (target.kind != DisplayTarget::Kind::MetalLayer) {
    error = "only DisplayTarget::Kind::MetalLayer is a NATIVE target on macOS";
    return nullptr;
  }
  id obj = (__bridge id)target.native;
  if (obj == nil || ![obj isKindOfClass:[CAMetalLayer class]]) {
    error = "DisplayTarget.native is not a CAMetalLayer*";
    return nullptr;
  }
  return std::make_unique<MetalLayerOutput>((CAMetalLayer*)obj);
}

}  // namespace

const NativeProvider& native_platform_provider() {
  static const NativeProvider provider{
      {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME},
      &create_metal_output};
  return provider;
}

}  // namespace ovt::render::detail
