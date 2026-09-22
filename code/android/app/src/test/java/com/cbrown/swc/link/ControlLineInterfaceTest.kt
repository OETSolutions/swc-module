package com.oetsolutions.swc.link

import android.hardware.usb.UsbConstants
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Where `SET_CONTROL_LINE_STATE` is addressed.
 *
 * This rule is worth a dedicated test because every failure of it is SILENT: a
 * request sent to the wrong interface stalls on the device, the DTR callback never
 * fires, and the firmware never sends its opening `hello`. Nothing errors at the
 * app, nothing errors at the device, and the link is simply dead -- which reads as
 * a broken adapter. See [selectControlLineInterface].
 */
class ControlLineInterfaceTest {

    private val comm = UsbConstants.USB_CLASS_COMM          // 0x02
    private val data = UsbConstants.USB_CLASS_CDC_DATA      // 0x0A

    @Test
    fun `the request goes to the communication interface, not the data one`() {
        // The device's own layout: TUD_CDC_DESCRIPTOR emits comm = n, data = n + 1.
        val interfaces = listOf(0 to comm, 1 to data)
        assertEquals(0, selectControlLineInterface(interfaces, dataId = 1))
    }

    @Test
    fun `it does not return the data interface even when that is the only one listed first`() {
        // A device whose data interface precedes its comm interface in enumeration
        // order. The answer is still a COMMUNICATION interface: naming the data
        // interface is the defect this function exists to prevent.
        val interfaces = listOf(0 to data, 1 to comm)
        val chosen = selectControlLineInterface(interfaces, dataId = 0)
        assertEquals(1, chosen)
    }

    @Test
    fun `a second CDC instance's pair is matched, not the first comm interface`() {
        // Two CDC instances: comm/data = (0,1) and (2,3). Claiming data interface 3
        // must select comm interface 2 -- the first comm interface would be the
        // WRONG CDC instance, so its `itf_num` would not match either.
        val interfaces = listOf(0 to comm, 1 to data, 2 to comm, 3 to data)
        assertEquals(2, selectControlLineInterface(interfaces, dataId = 3))
    }

    @Test
    fun `a device with no communication interface has no correct target`() {
        val interfaces = listOf(0 to data)
        assertNull(selectControlLineInterface(interfaces, dataId = 0))
    }

    @Test
    fun `the first communication interface is the fallback when the pair is not adjacent`() {
        // The union relation is normally commId + 1, but the API cannot read the
        // union descriptor, so when the data interface does not sit right after a
        // comm interface the first one is still a better answer than the data
        // interface.
        val interfaces = listOf(0 to comm, 1 to UsbConstants.USB_CLASS_HID, 2 to data)
        assertEquals(0, selectControlLineInterface(interfaces, dataId = 2))
    }
}
