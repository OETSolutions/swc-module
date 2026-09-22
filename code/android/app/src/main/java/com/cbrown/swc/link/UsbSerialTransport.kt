package com.oetsolutions.swc.link

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlin.coroutines.coroutineContext

/**
 * The device's USB CDC ACM endpoint, over Android's USB host API.
 *
 * **Why this does not use `usb-serial-for-android`.** Plan Task 20 named
 * `com.github.mik3y:usb-serial-for-android:3.11.0`, and that dependency cannot
 * resolve from this project: it is a JitPack artifact (`repo1.maven.org/.../mik3y/`
 * returns 404) and `settings.gradle.kts` declares only `google()` and
 * `mavenCentral()`. It was also the wrong tool. That library drives UART-bridge
 * chips (FTDI, CP210x, CH34x) that expose a vendor-specific interface; the
 * firmware here is a **native CDC ACM** device, which is a USB class Android's
 * own host API speaks directly. Adopting a third-party serial stack to talk to an
 * endpoint we implement would add a dependency, a JitPack repository, and a
 * USB-permission model that does not match [LinkProblem.NoUsbPermission]'s
 * four-state design — to do a job `UsbDeviceConnection` already does.
 *
 * **What is NOT verified here, stated plainly.** Enumeration, the permission
 * dialog, interface claiming and re-enumeration need a real adapter on a real
 * phone. The emulator has no USB host controller, so none of this class can be
 * exercised on it. What IS verified is everything downstream: `SwcClient`'s frame
 * reassembly, nack handling, version mismatch and the chunked config round trip
 * are all JVM-tested against a fake transport, which is what putting the bytes
 * behind `SwcTransport` was for. The untested part is deliberately thin — find
 * endpoints, read, write — and every failure mode it has maps to a [LinkProblem]
 * the user can act on rather than a generic error.
 */
class UsbSerialTransport(private val context: Context) : SwcTransport {

    private val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager

    private val _incoming = MutableSharedFlow<ByteArray>(
        extraBufferCapacity = 64,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )
    override val incoming: Flow<ByteArray> = _incoming.asSharedFlow()

    private var connection: UsbDeviceConnection? = null
    private var dataInterface: UsbInterface? = null
    private var epIn: UsbEndpoint? = null
    private var epOut: UsbEndpoint? = null
    private var reader: Job? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /**
     * Find the adapter, get permission if needed, and open the port.
     *
     * Returns the [LinkProblem] to show the user, or null on success — a return
     * value rather than a thrown exception because every failure here is a state
     * the UI already has a message for, and `describe()` is exhaustive over
     * [LinkProblem]. Throwing would push the mapping back to the caller and let a
     * new failure mode slip through as a crash.
     */
    suspend fun open(): LinkProblem? {
        val device = findAdapter()
        if (device == null) {
            // Distinguish "nothing plugged in" from "something else is". The two
            // have different fixes, which is the whole reason LinkProblem exists.
            val any = manager.deviceList.values.firstOrNull()
            return if (any == null) LinkProblem.NoDevice
            else LinkProblem.NotOurDevice(any.deviceName)
        }
        if (!manager.hasPermission(device)) {
            if (!awaitPermission(device)) {
                return LinkProblem.NoUsbPermission(device.deviceName)
            }
        }
        val (iface, inEp, outEp) = findEndpoints(device)
            ?: return LinkProblem.NotOurDevice(device.deviceName)

        val conn = manager.openDevice(device)
            ?: return LinkProblem.NotOurDevice(device.deviceName)
        if (!conn.claimInterface(iface, true)) {
            conn.close()
            return LinkProblem.NotOurDevice(device.deviceName)
        }
        // The firmware waits for DTR before it sends `hello`, because a host that
        // has not opened the port is not reading. Asserting it here is what makes
        // the opening frame arrive rather than be discarded.
        //
        // Android's UsbDeviceConnection has NO setDtr: there is no Android API for
        // modem control lines, so the CDC SET_CONTROL_LINE_STATE request is issued
        // as a raw control transfer. This is the one place the class speaks CDC
        // rather than generic USB, and getting it wrong is silent -- the port opens,
        // nothing errors, and no frame ever arrives.
        //
        // **It must name the COMMUNICATION interface, not the data interface.**
        // CDC 1.2 §6.3.12 puts this request on the communication interface, and the
        // device side enforces that: TinyUSB's `cdcd_control_xfer_cb` matches the
        // request's `wIndex` against `p_cdc->itf_num`, which is the communication
        // interface number (`cdc_device.c`; TUD_CDC_DESCRIPTOR emits comm = n,
        // data = n+1). Addressed to the data interface the `wIndex` matches no CDC
        // instance, the request STALLs, `tud_cdc_line_state_cb` never fires, and the
        // firmware's `CdcLineStateCallback` never records an open -- so `hello` is
        // never sent, the app never leaves `Disconnected`, and the config reply run
        // never starts. The failure is exactly the silence the comment above warns
        // about: everything enumerates, nothing errors, no frame arrives.
        val commId = cdcCommunicationInterfaceId(device, iface.id)
            ?: return LinkProblem.NotOurDevice(device.deviceName)
        conn.setControlLineState(commId, dtr = true)
        connection = conn
        dataInterface = iface
        epIn = inEp
        epOut = outEp
        startReader()
        return null
    }

