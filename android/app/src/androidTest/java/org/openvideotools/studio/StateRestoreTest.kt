// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.accessibilityservice.AccessibilityService
import android.content.Context
import android.content.Intent
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.runner.lifecycle.ActivityLifecycleMonitorRegistry
import androidx.test.runner.lifecycle.Stage
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Instrumentation tests of WI-A.455 (chapter 11 §11.5.7): the GUI state (project files, `SelectionState`, `ViewState`,
 * layout preset, panel state) survives the Activity being destroyed and created again, which "Don't keep activities"
 * does on every navigation away. Run from `android/` with `./gradlew :app:connectedFossDebugAndroidTest` on the x86_64
 * API 36 emulator (`-gpu host`); the record is docs/acceptance/wi/WI-A.455.md until the emulator CI (#487) runs it.
 *
 * The state is put into the live QML root through the same `restoredState` property a restore uses, so the tests need
 * no file and no touch input; what they check is that the *new* Activity's QML engine, which starts from defaults,
 * publishes that frame and selection again.
 */
@RunWith(AndroidJUnit4::class)
class StateRestoreTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context: Context = instrumentation.targetContext

    // scenario.recreate() runs the path "Don't keep activities" runs: onSaveInstanceState, onDestroy (not finishing),
    // then a new instance's onCreate with the Bundle.
    @Test
    fun recreationRestoresFrameAndSelection() {
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            val first = showState(scenario.activity())

            val recreatedAt = SystemClock.uptimeMillis()
            scenario.recreate()
            val second = scenario.activity()
            assertNotSame("recreate() kept the old instance", first, second)
            val elapsed = waitForRestoredState(second, recreatedAt)
            assertTrue("restored after $elapsed ms, budget $RESTORE_BUDGET_MS ms", elapsed <= RESTORE_BUDGET_MS)
        }
    }

    // Acceptance test 1: with "Don't keep activities" on, going to the launcher destroys the Activity; coming back
    // creates a new one, which shows the same frame and selection within 2 s. ActivityScenario refuses to launch while
    // the setting is on, so the Activities are started with plain intents and found through the lifecycle monitor.
    @Test
    fun dontKeepActivitiesBackgroundAndResume() {
        val before = shell("settings get global always_finish_activities").trim()
        shell("settings put global always_finish_activities 1")
        var second: MainActivity? = null
        try {
            context.startActivity(
                Intent(context, MainActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK),
            )
            var launched: MainActivity? = null
            waitUntil("MainActivity is resumed", SHELL_TIMEOUT_NS) {
                launched = resumedMainActivity()
                launched != null
            }
            val first = showState(launched!!)

            instrumentation.uiAutomation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_HOME)
            waitUntil("the Activity is destroyed in the background") { first.isDestroyed }

            val resumedAt = SystemClock.uptimeMillis()
            context.startActivity(Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            waitUntil("a new MainActivity is resumed") {
                second = resumedMainActivity()
                second != null && second !== first
            }
            val elapsed = waitForRestoredState(second!!, resumedAt)
            assertTrue("restored after $elapsed ms, budget $RESTORE_BUDGET_MS ms", elapsed <= RESTORE_BUDGET_MS)
        } finally {
            shell("settings put global always_finish_activities ${if (before == "1") "1" else "0"}")
            second?.let { activity ->
                onMain { activity.finish() }
                waitUntil("the Activity finishes") { activity.isDestroyed }
            }
        }
    }

    /** Puts [STATE] into the QML root of [activity] and waits until the root publishes it back. */
    private fun showState(activity: MainActivity): MainActivity {
        waitUntil("the QML root is READY", SHELL_TIMEOUT_NS) { activity.isShellReady }
        onMain { activity.applyUiStateForTest(STATE.toString()) }
        waitUntil("the QML root published the frame") { hasState(activity.savedUiState) }
        return activity
    }

    /**
     * Waits until [activity]'s QML root, built after [since], has taken the saved state and published it again;
     * returns the milliseconds from [since]. The Bundle's copy is the same text, so the frame alone does not tell
     * the new engine's state apart: the Activity's log line and the first publish after READY do.
     */
    private fun waitForRestoredState(
        activity: MainActivity,
        since: Long,
    ): Long {
        var at = 0L
        waitUntil("the new QML root shows the saved frame and selection", SHELL_TIMEOUT_NS) {
            val done = activity.isShellReady && activity.restoredFromBundle && hasState(activity.savedUiState)
            if (done && at == 0L) at = SystemClock.uptimeMillis()
            done
        }
        val state = JSONObject(activity.savedUiState!!)
        val selection = state.getJSONObject("selection")
        assertEquals(42, selection.getJSONArray("frame").getInt(0))
        assertEquals(40, selection.getJSONArray("frame").getInt(1))
        assertEquals(1000L, selection.getJSONArray("byte_range").getLong(0))
        assertEquals(24L, selection.getJSONArray("byte_range").getLong(1))
        assertEquals(1, state.getJSONObject("project").getJSONArray("files").length())
        assertEquals("Compare", state.getJSONObject("layout").getString("preset"))
        return at - since
    }

    /** True when [json] is a published state showing frame 42 / 40 and the byte selection of [STATE]. */
    private fun hasState(json: String?): Boolean {
        if (json.isNullOrEmpty()) return false
        val selection = JSONObject(json).optJSONObject("selection") ?: return false
        val frame = selection.optJSONArray("frame") ?: return false
        return frame.optInt(0) == 42 && frame.optInt(1) == 40 && selection.optString("file_id").isNotEmpty()
    }

    private fun ActivityScenario<MainActivity>.activity(): MainActivity {
        var activity: MainActivity? = null
        onActivity { activity = it }
        return activity!!
    }

    private fun resumedMainActivity(): MainActivity? =
        ActivityLifecycleMonitorRegistry
            .getInstance()
            .getActivitiesInStage(Stage.RESUMED)
            .filterIsInstance<MainActivity>()
            .firstOrNull()

    /** Runs a shell command as the shell user (settings needs it) and returns its output. */
    private fun shell(command: String): String {
        val pfd = instrumentation.uiAutomation.executeShellCommand(command)
        return ParcelFileDescriptor.AutoCloseInputStream(pfd).use { it.readBytes().toString(Charsets.UTF_8) }
    }

    private fun <T> onMain(block: () -> T): T {
        var result: Result<T>? = null
        instrumentation.runOnMainSync { result = runCatching(block) }
        return result!!.getOrThrow()
    }

    private fun waitUntil(
        what: String,
        timeoutNs: Long = TIMEOUT_NS,
        condition: () -> Boolean,
    ) {
        val deadline = System.nanoTime() + timeoutNs
        while (!onMain(condition)) {
            check(System.nanoTime() < deadline) { "timed out waiting until $what" }
            Thread.sleep(POLL_MS)
        }
    }

    private companion object {
        const val TIMEOUT_NS = 10_000_000_000L
        const val SHELL_TIMEOUT_NS = 60_000_000_000L // the first QML load on a cold emulator
        const val POLL_MS = 20L
        const val RESTORE_BUDGET_MS = 2_000L // §11.5.7 and WI-A.455 acceptance test 1

        /** A GUI state document (app/src/platform/android_state.h): one file, frame 42 / 40, a byte range, Compare. */
        val STATE: JSONObject =
            JSONObject()
                .put("version", 1)
                .put(
                    "project",
                    JSONObject()
                        .put("path", "")
                        .put("active_file", "f1")
                        .put(
                            "files",
                            JSONArray().put(
                                JSONObject()
                                    .put("id", "f1")
                                    .put("path", "content://org.openvideotools.test/document/clip.mp4")
                                    .put("role", "single")
                                    .put("track_index", 0),
                            ),
                        ),
                ).put(
                    "selection",
                    JSONObject()
                        .put("file_id", "f1")
                        .put("track_index", 0)
                        .put("frame", JSONArray().put(42).put(40))
                        .put("byte_range", JSONArray().put(1000).put(24)),
                ).put("layout", JSONObject().put("preset", "Compare"))
    }
}
