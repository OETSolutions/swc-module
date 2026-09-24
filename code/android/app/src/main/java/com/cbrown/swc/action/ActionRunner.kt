package com.oetsolutions.swc.action

import android.content.Context
import android.content.Intent
import android.net.Uri

/**
 * Why an app-side action could not run.
 *
 * **This type exists because "it did nothing" is the worst possible outcome.** An
 * action the user bound to a button and then silently cannot fire is a button that
 * appears broken; the user has no way to tell an unconfigured action from a blocked
 * one. Every failure below is therefore reportable, and every one has a different
 * fix the UI can name.
 */
sealed interface ActionOutcome {
    data object Ran : ActionOutcome

    /** The package is not installed on this phone. */
    data class AppNotInstalled(val pkg: String) : ActionOutcome

    /**
     * The intent was well-formed but no app on this device can handle it.
     *
     * Distinct from [AppNotInstalled]: the package may exist while the ACTION does
     * not, and the user's fix is different (check the action string rather than
     * install something).
     */
    data class NoHandler(val action: String) : ActionOutcome

    /** The system blocked the launch. */
    data class Blocked(val reason: String) : ActionOutcome

    /** The action kind is not one the app executes (an `OUT_` or `BUZZ`). */
    data class NotAppSide(val kind: String) : ActionOutcome

    /**
     * The kind IS app-side and this build would run it, but the specific command
     * needs a privilege an ordinary app does not have (root, an accessibility
     * service, a device-owner / system-app install — spec N-6).
     *
     * **Distinct from [NotAppSide].** `NotAppSide` says "the firmware owns this
     * kind"; this one says "the KIND works, but THIS command needs elevated
     * access" — the honest report for `SYSTEM{night_mode}` or an arbitrary
     * `KEYCODE`, where the user's fix is to grant the role, not to change the
     * binding. (An earlier `NotImplemented` variant carried this job for the whole
     * four-kind block; it is gone now that the unprivileged subset runs.)
     */
    data class Privileged(val command: String) : ActionOutcome
}

/** A `VOLUME` target name -> its `AudioManager` stream (spec 3.6). */
private val STREAMS = mapOf(
    "media" to android.media.AudioManager.STREAM_MUSIC,
    "call" to android.media.AudioManager.STREAM_VOICE_CALL,
    "ring" to android.media.AudioManager.STREAM_RING,
    "alarm" to android.media.AudioManager.STREAM_ALARM,
)

/** A `MEDIA` command -> its keycode (spec 3.6). */
private val MEDIA_KEYS = mapOf(
    "play" to android.view.KeyEvent.KEYCODE_MEDIA_PLAY,
    "pause" to android.view.KeyEvent.KEYCODE_MEDIA_PAUSE,
    "next" to android.view.KeyEvent.KEYCODE_MEDIA_NEXT,
    "prev" to android.view.KeyEvent.KEYCODE_MEDIA_PREVIOUS,
    "stop" to android.view.KeyEvent.KEYCODE_MEDIA_STOP,
)

/**
 * The keycodes an ordinary app may inject, via the media dispatch — the media
 * transport and volume keys. Anything else needs `INJECT_EVENTS` (signature-level),
 * so it is reported as [ActionOutcome.Privileged] rather than attempted.
 */
private val KEYCODE_KEYS = mapOf(
    "KEYCODE_MEDIA_PLAY" to android.view.KeyEvent.KEYCODE_MEDIA_PLAY,
    "KEYCODE_MEDIA_PAUSE" to android.view.KeyEvent.KEYCODE_MEDIA_PAUSE,
    "KEYCODE_MEDIA_PLAY_PAUSE" to android.view.KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
    "KEYCODE_MEDIA_NEXT" to android.view.KeyEvent.KEYCODE_MEDIA_NEXT,
    "KEYCODE_MEDIA_PREVIOUS" to android.view.KeyEvent.KEYCODE_MEDIA_PREVIOUS,
    "KEYCODE_MEDIA_STOP" to android.view.KeyEvent.KEYCODE_MEDIA_STOP,
    "KEYCODE_VOLUME_UP" to android.view.KeyEvent.KEYCODE_VOLUME_UP,
    "KEYCODE_VOLUME_DOWN" to android.view.KeyEvent.KEYCODE_VOLUME_DOWN,
    "KEYCODE_VOLUME_MUTE" to android.view.KeyEvent.KEYCODE_VOLUME_MUTE,
)

