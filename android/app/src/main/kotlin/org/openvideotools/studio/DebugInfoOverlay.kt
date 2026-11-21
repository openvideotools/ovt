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
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updateLayoutParams

/**
 * Debug-only overlay of WI-0.051: an "Open file…" button (the Qt shell has no file UI until WI-A.450) and the JSON
 * of the picked file from [OvtNative.infoJson]. [MainActivity] adds it only to debuggable builds.
 *
 * WI-A.452 adds the Storage Access Framework flows of §11.5.6 that have no QML front end yet (SPEC-ASSUMPTION(#724)):
 * an "Export info…" button (`ACTION_CREATE_DOCUMENT`) and the recent list, one tappable row per entry.
 */
internal class DebugInfoOverlay(
    context: Context,
    onOpenClicked: () -> Unit,
    onExportClicked: () -> Unit,
    private val onRecentClicked: (RecentFiles.Entry) -> Unit,
) : LinearLayout(context) {
    private val info =
        TextView(context).apply {
            typeface = Typeface.MONOSPACE
            textSize = 12f
            setTextColor(Color.WHITE)
            contentDescription = "ovt-debug-info"
            text = EMPTY
        }
    private val recentList = LinearLayout(context).apply { orientation = VERTICAL }

    init {
        orientation = VERTICAL
        setBackgroundColor(Color.argb(160, 0, 0, 0))
        setPadding(PAD, PAD, PAD, PAD)
        addView(
            LinearLayout(context).apply {
                orientation = HORIZONTAL
                addView(
                    Button(context).apply {
                        text = OPEN_LABEL
                        setOnClickListener { onOpenClicked() }
                    },
                )
                addView(
                    Button(context).apply {
                        text = EXPORT_LABEL
                        setOnClickListener { onExportClicked() }
                    },
                )
            },
            LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT),
        )
        addView(recentList, LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        addView(
            ScrollView(context).apply { addView(info) },
            LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, MAX_INFO_HEIGHT),
        )
        // Edge-to-edge (the default from API 35): keep the overlay clear of the status bar and display cutout, or the
        // button sits under the status bar and taps on it (pick_file.py, AT1 on API 36) never reach it.
        ViewCompat.setOnApplyWindowInsetsListener(this) { view, insets ->
            val bars =
                insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.updateLayoutParams<MarginLayoutParams> {
                topMargin = bars.top
                leftMargin = bars.left
                rightMargin = bars.right
            }
            insets
        }
    }

    /** Shows the JSON for the file just opened. */
    fun show(json: String?) {
        info.text = json ?: EMPTY
    }

    /** Shows the recent list (§11.5.6): display name and codec, newest first; a tap reopens the entry. */
    fun showRecent(entries: List<RecentFiles.Entry>) {
        recentList.removeAllViews()
        if (entries.isEmpty()) return
        recentList.addView(label(RECENT_HEADING))
        entries.take(MAX_RECENT_ROWS).forEachIndexed { index, entry ->
            recentList.addView(
                label(if (entry.codec.isEmpty()) entry.displayName else "${entry.displayName}  ·  ${entry.codec}").apply {
                    contentDescription = "ovt-recent-$index"
                    setPadding(0, ROW_PAD, 0, ROW_PAD)
                    setOnClickListener { onRecentClicked(entry) }
                },
            )
        }
    }

    private fun label(value: String): TextView =
        TextView(context).apply {
            text = value
            textSize = 14f
            setTextColor(Color.WHITE)
        }

    /** Layout parameters that pin the overlay to the top-start corner of a FrameLayout. */
    fun topStartParams(): FrameLayout.LayoutParams =
        FrameLayout.LayoutParams(OVERLAY_WIDTH, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START)

    companion object {
        /** Text of the button; tools/android/uiautomator/pick_file.py finds it by this text. */
        const val OPEN_LABEL = "Open file…"
        const val EXPORT_LABEL = "Export info…"
        private const val RECENT_HEADING = "Recent files"
        private const val MAX_RECENT_ROWS = 5
        private const val ROW_PAD = 12
        private const val EMPTY = "no file"
        private const val PAD = 16
        private const val MAX_INFO_HEIGHT = 600
        private const val OVERLAY_WIDTH = 720
    }
}
