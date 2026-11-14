// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Application module of OVT Studio for Android and ChromeOS (chapter 13 §13.4.2; WI-0.050).
//
// Native code: the Qt Gradle plugin (QtBuild below, §13.4.3) configures the whole repository with the Qt kit's
// qt-cmake as a multi-ABI build (one CMake configure per ABI, QT_ANDROID_BUILD_ALL_ABIS), builds the `aar` target
// and adds the resulting AAR (Qt's Java classes, the native libraries of every ABI, QML modules and assets) to this
// module. AGP's own externalNativeBuild is therefore not used: it would compile the same CMake project a second
// time without androiddeployqt (SPEC-ASSUMPTION(#355): §13.4.2 lists both).
//
// Outputs: `assembleFossDebug` writes one APK per ABI (splits below); `bundlePlayRelease` writes the AAB, unsigned
// until WI-0.056 adds the release signing configuration.

import com.android.build.api.variant.FilterConfiguration
import com.github.jk1.license.render.JsonReportRenderer
import com.github.jk1.license.render.ReportRenderer
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.qt)
    alias(libs.plugins.license.report)
}

// ---- Inputs ------------------------------------------------------------------------------------------------------

// The repository root: android/ is one level below it.
val ovtRoot: File = rootDir.parentFile

val localProperties =
    Properties().apply {
        val file = rootDir.resolve("local.properties")
        if (file.isFile) file.inputStream().use { load(it) }
    }

// A setting from -P/gradle.properties, then the environment variable, then android/local.properties.
fun setting(
    name: String,
    environmentVariable: String? = null,
): String? {
    val value =
        providers.gradleProperty(name).orNull
            ?: environmentVariable?.let { System.getenv(it) }
            ?: localProperties.getProperty(name)
    return value?.trim()?.takeIf { it.isNotEmpty() }
}

val ovtProductName = setting("ovt.productName") ?: "OVT Studio"
val ovtApplicationId = setting("ovt.applicationId") ?: "org.openvideotools.studio"
val ovtAbis = (setting("ovt.abis") ?: "arm64-v8a;x86_64").split(';').map { it.trim() }.filter { it.isNotEmpty() }
val ovtNativeBuildType = setting("ovt.nativeBuildType") ?: "RelWithDebInfo"

// versionName and versionCode from /VERSION (chapter 13 §13.8 item 1): MAJOR*1_000_000 + MINOR*10_000 + PATCH*100
// + abiIndex, with arm64-v8a = 1, x86_64 = 2 and 0 for the AAB.
val ovtVersion = ovtRoot.resolve("VERSION").readText().trim()
val ovtVersionParts =
    Regex("""^(\d+)\.(\d+)\.(\d+)""")
        .find(ovtVersion)
        ?.groupValues
        ?.drop(1)
        ?.map(String::toInt)
        ?: throw GradleException("VERSION: expected MAJOR.MINOR.PATCH, found '$ovtVersion'")
val ovtVersionCodeBase = ovtVersionParts[0] * 1_000_000 + ovtVersionParts[1] * 10_000 + ovtVersionParts[2] * 100
val ovtAbiIndex = mapOf("arm64-v8a" to 1, "x86_64" to 2)

// Machine paths: written to local.properties by tools/bootstrap_android.sh (sdk.dir, qt.dir).
val sdkDir = setting("sdk.dir", "ANDROID_SDK_ROOT") ?: System.getenv("ANDROID_HOME")?.takeIf { it.isNotEmpty() }
val ndkDir = sdkDir?.let { "$it/ndk/${libs.versions.ndk.get()}" }
// QT_ROOT_DIR as tools/install_qt.py prints it (<dest>/6.8.3): android_arm64_v8a, android_x86_64 and the host kit.
val qtDir = setting("qt.dir", "QT_ROOT_DIR")
val qtHostKit =
    when {
        System.getProperty("os.name").startsWith("Mac") -> "macos"
        System.getProperty("os.arch") in setOf("aarch64", "arm64") -> "gcc_arm64"
        else -> "gcc_64"
    }

// aqtinstall names the kits android_arm64_v8a and android_x86_64.
fun qtKitDir(abi: String): String = "$qtDir/android_${abi.replace('-', '_')}"

// ---- Android -----------------------------------------------------------------------------------------------------