/**
 * Runs the app-side action kinds this build implements.
 *
 * **Spec 3.6 gives the app seven kinds, and this class runs all but one now** (N-11).
 * `APP_LAUNCH`, `APP_INTENT`, `APP_RAW`, `VOLUME`, `MEDIA`, `SYSTEM` (its
 * `open_settings` command) and the unprivileged `KEYCODE` set are implemented; the
 * only kind still refused is a `KEYCODE` outside the small set an ordinary app may
 * inject (see [KEYCODE_REQUIRES_PRIVILEGE]), which needs root, an accessibility
 * service or a system-app install (spec N-6).
 *
 * **The four "unimplemented" kinds were not all privileged, and treating them as
 * one block was the defect.** An earlier version reported `KEYCODE`, `MEDIA`,
 * `VOLUME` and `SYSTEM` as one "not implemented" set and justified it
 * with "key injection and screen control need root". That is true of arbitrary
 * keycodes and false of the rest: `Volume` is `AudioManager`, `MEDIA`'s transport
 * is `dispatchMediaKeyEvent`, and `SYSTEM`'s `open_settings` is a plain
 * `ACTION_SETTINGS` intent -- all available to an ordinary app. So the block told
 * users their (runnable) bindings could not run, which is the same "your binding
 * is wrong" misdirection the outcome split exists to avoid.
 *
 * **This is the module Android 15's background-activity-launch (BAL) hardening
 * applies to** (spec 3.6): since the app must launch activities from a background
 * service (it is driven by button presses arriving over USB), the system may refuse
 * the launch. That refusal is reported through [ActionOutcome.Blocked] rather than
 * swallowed -- a silently dropped launch is indistinguishable from a broken button.
 *
 * It takes a [Context] and does no work of its own, so it is a thin, testable seam:
 * the decision logic is the `when` below, and a fake context can drive every branch.
 */
class ActionRunner(private val context: Context) {

    /**
     * Run [target] as the action kind's own meaning (spec 3.6).
     *
     * `kind` is the wire name from the generated contract, so the caller cannot pass
     * a kind this class does not implement without it being visible here.
     */
    fun run(kind: String, target: String, payload: String = ""): ActionOutcome {
        return when (kind) {
            "APP_LAUNCH" -> launchPackage(target)
            "APP_INTENT" -> sendIntent(target, payload)
            "APP_RAW" -> sendIntent(target, payload)
            "VOLUME" -> setVolume(target)
            "MEDIA" -> sendMediaCommand(target)
            // SYSTEM covers four commands; only `open_settings` maps to an
            // unprivileged intent. The other three (`screen_off`, `night_mode`,
            // `screenshot`) need a device-owner or root API, so they report the
            // privilege requirement rather than pretending.
            "SYSTEM" -> runSystemCommand(target)
            // KEYCODE splits: the media-transport keycodes are dispatchable by an
            // ordinary app, everything else needs privilege.
            "KEYCODE" -> sendKeycode(target)
            // The firmware's half (OUT_*, NONE, BUZZ). Reaching here means the caller
            // dispatched an action it should have skipped.
            else -> ActionOutcome.NotAppSide(kind)
        }
    }

    private fun launchPackage(pkg: String): ActionOutcome {
        if (pkg.isEmpty()) return ActionOutcome.NoHandler(pkg)
        val intent = context.packageManager.getLaunchIntentForPackage(pkg)
        // **A null here is NOT "not installed".** `getLaunchIntentForPackage`
        // honors Android 11's package-visibility filtering, so it returns null for
        // an app that IS installed but is not visible to us. The manifest's
        // `<queries>` widens what we can see, but it cannot be complete: the user
        // picks the package at runtime, so a package we never declared stays
        // invisible. Reporting [AppNotInstalled] on a null would tell the user to
        // install something already installed -- and it would REPLACE a launch that
        // works, because startActivity does not need visibility at all. So fall
        // back to the explicit (package-named) intent, which needs no query and no
        // visibility, and let the launch itself be the test.
        val target = intent ?: Intent(Intent.ACTION_MAIN).apply {
            addCategory(Intent.CATEGORY_LAUNCHER)
            setPackage(pkg)
        }
        // FLAG_ACTIVITY_NEW_TASK is required to start an activity from a
        // non-activity context, which is the only context this runs in.
        target.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        return try {
            context.startActivity(target)
            ActionOutcome.Ran
        } catch (e: android.content.ActivityNotFoundException) {
            // Only NOW does "not installed" have evidence: the system rejected the
            // launch, so no activity exists for this package. This is the branch the
            // old null-check was standing in for, and it is reachable only after
            // actually trying.
            ActionOutcome.AppNotInstalled(pkg)
        } catch (e: SecurityException) {
            // BAL hardening surfaces here: the system refuses a background activity
            // start. Reported as a security refusal so the user learns the role or
            // permission that would fix it, rather than seeing nothing happen.
            ActionOutcome.Blocked(
                "Android refused to start $pkg from the background. Grant the " +
                    "\"display over other apps\" or \"start activities from " +
                    "background\" permission for SWC, then retry."
            )
        } catch (e: Exception) {
            ActionOutcome.Blocked(e.message ?: e::class.java.simpleName)
        }
    }

