// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The empty state of a panel (chapter 11 §11.8, WI-1.464): one centred sentence in the muted text colour. Every panel
// shows this control while it has nothing to display; the sentences are the specification's, via EmptyState.text(id).

import QtQuick
import QtQuick.Controls
import OvtStudio

Item {
    id: emptyState

    property string text: ""

    // The §11.8 sentence of a dock panel; "" for a panel that is never empty (Display Capabilities) or has none.
    function textFor(panelId) {
        switch (panelId) {
        case "timeline": return qsTr("Open a file to see its frames.")
        case "picture": return qsTr("Decoding first frame…")
        case "syntax": return qsTr("Select a unit in the Unit list or a frame.")
        case "hex": return qsTr("Select bytes, a unit, a box or a syntax field.")
        case "container": return qsTr("This file has no container (elementary stream).")
        case "selection_info": return qsTr("Click a block in the Picture (analysis required: press A).")
        case "block_samples": return qsTr("Select a block.")
        case "ref_graph": return qsTr("Index in progress…")
        case "statistics": return qsTr("No analysed frames in scope — press A.")
        case "metrics": return qsTr("Open a reference (Ctrl+Shift+O) or run a no-reference metric.")
        case "hdr": return qsTr("SDR content — measurements use 203 cd/m² reference white.")
        case "diagnostics": return qsTr("No diagnostics.")
        case "bookmarks": return qsTr("Press B to bookmark the current frame.")
        default: return ""
        }
    }

    Accessible.role: Accessible.StaticText
    Accessible.name: text

    Label {
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * OvtStyle.spacing, 480)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: emptyState.text
        color: OvtStyle.textMuted
    }
}