android {
    namespace = "org.openvideotools.studio"
    compileSdk =
        libs.versions.compileSdk
            .get()
            .toInt()
    buildToolsVersion = libs.versions.buildTools.get()
    ndkVersion = libs.versions.ndk.get()

    defaultConfig {
        applicationId = ovtApplicationId
        minSdk =
            libs.versions.minSdk
                .get()
                .toInt()
        targetSdk =
            libs.versions.compileSdk
                .get()
                .toInt() // 36 (D5); the yearly target-level rule of §13.4.2
        versionCode = ovtVersionCodeBase // AAB: abiIndex 0; the per-ABI APKs get theirs in androidComponents below
        versionName = ovtVersion
        resValue("string", "app_name", ovtProductName)
        // No ndk.abiFilters: AGP rejects them next to splits.abi, and the Qt build produces only ovt.abis anyway.
    }

    // One APK per ABI for GitHub releases and the F-Droid repository; the Play AAB ignores splits (§13.4.2).
    splits {
        abi {
            isEnable = true
            reset()
            include(*ovtAbis.toTypedArray())
            isUniversalApk = false
        }
    }

    // `play` (Play Store AAB; Play Feature Delivery for the codec pack arrives with WI-0.053) and `foss` (F-Droid and
    // GitHub APKs, no Google libraries). Neither adds a dependency yet.
    flavorDimensions += "distribution"
    productFlavors {
        create("foss") { dimension = "distribution" }
        create("play") { dimension = "distribution" }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            isShrinkResources = false
            // WI-0.056 adds signing.gradle.kts with the release key from CI secrets; until then release is unsigned.
            signingConfig = signingConfigs.findByName("release")
        }
        debug {
            applicationIdSuffix = ".debug"
        }
    }

    packaging {
        jniLibs {
            // Uncompressed, page-aligned .so files that load straight from the APK (extractNativeLibs=false).
            useLegacyPackaging = false
            keepDebugSymbols += "**/libovt_plugin_*.so"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions { jvmTarget.set(JvmTarget.JVM_17) }
}

androidComponents {
    onVariants { variant ->
        variant.outputs.forEach { output ->
            val abi = output.filters.find { it.filterType == FilterConfiguration.FilterType.ABI }?.identifier
            if (abi != null) {
                val index = ovtAbiIndex[abi] ?: throw GradleException("no versionCode abiIndex for ABI $abi")
                output.versionCode.set(ovtVersionCodeBase + index)
            }
        }
    }
}

// ---- Qt (§13.4.3) ------------------------------------------------------------------------------------------------

// The plugin runs `<kit>/bin/qt-cmake -S <repository> -B app/build/qt_generated/<repository dir name>` and then
// `ninja aar`. Without qtKitDir it picks the first android_* kit under qtPath as the primary ABI and builds the
// others through QT_ANDROID_BUILD_ALL_ABIS; QT_PATH_ANDROID_ABI_<abi> below names every kit explicitly.
// The SDK and NDK arguments come first: the plugin searches extraCMakeArguments for them by substring.
val qtCMakeArguments =
    listOf(
        "-DANDROID_SDK_ROOT=$sdkDir",
        "-DANDROID_NDK_ROOT=$ndkDir",
        "-DANDROID_PLATFORM=android-${libs.versions.minSdk.get()}",
        "-DANDROID_STL=c++_shared",
        "-DCMAKE_BUILD_TYPE=$ovtNativeBuildType",
        "-DQT_HOST_PATH=$qtDir/$qtHostKit",
        "-DQT_ANDROID_ABIS=${ovtAbis.joinToString(";")}",
        // The QML root is loaded by URL in MainActivity, so no generated QtQuickViewContent classes are needed.
        "-DQT_ANDROID_GENERATE_JAVA_QTQUICKVIEW_CONTENTS=OFF",
        // Toolchain chain of every ABI build: Qt's qt.toolchain.cmake -> cmake/toolchains/android-vcpkg.cmake (the
        // -ovt triplet from ANDROID_ABI) -> vcpkg -> the pinned NDK's android.toolchain.cmake.
        "-DQT_CHAINLOAD_TOOLCHAIN_FILE=$ovtRoot/cmake/toolchains/android-vcpkg.cmake",
        "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$ndkDir/build/cmake/android.toolchain.cmake",
        // §13.4.2: GUI shell on, CLI off. The renderer (WI-A.401), the aom plugin and the codec-pack plugins
        // (WI-0.053) join with the work items that put them in the APK; WI-0.050 ships the empty window only.
        "-DOVT_BUILD_GUI=ON",
        "-DOVT_BUILD_CLI=OFF",
        "-DOVT_BUILD_TESTS=OFF",
        "-DOVT_BUILD_PYTHON=OFF",
        "-DOVT_BUILD_RENDER=OFF",
        "-DOVT_BUILD_PLUGIN_AOM=OFF",
        "-DOVT_BUILD_PLUGIN_DE265=OFF",
        "-DOVT_BUILD_PLUGIN_FFMPEG=OFF",
        "-DOVT_BUILD_PLUGIN_HM=OFF",
        "-DOVT_BUILD_METRIC_XPSNR=OFF",
        "-DOVT_PRODUCT_NAME=$ovtProductName",
    ) +
        ovtAbis.map { "-DQT_PATH_ANDROID_ABI_$it=${qtKitDir(it)}" }

// The secondary-ABI builds start from Qt's own arguments; these are the user settings they must see as well.
val qtForwardedVariables =
    listOf(
        "ANDROID_PLATFORM",
        "ANDROID_STL",
        "QT_CHAINLOAD_TOOLCHAIN_FILE",
        "VCPKG_CHAINLOAD_TOOLCHAIN_FILE",
        "OVT_BUILD_GUI",
        "OVT_BUILD_CLI",
        "OVT_BUILD_TESTS",
        "OVT_BUILD_PYTHON",
        "OVT_BUILD_RENDER",
        "OVT_BUILD_PLUGIN_AOM",
        "OVT_BUILD_PLUGIN_DE265",
        "OVT_BUILD_PLUGIN_FFMPEG",
        "OVT_BUILD_PLUGIN_HM",
        "OVT_BUILD_METRIC_XPSNR",
        "OVT_PRODUCT_NAME",
    )

QtBuild {
    qtPath = file(qtDir ?: "qt-dir-not-set")
    projectPath = ovtRoot
    // The SDK's CMake package ships ninja next to cmake; otherwise the plugin searches PATH.
    sdkDir?.let { "$it/cmake/${libs.versions.cmake.get()}/bin" }?.takeIf { file("$it/ninja").exists() }?.let {
        ninjaPath = it
    }
    // Groovy compiles the String[] property's setter as varargs (setExtraCMakeArguments(String...)), which Kotlin does
    // not treat as a property setter: call it with the spread operator.
    setExtraCMakeArguments(
        *(qtCMakeArguments + "-DQT_ANDROID_MULTI_ABI_FORWARD_VARS=${qtForwardedVariables.joinToString(";")}")
            .toTypedArray(),
    )
}

// Fails early, with the fix, when the machine is not set up (otherwise the plugin stops with a bare null path).
val ovtCheckAndroidSetup by tasks.registering {
    group = "ovt"
    description = "Checks that the Android SDK, the pinned NDK and the Qt for Android kits are installed."
    val requiredDirs =
        buildMap {
            put("Android SDK (sdk.dir / ANDROID_SDK_ROOT)", sdkDir)
            put("NDK ${libs.versions.ndk.get()}", ndkDir)
            put("Qt host kit ($qtHostKit)", qtDir?.let { "$it/$qtHostKit" })
            ovtAbis.forEach { abi -> put("Qt for Android kit $abi", qtDir?.let { qtKitDir(abi) }) }
        }
    doLast {
        val missing = requiredDirs.filter { (_, path) -> path == null || !File(path).isDirectory }
        if (missing.isNotEmpty()) {
            throw GradleException(
                missing.entries.joinToString(
                    separator = "\n",
                    prefix = "Android build host not set up; run tools/bootstrap_android.sh (chapter 13 §13.9.1):\n",
                ) { (what, path) -> "  missing $what: ${path ?: "not configured"}" },
            )
        }
    }
}

// The plugin hooks only the task names of a single-variant project (assembleDebug, compileDebugKotlin, ...); with
// the foss/play flavours none of those exist, so every variant's preBuild waits for the Qt build instead.
tasks.named("QtBuildTask") { dependsOn(ovtCheckAndroidSetup) }
tasks.named("preBuild") { dependsOn("QtBuildTask") }
// QtBuildTask declares projectPath (the repository root) as an @InputDirectory, and that tree holds this project's
// build/ directory, so every task writing under build/ counts as a producer of its input. Gradle accepts that as long
// as the two are ordered (MissingTaskDependencyDetector: any order in either direction), so the native build goes
// first. Tasks that do not hang off preBuild, such as buildKotlinToolingMetadata, otherwise fail validation.
val ovtNativeBuildDeps = setOf("QtBuildTask", ovtCheckAndroidSetup.name)
tasks.matching { it.name !in ovtNativeBuildDeps }.configureEach { mustRunAfter("QtBuildTask") }

// ---- Dependencies ------------------------------------------------------------------------------------------------

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.activity.ktx)
    // The AAR of the Qt build. Qt writes it next to the ovt_studio target, in app/android-build-ovt_studio/ of the
    // build tree, not at the top where the plugin's own dependency looks (SPEC-ASSUMPTION(#355)). A
    // fileTree is resolved when the classpath is, after preBuild -> QtBuildTask has produced the file.
    implementation(
        fileTree(layout.buildDirectory.dir("qt_generated/${ovtRoot.name}/app/android-build-ovt_studio")) {
            include("ovt_studio.aar")
        },
    )
}

// Input of the licence gate (chapter 15 §15.4.2: --gradle android/app/build/reports/dependency-license/index.json).
licenseReport {
    configurations = arrayOf("fossReleaseRuntimeClasspath", "playReleaseRuntimeClasspath")
    renderers = arrayOf<ReportRenderer>(JsonReportRenderer("index.json"))
}
