// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drop targets while a tab is dragged (chapter 11 §11.2.2; WI-1.451), highlighted in #39CCCC at 40 %:
// - the left, right or bottom edge of the workspace: a new group at the end of that area (shown if hidden);
// - the outer quarter of a group along its area's axis: a new group before or after it in the same area;
// - the rest of a group (and all of the Center group): a tab of that group;
// - outside the window (desktop): a floating window.

import QtQuick
import OvtStudio

Item {
    id: indicator

    required property Item workspace
    property string panelId: ""
    // {kind: "tab" | "group" | "float", group, area, position, rect: {x, y, width, height}} or null.
    property var target: null
    readonly property real edgeBand: 32

    function begin(id, scenePoint) {
        panelId = id
        move(scenePoint)
    }

    function move(scenePoint) {
        target = targetAt(scenePoint)
    }

    // Applies the drop; false when there was no target or the model refused it (e.g. the Picture leaving Center).
    function finish() {
        const t = target
        const id = panelId
        panelId = ""
        target = null
        if (!t)
            return false
        if (t.kind === "float")
            return DockModel.floatPanel(id)
        if (t.kind === "group")
            return DockModel.movePanelToNewGroup(id, t.area, t.position)
        return DockModel.movePanelToGroup(id, t.group, 1 << 20)
    }

    function targetAt(scenePoint) {
        const p = workspace.mapFromItem(null, scenePoint.x, scenePoint.y)
        const w = workspace.width
        const h = workspace.height
        if (p.x < 0 || p.y < 0 || p.x > w || p.y > h)
            return DockModel.floatingSupported ? {"kind": "float", "rect": null} : null
        if (p.x < edgeBand)
            return edge("left", 0, 0, edgeBand * 2, h)
        if (p.x > w - edgeBand)
            return edge("right", w - edgeBand * 2, 0, edgeBand * 2, h)
        if (p.y > h - edgeBand)
            return edge("bottom", 0, h - edgeBand * 2, w, edgeBand * 2)
        for (const g of workspace.groupItems) {
            if (!g.visible || g.Window.window !== workspace.Window.window)
                continue
            const local = g.mapFromItem(workspace, p.x, p.y)
            if (local.x < 0 || local.y < 0 || local.x > g.width || local.y > g.height)
                continue
            const r = workspace.mapFromItem(g, 0, 0, g.width, g.height)
            const rect = {"x": r.x, "y": r.y, "width": r.width, "height": r.height}
            if (g.areaName !== "center" && g.areaName !== "floating") {
                const horizontal = g.areaName === "bottom"
                const f = horizontal ? local.x / g.width : local.y / g.height
                if (f < 0.25 || f > 0.75) {
                    const after = f > 0.75
                    if (horizontal) {
                        rect.width /= 4
                        rect.x += after ? rect.width * 3 : 0
                    } else {
                        rect.height /= 4
                        rect.y += after ? rect.height * 3 : 0
                    }
                    return {"kind": "group", "area": g.areaName, "position": g.areaPosition + (after ? 1 : 0), "rect": rect}
                }
            }
            return {"kind": "tab", "group": g.groupIndex, "rect": rect}
        }
        return null
    }

    function edge(area, x, y, width, height) {
        return {"kind": "group", "area": area, "position": -1, "rect": {"x": x, "y": y, "width": width, "height": height}}
    }

    Rectangle {
        objectName: "dockDropHighlight"
        visible: indicator.target !== null && indicator.target.rect !== null
        x: visible ? indicator.target.rect.x : 0
        y: visible ? indicator.target.rect.y : 0
        width: visible ? indicator.target.rect.width : 0
        height: visible ? indicator.target.rect.height : 0
        color: Qt.rgba(0x39 / 255, 0xcc / 255, 0xcc / 255, 0.4)
    }
}
