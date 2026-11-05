// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Main window of OVT Studio. WI-0.019 shows an empty window; WI-1.450 replaces the content with the
// application shell (chapter 11). The title comes from main.cpp (OVT_PRODUCT_NAME, chapter 15 §15.7.4), so
// no product name is written here.

import QtQuick

Window {
    id: root

    width: 1280
    height: 800
    minimumWidth: 640
    minimumHeight: 400
    visible: true
    color: "#1f2a44"
}
