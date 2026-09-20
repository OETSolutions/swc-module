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
 * reason is practical: the five failures below have five different fixes, and
 * "connection error" tells the user none of them. A version mismatch is not fixed by
 * reseating the cable; a maintenance-mode device is not fixed by reinstalling.
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

    /** The device is in maintenance mode and is not serving the app link. */
    data object InMaintenance : LinkProblem

    /** The cable is not connected, or nothing enumerated. */
    data object NoDevice : LinkProblem
}
