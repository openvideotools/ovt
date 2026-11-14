// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.455b: the Timeline panel and chart over a synthetic 300-frame index (context property `testIndex`, built in
// tst_main.cpp): click selects a frame, drag selects a range with its status line, wheel zooms, double-click fits,
// the order toggle keeps the selection on the same frame.

import QtQuick
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "Timeline"
    width: 800
    height: 300
    visible: true
    when: windowShown

    Component {
        id: panelComponent

        TimelinePanel {
            width: 800
            height: 300
            index: testIndex
        }
    }

    function initTestCase() {
        testSetup.attachSelection()
    }

    function createPanel() {
        testIndex.clearRange()
        testIndex.displayOrder = false
        Selection.clearSelection("test")
        const panel = createTemporaryObject(panelComponent, testCase)
        verify(panel !== null)
        waitForRendering(panel.chart)
        return panel
    }

    function test_clickSelectsFrame() {
        const panel = createPanel()
        const chart = panel.chart
        // Zoomed to fit, 300 frames across the chart: the frame at a given x.
        const x = chart.width * (120.5 / 300)
        mouseClick(chart, x, chart.height / 2)
        compare(Selection.frameDecodeIndex, 120)
        compare(Selection.origin, "timeline")
        compare(chart.selectedFrame, 120)
    }

    function test_dragSelectsRangeAndShowsStatus() {
        const panel = createPanel()
        const chart = panel.chart
        mouseDrag(chart, chart.width * (120.5 / 300), chart.height / 2, chart.width * (120 / 300), 0)
        verify(testIndex.hasRange)
        compare(testIndex.rangeFirst, 120)
        compare(testIndex.rangeLast, 240)
        compare(testIndex.rangeStats.frames, 121)
        verify(panel.rangeText().indexOf("121 frames") >= 0)
    }

    function test_wheelZoomsAndDoubleClickFits() {
        const panel = createPanel()
        const chart = panel.chart
        compare(chart.viewCount, 300)
        mouseWheel(chart, chart.width / 2, chart.height / 2, 0, 120)
        fuzzyCompare(chart.viewCount, 300 / 1.25, 0.001)
        mouseDoubleClickSequence(chart, chart.width / 2, chart.height / 2)
        compare(chart.viewCount, 300)
    }

    function test_orderToggleKeepsSelectedFrame() {
        const panel = createPanel()
        Selection.selectFrame(7, -1, "test")
        testIndex.displayOrder = true
        compare(testIndex.positionOf(7), testIndex.displayIndexOf(7))
        compare(panel.chart.selectedFrame, 7)
    }

    function test_arrowKeysStepFrames() {
        const panel = createPanel()
        Selection.selectFrame(10, -1, "test")
        panel.chart.forceActiveFocus()
        keyClick(Qt.Key_Right)
        compare(Selection.frameDecodeIndex, 11)
        keyClick(Qt.Key_Left)
        keyClick(Qt.Key_Left)
        compare(Selection.frameDecodeIndex, 9)
    }

    function test_tooltipText() {
        const panel = createPanel()
        const tip = panel.chart.frameTooltip(5)
        verify(tip.indexOf("#") === 0)
        verify(tip.indexOf("QP") > 0)
        compare(panel.chart.frameTooltip(-1), "")
    }

    function test_gopMarkersAndRefArrows() {
        const panel = createPanel()
        const chart = panel.chart
        // 300 frames with an I frame every 60: five markers, all labelled "I".
        compare(chart.gopMarkers.length, 5)
        compare(chart.gopMarkers[0].label, "I")
        verify(chart.gopMarkers[1].x > chart.gopMarkers[0].x)
        // Zoomed in on the first 20 frames only the first marker remains.
        mouseWheel(chart, 1, chart.height / 2, 0, 120 * 12)
        verify(chart.viewCount < 60)
        compare(chart.gopMarkers.length, 1)
        compare(chart.refArrows, false)
        chart.refArrows = true
        compare(chart.refArrows, true)
        waitForRendering(chart)
    }
}
