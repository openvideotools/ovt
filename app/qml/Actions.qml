// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The central Actions object of the shell (chapter 11 §11.2.3, WI-1.450). The menus and the toolbar are built from
// `menus` below; an action is enabled only when `enabledIds` lists it, so a menu item whose feature does not
// exist yet is visible but disabled. Later work items enable their actions by adding the id to `enabledIds`
// and handling it in `trigger()`. The menu text is the specification's wording (§11.2.3), untranslated.

import QtQuick
import QtQuick.Controls
import OvtStudio

QtObject {
    id: actions

    // The three theme actions are mutually exclusive, so clicking the active theme keeps its check mark.
    readonly property ActionGroup themeGroup: ActionGroup {
        exclusive: true
    }

    // Ids that do something so far: Quit, the three themes and Cancel all jobs (WI-1.453).
    readonly property var enabledIds: ["file.quit", "analysis.cancelAll", "view.theme.system", "view.theme.light", "view.theme.dark"]

    function isEnabled(id) {
        return enabledIds.indexOf(id) >= 0
    }

    function isChecked(id) {
        if (id.indexOf("view.theme.") === 0)
            return Settings.theme === id.substring("view.theme.".length)
        return false
    }

    function trigger(id) {
        if (id === "file.quit")
            Qt.quit()
        else if (id === "analysis.cancelAll")
            JobsModel.cancelAll()
        else if (id.indexOf("view.theme.") === 0)
            Settings.theme = id.substring("view.theme.".length)
    }

    // Entries: {id, text, shortcut, checkable}, {separator: true} or {text, items: [...]} for a submenu.
    function item(id, text, shortcut, checkable) {
        return {"id": id, "text": text, "shortcut": shortcut || "", "checkable": checkable === true,
                "group": id.indexOf("view.theme.") === 0 ? "theme" : ""}
    }
    readonly property var separator: ({"separator": true})

    readonly property var menus: [
        {"text": "File", "items": [
            item("file.open", "Open…", "Ctrl+O"),
            item("file.openReference", "Open Reference…", "Ctrl+Shift+O"),
            {"text": "Open Recent", "items": [item("file.recent.none", "No recent files")]},
            item("file.newWindow", "New Window", "Ctrl+N"),
            separator,
            item("file.save", "Save Project", "Ctrl+S"),
            item("file.saveAs", "Save Project As…"),
            item("file.close", "Close File", "Ctrl+W"),
            separator,
            {"text": "Export", "items": [
                item("file.export.framePng", "Frame as PNG (SDR/HDR)…"),
                item("file.export.plane", "Plane as PGM/Y4M…"),
                item("file.export.statisticsCsv", "Statistics CSV…"),
                item("file.export.metrics", "Metrics CSV/JSON…"),
                item("file.export.syntaxJson", "Syntax as JSON…"),
                item("file.export.containerJson", "Container as JSON…"),
                item("file.export.unitBytes", "Selected Unit bytes…")]},
            separator,
            item("file.quit", "Quit", "Ctrl+Q")]},
        {"text": "Edit", "items": [
            item("edit.copy", "Copy", "Ctrl+C"),
            item("edit.copyJson", "Copy as JSON", "Ctrl+Shift+C"),
            item("edit.find", "Find…", "Ctrl+F"),
            item("edit.goTo", "Go to…", "Ctrl+G"),
            separator,
            item("edit.preferences", "Preferences…", "Ctrl+,")]},
        {"text": "View", "items": [
            {"text": "Layout", "items": [
                item("view.layout.analyze", "Analyze"),
                item("view.layout.syntax", "Syntax"),
                item("view.layout.quality", "Quality"),
                item("view.layout.hdr", "HDR"),
                item("view.layout.compare", "Compare"),
                separator,
                item("view.layout.save", "Save current…"),
                item("view.layout.manage", "Manage…")]},
            {"text": "Panels", "items": [item("view.panels.none", "No panels yet")]},
            item("view.pictureOnly", "Picture only", "Ctrl+Shift+F"),
            item("view.dualView", "Dual view", "Ctrl+D", true),
            {"text": "Arrangement", "items": [
                item("view.arrangement.horizontal", "Horizontal", "", true),
                item("view.arrangement.vertical", "Vertical", "", true)]},
            item("view.fullScreen", "Full screen", "F"),
            item("view.order", "Decode order / Display order", "O"),
            {"text": "Zoom", "items": [
                item("view.zoom.in", "In", "+"),
                item("view.zoom.out", "Out", "-"),
                item("view.zoom.fit", "Fit", "R"),
                item("view.zoom.100", "100 %", "Ctrl+1"),
                item("view.zoom.200", "200 %", "Ctrl+2"),
                item("view.zoom.400", "400 %", "Ctrl+4")]},
            {"text": "Plane", "items": [
                item("view.plane.yuv", "YUV", "1"),
                item("view.plane.y", "Y", "2"),
                item("view.plane.u", "U", "3"),
                item("view.plane.v", "V", "4"),
                item("view.plane.gray", "Gray", "5")]},
            item("view.hdrOutput", "HDR output", "Ctrl+H", true),
            {"text": "Theme", "items": [
                item("view.theme.system", "System", "", true),
                item("view.theme.light", "Light", "", true),
                item("view.theme.dark", "Dark", "", true)]}]},
        {"text": "Overlays", "items": [
            {"text": "Modes", "items": [
                item("overlays.mode.f1", "Coding flow / partitions", "F1"),
                item("overlays.mode.f2", "Prediction", "F2"),
                item("overlays.mode.f3", "Motion vectors", "F3"),
                item("overlays.mode.f4", "Transform / residual", "F4"),
                item("overlays.mode.f5", "QP map", "F5"),
                item("overlays.mode.f6", "Bits heatmap", "F6"),
                item("overlays.mode.f7", "Reference indices", "F7"),
                item("overlays.mode.f8", "Loop filter", "F8"),
                item("overlays.mode.f9", "In-loop filter params (SAO / CDEF / LR)", "F9"),
                item("overlays.mode.f10", "Metrics heatmap", "F10"),
                item("overlays.mode.f11", "Plain picture", "F11"),
                item("overlays.mode.f12", "HDR false colour", "F12")]},
            separator,
            item("overlays.picture", "Picture", "P", true),
            item("overlays.info", "Info labels", "I", true),
            item("overlays.grid", "Grid", "G", true),
            item("overlays.motionVectors", "Motion vectors", "M", true),
            item("overlays.heatmap", "Heatmap", "T", true),
            item("overlays.legend", "Legend", "L", true),
            item("overlays.hex", "Hex", "H", true),
            item("overlays.dec", "Dec", "D", true),
            {"text": "Heatmap colour map", "items": [
                item("overlays.colourMap.viridis", "Viridis", "", true)]},
            separator,
            item("overlays.colourBlindSafe", "Colour-blind-safe palette", "", true)]},
        {"text": "Navigate", "items": [
            item("navigate.next", "Next frame", "Right"),
            item("navigate.previous", "Previous frame", "Left"),
            item("navigate.next10", "Forward 10 frames", "Shift+Right"),
            item("navigate.previous10", "Back 10 frames", "Shift+Left"),
            {"text": "Next frame of type", "items": [
                item("navigate.nextType.i", "I"),
                item("navigate.nextType.p", "P"),
                item("navigate.nextType.b", "B"),
                item("navigate.nextType.temporalId", "Same temporal id")]},
            {"text": "Previous frame of type", "items": [
                item("navigate.previousType.i", "I"),
                item("navigate.previousType.p", "P"),
                item("navigate.previousType.b", "B"),
                item("navigate.previousType.temporalId", "Same temporal id")]},
            item("navigate.nextKey", "Next I/Key frame", "Ctrl+Right"),
            item("navigate.previousKey", "Previous I/Key frame", "Ctrl+Left"),
            separator,
            item("navigate.first", "First frame", "Home"),
            item("navigate.last", "Last frame", "End"),
            item("navigate.nextError", "Next error", "Ctrl+Down"),
            item("navigate.previousError", "Previous error", "Ctrl+Up"),
            item("navigate.nextBookmark", "Next bookmark", "N"),
            item("navigate.previousBookmark", "Previous bookmark", "Shift+N"),
            separator,
            item("navigate.playPause", "Play/Pause", "Space"),
            {"text": "Playback rate", "items": [
                item("navigate.rate.025", "0.25×", "", true),
                item("navigate.rate.05", "0.5×", "", true),
                item("navigate.rate.1", "1×", "", true),
                item("navigate.rate.2", "2×", "", true)]}]},
        {"text": "Analysis", "items": [
            item("analysis.frame", "Analyse current frame", "A"),
            item("analysis.gop", "Analyse GOP", "Shift+A"),
            item("analysis.range", "Analyse range…"),
            item("analysis.cancelAll", "Cancel all jobs", "Ctrl+."),
            separator,
            item("analysis.metrics", "Metrics job…", "Ctrl+M"),
            item("analysis.hdrScan", "HDR scan range…"),
            item("analysis.clearCache", "Clear cache for file")]},
        {"text": "Compare", "items": [
            {"text": "Mode", "items": [
                item("compare.single", "Single"),
                item("compare.sideBySide", "Side by side"),
                item("compare.splitVertical", "Split vertical"),
                item("compare.splitHorizontal", "Split horizontal"),
                item("compare.overlay", "Overlay"),
                item("compare.difference", "Difference"),
                item("compare.flicker", "Flicker")]},
            item("compare.swap", "Swap A/B", "X"),
            {"text": "Difference gain", "items": [
                item("compare.gain.1", "1", "", true),
                item("compare.gain.2", "2", "", true),
                item("compare.gain.4", "4", "", true),
                item("compare.gain.8", "8", "", true),
                item("compare.gain.16", "16", "", true),
                item("compare.gain.32", "32", "", true)]},
            item("compare.falseColour", "False colour difference", "", true),
            item("compare.lockPanZoom", "Lock pan/zoom", "", true)]},
        {"text": "Help", "items": [
            item("help.keyboardMap", "Keyboard map", "?"),
            item("help.specReferences", "Spec references"),
            item("help.about", "About"),
            item("help.diagnosticsBundle", "Report diagnostics bundle…")]}
    ]
}
