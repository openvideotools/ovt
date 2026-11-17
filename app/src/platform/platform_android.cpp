// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Platform values from Android (chapter 11 §11.5.2, §11.5.3; WI-A.451): the width the breakpoint table reads, the
// presence of a pointer and the windowing mode. Built into Android builds only; platform.cpp has the no-op for the
// other platforms.
//
// The width is `Configuration.screenWidthDp` of the Activity's Resources, which follows the window (split-screen,
// freeform, desktop windowing). `DisplayMetrics` of the display would report the whole screen, so it is never used.
// The Activity is not recreated on a resize (the manifest declares the configuration changes), so Qt only resizes
// the window; AndroidShell.qml calls Platform.refresh() from that resize.

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QString>

#include "platform/platform.h"

namespace ovt::app {

namespace {

// android.view.InputDevice.SOURCE_MOUSE and SOURCE_STYLUS (SOURCE_CLASS_POINTER | 0x2000 / 0x4000 | ...); a source
// "contains" a flag when the bits are all set (InputDevice.supportsSource).
constexpr jint kSourceMouse = 0x00002002;
constexpr jint kSourceStylus = 0x00004002;

// Any attached pointer that hovers: a mouse (also a ChromeOS touchpad) or a stylus (§11.5.3).
bool anyHoverDevice() {
  const QJniObject ids = QJniObject::callStaticObjectMethod("android/view/InputDevice", "getDeviceIds", "()[I");
  if (!ids.isValid()) {
    return false;
  }
  QJniEnvironment env;
  const auto array = static_cast<jintArray>(ids.object());
  const jsize count = env->GetArrayLength(array);
  jint* values = env->GetIntArrayElements(array, nullptr);
  bool found = false;
  for (jsize i = 0; i < count && !found; ++i) {
    const QJniObject device = QJniObject::callStaticObjectMethod("android/view/InputDevice", "getDevice",
                                                                 "(I)Landroid/view/InputDevice;", values[i]);
    if (!device.isValid()) {
      continue;
    }
    const jint sources = device.callMethod<jint>("getSources");
    found = (sources & kSourceMouse) == kSourceMouse || (sources & kSourceStylus) == kSourceStylus;
  }
  env->ReleaseIntArrayElements(array, values, JNI_ABORT);
  return found;
}

}  // namespace

bool Platform::refreshFromAndroid() {
  const QJniObject context = QNativeInterface::QAndroidApplication::context();
  if (!context.isValid()) {
    return false;
  }
  const QJniObject resources = context.callObjectMethod("getResources", "()Landroid/content/res/Resources;");
  const QJniObject configuration =
      resources.isValid() ? resources.callObjectMethod("getConfiguration", "()Landroid/content/res/Configuration;")
                          : QJniObject();
  bool ok = false;
  if (configuration.isValid()) {
    const jint width = configuration.getField<jint>("screenWidthDp");
    if (width > 0) {
      setWidthDp(width);
      ok = true;
    }
  }
  setHasHover(anyHoverDevice());
  // Activity.isInMultiWindowMode() (API 24): split-screen, freeform and desktop windowing all report true. An
  // application Context has no such method; the call then leaves a pending exception that is cleared.
  const bool multi_window = context.callMethod<jboolean>("isInMultiWindowMode") != JNI_FALSE;
  QJniEnvironment env;
  static_cast<void>(env.checkAndClearExceptions());
  setWindowingMode(multi_window ? QStringLiteral("multiWindow") : QStringLiteral("fullscreen"));
  return ok;
}

}  // namespace ovt::app
