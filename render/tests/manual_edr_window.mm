// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AppKit side of the manual EDR tool (see manual_edr_window.h). Compiled with ARC.

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "manual_edr_window.h"

@interface OVTMetalView : NSView
@end
@implementation OVTMetalView
- (CALayer*)makeBackingLayer {
  return [CAMetalLayer layer];
}
@end

namespace ovt::render::manual {
namespace {
NSWindow* g_window = nil;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): one window per process
}

bool open_edr_window(int width_pt, int height_pt, EdrWindow& out) {
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  NSScreen* screen = NSScreen.mainScreen;
  if (screen == nil) {
    return false;
  }
  const CGFloat strip = 80;  // points of plain UI white under the pattern
  const NSRect frame = NSMakeRect(0, 0, width_pt, height_pt + strip);
  g_window = [[NSWindow alloc] initWithContentRect:frame
                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
  g_window.title = @"OVT manual EDR pattern (WI-1.406)";
  g_window.releasedWhenClosed = NO;
  NSView* content = [[NSView alloc] initWithFrame:frame];
  NSView* white = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width_pt, strip)];
  white.wantsLayer = YES;
  white.layer.backgroundColor = NSColor.whiteColor.CGColor;
  OVTMetalView* metal = [[OVTMetalView alloc] initWithFrame:NSMakeRect(0, strip, width_pt, height_pt)];
  metal.wantsLayer = YES;
  [content addSubview:white];
  [content addSubview:metal];
  g_window.contentView = content;
  [g_window center];
  [g_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  pump_events(0.2);

  const CGFloat scale = g_window.backingScaleFactor;
  auto* layer = (CAMetalLayer*)metal.layer;
  layer.contentsScale = scale;
  out.layer = (__bridge void*)layer;
  out.device_pixel_ratio = static_cast<float>(scale);
  out.width_px = static_cast<int>(width_pt * scale);
  out.height_px = static_cast<int>(height_pt * scale);
  out.potential_headroom = static_cast<float>(screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
  return true;
}

void pump_events(double seconds) {
  NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
  for (;;) {
    NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:until
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
    if (event == nil) {
      break;
    }
    [NSApp sendEvent:event];
  }
}

float current_headroom() {
  NSScreen* screen = g_window != nil && g_window.screen != nil ? g_window.screen : NSScreen.mainScreen;
  return screen != nil ? static_cast<float>(screen.maximumExtendedDynamicRangeColorComponentValue) : 0.0F;
}

}  // namespace ovt::render::manual
