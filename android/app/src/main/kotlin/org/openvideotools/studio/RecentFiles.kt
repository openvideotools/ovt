// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.ContentResolver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.annotation.MainThread
import androidx.annotation.VisibleForTesting
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject

/**
 * The Android recent files list (chapter 11 §11.5.6; WI-A.452): SAF documents, newest first, as {uri, display name,
 * codec, last opened}, kept in SharedPreferences so it survives a restart (the document reopens thanks to the grant
 * [PersistedGrants] took). [FilePicker.reopen] drops entries whose document is gone. [CAPACITY] as on the desktop.
 *
 * SPEC-ASSUMPTION(#723): `codec` holds the container format of `ovt_container_format()` (for example `mp4`, `h264`)
 * until the JNI info carries the codec of the first video track (WI-1.500).
 *
 * Main thread only.
 */
internal class RecentFiles(
    context: Context,
) {
    /** One recent document. [lastOpened] is in milliseconds since the epoch. */
    data class Entry(
        val uri: Uri,
        val displayName: String,
        val codec: String,
        val lastOpened: Long,
    )

    /** Told after every change of [entries]. */
    fun interface Listener {
        @MainThread
        fun onRecentFilesChanged(entries: List<Entry>)
    }

    private val prefs = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    private val listeners = mutableListOf<Listener>()

    /** The entries, newest first. */
    @get:MainThread
    var entries: List<Entry> = load()
        private set

    /** The URIs of [entries]; [PersistedGrants] evicts their grants last. */
    @MainThread
    fun uris(): Set<Uri> = entries.mapTo(HashSet()) { it.uri }

    /** Puts [uri] at the top of the list (moving it if it is there already) and trims the list to [CAPACITY]. */
    @MainThread
    fun add(
        uri: Uri,
        displayName: String,
        codec: String,
        now: Long = System.currentTimeMillis(),
    ) {
        val entry = Entry(uri, displayName, codec, now)
        update((listOf(entry) + entries.filter { it.uri != uri }).take(CAPACITY))
    }

    @MainThread
    fun remove(uri: Uri) {
        if (entries.any { it.uri == uri }) update(entries.filter { it.uri != uri })
    }

    @MainThread
    fun clear() {
        update(emptyList())
    }

    @MainThread
    fun addListener(listener: Listener) {
        listeners += listener
        listener.onRecentFilesChanged(entries)
    }

    @MainThread
    fun removeListener(listener: Listener) {
        listeners -= listener
    }

    private fun update(newEntries: List<Entry>) {
        entries = newEntries
        save()
        listeners.toList().forEach { it.onRecentFilesChanged(entries) }
    }

    private fun load(): List<Entry> {
        val json = prefs.getString(KEY_ENTRIES, null) ?: return emptyList()
        return try {
            val array = JSONArray(json)
            (0 until array.length()).mapNotNull { index ->
                val item = array.optJSONObject(index) ?: return@mapNotNull null
                val uri = item.optString(FIELD_URI).takeIf { it.isNotEmpty() } ?: return@mapNotNull null
                Entry(
                    uri = Uri.parse(uri),
                    displayName = item.optString(FIELD_NAME, uri),
                    codec = item.optString(FIELD_CODEC),
                    lastOpened = item.optLong(FIELD_OPENED),
                )
            }
        } catch (e: JSONException) {
            Log.w(TAG, "recent files list unreadable, starting empty: ${e.message}")
            emptyList()
        }
    }

    private fun save() {
        val array = JSONArray()
        entries.forEach { entry ->
            array.put(
                JSONObject()
                    .put(FIELD_URI, entry.uri.toString())
                    .put(FIELD_NAME, entry.displayName)
                    .put(FIELD_CODEC, entry.codec)
                    .put(FIELD_OPENED, entry.lastOpened),
            )
        }
        prefs.edit().putString(KEY_ENTRIES, array.toString()).apply()
    }

    companion object {
        /** Entries kept: the size of the desktop list (FR-GUI-027). */
        const val CAPACITY = 20

        @VisibleForTesting
        internal const val PREFS_NAME = "ovt_recent_files"
        private const val KEY_ENTRIES = "entries"
        private const val FIELD_URI = "uri"
        private const val FIELD_NAME = "name"
        private const val FIELD_CODEC = "codec"
        private const val FIELD_OPENED = "last_opened"
        private const val TAG = "OVT"
    }
}

/**
 * Persisted URI read grants (§11.5.6: `takePersistableUriPermission` so that a document reopens after a restart).
 * The platform keeps at most [limit] persisted grants per app (128 before Android 11, 512 from it) and prunes on its
 * own past that; OVT evicts first, oldest first, so that the grants of documents still in the recent list go last.
 */
internal object PersistedGrants {
    /** The platform's cap on persisted grants per app for [sdkInt]. */
    fun limit(sdkInt: Int = Build.VERSION.SDK_INT): Int = if (sdkInt >= Build.VERSION_CODES.R) 512 else 128

    /**
     * Takes a persistable read grant for [uri], first releasing the oldest grants when the app holds [limit] of them.
     * Grants of [keep] (the recent list) are released only after every other one. Returns false when the provider did
     * not offer a persistable grant (a VIEW/SEND intent, a drag-and-drop, the app's own provider): the document opens
     * now but may not reopen after a restart.
     */
    fun take(
        resolver: ContentResolver,
        uri: Uri,
        keep: Set<Uri>,
        limit: Int = limit(),
    ): Boolean {
        val held = resolver.persistedUriPermissions.filter { it.isReadPermission }
        evictionOrder(held.map { it.uri to it.persistedTime }, uri, keep, limit).forEach { old ->
            try {
                resolver.releasePersistableUriPermission(old, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                Log.i(TAG, "released the persisted grant of $old (limit $limit)")
            } catch (e: SecurityException) {
                Log.w(TAG, "cannot release the grant of $old: ${e.message}")
            }
        }
        return try {
            resolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            true
        } catch (e: SecurityException) {
            Log.w(TAG, "URI permission of $uri is not persistable: ${e.message}")
            false
        }
    }

    /**
     * The grants to release before taking one for [incoming], given the [held] grants as (URI, persisted time):
     * none while there is room or [incoming] is held already; otherwise the oldest, those outside [keep] first, until
     * one slot is free.
     */
    @VisibleForTesting
    internal fun evictionOrder(
        held: List<Pair<Uri, Long>>,
        incoming: Uri,
        keep: Set<Uri>,
        limit: Int,
    ): List<Uri> {
        if (held.any { it.first == incoming }) return emptyList()
        val excess = held.size + 1 - limit
        if (excess <= 0) return emptyList()
        return held
            .sortedWith(compareBy<Pair<Uri, Long>> { it.first in keep }.thenBy { it.second })
            .take(excess)
            .map { it.first }
    }

    private const val TAG = "OVT"
}
