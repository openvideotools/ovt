# android/

Gradle project of the Android/ChromeOS shell (chapter 13 §13.4; WI-0.050): `app/` with the Kotlin `MainActivity`,
which embeds the Qt Quick UI through `QtQuickView`, the `HdrSurfaceView` sibling (stub until WI-0.052) and the JNI
glue `app/src/main/cpp/ovt_jni.cpp`. Licence: GPL-3.0-or-later (the Gradle wrapper files are Apache-2.0).

Set up a build host once, then build:

```sh
tools/bootstrap.sh --no-qt && tools/bootstrap_android.sh   # SDK packages, Qt for Android kits, local.properties
cd android
./gradlew :app:assembleFossDebug     # app/build/outputs/apk/foss/debug/: one APK per ABI (arm64-v8a, x86_64)
./gradlew :app:bundlePlayRelease     # app/build/outputs/bundle/playRelease/: the AAB (unsigned until WI-0.056)
```

The native code is built by the Qt Gradle plugin, which configures the repository root with the Qt kit's
`qt-cmake` for every ABI and packages the result as an AAR (see the header of `app/build.gradle.kts`).
