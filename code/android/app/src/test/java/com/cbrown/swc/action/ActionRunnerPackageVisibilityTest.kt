package com.oetsolutions.swc.action

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.w3c.dom.Element
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * `ActionRunner` must not use a PackageManager QUERY as a launch gate.
 *
 * **The defect this pins.** On Android 11+ (API 30+, and this app targets 34),
 * `PackageManager` queries are filtered by package visibility: `getLaunchIntentForPackage`
 * and `queryIntentActivities` return null/empty for an app that is installed but not
 * visible to us. STARTING an activity, by contrast, needs no visibility at all --
 * Android's own docs say you "can start another app's activity using either an
 * implicit or explicit intent regardless of whether that app is visible to your app"
 * (developer.android.com/training/package-visibility/automatic).
 *
 * The old code used the query as a hard gate:
 *
 *     val intent = getLaunchIntentForPackage(pkg) ?: return AppNotInstalled(pkg)
 *
 * so a filtered (null) query aborted the action with "not installed" and NEVER
 * ATTEMPTED the launch -- which would have worked. Every `APP_LAUNCH` binding was
 * dead for any app not automatically visible. The fix attempts the launch and lets
 * the system's own rejection (`ActivityNotFoundException`) be the evidence.
 *
 * **How the asymmetry is modelled.** A [ContextWrapper] records `startActivity` and
 * decides whether it throws, over a real (Robolectric) `PackageManager` in which no
 * package is registered -- so the QUERY returns null. That is exactly the production
 * shape: the query is filtered while the launch is possible. Reverting the fix (the
 * `?: return AppNotInstalled` line) makes the first two tests fail, because
 * `startActivity` is then never called.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class ActionRunnerPackageVisibilityTest {

    /** A context whose launch succeeds (and is recorded), over an unregistered PM. */
    private class LaunchRecordingContext(base: Context) : ContextWrapper(base) {
        val started = mutableListOf<Intent>()
        override fun startActivity(intent: Intent) {
            started += intent
        }
    }

    /** A context whose launch is refused, as the system does for an absent app. */
    private class LaunchRefusingContext(
        base: Context,
        private val thrown: Exception,
    ) : ContextWrapper(base) {
        override fun startActivity(intent: Intent) {
            throw thrown
        }
    }

    private fun appContext(): Context = RuntimeEnvironment.getApplication()

    @Test
    fun `a filtered package query does not stop the launch`() {
        // The query MUST be null here for the test to mean anything: nothing is
        // registered with the package manager, so getLaunchIntentForPackage returns
        // null -- the same result a visibility-filtered query gives in production.
        val ctx = LaunchRecordingContext(appContext())
        assertEquals(
            "this test is only meaningful when the query is filtered (null)",
            null, ctx.packageManager.getLaunchIntentForPackage("com.some.player"),
        )

        val outcome = ActionRunner(ctx).run("APP_LAUNCH", "com.some.player")

        assertEquals(ActionOutcome.Ran, outcome)
        assertEquals(
            "the launch must be ATTEMPTED despite the null query, or APP_LAUNCH is " +
                "dead for every app the visibility rules hide",
            1, ctx.started.size,
        )
        // The fallback is an EXPLICIT intent (package-named), which needs no
        // visibility. An implicit intent would be subject to resolution again.
        val sent = ctx.started.single()
        assertEquals("com.some.player", sent.`package`)
    }

    @Test
    fun `an absent package is reported as not installed only after the launch fails`() {
        // The real evidence for "not installed" is the system refusing the launch,
        // not a query returning null. Here the launch is refused, so this is a
        // genuinely absent package and the report is correct.
        val ctx = LaunchRefusingContext(
            appContext(), ActivityNotFoundException("no activity for com.gone.app"),
        )
        val outcome = ActionRunner(ctx).run("APP_LAUNCH", "com.gone.app")
        assertEquals(ActionOutcome.AppNotInstalled("com.gone.app"), outcome)
    }

    @Test
    fun `an intent with no handler is reported when the launch is refused`() {
        val ctx = LaunchRefusingContext(
            appContext(), ActivityNotFoundException("no handler"),
        )
        val outcome = ActionRunner(ctx).run(
            "APP_INTENT", "com.example.DOES_NOT_EXIST", "geo:1,2",
        )
        assertEquals(ActionOutcome.NoHandler("com.example.DOES_NOT_EXIST"), outcome)
    }

    @Test
    fun `an intent the system resolves still runs`() {
        // The happy path must be unchanged: the launch is attempted and its success
        // is `Ran`.
        val ctx = LaunchRecordingContext(appContext())
        val outcome = ActionRunner(ctx).run("APP_INTENT", "android.intent.action.VIEW", "geo:1,2")
        assertEquals(ActionOutcome.Ran, outcome)
        assertTrue("the intent must carry its data payload", ctx.started.single().data != null)
    }

    @Test
    fun `a refused background launch is still reported as blocked, not swallowed`() {
        // Spec 3.6's BAL hardening: a SecurityException must remain its own outcome,
        // distinct from both not-installed and no-handler. The fix must not have
        // collapsed these.
        val ctx = LaunchRefusingContext(appContext(), SecurityException("BAL"))
        val outcome = ActionRunner(ctx).run("APP_LAUNCH", "com.some.player")
        assertTrue("a security refusal must be Blocked, was $outcome",
            outcome is ActionOutcome.Blocked)
    }

    @Test
    fun `the manifest declares the package visibility the action library needs`() {
        // The OTHER half of the fix, and nothing else checks it. The `<queries>`
        // element has no runtime test (Robolectric uses the merged manifest and does
        // not enforce visibility filtering), no compile error if removed, and no
        // effect on any other gate -- so deleting it would silently strip
        // `getLaunchIntentForPackage` of its ability to resolve a launcher app,
        // leaving only the hand-built fallback. This reads the SOURCE manifest, like
        // `LauncherActivityTest`: it is a check on what we wrote.
        val candidates = listOf(
            File("src/main/AndroidManifest.xml"),
            File("app/src/main/AndroidManifest.xml"),
        )
        val file = candidates.firstOrNull { it.isFile }
            ?: error("could not locate AndroidManifest.xml from ${File(".").absolutePath}")
        val factory = DocumentBuilderFactory.newInstance().apply { isNamespaceAware = true }
        val root = factory.newDocumentBuilder().parse(file).documentElement

        val queries = root.getElementsByTagName("queries")
        assertTrue(
            "AndroidManifest.xml declares no <queries>, so on API 30+ every " +
                "PackageManager query ActionRunner makes is filtered and " +
                "getLaunchIntentForPackage cannot resolve a launcher app",
            queries.length == 1,
        )

        // It must actually name the MAIN/LAUNCHER signature that
        // getLaunchIntentForPackage resolves for APP_LAUNCH.
        val intents = (queries.item(0) as Element).getElementsByTagName("intent")
        val namesLauncher = (0 until intents.length).any { i ->
            val intent = intents.item(i) as Element
            val actions = intent.getElementsByTagName("action")
            val categories = intent.getElementsByTagName("category")
            val hasMain = (0 until actions.length).any {
                (actions.item(it) as Element).getAttribute("android:name") ==
                    "android.intent.action.MAIN"
            }
            val hasLauncher = (0 until categories.length).any {
                (categories.item(it) as Element).getAttribute("android:name") ==
                    "android.intent.category.LAUNCHER"
            }
            hasMain && hasLauncher
        }
        assertTrue(
            "<queries> must declare MAIN/LAUNCHER; that is the intent " +
                "getLaunchIntentForPackage resolves, so it is what makes an " +
                "arbitrary installed app discoverable for APP_LAUNCH",
            namesLauncher,
        )
    }

    // --- N-11: the four kinds that were blanket-"NotImplemented" --------------

    @Test
    fun `VOLUME adjusts the named stream and refuses an unknown one`() {
        // Spec 3.6's VOLUME is the app's kind, and it needs no privilege: an
        // ordinary app may adjust streams via AudioManager. Reporting it "not
        // implemented" told the user their runnable binding was wrong (N-11).
        val ctx = RuntimeEnvironment.getApplication()
        val runner = ActionRunner(ctx)
        assertEquals(ActionOutcome.Ran, runner.run("VOLUME", "media"))
        // An unknown stream is refused rather than defaulting to media -- acting
        // on the wrong stream is worse than acting on none.
        assertTrue(runner.run("VOLUME", "not-a-stream") is ActionOutcome.NoHandler)
    }

    @Test
    fun `MEDIA dispatches a media key and refuses an unknown command`() {
        val runner = ActionRunner(RuntimeEnvironment.getApplication())
        for (cmd in listOf("play", "pause", "next", "prev", "stop")) {
            assertEquals("MEDIA $cmd", ActionOutcome.Ran, runner.run("MEDIA", cmd))
        }
        assertTrue(runner.run("MEDIA", "rewind-faster") is ActionOutcome.NoHandler)
    }

    @Test
    fun `the four kinds run rather than reporting a blanket not-implemented`() {
        // The regression this guards: `KEYCODE`, `MEDIA`, `VOLUME` and `SYSTEM` were
        // one block that refused every one of them, so a user binding MEDIA got
        // "this build does not implement it" for a command an ordinary app can run.
        // Each of the unprivileged commands now returns `Ran`.
        val runner = ActionRunner(RuntimeEnvironment.getApplication())
        assertEquals(ActionOutcome.Ran, runner.run("MEDIA", "next"))
        assertEquals(ActionOutcome.Ran, runner.run("KEYCODE", "KEYCODE_MEDIA_NEXT"))
        assertEquals(ActionOutcome.Ran, runner.run("VOLUME", "media"))
        assertEquals(ActionOutcome.Ran, runner.run("SYSTEM", "open_settings"))
    }

    @Test
    fun `SYSTEM open_settings runs and the privileged commands say so`() {
        val runner = ActionRunner(RuntimeEnvironment.getApplication())
        // `open_settings` is an ordinary ACTION_SETTINGS intent, so it runs.
        assertEquals(ActionOutcome.Ran, runner.run("SYSTEM", "open_settings"))
        // The other three need a device-owner / root API; the honest report is the
        // privilege requirement, not a false success and not "not implemented".
        for (cmd in listOf("screen_off", "night_mode", "screenshot")) {
            assertTrue("SYSTEM $cmd", runner.run("SYSTEM", cmd) is ActionOutcome.Privileged)
        }
        assertTrue(runner.run("SYSTEM", "nonsense") is ActionOutcome.NoHandler)
    }

    @Test
    fun `an injectable KEYCODE runs and a privileged one says so`() {
        val runner = ActionRunner(RuntimeEnvironment.getApplication())
        // The media transport keys are dispatchable by an ordinary app.
        assertEquals(ActionOutcome.Ran, runner.run("KEYCODE", "KEYCODE_MEDIA_PLAY_PAUSE"))
        assertEquals(ActionOutcome.Ran, runner.run("KEYCODE", "KEYCODE_VOLUME_UP"))
        // Anything else needs INJECT_EVENTS, a signature permission.
        assertTrue(runner.run("KEYCODE", "KEYCODE_HOME") is ActionOutcome.Privileged)
        assertTrue(runner.run("KEYCODE", "KEYCODE_BACK") is ActionOutcome.Privileged)
    }
}
