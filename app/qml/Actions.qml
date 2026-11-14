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

    // Ids that do something so far: Quit, the three themes, Cancel all jobs (WI-1.453), the Jobs popover, Go to,
    // Preferences and the Keyboard map (WI-1.466), the dock actions (WI-1.451: presets, Reset Layout, Picture only,
    // area toggles, focus panel N and tab cycling; the last two are handled by DockWorkspace through activated()).
    readonly property var enabledIds: ["file.quit", "analysis.cancelAll", "view.theme.system", "view.theme.light", "view.theme.dark",
        "view.jobs", "navigate.addBookmark", "navigate.addComment", "navigate.nextBookmark", "navigate.previousBookmark", "edit.goTo", "edit.preferences", "help.keyboardMap",
        "view.layout.analyze", "view.layout.syntax", "view.layout.quality", "view.layout.hdr", "view.layout.compare",
        "view.resetLayout", "view.pictureOnly", "view.area.left", "view.area.right", "view.area.bottom",
        "view.nextTab", "view.previousTab", "view.focusPanel1", "view.focusPanel2", "view.focusPanel3",
        "view.focusPanel4", "view.focusPanel5", "view.focusPanel6", "view.focusPanel7", "view.focusPanel8",
        "view.focusPanel9"]

    readonly property var presetIds: ({"view.layout.analyze": "Analyze", "view.layout.syntax": "Syntax",
                                       "view.layout.quality": "Quality", "view.layout.hdr": "HDR",
                                       "view.layout.compare": "Compare"})

    // Emitted for every enabled action, from a menu or a shortcut (the shortcut layer calls trigger()). The windows
    // and dialogs that implement an action listen here; later work items add their actions the same way.
    signal activated(string id)

    // Go to handler of the panels (WI-1.455, WI-1.458): called with the parsed target ({kind, value}), returns "" when
    // it navigated or the reason ("no frame") when it did not, leaving the current frame unchanged.
    property var goToHandler: null

    // The bindings shown in the menus: the shortcut map is the Keymap (§11.4.3), never this file.
    function shortcutText(id) {
        return Keymap.display("action." + id)
    }

    function isEnabled(id) {
        // Every panel but the Picture can be closed and re-opened (FR-GUI-001).
        if (id.indexOf("view.panels.") === 0)
            return id !== "view.panels.picture"
        return enabledIds.indexOf(id) >= 0
    }

    function isChecked(id) {
        if (id.indexOf("view.theme.") === 0)
            return Settings.theme === id.substring("view.theme.".length)
        if (id.indexOf("view.panels.") === 0)
            return DockModel.openPanels.indexOf(id.substring("view.panels.".length)) >= 0
        if (id === "view.pictureOnly")
            return DockModel.pictureOnly
        return false
    }

    function trigger(id) {
        if (!isEnabled(id))
            return
        activated(id)
        if (id === "file.quit")
            Qt.quit()
        else if (id === "analysis.cancelAll")
            JobsModel.cancelAll()
        else if (id.indexOf("view.theme.") === 0)
            Settings.theme = id.substring("view.theme.".length)
        else if (presetIds[id] !== undefined)
            DockModel.applyPreset(presetIds[id])
        else if (id === "view.resetLayout")
            DockModel.resetLayout()
        else if (id === "view.pictureOnly")
            DockModel.pictureOnly = !DockModel.pictureOnly
        else if (id === "view.area.left")
            DockModel.leftVisible = !DockModel.leftVisible
        else if (id === "view.area.right")
            DockModel.rightVisible = !DockModel.rightVisible
        else if (id === "view.area.bottom")
            DockModel.bottomVisible = !DockModel.bottomVisible
        else if (id.indexOf("view.panels.") === 0)
            DockModel.togglePanel(id.substring("view.panels.".length))
    }

    // View ▸ Panels: one checkable entry per dock panel.
    function panelItems() {
        return DockModel.allPanels().map(p => item("view.panels." + p, DockModel.panelTitle(p), true))
    }

    // Entries: {id, text, shortcut, checkable}, {separator: true} or {text, items: [...]} for a submenu.
    function item(id, text, checkable) {
        return {"id": id, "text": text, "checkable": checkable === true,
                "group": id.indexOf("view.theme.") === 0 ? "theme" : ""}
    }
    readonly property var separator: ({"separator": true})

    readonly property var menus: [
        {"text": "File", "items": [
            item("file.open", "Open…"),
            item("file.openReference", "Open Reference…"),
            {"text": "Open Recent", "items": [item("file.recent.none", "No recent files")]},
            item("file.newWindow", "New Window"),
            separator,
            item("file.save", "Save Project"),
            item("file.saveAs", "Save Project As…"),
            item("file.close", "Close File"),
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
            item("file.quit", "Quit")]},
        {"text": "Edit", "items": [
            item("edit.copy", "Copy"),
            item("edit.copyJson", "Copy as JSON"),
            item("edit.find", "Find…"),
            item("edit.goTo", "Go to…"),
            separator,
            item("edit.preferences", "Preferences…")]},
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
            // FR-GUI-002 (chapter 4 wins over the §11.2.3 menu table, which does not list it).
            item("view.resetLayout", "Reset Layout"),
            {"text": "Panels", "items": panelItems()},
            item("view.pictureOnly", "Picture only", true),
            item("view.dualView", "Dual view", true),
            {"text": "Arrangement", "items": [
                item("view.arrangement.horizontal", "Horizontal", true),
                item("view.arrangement.vertical", "Vertical", true)]},
            item("view.fullScreen", "Full screen"),
            item("view.order", "Decode order / Display order"),
            {"text": "Zoom", "items": [
                item("view.zoom.in", "In"),
                item("view.zoom.out", "Out"),
                item("view.zoom.fit", "Fit"),
                item("view.zoom.100", "100 %"),
                item("view.zoom.200", "200 %"),
                item("view.zoom.400", "400 %")]},
            {"text": "Plane", "items": [
                item("view.plane.yuv", "YUV"),
                item("view.plane.y", "Y"),
                item("view.plane.u", "U"),
                item("view.plane.v", "V"),
                item("view.plane.gray", "Gray")]},
            item("view.hdrOutput", "HDR output", true),
            {"text": "Theme", "items": [
                item("view.theme.system", "System", true),
                item("view.theme.light", "Light", true),
                item("view.theme.dark", "Dark", true)]}]},
        {"text": "Overlays", "items": [
            {"text": "Modes", "items": [
                item("overlays.mode.f1", "Coding flow / partitions"),
                item("overlays.mode.f2", "Prediction"),
                item("overlays.mode.f3", "Motion vectors"),
                item("overlays.mode.f4", "Transform / residual"),
                item("overlays.mode.f5", "QP map"),
                item("overlays.mode.f6", "Bits heatmap"),
                item("overlays.mode.f7", "Reference indices"),
                item("overlays.mode.f8", "Loop filter"),
                item("overlays.mode.f9", "In-loop filter params (SAO / CDEF / LR)"),
                item("overlays.mode.f10", "Metrics heatmap"),
                item("overlays.mode.f11", "Plain picture"),
                item("overlays.mode.f12", "HDR false colour")]},
            separator,
            item("overlays.picture", "Picture", true),
            item("overlays.info", "Info labels", true),
            item("overlays.grid", "Grid", true),
            item("overlays.motionVectors", "Motion vectors", true),
            item("overlays.heatmap", "Heatmap", true),
            item("overlays.legend", "Legend", true),
            item("overlays.hex", "Hex", true),
            item("overlays.dec", "Dec", true),
            {"text": "Heatmap colour map", "items": [
                item("overlays.colourMap.viridis", "Viridis", true)]},
            separator,
            item("overlays.colourBlindSafe", "Colour-blind-safe palette", true)]},
        {"text": "Navigate", "items": [
            item("navigate.next", "Next frame"),
            item("navigate.previous", "Previous frame"),
            item("navigate.next10", "Forward 10 frames"),
            item("navigate.previous10", "Back 10 frames"),
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
            item("navigate.nextKey", "Next I/Key frame"),
            item("navigate.previousKey", "Previous I/Key frame"),
            separator,
            item("navigate.first", "First frame"),
            item("navigate.last", "Last frame"),
            item("navigate.nextError", "Next error"),
            item("navigate.previousError", "Previous error"),
            item("navigate.nextBookmark", "Next bookmark"),
            item("navigate.previousBookmark", "Previous bookmark"),
            separator,
            item("navigate.playPause", "Play/Pause"),
            {"text": "Playback rate", "items": [
                item("navigate.rate.025", "0.25×", true),
                item("navigate.rate.05", "0.5×", true),
                item("navigate.rate.1", "1×", true),
                item("navigate.rate.2", "2×", true)]}]},
        {"text": "Analysis", "items": [
            item("analysis.frame", "Analyse current frame"),
            item("analysis.gop", "Analyse GOP"),
            item("analysis.range", "Analyse range…"),
            item("analysis.cancelAll", "Cancel all jobs"),
            separator,
            item("analysis.metrics", "Metrics job…"),
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
            item("compare.swap", "Swap A/B"),
            {"text": "Difference gain", "items": [
                item("compare.gain.1", "1", true),
                item("compare.gain.2", "2", true),
                item("compare.gain.4", "4", true),
                item("compare.gain.8", "8", true),
                item("compare.gain.16", "16", true),
                item("compare.gain.32", "32", true)]},
            item("compare.falseColour", "False colour difference", true),
            item("compare.lockPanZoom", "Lock pan/zoom", true)]},
        {"text": "Help", "items": [
            item("help.keyboardMap", "Keyboard map"),
            item("help.specReferences", "Spec references"),
            item("help.about", "About"),
            item("help.diagnosticsBundle", "Report diagnostics bundle…")]}
    ]
}
