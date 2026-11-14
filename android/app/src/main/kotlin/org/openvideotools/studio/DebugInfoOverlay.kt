// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * Debug-only overlay of WI-0.051: an "Open file…" button (the Qt shell has no file UI until WI-A.450) and the JSON
 * of the picked file from [OvtNative.infoJson]. [MainActivity] adds it only to debuggable builds.
 */
internal class DebugInfoOverlay(
    context: Context,
    onOpenClicked: () -> Unit,
) : LinearLayout(context) {
    private val info =
        TextView(context).apply {
            typeface = Typeface.MONOSPACE
            textSize = 12f
            setTextColor(Color.WHITE)
            contentDescription = "ovt-debug-info"
            text = EMPTY
        }

    init {
        orientation = VERTICAL
        setBackgroundColor(Color.argb(160, 0, 0, 0))
        setPadding(PAD, PAD, PAD, PAD)
        addView(
            Button(context).apply {
                text = OPEN_LABEL
                setOnClickListener { onOpenClicked() }
            },
            LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT),
        )
        addView(
            ScrollView(context).apply { addView(info) },
            LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, MAX_INFO_HEIGHT),
        )
    }

    /** Shows the JSON for the file just opened. */
    fun show(json: String?) {
        info.text = json ?: EMPTY
    }

    /** Layout parameters that pin the overlay to the top-start corner of a FrameLayout. */
    fun topStartParams(): FrameLayout.LayoutParams =
        FrameLayout.LayoutParams(OVERLAY_WIDTH, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START)

    companion object {
        /** Text of the button; tools/android/uiautomator/pick_file.py finds it by this text. */
        const val OPEN_LABEL = "Open file…"
        private const val EMPTY = "no file"
        private const val PAD = 16
        private const val MAX_INFO_HEIGHT = 600
        private const val OVERLAY_WIDTH = 720
    }
}
