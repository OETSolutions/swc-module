package com.oetsolutions.swc.link

import kotlinx.coroutines.flow.Flow

/**
 * The byte-level link to the device.
 *
 * **An interface, not a class, and that is the point.** The USB transport needs
 * real hardware and real host-mode permissions, so anything that depends on it
 * directly can only be tested on a device. Everything interesting about the link
 * -- frame reassembly, nack handling, version mismatch, the config round trip --
 * is pure logic, and putting the bytes behind this seam is what lets the JVM
 * suite cover all of it with no board attached.
 *
 * Implementations must be safe to call [close] on more than once.
 */
interface SwcTransport {
    /** Send raw bytes. The caller supplies whole frames, newline included. */
    suspend fun write(bytes: ByteArray)

    /**
     * Bytes as they arrive. **Chunking is arbitrary**: a USB read may deliver
     * half a frame or three frames at once, so the consumer must reassemble on
     * newlines rather than assuming one emission is one frame.
     */
    val incoming: Flow<ByteArray>

    fun close()
}