    /**
     * CDC SET_CONTROL_LINE_STATE (USB CDC 1.2 §6.3.12), as a class-interface
     * control transfer.
     *
     * bmRequestType 0x21 is host-to-device | class | interface; bRequest 0x22 is
     * SET_CONTROL_LINE_STATE; wValue's bit 0 is DTR and bit 1 is RTS. The device's
     * TinyUSB stack is what raises `callback_line_state_changed` on this, which is
     * the callback that sends `hello`.
     */
    private fun UsbDeviceConnection.setControlLineState(interfaceId: Int, dtr: Boolean) {
        val value = if (dtr) 0x01 else 0x00
        try {
            controlTransfer(0x21, 0x22, value, interfaceId, null, 0, 1000)
        } catch (e: Exception) {
            // A device that refuses the request still enumerates; `hello` then
            // never arrives and the link reports no device, which is the truthful
            // outcome rather than a crash here.
        }
    }

    /**
     * The adapter, by USB CDC class rather than by VID/PID.
     *
     * Matching the interface class is deliberate. Espressif's VID/PIDs are shared
     * across every ESP32 board that enumerates as CDC, so a VID/PID match would
     * also select a different ESP32-based product on the same hub — and the
     * firmware exposes no product string the app can rely on before it has a
     * `hello`. Claiming whatever presents a CDC data interface, then letting the
     * protocol version check in `SwcClient` reject a stranger, keeps the identity
     * decision in one place (spec 4.5) instead of duplicating it here.
     */
    private fun findAdapter(): UsbDevice? =
        manager.deviceList.values.firstOrNull { findEndpoints(it) != null }

