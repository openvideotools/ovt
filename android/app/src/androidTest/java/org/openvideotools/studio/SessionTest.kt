// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.StrictMode
import androidx.lifecycle.Lifecycle
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Instrumentation tests of WI-A.450 (chapter 11 §11.5.7): [OvtSession] owns the core handles across Activity
 * recreation, and a VIEW intent reaches the running Activity. Run with
 * `./gradlew -p android :app:connectedFossDebugAndroidTest` on the x86_64 API 36 emulator.
 *
 * The files are tiny Y4M streams written to the cache directory and sent as `file://` URIs (the test owns no
 * `ContentProvider`); StrictMode's file-URI check is off during each test (ActivityScenario starts the intent later,
 * from its own bootstrap Activity, so it cannot be scoped to one call). `ContentResolver.openFileDescriptor`
 * serves `file://` and `content://` the same way, so FilePicker takes the same path as for a document from a provider.
 */
@RunWith(AndroidJUnit4::class)
class SessionTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context: Context = instrumentation.targetContext

    private lateinit var vmPolicy: StrictMode.VmPolicy

    @Before
    fun setUp() {
        onMain { OvtSession.close() }
        vmPolicy = StrictMode.getVmPolicy()
        // Only the test sends file:// URIs; the app itself never does.
        StrictMode.setVmPolicy(StrictMode.VmPolicy.Builder().build())
    }

    @After
    fun tearDown() {
        onMain { OvtSession.close() }
        StrictMode.setVmPolicy(vmPolicy)
    }

    // Acceptance test 2: an ACTION_VIEW intent opens the file in the running Activity, which is not recreated.
    @Test
    fun viewIntentOpensFileInRunningActivity() {
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            var first: MainActivity? = null
            scenario.onActivity { first = it }
            val opensBefore = onMain { OvtSession.openCount }

            val file = writeY4m("view_intent.y4m")
            context.startActivity(viewIntent(file).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))

            waitUntil("the VIEW intent's file is open") { OvtSession.openCount == opensBefore + 1 }
            scenario.onActivity { assertSame("the Activity was recreated", first, it) }
            assertEquals(Lifecycle.State.RESUMED, scenario.state)
            onMain { assertEquals("view_intent.y4m", OvtSession.current?.displayName) }
        }
    }

    // Acceptance test 3: with a file open, recreating the Activity (what "Don't keep activities" does on every
    // navigation away) keeps the same native handles and opens nothing again, so nothing is indexed again.
    // SPEC-ASSUMPTION(#444): there is no ovt_Stream or source_hash in the session yet; the same live handle and an
    // unchanged open count stand in for "same source_hash, no re-index".
    @Test
    fun recreationKeepsTheOpenHandles() {
        val file = writeY4m("recreate.y4m")
        ActivityScenario.launch<MainActivity>(viewIntent(file)).use { scenario ->
            waitUntil("the launch intent's file is open") { OvtSession.current != null }
            val handle = onMain { OvtSession.current!!.handle }
            val opens = onMain { OvtSession.openCount }
            var first: MainActivity? = null
            scenario.onActivity { first = it }

            scenario.recreate()

            var second: MainActivity? = null
            scenario.onActivity { second = it }
            assertNotNull(second)
            assertNotEquals("recreate() kept the old instance", first, second)
            onMain {
                assertEquals("the session handle changed", handle, OvtSession.current?.handle)
                assertEquals("the file was opened again", opens, OvtSession.openCount)
                assertNotNull("the new Activity did not re-bind", OvtSession.boundListener)
            }
            // The handle is still live: the core reports the file's size through it.
            assertEquals(file.length(), OvtNative.fileSize(handle))
        }
    }

    // §11.5.7: the handles are released when the Activity finishes, not before.
    @Test
    fun finishingClosesTheSession() {
        val file = writeY4m("finish.y4m")
        val scenario = ActivityScenario.launch<MainActivity>(viewIntent(file))
        waitUntil("the launch intent's file is open") { OvtSession.current != null }
        scenario.close() // finishes the Activity
        onMain {
            assertNull(OvtSession.current)
            assertNull(OvtSession.boundListener)
        }
    }

    private fun viewIntent(file: File): Intent =
        Intent(Intent.ACTION_VIEW)
            .setClass(context, MainActivity::class.java)
            .setDataAndType(Uri.fromFile(file), "application/octet-stream")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)

    /** A one-frame 16×16 4:2:0 Y4M stream, enough for the container sniffer to open. */
    private fun writeY4m(name: String): File {
        val header = "YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420jpeg\nFRAME\n".toByteArray(Charsets.US_ASCII)
        return File(context.cacheDir, name).apply { writeBytes(header + ByteArray(16 * 16 * 3 / 2)) }
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