    /**
     * `VOLUME` (spec 3.6): adjust a stream's volume. `target` names the stream
     * (`media`/`call`/`ring`/`alarm`); the action raises it one step.
     *
     * **`adjustStreamVolume`, not `setStreamVolume`.** Spec 3.6 notes the value of
     * `VOLUME` over a stock SWC is the *absolute* set, but an absolute value needs a
     * second parameter this kind does not carry (spec 3.6's two-string budget: the
     * one slot is the stream NAME). A relative step is what the one parameter can
     * express, it works for every stream, and it needs no `MODIFY_AUDIO_SETTINGS`
     * beyond the default a foreground app already has for its own streams. An
     * unknown stream name is refused rather than silently defaulting to media --
     * acting on the wrong stream is worse than acting on none.
     */
    private fun setVolume(streamName: String): ActionOutcome {
        val stream = STREAMS[streamName.lowercase()]
            ?: return ActionOutcome.NoHandler(streamName)
        return try {
            val am = context.getSystemService(Context.AUDIO_SERVICE) as android.media.AudioManager
            am.adjustStreamVolume(stream, android.media.AudioManager.ADJUST_RAISE, 0)
            ActionOutcome.Ran
        } catch (e: SecurityException) {
            ActionOutcome.Blocked(
                "Android refused to change the $streamName volume. Grant SWC the " +
                    "\"modify audio settings\" permission, then retry."
            )
        } catch (e: Exception) {
            ActionOutcome.Blocked(e.message ?: e::class.java.simpleName)
        }
    }

    /**
     * `MEDIA` (spec 3.6): transport control via the media-session dispatch, which is
     * `AudioManager.dispatchMediaKeyEvent`. This reaches whatever holds the active
     * media session, so it works on any player rather than a named one.
     *
     * Key DOWN then UP, because a media session acts on the pair -- sending only a
     * DOWN leaves the command half-issued on some players.
     */
    private fun sendMediaCommand(command: String): ActionOutcome {
        val key = MEDIA_KEYS[command.lowercase()] ?: return ActionOutcome.NoHandler(command)
        return try {
            val am = context.getSystemService(Context.AUDIO_SERVICE) as android.media.AudioManager
            am.dispatchMediaKeyEvent(android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN, key))
            am.dispatchMediaKeyEvent(android.view.KeyEvent(android.view.KeyEvent.ACTION_UP, key))
            ActionOutcome.Ran
        } catch (e: Exception) {
            ActionOutcome.Blocked(e.message ?: e::class.java.simpleName)
        }
    }

    /**
     * `SYSTEM` (spec 3.6): one of four housekeeping commands.
     *
     * Only `open_settings` is reachable without privilege, and it is an ordinary
     * `ACTION_SETTINGS` intent. The other three (`screen_off`, `night_mode`,
     * `screenshot`) are device-owner / root APIs -- `DevicePolicyManager` for the
     * first two, a privileged screenshot API for the third -- so they report the
     * privilege requirement (spec N-6) instead of silently doing nothing.
     */
    private fun runSystemCommand(command: String): ActionOutcome = when (command.lowercase()) {
        "open_settings" -> {
            // Reuse the intent path: ACTION_SETTINGS needs no data and no package.
            sendIntent(android.provider.Settings.ACTION_SETTINGS, "")
        }
        "screen_off", "night_mode", "screenshot" ->
            ActionOutcome.Privileged(command)
        else -> ActionOutcome.NoHandler(command)
    }

    /**
     * `KEYCODE` (spec 3.6): inject a key event.
     *
     * **Only the media/volume keycodes are injectable by an ordinary app**, and via
     * the media dispatch rather than `InputManager.injectInputEvent` (which needs
     * `INJECT_EVENTS`, a signature permission). A named keycode outside that set --
     * `KEYCODE_HOME`, `KEYCODE_BACK`, an arbitrary letter -- reports the privilege
     * requirement rather than a false `Ran`. The set is deliberately the media
     * transport keys, which is what the spec's own example
     * (`KEYCODE_MEDIA_NEXT`) is.
     */
    private fun sendKeycode(name: String): ActionOutcome {
        val key = KEYCODE_KEYS[name.uppercase()] ?: return ActionOutcome.Privileged(name)
        return try {
            val am = context.getSystemService(Context.AUDIO_SERVICE) as android.media.AudioManager
            am.dispatchMediaKeyEvent(android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN, key))
            am.dispatchMediaKeyEvent(android.view.KeyEvent(android.view.KeyEvent.ACTION_UP, key))
            ActionOutcome.Ran
        } catch (e: Exception) {
            ActionOutcome.Blocked(e.message ?: e::class.java.simpleName)
        }
    }

    private fun sendIntent(action: String, data: String): ActionOutcome {
        if (action.isEmpty()) return ActionOutcome.NoHandler(action)
        val intent = Intent(action)
        if (data.isNotEmpty()) intent.data = Uri.parse(data)
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

        return try {
            context.startActivity(intent)
            ActionOutcome.Ran
        } catch (e: android.content.ActivityNotFoundException) {
            // The system found no handler for the action. The message is the same as
            // the old pre-query version's, but the DETECTION is now the launch
            // itself rather than `queryIntentActivities`, which Android 11's
            // visibility filtering can empty out for an intent the system can in
            // fact resolve -- reporting [NoHandler] for a perfectly good action.
            ActionOutcome.NoHandler(action)
        } catch (e: SecurityException) {
            ActionOutcome.Blocked(
                "Android refused to send $action from the background. Grant SWC " +
                    "permission to start activities, then retry."
            )
        } catch (e: Exception) {
            ActionOutcome.Blocked(e.message ?: e::class.java.simpleName)
        }
    }
}
