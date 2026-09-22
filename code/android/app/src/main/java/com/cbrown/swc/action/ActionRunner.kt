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
     * The kind IS app-side per spec §3.6, but this build does not implement it.
     *
     * **Distinct from [NotAppSide] on purpose.** `KEYCODE`, `MEDIA`, `VOLUME` and
     * `SYSTEM` are all the app's to execute, so reporting them as "not an app-side
     * action" contradicts the spec and sends the user to change a binding that is
     * correctly configured. Saying "not implemented in this build" is the honest
     * version, and it is the difference between "your binding is wrong" and "this
     * app cannot do that yet".
     */
    data class NotImplemented(val kind: String) : ActionOutcome
}

/**
 * Runs the app-side action kinds this build implements: `APP_LAUNCH`, `APP_INTENT`
 * and the escape hatch `APP_RAW`.
 *
 * **Spec §3.6 gives the app FOUR more — `KEYCODE`, `MEDIA`, `VOLUME` and `SYSTEM` —
 * and this class does not implement them.** An earlier version of this comment
 * claimed it ran `KEYCODE`, which it never did: the `when` below had no branch for
 * it, so binding a button to a keycode produced `NotAppSide` and the user was told
 * their (valid) binding was not an app-side action. The claim is removed and the
 * gap is now reported as [ActionOutcome.NotImplemented].
 *
 * The bindings screen offers every kind in the generated `ActionKind` enum, so all
 * seven are selectable today. A user can therefore bind one that will not run, and
 * will see the "not implemented" message rather than silence. Implementing the rest
 * is gated on the target head unit's privileges — see the spec's open item on the
 * Android environment — because key injection and screen control need root, an
 * accessibility service or a system-app install.
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
     * Run [target] as `APP_LAUNCH` (a package) or `APP_INTENT` (an action, optionally
     * with a data payload).
     *
     * `kind` is the wire name from the generated contract, so the caller cannot pass
     * a kind this class does not implement without it being visible here.
     */
    fun run(kind: String, target: String, payload: String = ""): ActionOutcome {
        return when (kind) {
            "APP_LAUNCH" -> launchPackage(target)
            "APP_INTENT" -> sendIntent(target, payload)
            "APP_RAW" -> sendIntent(target, payload)
            // App-side per spec 3.6, not implemented here. Named individually so the
            // intent is visible: these are not "unknown", they are "not yet".
            "KEYCODE", "MEDIA", "VOLUME", "SYSTEM" -> ActionOutcome.NotImplemented(kind)
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
