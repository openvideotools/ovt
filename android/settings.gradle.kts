// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Android project of OVT Studio (chapter 13 §13.4.1; WI-0.050). The codec pack module `:codecpack` joins when
// OVT_ANDROID_CODEC_PACK arrives with WI-0.053.

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "ovt-android"
include(":app")
