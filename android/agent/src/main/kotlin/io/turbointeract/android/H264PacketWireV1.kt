package io.turbointeract.android

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * A bounded network-byte-order envelope for a *copied* H264 access unit.
 * Hosts may send it ONLY over a separately authenticated and framed
 * transport. This is not an ADB command, RTP fragmentation or frame-sharing
 * protocol; remote FrameRef and input authorization never cross the wire.
 *
 * Exact header (116 bytes), all integers big endian:
 * magic[4], version:u16, headerBytes:u16, flags:u32, framing:u32,
 * width:u32, height:u32, payloadBytes:u32,
 * deviceSession:u64, sessionGeneration:u64, displayId:u32, reserved:u32,
 * displayGeneration:u64, geometryGeneration:u64,
 * captureUuidHi:u64, captureUuidLo:u64, streamGeneration:u64,
 * packetSequence:u64, ptsUs:u64, androidReceivedAtNs:u64, payload[N].
 *
 * androidReceivedAtNs is informational only; remote hosts MUST NOT compare
 * it to their own monotonic clock. DTS is deliberately absent: downstream
 * can assert DTS=PTS only after validating actual Baseline/no-reorder SPS.
 */
object H264PacketWireV1 {
    const val HEADER_BYTES = 116
    const val MAX_PACKET_BYTES = 2_097_152
    const val FLAG_CONFIG = 1
    const val FLAG_KEY = 2
    const val FLAG_EOS = 4

    fun encode(
        packet: AndroidH264Capture.EncodedPacket,
        width: Int,
        height: Int,
    ): ByteArray {
        val source = requireNotNull(packet.stamp) {
            "unbound H264 capture packets cannot be transmitted"
        }
        require(H264CaptureLineageGate.fitsI420FrameStore(width, height))
        require(packet.framing == AndroidH264Capture.FRAMING_ANNEX_B)
        require(packet.bytes.size <= MAX_PACKET_BYTES)
        require(packet.presentationTimeUs == source.presentationTimeUs)
        require(!packet.codecConfig || (!packet.keyFrame && !packet.endOfStream))
        require(!packet.endOfStream || packet.bytes.isEmpty())
        require(packet.endOfStream || packet.bytes.isNotEmpty())
        if (packet.bytes.isNotEmpty()) {
            require(H264CaptureLineageGate.hasAnnexBPrefix(packet.bytes))
        }
        val flags = (if (packet.codecConfig) FLAG_CONFIG else 0) or
            (if (packet.keyFrame) FLAG_KEY else 0) or
            (if (packet.endOfStream) FLAG_EOS else 0)
        val b = ByteBuffer.allocate(HEADER_BYTES + packet.bytes.size)
            .order(ByteOrder.BIG_ENDIAN)
        b.put(byteArrayOf(0x54, 0x49, 0x48, 0x50)) // TIHP
        b.putShort(1)
        b.putShort(HEADER_BYTES.toShort())
        b.putInt(flags)
        b.putInt(AndroidH264Capture.FRAMING_ANNEX_B)
        b.putInt(width)
        b.putInt(height)
        b.putInt(packet.bytes.size)
        b.putLong(source.display.deviceSessionId)
        b.putLong(source.display.sessionGeneration)
        b.putInt(source.display.displayId)
        b.putInt(0)
        b.putLong(source.display.displayGeneration)
        b.putLong(source.display.geometryGeneration)
        b.putLong(source.captureSessionHi)
        b.putLong(source.captureSessionLo)
        b.putLong(source.streamGeneration)
        b.putLong(source.packetSequence)
        b.putLong(source.presentationTimeUs)
        b.putLong(source.receivedAtNs)
        check(b.position() == HEADER_BYTES) { "packet header length drift" }
        b.put(packet.bytes)
        return b.array()
    }
}