    private fun findEndpoints(device: UsbDevice): Triple<UsbInterface, UsbEndpoint, UsbEndpoint>? {
        for (i in 0 until device.interfaceCount) {
            val iface = device.getInterface(i)
            // USB_CLASS_CDC_DATA: the interface that carries the actual bytes. The
            // companion comm interface (class 0x02) has only the interrupt
            // endpoint for notifications, so it is not the one to read.
            if (iface.interfaceClass != UsbConstants.USB_CLASS_CDC_DATA) continue
            var inEp: UsbEndpoint? = null
            var outEp: UsbEndpoint? = null
            for (e in 0 until iface.endpointCount) {
                val ep = iface.getEndpoint(e)
                if (ep.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
                if (ep.direction == UsbConstants.USB_DIR_IN) inEp = ep else outEp = ep
            }
            if (inEp != null && outEp != null) return Triple(iface, inEp, outEp!!)
        }
        return null
    }

    /**
     * The CDC communication interface the data interface [dataId] is paired with.
     *
     * SET_CONTROL_LINE_STATE is a request on the **communication** interface (CDC
     * 1.2 §6.3.12), and the device side enforces it: TinyUSB matches `wIndex`
     * against the CDC instance's communication interface number, so sending it to
     * the data interface stalls and the line state is never delivered. The pair is
     * identified by the CDC Union functional descriptor, which names the
     * communication interface and its subordinate data interface -- but Android's
     * USB host API does not expose class-specific descriptors, so the association
     * cannot be read here.
     *
     * Delegates the choice to [cdcCommunicationInterfaceId], which is a plain
     * function over (id, class) pairs so the rule is testable on the JVM with no
     * Android at all. That matters here: the failure this guards against is silent
     * (see [selectControlLineInterface]).
     */
    private fun cdcCommunicationInterfaceId(device: UsbDevice, dataId: Int): Int? {
        val ids = (0 until device.interfaceCount).map { i ->
            val iface = device.getInterface(i)
            iface.id to iface.interfaceClass
        }
        return selectControlLineInterface(ids, dataId)
    }

    private suspend fun awaitPermission(device: UsbDevice): Boolean {
        val action = "${context.packageName}.USB_PERMISSION"
        val granted = kotlinx.coroutines.CompletableDeferred<Boolean>()
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context?, intent: Intent?) {
                if (intent?.action != action) return
                granted.complete(intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false))
            }
        }
        val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_IMMUTABLE else 0
        val pi = PendingIntent.getBroadcast(context, 0, Intent(action), flags)
        // Android 14 requires the receiver to be registered with an explicit
        // exported flag; a runtime-registered receiver has no export surface, so
        // NOT_EXPORTED is both correct and the only value the platform accepts.
        if (Build.VERSION.SDK_INT >= 33) {
            context.registerReceiver(receiver, IntentFilter(action), Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            context.registerReceiver(receiver, IntentFilter(action))
        }
        try {
            manager.requestPermission(device, pi)
            return kotlinx.coroutines.withTimeoutOrNull(30_000) { granted.await() } ?: false
        } catch (e: Exception) {
            return false
        } finally {
            try {
                context.unregisterReceiver(receiver)
            } catch (e: Exception) {
                // Already unregistered; nothing to do.
            }
        }
    }

    /**
     * Read until closed. One `bulkTransfer` per iteration with a timeout rather
     * than a blocking read, so `close()` is observed promptly and the loop cannot
     * outlive the interface it is reading from.
     */
    private fun startReader() {
        val conn = connection ?: return
        val ep = epIn ?: return
        reader?.cancel()
        reader = scope.launch {
            val buf = ByteArray(ep.maxPacketSize.coerceAtLeast(64))
            while (coroutineContext.isActive) {
                val n = try {
                    conn.bulkTransfer(ep, buf, buf.size, READ_TIMEOUT_MS)
                } catch (e: Exception) {
                    -1
                }
                if (n > 0) _incoming.tryEmit(buf.copyOf(n))
                // A timeout (-1) is normal on an idle link and must NOT end the
                // loop: the device is silent whenever nothing is happening. Only
                // `close()` ends it, and it cancels this job.
            }
        }
    }

    override suspend fun write(bytes: ByteArray) {
        val conn = connection ?: return
        val ep = epOut ?: return
        var sent = 0
        // A Full-Speed bulk endpoint takes at most `maxPacketSize` per transfer, so
        // a 1024-byte frame needs several. Advancing by the bytes the transfer
        // reports is the same rule `UsbCdc` follows on the device side.
        while (sent < bytes.size) {
            val n = try {
                conn.bulkTransfer(ep, bytes.copyOfRange(sent, bytes.size), bytes.size - sent, 1000)
            } catch (e: Exception) {
                -1
            }
            if (n <= 0) return
            sent += n
        }
    }

    override suspend fun reopen(): LinkProblem? {
        // Release whatever is held BEFORE re-enumerating: a stale connection to a
        // device that was unplugged would otherwise be kept, and `openDevice`
        // would fail for the wrong reason. `close` is idempotent by contract.
        close()
        return open()
    }

    override fun close() {
        reader?.cancel()
        reader = null
        try {
            dataInterface?.let { connection?.releaseInterface(it) }
        } catch (e: Exception) {
            // Releasing an already-released interface throws; closing is idempotent
            // by contract (SwcTransport), so a second close must not propagate.
        }
        try {
            connection?.close()
        } catch (e: Exception) {
            // As above.
        }
        connection = null
        dataInterface = null
        epIn = null
        epOut = null
    }

    private companion object {
        const val READ_TIMEOUT_MS = 200
    }
}

/**
 * Which interface id `SET_CONTROL_LINE_STATE` must be addressed to.
 *
 * **This is the interface the request belongs on, and getting it wrong is silent.**
 * CDC 1.2 §6.3.12 makes `SET_CONTROL_LINE_STATE` a class request on the
 * **communication** interface, and the device side enforces it: TinyUSB's
 * `cdcd_control_xfer_cb` matches the request's `wIndex` against `p_cdc->itf_num`,
 * the CDC instance's communication interface number (`cdc_device.c`), stalling the
 * request when it matches no instance. Addressed to the **data** interface it
 * matches nothing, the request stalls, `tud_cdc_line_state_cb` never fires, and the
 * firmware's DTR callback never records the open -- so `hello` is never sent, the
 * app never leaves `Disconnected`, and the whole config round trip never starts.
 * Nothing errors at any point; the link is simply, permanently silent.
 *
 * [interfaces] is `(id, interfaceClass)` for every interface on the device, in
 * enumeration order, and [dataId] is the data interface the transport claimed.
 * `TUD_CDC_DESCRIPTOR` emits the pair as `(commId, commId + 1)`, so the
 * communication interface is normally the one immediately preceding the data
 * interface; a composite device with more than one CDC instance is why the
 * relation is checked rather than assumed. Falling back to the first communication
 * interface covers the same ordering when the data interface happens not to sit at
 * `commId + 1`, and null means the device exposes no communication interface at all
 * -- there is then no correct target, and the caller refuses the device rather than
 * sending the request somewhere that would stall.
 */
internal fun selectControlLineInterface(interfaces: List<Pair<Int, Int>>, dataId: Int): Int? {
    var firstComm: Int? = null
    for ((id, cls) in interfaces) {
        if (cls != UsbConstants.USB_CLASS_COMM) continue
        if (firstComm == null) firstComm = id
        if (id == dataId - 1) return id
    }
    return firstComm
}
