package com.oetsolutions.swc.link

/**
 * Why the app cannot talk to the device, stated so the user can ACT.
 *
 * **This lives in `link` rather than beside the screen that renders it**, because
 * it is produced by the link: `UsbSerialTransport` returns one from `open()`, and
 * `SwcClient` sets a `LinkState` that maps onto one. A type whose producers are in
 * `link` and whose consumer is in `ui` belongs with its producers — the other way
 * round forces the transport to import a screen package, at which point a UI
 * refactor is a change to a USB driver.
 *
 * The plan is explicit that a generic "connection error" is not acceptable, and the
 * reason is practical: the failures below have different fixes, and "connection
 * error" tells the user none of them. A version mismatch is not fixed by reseating
 * the cable; a failed config digest is not fixed by replugging.
 *
 * Every one is rendered by `ui/LinkScreen.kt`'s `describe()`, whose `when` is
 * exhaustive so that adding a case here is a compile error there rather than a
 * silently generic message.
 */
sealed interface LinkProblem {
    /** Android has not granted USB permission for this device. */
    data class NoUsbPermission(val deviceName: String) : LinkProblem

    /** A USB device is attached but it is not the adapter. */
    data class NotOurDevice(val found: String) : LinkProblem

    /** The firmware speaks a different protocol version (spec 4.5). */
    data class VersionMismatch(val firmware: Int, val app: Int) : LinkProblem

    /** The cable is not connected, or nothing enumerated. */
    data object NoDevice : LinkProblem

    /**
     * The link is up but the conversation failed, and the reason is known.
     *
     * Distinct from [NoDevice] on purpose. The client raises `LinkState.Failed`
     * with a specific reason for a torn config run, a digest mismatch, a chunk
     * that ran past its declared length, or an over-long input line -- every one
     * of which is a device that IS enumerated and answering. Mapping them onto
     * [NoDevice] told the user to check a cable that was working, and "Try again"
     * re-ran the same failing read with nothing naming what actually broke.
     */
    data class LinkFailed(val reason: String) : LinkProblem

    /**
     * Nothing has arrived for spec §4.4's 10 s while the link looked healthy --
     * the app's OWN finding, from the silence watchdog (open item N-27).
     *
     * Its own case, not folded into [NoDevice] or [LinkFailed], because it is a
     * different kind of fact: the device never said anything was wrong, it simply
     * stopped. The common cause is the cable coming out (the transport's read loop
     * ends on a `bulkTransfer` error and emits nothing) or the device losing power,
     * so the guidance is physical rather than a retry.
     */
    data object SilenceExpired : LinkProblem
}
