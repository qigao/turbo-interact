package io.turbointeract.android

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/** Actual product wire serialization in the existing Android Library tests. */
class H264PacketWireTest {
    private val ref = DisplayRef(44L, 5L, 7, 3L, 2L)
    private val stamp = H264CaptureStamp(ref, 1234L, 5678L, 1L, 8L, 33_333L, 88_000L)
    private val packet = AndroidH264Capture.EncodedPacket(
        byteArrayOf(0, 0, 0, 1, 0x65, 0x01),
        33_333L, false, true, false, stamp)

    @Test fun marshalsFullSourceIdentityInFixedNetworkByteOrder() {
        val bytes = H264PacketWireV1.encode(packet, 64, 64)
        assertEquals(H264PacketWireV1.HEADER_BYTES + packet.bytes.size, bytes.size)
        val b = ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        assertEquals("TIHP", String(bytes.copyOfRange(0, 4), Charsets.US_ASCII))
        b.position(4)
        assertEquals(1, b.short.toInt())
        assertEquals(116, b.short.toInt())
        assertEquals(H264PacketWireV1.FLAG_KEY, b.int)
        assertEquals(AndroidH264Capture.FRAMING_ANNEX_B, b.int)
        assertEquals(64, b.int)
        assertEquals(64, b.int)
        assertEquals(6, b.int)
        assertEquals(44L, b.long)
        assertEquals(5L, b.long)
        assertEquals(7, b.int)
        assertEquals(0, b.int)
        assertEquals(3L, b.long)
        assertEquals(2L, b.long)
        assertEquals(1234L, b.long)
        assertEquals(5678L, b.long)
        assertEquals(1L, b.long)
        assertEquals(8L, b.long)
        assertEquals(33_333L, b.long)
        assertEquals(88_000L, b.long)
        assertEquals(116, b.position())
        assertTrue(bytes.copyOfRange(116, bytes.size).contentEquals(packet.bytes))
    }

    @Test fun carriesZeroByteEosWithoutInventingPresentationOrDecodeClock() {
        val eos = packet.copy(bytes=ByteArray(0), keyFrame=false,
            endOfStream=true, presentationTimeUs=33_333L)
        val encoded = H264PacketWireV1.encode(eos, 64, 64)
        assertEquals(116, encoded.size)
        assertEquals(H264PacketWireV1.FLAG_EOS,
            ByteBuffer.wrap(encoded).order(ByteOrder.BIG_ENDIAN).getInt(8))
    }

    @Test fun refusesUnboundMalformedOrCrossBudgetPacket() {
        for (bad in listOf(
            packet.copy(stamp=null),
            packet.copy(presentationTimeUs=33_334L),
            packet.copy(bytes=byteArrayOf(0, 0, 0, 3, 0x65)),
            packet.copy(endOfStream=true),
            packet.copy(codecConfig=true),
            packet.copy(framing=2)
        )) {
            assertThrows(IllegalArgumentException::class.java) {
                H264PacketWireV1.encode(bad, 64, 64)
            }
        }
        assertThrows(IllegalArgumentException::class.java) {
            H264PacketWireV1.encode(packet, 4096, 2048)
        }
    }
}
