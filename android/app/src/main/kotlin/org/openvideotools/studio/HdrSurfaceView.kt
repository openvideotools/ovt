// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.Context
import android.util.AttributeSet
import android.view.SurfaceView

/**
 * The SurfaceView sibling below QtQuickView that carries HDR video (ADR-014, chapter 13 §13.4.4 item 2; never a
 * TextureView). Stub of WI-0.050: it exists in the layout but stays hidden and has no surface callbacks. WI-0.052
 * implements SurfaceHolder.Callback2, ovt_jni_surface_attach/detach and the dataspace.
 */
class HdrSurfaceView
    @JvmOverloads
    constructor(
        context: Context,
        attrs: AttributeSet? = null,
    ) : SurfaceView(context, attrs) {
        init {
            visibility = GONE
        }
    }
