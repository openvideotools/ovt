// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-2.484: the Reference Graph panel over the synthetic 300-frame index (context property `testIndex`, built in
// tst_main.cpp). A click on a node changes Selection.frame to that node's frame, and the panel outlines the selected
// frame back. The layout itself is covered by test_ref_graph_layout.cpp.

import QtQuick
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "RefGraph"
    width: 800
    height: 300
    visible: true
    when: windowShown

    Component {
        id: panelComponent

        RefGraphPanel {
            width: 800
            height: 300
            index: testIndex
        }
    }

    function initTestCase() {
        testSetup.attachSelection()
    }

    function createPanel() {
        testIndex.displayOrder = false
        Selection.clearSelection("test")
        const panel = createTemporaryObject(panelComponent, testCase)
        verify(panel !== null)
        waitForRendering(panel.graph)
        return panel
    }

    function test_windowHoldsFramesAndDeclaredRefs() {
        const panel = createPanel()
        const graph = panel.graph
        compare(graph.nodeCount, 129)  // ±64 around the first frame, clamped to the stream
        // The synthetic stream: every inter frame refers to the previous frame, the I frames to nothing.
        let refs = 0
        for (const a of graph.arrows()) {
            compare(a.from - a.to, 1)
            ++refs
        }
        compare(refs, graph.nodeCount - 3)  // frames 0, 60 and 120 are intra
    }

    function test_clickSelectsFrame() {
        const panel = createPanel()
        const graph = panel.graph
        const pos = graph.nodePosition(5)
        verify(pos.x !== undefined)
        mouseClick(graph, pos.x, pos.y)
        compare(Selection.frameDecodeIndex, 5)
        compare(Selection.origin, "ref_graph")
        compare(graph.selectedFrame, 5)
    }

    function test_selectionOutsideWindowRecentres() {
        const panel = createPanel()
        const graph = panel.graph
        Selection.selectFrame(250, -1, "test")
        verify(graph.windowFirst > 100)
        verify(graph.nodePosition(250).x !== undefined)
    }

    function test_noDpbForThisIndex() {
        const panel = createPanel()
        verify(!panel.graph.dpbKnown)
        Selection.selectFrame(3, -1, "test")
        verify(panel.dpbText().indexOf("not derivable") >= 0)
    }
}
