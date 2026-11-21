// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The HDR panel and the Display Capabilities panel (WI-1.462; chapter 11 §11.3.14, §11.3.15):
//  - AT1: with the HdrMetadata of syn_h265_main10_pq_hdr_mismatch (container colr BT.709, SPS VUI BT.2020/PQ, the
//    core's DEC-HDR-0001), the Metadata tab shows a ⚠ row with that diagnostic.
//  - AT2: on an SDR display the HDR output toggle is disabled and its tooltip is the probe's reason.
//  - SDR content, the Display tab controls writing HdrView, the Dolby label, and the DisplayCaps table, re-probe,
//    test pattern and the status-bar text.

import QtQuick
import QtQuick.Controls
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "HdrMetadataModel"
    when: windowShown

    Component {
        id: hdrComponent

        HdrPanel {
            width: 640
            height: 360
        }
    }

    Component {
        id: capsComponent

        DisplayCapsPanel {
            width: 640
            height: 400
        }
    }

    // The HdrMetadata table the core returns for syn_h265_main10_pq_hdr_mismatch (FlatBuffers JSON shape).
    readonly property var mismatch: ({
        "track_id": 1,
        "kind": "HDR10",
        "color": {"primaries": 1, "transfer": 1, "matrix": 1, "full_range": 0,
                  "mdcv_primaries": [[34000, 16000], [13250, 34500], [7500, 3000]],
                  "mdcv_white_point": [15635, 16450], "mdcv_max_luminance": 10000000, "mdcv_min_luminance": 1,
                  "max_cll": 1000, "max_fall": 400},
        "signals": [
            {"source": "colr", "box_offset": 1234, "present": true, "summary": "nclc 1/1/1"},
            {"source": "sps_vui", "unit_index": 1, "present": true, "summary": "BT.2020 / PQ / BT.2020-NCL"},
            {"source": "sei_137", "unit_index": 3, "present": true, "summary": "MDCV"},
            {"source": "sei_144", "unit_index": 4, "present": true, "summary": "CLL 1000/400"}],
        "box_vs_bitstream_mismatch": true,
        "diagnostics": [{"severity": "Warning", "code": "DEC-HDR-0001",
                         "message": "colour signalling differs between container (BT.709/BT.709) and bitstream (BT.2020/PQ)"}]
    })

    function cleanup() {
        HdrMetadataModel.clear()
        HdrView.reset()
        DisplayCaps.testPattern = false
        DisplayCaps.outputEncoding = "Srgb8"
    }

    // ListView delegates are not found by findChild before they exist; walk the content item instead.
    function rowItem(list, index) {
        const items = list.contentItem.children
        for (let i = 0; i < items.length; ++i) {
            if (items[i].objectName === "metadataRow" + index)
                return items[i]
        }
        return null
    }

    function test_mismatchShowsWarningRow() {
        verify(HdrMetadataModel.setFromJson(mismatch))
        const panel = createTemporaryObject(hdrComponent, testCase)
        verify(panel !== null)
        compare(panel.currentTab, 0)  // the Metadata tab opens first
        compare(findChild(panel, "hdrKind").text, "HDR10")
        compare(findChild(panel, "metadataTab").text, "Metadata ⚠")

        const row = HdrMetadataModel.findCode("DEC-HDR-0001")
        verify(row >= 0)
        const list = findChild(panel, "metadataList")
        compare(list.count, HdrMetadataModel.rowCount())
        list.positionViewAtIndex(row, ListView.Contain)
        let item = null
        tryVerify(() => (item = rowItem(list, row)) !== null)
        verify(item.warning)
        compare(item.code, "DEC-HDR-0001")
        compare(item.value, mismatch.diagnostics[0].message)
        // The icon shows exactly when the row is a ⚠ row (the test window keeps the panel itself hidden).
        compare(findChild(item, "warningIcon").text, "⚠")

        // The colour rows carry the same ⚠, with the container as provenance.
        const primaries = HdrMetadataModel.row(1)
        compare(primaries.item, "Colour primaries")
        compare(primaries.provenance, "container colr")
        verify(primaries.warning)
    }

    function test_sdrDisplayDisablesHdrOutput() {
        DisplayCaps.setCaps({"hdr_capable": false, "reason": "display has no EDR headroom", "source": "NSScreen",
                             "primaries_name": "BT.709"})
        const panel = createTemporaryObject(hdrComponent, testCase)
        findChild(panel, "hdrTabs").currentIndex = 2
        compare(panel.currentTab, 2)
        const toggle = findChild(panel, "hdrOutputToggle")
        verify(toggle !== null)
        verify(!toggle.enabled)
        verify(!toggle.checked)
        compare(toggle.toolTipText, "display has no EDR headroom")
        compare(toggle.parent.ToolTip.text, "display has no EDR headroom")

        // An HDR-capable display enables it, and the toggle writes the request.
        DisplayCaps.setCaps({"hdr_capable": true, "potential_headroom": 16, "source": "NSScreen"})
        verify(toggle.enabled)
        toggle.toggle()
        toggle.toggled()
        verify(HdrView.hdrOutput)
    }

    function test_displayControlsWriteViewState() {
        const panel = createTemporaryObject(hdrComponent, testCase)
        findChild(panel, "hdrTabs").currentIndex = 2
        const toneMap = findChild(panel, "toneMapCombo")
        compare(toneMap.currentIndex, 0)  // Auto
        verify(!findChild(panel, "clipNits").enabled)
        toneMap.currentIndex = 4
        toneMap.activated(4)
        compare(HdrView.toneMap, 4)  // Clip
        verify(findChild(panel, "clipNits").enabled)
        const falseColor = findChild(panel, "falseColorToggle")
        falseColor.toggle()
        falseColor.toggled()
        verify(HdrView.falseColor)
        const dither = findChild(panel, "ditherToggle")
        verify(dither.checked)
        compare(findChild(panel, "dolbyLabel").text, "—")
        HdrMetadataModel.setFromJson({"kind": "DolbyVision", "dv_profile": 8, "dv_bl_compat_id": 1, "dv_level": 6})
        compare(findChild(panel, "dolbyLabel").text, "Dolby Vision profile 8.1, level 6")
    }

    function test_sdrContentAndDeclaredValues() {
        HdrMetadataModel.setFromJson({"kind": "SDR", "color": {"primaries": 1, "transfer": 1, "matrix": 1}})
        const panel = createTemporaryObject(hdrComponent, testCase)
        compare(findChild(panel, "hdrKind").text, "SDR (BT.709)")
        compare(findChild(panel, "metadataTab").text, "Metadata")
        findChild(panel, "hdrTabs").currentIndex = 1
        compare(findChild(panel, "declaredMaxCll").text, "—")
        HdrMetadataModel.setFromJson(mismatch)
        compare(findChild(panel, "declaredMaxCll").text, "1000 cd/m²")
        compare(findChild(panel, "declaredMaxFall").text, "400 cd/m²")
        verify(!findChild(panel, "scanRangeButton").enabled)
    }

    function test_displayCapsPanel() {
        DisplayCaps.setCaps({"hdr_capable": false, "reason": "X11 session", "source": "none"})
        const panel = createTemporaryObject(capsComponent, testCase)
        const list = findChild(panel, "capsList")
        compare(list.count, 13)
        compare(DisplayCaps.row(1).value, "no (X11 session)")
        compare(DisplayCaps.row(9).value, "— (X11 session)")  // source "none" is unknown
        compare(DisplayCaps.statusText, "SDR")
        verify(DisplayCaps.reportText().indexOf("HDR capable: no (X11 session)") >= 0)

        const pattern = findChild(panel, "testPatternButton")
        pattern.toggle()
        pattern.toggled()
        verify(DisplayCaps.testPattern)

        const before = DisplayCaps.lastProbe
        findChild(panel, "reprobeButton").clicked()
        verify(DisplayCaps.probing)
        tryVerify(() => !DisplayCaps.probing)
        verify(DisplayCaps.lastProbe >= before)

        DisplayCaps.outputEncoding = "Pq10"
        compare(DisplayCaps.statusText, "PQ pass-through")
    }
}
