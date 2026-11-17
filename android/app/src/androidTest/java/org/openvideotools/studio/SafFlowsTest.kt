// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.DocumentsContract
import androidx.appcompat.app.AlertDialog
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Instrumentation tests of WI-A.452 (chapter 11 §11.5.6): the Storage Access Framework flows over a test
 * `DocumentsProvider` ([TestDocumentsProvider], in the debug build, enabled only while a test runs). Run with
 * `./gradlew -p android :app:connectedFossDebugAndroidTest` on the API 36 emulator.
 *
 * The documents are `content://` URIs of that provider, so FilePicker takes the same path as for a document picked
 * in the system picker; only the picker UI itself is skipped (the test calls `FilePicker.open(uri, persist = true)`,
 * which is what the picker's result callback does). A provider of the app's own UID offers no persistable grant, so
 * the grant step logs and goes on; reopening after a real process restart, with a real provider, is in
 * `docs/acceptance/wi/WI-A.452.md`.
 */
@RunWith(AndroidJUnit4::class)
class SafFlowsTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context: Context = instrumentation.targetContext
    private val provider = ComponentName(context, TestDocumentsProvider::class.java)
    private val documents = File(context.cacheDir, TestDocumentsProvider.DIR_NAME)

    @Before
    fun setUp() {
        onMain {
            OvtSession.close()
            RecentFiles(context).clear()
        }
        documents.deleteRecursively()
        documents.mkdirs()
        CopyToAppStorage.destinationDir(context).deleteRecursively()
        context.packageManager.setComponentEnabledSetting(
            provider,
            PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
            PackageManager.DONT_KILL_APP,
        )
    }

    @After
    fun tearDown() {
        onMain {
            OvtSession.close()
            RecentFiles(context).clear()
        }
        context.packageManager.setComponentEnabledSetting(
            provider,
            PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
            PackageManager.DONT_KILL_APP,
        )
        documents.deleteRecursively()
        CopyToAppStorage.destinationDir(context).deleteRecursively()
    }

    // Acceptance test 1: a document URI opens through ovt_file_open_fd, the recent list shows its display name, and
    // the URI reopens from the list once the Activity (and with it everything but the stored list) is gone.
    @Test
    fun documentOpensShowsInRecentListAndReopens() {
        val uri = document("seekable.y4m")
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            scenario.onActivity { it.filePicker.open(uri, persist = true) }
            waitUntil("the document is open") { OvtSession.current?.displayName == "seekable.y4m" }
            scenario.onActivity { activity ->
                val top = activity.recentFiles.entries.first()
                assertEquals(uri, top.uri)
                assertEquals("seekable.y4m", top.displayName)
                assertEquals("the codec field (SPEC-ASSUMPTION(#723))", "y4m", top.codec)
            }
            // The handle is a live ovt_File: the core reports the document's size through it.
            assertEquals(File(documents, "seekable.y4m").length(), OvtNative.fileSize(onMain { OvtSession.current!!.handle }))
            // Close only once Qt has started; MainActivity.finish defers until then anyway (the Mini's crash).
            val activity = activityOf(scenario)
            waitUntil("Qt has started") { activity.qtStarted }
        } // finishes the Activity, which closes the session

        onMain { assertEquals(null, OvtSession.current) }
        // A new list reads the stored entries, as the next process does.
        val stored = onMain { RecentFiles(context).entries }
        assertEquals(listOf("seekable.y4m"), stored.map { it.displayName })
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            scenario.onActivity { it.filePicker.reopen(stored.first()) }
            waitUntil("the recent entry is open again") { OvtSession.current?.displayName == "seekable.y4m" }
        }
    }

    // A recent entry whose document is gone is dropped from the list when reopening fails.
    @Test
    fun recentEntryOfAGoneDocumentIsRemoved() {
        val uri = document("gone.y4m")
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            scenario.onActivity { it.filePicker.open(uri, persist = true) }
            waitUntil("the document is open") { OvtSession.current?.displayName == "gone.y4m" }
            onMain { OvtSession.close() }
            assertTrue(File(documents, "gone.y4m").delete())
            val activity = activityOf(scenario)
            onMain { activity.filePicker.reopen(activity.recentFiles.entries.first()) }
            waitUntil("the entry is removed") { activity.recentFiles.entries.isEmpty() }
        }
    }

    // Acceptance test 2: a provider that hands out a pipe raises GUI-AND-0001 with the copy action; after the copy,
    // the local file opens and is what the recent list keeps.
    @Test
    fun nonSeekableDocumentRaisesGuiAnd0001AndOpensAfterCopy() {
        val uri = document(TestDocumentsProvider.PIPE_PREFIX + "stream.y4m")
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            val opensBefore = onMain { OvtSession.openCount }
            val activity = activityOf(scenario)
            onMain { activity.filePicker.open(uri, persist = true) }
            waitUntil("GUI-AND-0001 is shown") { activity.copyPrompt.dialog?.isShowing == true }
            val prompt = onMain { activity.copyPrompt.dialog }
            onMain {
                assertEquals("nothing was opened from the pipe", opensBefore, OvtSession.openCount)
                val copy = prompt!!.getButton(AlertDialog.BUTTON_POSITIVE)
                assertEquals(CopyPrompt.COPY_LABEL, copy.text.toString())
                copy.performClick()
            }
            waitUntil("the copy is open") { OvtSession.current?.displayName == "pipe-stream.y4m" }
            val copied = File(CopyToAppStorage.destinationDir(context), "pipe-stream.y4m")
            assertTrue("no copy in ${copied.parent}", copied.isFile)
            assertEquals(File(documents, "pipe-stream.y4m").length(), copied.length())
            scenario.onActivity { activity ->
                val top = activity.recentFiles.entries.first()
                assertEquals(Uri.fromFile(copied), top.uri)
                assertEquals("pipe-stream.y4m", top.displayName)
            }
        }
    }

    // Seekability comes from fstat on the descriptor; the pipe is closed unread and nothing reaches the core.
    @Test
    fun fstatDecidesSeekability() {
        val pipe = document(TestDocumentsProvider.PIPE_PREFIX + "probe.y4m")
        val result = FilePicker.openDocument(context.contentResolver, pipe)
        assertTrue("$result", result is FilePicker.OpenResult.NotSeekable)
        assertEquals("pipe-probe.y4m", (result as FilePicker.OpenResult.NotSeekable).displayName)

        val seekable = FilePicker.openDocument(context.contentResolver, document("plain.y4m"))
        assertTrue("$seekable", seekable is FilePicker.OpenResult.Opened)
        OvtNative.close((seekable as FilePicker.OpenResult.Opened).file.session)
    }

    // Copies never replace an existing file and keep only safe file names.
    @Test
    fun copyNamesAreSafeAndUnique() {
        val uri = document(TestDocumentsProvider.PIPE_PREFIX + "twice.y4m")
        val copier = CopyToAppStorage(context)
        val first = copier.copy(uri, "../twice.y4m")
        val second = copier.copy(uri, "../twice.y4m")
        assertEquals("_twice.y4m", first.name)
        assertEquals("_twice (1).y4m", second.name)
        assertEquals(CopyToAppStorage.destinationDir(context), first.parentFile)
        assertEquals("a_b_c", CopyToAppStorage.safeFileName("a/b\\c"))
        assertEquals("document", CopyToAppStorage.safeFileName("..."))
    }

    // A cancelled copy leaves nothing behind.
    @Test
    fun cancelledCopyLeavesNoFile() {
        val uri = document(TestDocumentsProvider.PIPE_PREFIX + "cancel.y4m")
        val result = runCatching { CopyToAppStorage(context).copy(uri, "cancel.y4m", cancelled = { true }) }
        assertTrue(result.exceptionOrNull() is CopyToAppStorage.CancelledException)
        assertEquals(emptyList<String>(), CopyToAppStorage.destinationDir(context).list()?.toList() ?: emptyList<String>())
    }

    // Persisted grants are capped by the platform (128/512): the oldest go first, recent-list documents last.
    @Test
    fun grantEvictionIsOldestFirstAndKeepsRecentsLongest() {
        assertEquals(128, PersistedGrants.limit(28))
        assertEquals(512, PersistedGrants.limit(30))
        val a = Uri.parse("content://p/a")
        val b = Uri.parse("content://p/b")
        val c = Uri.parse("content://p/c")
        val d = Uri.parse("content://p/d")
        val held = listOf(a to 3L, b to 1L, c to 2L)
        assertEquals(emptyList<Uri>(), PersistedGrants.evictionOrder(held, d, emptySet(), limit = 4))
        assertEquals(listOf(b), PersistedGrants.evictionOrder(held, d, emptySet(), limit = 3))
        assertEquals(listOf(c), PersistedGrants.evictionOrder(held, d, setOf(b), limit = 3))
        assertEquals(listOf(c, a), PersistedGrants.evictionOrder(held, d, setOf(b), limit = 2))
        assertEquals("already held", emptyList<Uri>(), PersistedGrants.evictionOrder(held, a, emptySet(), limit = 3))
    }

    // The recent list keeps 20 entries, newest first, and re-opening moves an entry to the top.
    @Test
    fun recentListOrderAndCapacity() {
        onMain {
            val list = RecentFiles(context)
            (0 until RecentFiles.CAPACITY + 3).forEach { list.add(Uri.parse("content://p/$it"), "f$it", "", now = it.toLong()) }
            assertEquals(RecentFiles.CAPACITY, list.entries.size)
            assertEquals("f22", list.entries.first().displayName)
            list.add(Uri.parse("content://p/10"), "f10", "h264", now = 100L)
            assertEquals("f10", list.entries.first().displayName)
            assertEquals(1, list.entries.count { it.displayName == "f10" })
            assertEquals("h264", RecentFiles(context).entries.first().codec)
        }
    }

    // Export: ACTION_CREATE_DOCUMENT with the type and suggested name, and the writer's bytes land in the document.
    @Test
    fun exportCreatesAndWritesTheDocument() {
        val request = ExportDocument.Request("clip.info.json", "application/json") { it.write("{\"a\":1}".toByteArray()) }
        val intent = ExportDocument.CreateDocument().createIntent(context, request)
        assertEquals(Intent.ACTION_CREATE_DOCUMENT, intent.action)
        assertEquals("application/json", intent.type)
        assertEquals("clip.info.json", intent.getStringExtra(Intent.EXTRA_TITLE))
        assertTrue(intent.hasCategory(Intent.CATEGORY_OPENABLE))

        // What the system picker returns: a new document of the chosen provider.
        val target =
            DocumentsContract.createDocument(
                context.contentResolver,
                DocumentsContract.buildDocumentUri(authority(), TestDocumentsProvider.ROOT_DOCUMENT_ID),
                "application/json",
                "clip.info.json",
            )!!
        assertTrue(ExportDocument.write(context.contentResolver, target, request.writer))
        assertEquals("{\"a\":1}", File(documents, "clip.info.json").readText())
        // A writer that throws fails the export instead of crashing the worker.
        assertFalse(ExportDocument.write(context.contentResolver, target) { error("boom") })

        assertTrue(ExportDocument.needsSdrNotice("image/png", hdrContent = true))
        assertFalse(ExportDocument.needsSdrNotice("image/png", hdrContent = false))
        assertFalse(ExportDocument.needsSdrNotice("application/json", hdrContent = true))
    }

    private fun activityOf(scenario: ActivityScenario<MainActivity>): MainActivity {
        var activity: MainActivity? = null
        scenario.onActivity { activity = it }
        return activity!!
    }

    private fun authority(): String = TestDocumentsProvider.authority(context.packageName)

    /** A one-frame 16×16 4:2:0 Y4M stream in the test provider, as a document URI. */
    private fun document(name: String): Uri {
        val header = "YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420jpeg\nFRAME\n".toByteArray(Charsets.US_ASCII)
        File(documents, name).writeBytes(header + ByteArray(16 * 16 * 3 / 2))
        val uri = DocumentsContract.buildDocumentUri(authority(), name)
        assertNotEquals(null, uri)
        return uri
    }

    private fun <T> onMain(block: () -> T): T {
        var result: Result<T>? = null
        instrumentation.runOnMainSync { result = runCatching(block) }
        return result!!.getOrThrow()
    }

    private fun waitUntil(
        what: String,
        condition: () -> Boolean,
    ) {
        val deadline = System.nanoTime() + TIMEOUT_NS
        while (!onMain(condition)) {
            check(System.nanoTime() < deadline) { "timed out waiting until $what" }
            Thread.sleep(POLL_MS)
        }
    }

    private companion object {
        const val TIMEOUT_NS = 10_000_000_000L
        const val POLL_MS = 50L
    }
}
