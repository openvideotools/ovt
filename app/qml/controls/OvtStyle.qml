// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Theme tokens of OVT Studio (chapter 11 §11.4.5, WI-1.450): the light and dark palettes, spacing and font
// sizes, chosen by Settings.theme ("system" follows the operating system's palette). Every text/background
// pair below is at least 4.5:1 (NFR-023, FR-GUI-024); app/tests/a11y/test_contrast.cpp (WI-0.568) checks them.
// Panels use these tokens and never literal colours. CMake flattens this file to the module root.

pragma Singleton

import QtQuick
import OvtStudio

QtObject {
    id: style

    readonly property SystemPalette systemPalette: SystemPalette {
        colorGroup: SystemPalette.Active
    }

    // "system" follows the operating system: a dark window colour means a dark theme.
    readonly property bool dark: Settings.theme === "dark"
                                 || (Settings.theme !== "light" && systemPalette.window.hslLightness < 0.5)

    // Surfaces
    readonly property color window: dark ? "#1b1f27" : "#f3f4f6"
    readonly property color panel: dark ? "#232834" : "#ffffff"
    readonly property color border: dark ? "#3a4150" : "#c7ccd4"
    // Text on window and panel
    readonly property color text: dark ? "#e8eaed" : "#1b1f24"
    readonly property color textMuted: dark ? "#aab2bf" : "#555d69"
    readonly property color textDisabled: dark ? "#9aa3b0" : "#5f6670"
    // Selection and focus. `highlight` carries white text (5.7:1), so it is #1565C0 in both themes; the translucent
    // row tints of §11.4.5 (#1E88E5 30 % in dark) are drawn by the panels over the panel colour.
    readonly property color accent: dark ? "#6aa9f5" : "#1565c0"
    readonly property color error: dark ? "#ff8a80" : "#b3261e"
    readonly property color highlight: "#1565c0"
    readonly property color highlightedText: "#ffffff"

    // Metrics (dip)
    readonly property int spacing: 6
    readonly property int radius: 3
    readonly property int fontSize: 13
    readonly property int minHitTarget: Qt.platform.os === "android" ? 48 : 24
}
