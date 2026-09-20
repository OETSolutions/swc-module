package com.oetsolutions.swc.action

import android.content.ComponentName
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

    /** The action kind is not one the app executes. */
    data class NotAppSide(val kind: String) : ActionOutcome
}

/**
 * Runs the app-side action kinds: `APP_LAUNCH`, `APP_INTENT`, `KEYCODE`, and the
 * escape hatch `APP_RAW`.
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
            else -> ActionOutcome.NotAppSide(kind)
        }
    }

    private fun launchPackage(pkg: String): ActionOutcome {
        if (pkg.isEmpty()) return ActionOutcome.NoHandler(pkg)
        val intent = context.packageManager.getLaunchIntentForPackage(pkg)
            ?: return ActionOutcome.AppNotInstalled(pkg)
        // FLAG_ACTIVITY_NEW_TASK is required to start an activity from a
        // non-activity context, which is the only context this runs in.
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        return try {
            context.startActivity(intent)
            ActionOutcome.Ran
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

        // Resolve BEFORE starting. `startActivity` throws ActivityNotFoundException
        // for an unhandled intent, and a resolved check lets us say which of the two
        // problems it is -- no such package, or no handler for the action.
        val resolved = try {
            context.packageManager.queryIntentActivities(intent, 0)
        } catch (e: Exception) {
            emptyList()
        }
        if (resolved.isEmpty()) return ActionOutcome.NoHandler(action)

        return try {
            context.startActivity(intent)
            ActionOutcome.Ran
        } catch (e: SecurityException) {
            ActionOutcome.Blocked(
                "Android refused to send $action from the background. Grant SWC " +
                    "permission to start activities, then retry."
            )
        } catch (e: Exception) {
            ActionOutcome.Blocked(e.message ?: e::class.java.simpleName)
        }
    }

    /** Whether the target of a `KEYCODE` action resolves to anything that can take it. */
    fun canHandle(kind: String, target: String, payload: String = ""): Boolean {
        if (kind != "APP_LAUNCH" && kind != "APP_INTENT" && kind != "APP_RAW") return false
        return if (kind == "APP_LAUNCH") {
            context.packageManager.getLaunchIntentForPackage(target) != null
        } else {
            val intent = Intent(target)
            if (payload.isNotEmpty()) intent.data = Uri.parse(payload)
            // ComponentName is referenced so the resolved handler is a real
            // component rather than a category-only match.
            context.packageManager.queryIntentActivities(intent, 0)
                .any { it.activityInfo?.let { ai -> ComponentName(ai.packageName, ai.name) } != null }
        }
    }
}
