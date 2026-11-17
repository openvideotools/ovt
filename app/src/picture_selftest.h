// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The on-device check of WI-A.403's acceptance tests (chapter 10 §10.11), for the emulator and devices, where no test
// binary runs. It exists only on Android and only runs in a debuggable APK (ApplicationInfo.FLAG_DEBUGGABLE) when
// the system property debug.ovt.picture_selftest is "1":
//
//   adb shell setprop debug.ovt.picture_selftest 1
//   adb shell am start -n org.openvideotools.studio.debug/org.openvideotools.studio.MainActivity
//   adb logcat -d | grep "OVT: picture-selftest"
//
// A few seconds after start-up it renders a PictureHost through QQuickRenderControl on Qt's OpenGL ES backend (so
// through Interop::GlTexture and the fence of §10.2.2), reads the Qt Quick window back and compares it with a
// separate Interop::None VideoSurface of the same picture (the image SdrPng8 encodes, as in WI-1.405's desktop test):
//
//   AT1  frame 0 of sdr709_ramp (1920x1080), ≤ 0.1 % of the pixels differing by more than 1 per channel;
//   AT2  300 different pictures presented one after another, none with a pixel differing by more than 1 (a frame
//        sampled before the surface finished it would show the previous picture or garbage).
//
// Each test logs one line "OVT: picture-selftest AT<n> ... result=PASS|FAIL", then "OVT: picture-selftest done".

#ifndef OVT_APP_PICTURE_SELFTEST_H
#define OVT_APP_PICTURE_SELFTEST_H

namespace ovt::app {

// Schedules the self-check when it is enabled (see above); does nothing otherwise. GUI thread, after the
// QGuiApplication exists.
void maybe_schedule_picture_selftest();

}  // namespace ovt::app

#endif  // OVT_APP_PICTURE_SELFTEST_H
